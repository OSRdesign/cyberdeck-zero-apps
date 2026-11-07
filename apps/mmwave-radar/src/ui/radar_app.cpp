/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the screen, the pages, the timer and the input (see radar_app.hpp). The drawing is radar_draw.cpp.
 */
#include "radar_app.hpp"

#include "app.hpp"
#include "bluez_backend.hpp"
#include "demo_ble.hpp"
#include "demo_sys.hpp"
#include "diag.hpp"
#include "keyboard_input.h"
#include "palette.hpp"
#include "posix_sys.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace mmwui {

namespace {

/* one line of log with the app's tag (flushed: the harness and a pipe to the journal see it at once) */
void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *format, ...)
{
    std::fputs("[mmwave-radar] ", stderr);
    va_list ap;
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

std::int64_t mono_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char *const kTabNames[4] = {"Radar", "Table", "Graph", "Sensor"};
constexpr int kSettingCount = 7;
const char *const kSettingNames[kSettingCount] = {"Units", "Orientation", "Range", "Trails", "Hide held targets",
                                                   "Sensor type", "Connection"};

std::uint32_t state_rgb(mmw::LinkState s)
{
    switch (s) {
    case mmw::LinkState::Found: return pal::kOk;
    case mmw::LinkState::Searching: return pal::kWarn;
    case mmw::LinkState::Busy: return pal::kMuted;
    default: return pal::kBad;
    }
}

void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

void set_text_if(lv_obj_t *label, const std::string &text)
{
    const char *cur = lv_label_get_text(label);
    if (!cur || text != cur) lv_label_set_text(label, text.c_str());
}

} // namespace

/* ------------------------------------------------------------------------------------------------ construction */

RadarApp::RadarApp(lv_obj_t *screen) : screen_(screen)
{
    compute_sizes();

    /* the files: settings and the remembered port */
    if (const char *t = std::getenv("MMWAVE_TEST_CONFIG"); t && *t && std::strcmp(t, "0") != 0) {
        const char *state = std::getenv("APPLAUNCH_STATE_DIR");
        config_dir_ = std::string(state && *state ? state : "/tmp") + "/mmwave-config";
    } else {
        config_dir_ = mmw::config_dir();
    }
    std::string text;
    if (!config_dir_.empty() && mmw::read_small_file(config_dir_ + "/settings.conf", text))
        settings_ = mmw::parse_settings(text);
    mmw::Remembered remembered;
    if (!config_dir_.empty() && mmw::read_small_file(config_dir_ + "/remembered.conf", text))
        remembered = mmw::Remembered::parse(text);
    if (settings_.range_m) auto_range_m_ = settings_.range_m;

    /* the launcher's page header, as in every app */
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &page_layout_);
    const bool narrow = sz_.narrow;
#ifndef CP0_UI_NARROW_PORTRAIT_SIZE
    /* a toolkit before the narrow portrait shell (launcher v0.9.4): its rule done here (Esc TARGET wide at the left
     * edge, no title); the bar keeps the toolkit's height, the status strip is drawn for it */
    if (narrow) {
        page_layout_.esc_x = sz_.space_m;
        page_layout_.esc_w = sz_.target;
        page_layout_.title_max_w = 0;
    }
#endif
    page_ = std::make_unique<cp0::ui::Page>(
        screen_, page_layout_,
        [narrow](lv_obj_t *parent, const char *t) -> lv_obj_t * {
            lv_obj_t *l = cp0::ui::add_title(parent, t);
            if (narrow && l) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            return l;
        },
        [] { cdk::request_quit(0); });
    cp0::ui::StatusStrip::add(page_->header());
    page_->set_title({"mmWave Radar"});
#ifndef CP0_UI_NARROW_PORTRAIT_SIZE
    if (narrow) { /* the Esc's tap area over the whole bar height */
        const uint32_t n = lv_obj_get_child_count(page_->header());
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t *c = lv_obj_get_child(page_->header(), static_cast<int32_t>(i));
            if (lv_obj_check_type(c, &lv_button_class))
                lv_obj_set_ext_click_area(c, std::max(0, (page_layout_.header_h - page_layout_.esc_h) / 2));
        }
    }
#endif

    w_ = page_layout_.w;
    h_ = page_layout_.h;
    body_y_ = page_layout_.body_y;
    view_h_ = h_ - body_y_ - sz_.tab_h;

    surface_ = lv_obj_create(page_->root());
    plain(surface_);
    lv_obj_set_pos(surface_, 0, body_y_);
    lv_obj_set_size(surface_, w_, h_ - body_y_);
    lv_obj_add_event_cb(surface_, key_cb, LV_EVENT_KEY, this);
    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    if (lv_group_t *g = cdk::input_group()) {
        lv_group_add_obj(g, surface_);
        lv_group_focus_obj(surface_);
    }

    build_view();
    build_notfound();
    build_sensor();
    build_chips();
    build_tabs();
    compute_layout();

    /* the link: a scripted port layer for the scenes and the demonstration, else the real ports; the Bluetooth radar
     * through BlueZ (or the scripted backend of MMWAVE_DEMO=ble...), started only when the Connection asks for it */
    const char *demo = std::getenv("MMWAVE_DEMO");
    const bool demo_ble = demo && std::strncmp(demo, "ble", 3) == 0;
    if (demo && *demo) sys_ = std::make_unique<mmw::DemoSys>(demo, [] { return mono_ms(); });
    else sys_ = std::make_unique<mmw::PosixSys>();
    if (demo && *demo) { /* a demonstration never talks to BlueZ: the scripted radios (demo_ble.hpp) */
        ble_ = std::make_unique<mmw::BleSys>(std::make_unique<mmw::DemoBle>(demo_ble ? demo : "ble", [] { return mono_ms(); }));
        if (demo_ble && settings_.connection == mmw::Connection::Usb) settings_.connection = mmw::Connection::Bluetooth; /* not saved */
    } else {
        auto bluez = std::make_unique<mmw::BluezBackend>();
        bluez->set_log([](const std::string &l) { log_line("%s", l.c_str()); });
        ble_ = std::make_unique<mmw::BleSys>(std::move(bluez));
    }
    ble_->set_remembered(remembered.ble_address, remembered.ble_name);
    mux_ = std::make_unique<mmw::MuxSys>(*sys_, ble_.get());
    mux_->set_mode(settings_.connection);
    mmw::LinkConfig link_cfg;
    link_cfg.detect.sensor = settings_.sensor;
    link_ = std::make_unique<mmw::Link>(*mux_, link_cfg);
    now_ = mono_ms();
    link_->start(now_, remembered);
    log_line("%s %dx%d%s%s, listen only, remembered %s%s%s%s", kVersion, w_, h_,
                 narrow ? " narrow portrait" : "", demo && *demo ? (std::string(" demo ") + demo).c_str() : "",
                 remembered.empty() ? "none" : remembered.port_id.c_str(),
                 settings_.connection == mmw::Connection::Usb ? "" : ", connection ",
                 settings_.connection == mmw::Connection::Usb ? "" : mmw::connection_name(settings_.connection),
                 remembered.ble_empty() ? "" : (", bluetooth " + remembered.ble_address).c_str());
    if (lay_.tall)
        log_line("tall radar page: view %dx%d, plot y %d h %d, list y %d h %d (3 x %d + gaps %d), chart y %d h %d", w_,
                 view_h_, lay_.plot_y, lay_.plot_h, lay_.list_y, lay_.list_h, list_block_h_, list_gap_, lay_.chart_y,
                 lay_.chart_h);

    label_w_max_ = std::max(text_w("T3 88.88 m  88.8 km/h", sz_.caption_px), text_w("T3 8888 mm  888 cm/s", sz_.caption_px)) + 6 +
                    text_w(" held", sz_.caption_px);
    update_tabs();
    update_chips();
    update_view_visibility();
    update_notfound(true);
    update_sensor(true);
    timer_ = lv_timer_create(timer_cb, 30, this);
    /* the render time paces the redraws (a slow board gets fewer frames, never a backlog) and, with $MMWAVE_TIMING=1,
     * goes to the log every 5 s */
    const char *t = std::getenv("MMWAVE_TIMING");
    timing_ = t && *t && std::strcmp(t, "0") != 0;
    timing_since_ = mono_ms();
    /* the diagnostic log: $MMWAVE_LOG, or the marker file <config dir>/diag (the launcher starts the app: no variable
     * there) -> <config dir>/mmwave.log, kept to its last 200 KB; neither: no file is written at all */
    bool diag_marker = false;
    diag_path_ = mmw::diag_log_path(std::getenv("MMWAVE_LOG"), config_dir_, &diag_marker);
    diag_bounded_ = diag_marker;
    if (!diag_path_.empty()) {
        link_->set_diag_counting(true); /* the frame counters of the per-second line (nothing counted without a log) */
        if (diag_bounded_) mmw::diag_trim(diag_path_, mmw::kDiagKeepBytes);
        log_line("diagnostic log on: %s%s", diag_path_.c_str(), diag_marker ? " (marker file diag)" : " ($MMWAVE_LOG)");
        diag_write(std::string("log on ") + (diag_marker ? "(marker " + config_dir_ + "/diag)" : std::string("($MMWAVE_LOG)")) +
                   " " + diag_path_ + "\n");
        diag_write(std::string("start ") + kVersion + (demo && *demo ? " demo " : " real ports") + (demo && *demo ? demo : "") +
                   " config_dir=" + config_dir_ + " remembered=" + (remembered.empty() ? "none" : remembered.port_id + " " + std::to_string(remembered.baud)) +
                   mmw::diag_start_tail(settings_.connection, remembered) + "\n");
        /* the Bluetooth and link events of the log (nothing is built while the log is off) */
        ble_->set_diag([this](const std::string &l) { diag_write(l + "\n"); });
        link_->set_event_log([this](const std::string &l) { diag_write(l + "\n"); });
    }
    lv_display_add_event_cb(lv_obj_get_display(screen_), render_cb, LV_EVENT_ALL, this);
}

void RadarApp::diag_write(const std::string &text) const
{
    if (FILE *f = std::fopen(diag_path_.c_str(), "a")) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
}

void RadarApp::render_cb(lv_event_t *e)
{
    auto *self = static_cast<RadarApp *>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        self->render_start_ = mono_ms();
    } else if (code == LV_EVENT_REFR_READY && self->render_start_ > 0) {
        const std::int64_t now = mono_ms(), d = now - self->render_start_;
        self->render_start_ = 0;
        self->last_render_ms_ = d;
        if (!self->timing_) return;
        self->render_sum_ += d;
        self->render_max_ = std::max(self->render_max_, d);
        ++self->render_n_;
        if (now - self->timing_since_ >= 5000) {
            log_line("timing: %d refreshes in %lld ms, render avg %.1f ms, max %lld ms (page %d)",
                         self->render_n_, static_cast<long long>(now - self->timing_since_),
                         self->render_n_ ? static_cast<double>(self->render_sum_) / self->render_n_ : 0.0,
                         static_cast<long long>(self->render_max_), static_cast<int>(self->tab_));
            self->timing_since_ = now;
            self->render_sum_ = self->render_max_ = 0;
            self->render_n_ = 0;
        }
    }
}

RadarApp::~RadarApp()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (lv_obj_is_valid(screen_)) lv_display_remove_event_cb_with_user_data(lv_obj_get_display(screen_), render_cb, this);
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(screen_))
        lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    if (link_) link_->close(); /* the port's settings restored, the lock released; a Bluetooth radar let go */
    link_.reset();
    mux_.reset();
    if (ble_) ble_->shutdown(); /* the Bluetooth radar disconnected, the discovery ended, BlueZ closed (flushed) */
    ble_.reset();
    sys_.reset();
    page_.reset();
    if (bg_buf_) lv_draw_buf_destroy(bg_buf_); /* after the canvas that showed it */
    bg_buf_ = nullptr;
}

void RadarApp::compute_sizes()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int w = m->w, h = m->h;
    sz_.portrait = h > w;
    sz_.narrow = sz_.portrait && w < 480;
    sz_.space_s = m->tok[CP0_TOK_SPACE_S];
    sz_.space_m = m->tok[CP0_TOK_SPACE_M];
    sz_.row = m->tok[CP0_TOK_ROW_H];
    sz_.target = m->tok[CP0_TOK_TARGET];
    sz_.body_px = m->text_px[CP0_TEXT_BODY];
    sz_.caption_px = m->text_px[CP0_TEXT_CAPTION];
#ifndef CP0_UI_NARROW_PORTRAIT_SIZE
    if (sz_.narrow) { /* the launcher's narrow portrait numbers (v0.9.4), not in the pinned toolkit */
        sz_.body_px = 16;
        sz_.caption_px = 14;
        sz_.space_s = 5;
        sz_.space_m = 10;
    }
#endif
    sz_.tab_h = std::max(sz_.row, sz_.target);
    sz_.dot = std::clamp(std::min(w, h) / 26, 10, 24);
}

lv_obj_t *RadarApp::make_label(lv_obj_t *parent, int px, std::uint32_t rgb, bool wrap)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, cp0_ui_font_px(px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_CLIP);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *RadarApp::make_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (lv_obj_get_group(b)) lv_group_remove_obj(b); /* touch; the keyboard has R */
    lv_obj_set_height(b, sz_.target);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, sz_.target * 2, 0);
    lv_obj_set_style_pad_hor(b, sz_.space_m * 2, 0);
    lv_obj_set_style_radius(b, page_layout_.esc_radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pal::kPanel), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pal::kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = make_label(b, sz_.body_px, pal::kText, false);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

RadarApp::InfoRow RadarApp::make_info(lv_obj_t *parent, const char *key)
{
    InfoRow r;
    r.row = lv_obj_create(parent);
    plain(r.row);
    lv_obj_set_size(r.row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r.row, sz_.space_m, 0);
    r.key = make_label(r.row, sz_.body_px, pal::kMuted, false);
    lv_label_set_text(r.key, key);
    lv_obj_set_width(r.key, key_w_);
    r.value = make_label(r.row, sz_.body_px, pal::kMuted, true);
    lv_obj_set_flex_grow(r.value, 1);
    return r;
}

lv_obj_t *RadarApp::make_heading(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = make_label(parent, sz_.caption_px, pal::kMuted, false);
    lv_label_set_text(l, text);
    lv_obj_set_style_pad_top(l, sz_.space_m, 0);
    return l;
}

void RadarApp::build_view()
{
    bg_ = lv_canvas_create(surface_);
    lv_obj_remove_flag(bg_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bg_, 0, 0);
    bg_buf_ = lv_draw_buf_create(static_cast<uint32_t>(w_), static_cast<uint32_t>(view_h_),
                                 lv_display_get_color_format(lv_obj_get_display(screen_)), LV_STRIDE_AUTO);
    if (bg_buf_) lv_canvas_set_draw_buf(bg_, bg_buf_);
    else lv_obj_add_flag(bg_, LV_OBJ_FLAG_HIDDEN);
    view_ = lv_obj_create(surface_);
    plain(view_);
    lv_obj_set_pos(view_, 0, 0);
    lv_obj_set_size(view_, w_, view_h_);
    lv_obj_add_event_cb(view_, draw_cb, LV_EVENT_DRAW_MAIN, this);
}

void RadarApp::build_chips()
{
    chip_ = lv_obj_create(surface_);
    plain(chip_);
    lv_obj_set_pos(chip_, sz_.space_s, sz_.space_s);
    lv_obj_set_size(chip_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chip_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chip_, sz_.space_s, 0);

    lv_obj_t *pill = lv_obj_create(chip_);
    plain(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(pill, sz_.space_s + 2, 0);
    lv_obj_set_style_pad_ver(pill, 2, 0);
    lv_obj_set_style_pad_column(pill, sz_.space_s, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(pal::kPanel), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    chip_dot_ = lv_obj_create(pill);
    plain(chip_dot_);
    const int d = std::max(8, sz_.caption_px * 2 / 3);
    lv_obj_set_size(chip_dot_, d, d);
    lv_obj_set_style_radius(chip_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(chip_dot_, LV_OPA_COVER, 0);
    chip_label_ = make_label(pill, sz_.caption_px, pal::kText, false);

    paused_chip_ = make_label(chip_, sz_.caption_px, 0x000000, false);
    lv_label_set_text(paused_chip_, "PAUSED");
    lv_obj_set_style_bg_color(paused_chip_, lv_color_hex(pal::kWarn), 0);
    lv_obj_set_style_bg_opa(paused_chip_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(paused_chip_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(paused_chip_, sz_.space_s + 2, 0);
    lv_obj_set_style_pad_ver(paused_chip_, 2, 0);
    lv_obj_add_flag(paused_chip_, LV_OBJ_FLAG_HIDDEN);
    /* "N held" (muted): shown only while "Hide held targets" is on and some target is held, so nothing is hidden silently */
    held_chip_ = make_label(chip_, sz_.caption_px, pal::kMuted, false);
    lv_obj_set_style_pad_hor(held_chip_, sz_.space_s, 0);
    lv_obj_add_flag(held_chip_, LV_OBJ_FLAG_HIDDEN);
}

/* The Radar page's split (core/view.hpp radar_layout): the list's block is the Table's two-line block without heads
 * (the values carry their units), the chip band is the chips' row and a small gap, the strip chart needs 120 px
 * (its title, a chart of at least 60 px and its time axis: 480x800 has 136 px under the list). */
void RadarApp::compute_layout()
{
    const int pad = sz_.space_m;
    list_block_h_ = 2 * lv_font_get_line_height(cp0_ui_font_px(sz_.body_px)) + pad / 2 + 2;
    list_gap_ = pad / 2;
    lv_obj_update_layout(chip_);
    const int band = sz_.space_s + lv_obj_get_height(chip_) + sz_.space_s;
    lay_ = mmw::radar_layout(w_, view_h_, band, std::max(sz_.space_m, sz_.dot), std::max(sz_.space_m, sz_.dot / 2 + 4),
                             list_block_h_, list_gap_, 120);
}

void RadarApp::build_tabs()
{
    tabbar_ = lv_obj_create(surface_);
    plain(tabbar_);
    lv_obj_set_pos(tabbar_, 0, view_h_);
    lv_obj_set_size(tabbar_, w_, sz_.tab_h);
    lv_obj_set_style_bg_color(tabbar_, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(tabbar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(tabbar_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(tabbar_, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_border_width(tabbar_, 1, 0);

    const int pause_w = std::max(sz_.target, sz_.tab_h);
    const int tab_w = (w_ - pause_w) / 4;
    const int bar = std::max(3, sz_.tab_h / 14);
    auto make_cell = [&](Cell &c, int x, int w, const char *text, lv_event_cb_t cb, int index) {
        c.obj = lv_obj_create(tabbar_);
        plain(c.obj);
        lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(c.obj, x, 0);
        lv_obj_set_size(c.obj, w, sz_.tab_h);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(pal::kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(c.obj, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_user_data(c.obj, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
        lv_obj_add_event_cb(c.obj, cb, LV_EVENT_CLICKED, this);
        c.label = make_label(c.obj, sz_.body_px, pal::kMuted, false);
        lv_label_set_text(c.label, text);
        lv_obj_center(c.label);
        c.mark = lv_obj_create(c.obj);
        plain(c.mark);
        lv_obj_set_size(c.mark, w - 2 * sz_.space_s, bar);
        lv_obj_set_pos(c.mark, sz_.space_s, 1);
        lv_obj_set_style_bg_color(c.mark, lv_color_hex(pal::kAccent), 0);
        lv_obj_set_style_bg_opa(c.mark, LV_OPA_COVER, 0);
        lv_obj_add_flag(c.mark, LV_OBJ_FLAG_HIDDEN);
    };
    for (int i = 0; i < 4; ++i) make_cell(tabs_[i], i * tab_w, tab_w, kTabNames[i], tab_cb, i);
    make_cell(pause_, 4 * tab_w, w_ - 4 * tab_w, LV_SYMBOL_PAUSE, pause_cb, 0);
    lv_obj_set_style_border_side(pause_.obj, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(pause_.obj, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_border_width(pause_.obj, 1, 0);
}

void RadarApp::build_notfound()
{
    nf_ = lv_obj_create(surface_);
    plain(nf_);
    lv_obj_add_flag(nf_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nf_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nf_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_pos(nf_, 0, 0);
    lv_obj_set_size(nf_, w_, view_h_);
    lv_obj_set_flex_flow(nf_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nf_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(nf_, sz_.space_m, 0);
    lv_obj_set_style_pad_row(nf_, sz_.space_s, 0);

    /* the key column of the tables: the widest key */
    key_w_ = 0;
    for (const char *k : {"Status", "Port id", "Frames/s", "Good frames", "Bad frames", "Last frame", "Sensor",
                          "Baud", "Port", "5V (VCC)"})
        key_w_ = std::max(key_w_, text_w(k, sz_.body_px));
    key_w_ = std::min(key_w_ + 4, w_ / 2);

    nf_title_ = make_label(nf_, sz_.body_px, pal::kText, true);
    lv_obj_set_width(nf_title_, lv_pct(100));
    lv_obj_set_style_text_align(nf_title_, LV_TEXT_ALIGN_CENTER, 0);
    nf_status_ = make_label(nf_, sz_.caption_px, pal::kMuted, true);
    lv_obj_set_width(nf_status_, lv_pct(100));
    lv_obj_set_style_text_align(nf_status_, LV_TEXT_ALIGN_CENTER, 0);
    /* the Bluetooth radars (Connection Bluetooth), under the status line and before the Rescan button so the first
     * one stays in view at 480x320: hidden otherwise, so the USB page is laid out as before */
    const int slot_w = text_w(LV_SYMBOL_RIGHT, sz_.body_px); /* the setting rows' chevron column */
    ble_slot_w_ = slot_w;
    ble_row_w_ = w_ - 2 * sz_.space_m - 2 * sz_.space_s; /* inside the page's and the row's padding */
    const int dot_d = std::max(8, sz_.body_px / 2);
    for (int i = 0; i < kBleRows; ++i) {
        BleRow &r = ble_rows_[i];
        r.row = lv_obj_create(nf_);
        plain(r.row);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(r.row, lv_pct(100), sz_.row);
        lv_obj_set_style_radius(r.row, page_layout_.esc_radius, 0);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(pal::kBar), 0);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(pal::kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(r.row, sz_.space_s, 0);
        lv_obj_set_user_data(r.row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(r.row, ble_row_cb, LV_EVENT_CLICKED, this);
        r.name = make_label(r.row, sz_.body_px, pal::kText, false);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS); /* one line, cut with dots if it must */
        lv_obj_set_size(r.name, ble_row_w_ / 2, lv_font_get_line_height(cp0_ui_font_px(sz_.body_px)));
        lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 0, 0);
        r.dot = lv_obj_create(r.row);
        plain(r.dot);
        lv_obj_set_size(r.dot, dot_d, dot_d);
        lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r.dot, lv_color_hex(cp0_ui_list_dot_rgb(CP0_UI_LIST_AFFORDANCE_ACTION)), 0);
        lv_obj_align(r.dot, LV_ALIGN_RIGHT_MID, -(slot_w - dot_d) / 2, 0);
        r.detail = make_label(r.row, sz_.caption_px, pal::kMuted, false);
        lv_obj_align(r.detail, LV_ALIGN_RIGHT_MID, -(slot_w + sz_.space_m), 0);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *b = make_button(nf_, "Rescan");
    lv_obj_add_event_cb(b, rescan_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *tried = make_heading(nf_, "TRIED");
    lv_obj_set_width(tried, lv_pct(100));
    nf_tried_ = make_label(nf_, sz_.caption_px, pal::kText, true);
    lv_obj_set_width(nf_tried_, lv_pct(100));
    nf_usb_parts_.push_back(tried);
    nf_usb_parts_.push_back(nf_tried_);
    lv_obj_t *wiring = make_heading(nf_, "WIRING: SENSOR TO USB-SERIAL ADAPTER");
    lv_obj_set_width(wiring, lv_pct(100));
    lv_label_set_long_mode(wiring, LV_LABEL_LONG_MODE_WRAP);
    nf_usb_parts_.push_back(wiring);
    static const char *const kWiring[][2] = {
        {"Sensor", "Adapter"}, {"5V (VCC)", "5V"}, {"GND", "GND"}, {"TX", "RX"}, {"RX", "TX"}};
    for (int i = 0; i < 5; ++i) {
        InfoRow r = make_info(nf_, kWiring[i][0]);
        lv_label_set_text(r.value, kWiring[i][1]);
        lv_obj_set_style_text_color(r.key, lv_color_hex(i == 0 ? pal::kMuted : pal::kText), 0);
        lv_obj_set_style_text_color(r.value, lv_color_hex(i == 0 ? pal::kMuted : pal::kText), 0);
        nf_usb_parts_.push_back(r.row);
    }
    nf_note_ = make_label(nf_, sz_.caption_px, pal::kMuted, true);
    nf_usb_parts_.push_back(nf_note_);
    lv_obj_set_width(nf_note_, lv_pct(100));
    lv_label_set_text(nf_note_, "The sensor's UART uses 3.3 V logic: connect it through a USB-serial adapter (CH340, "
                                "CP2102, FTDI), never to a 5 V port. Power it from 5 V (about 130 mA). It is found "
                                "by itself once plugged in.");
    nf_wiring_ = wiring;
}

void RadarApp::build_sensor()
{
    sensor_ = lv_obj_create(surface_);
    plain(sensor_);
    lv_obj_add_flag(sensor_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sensor_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sensor_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_pos(sensor_, 0, 0);
    lv_obj_set_size(sensor_, w_, view_h_);
    lv_obj_set_flex_flow(sensor_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sensor_, sz_.space_m, 0);
    lv_obj_set_style_pad_row(sensor_, sz_.space_s, 0);

    si_state_ = make_info(sensor_, "Status");
    si_port_ = make_info(sensor_, "Port");
    si_id_ = make_info(sensor_, "Port id");
    si_baud_ = make_info(sensor_, "Baud");
    si_fps_ = make_info(sensor_, "Frames/s");
    si_good_ = make_info(sensor_, "Good frames");
    si_bad_ = make_info(sensor_, "Bad frames");
    si_age_ = make_info(sensor_, "Last frame");
    si_model_ = make_info(sensor_, "Sensor");
    si_ble_ = make_info(sensor_, "Bluetooth"); /* shown with Connection Bluetooth or Auto */
    lv_obj_add_flag(si_ble_.row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *b = make_button(sensor_, "Rescan");
    lv_obj_add_event_cb(b, rescan_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *h = make_heading(sensor_, "DETECTION");
    lv_obj_set_width(h, lv_pct(100));
    si_reports_ = make_label(sensor_, sz_.caption_px, pal::kText, true);
    lv_obj_set_width(si_reports_, lv_pct(100));
    lv_obj_t *note = make_label(sensor_, sz_.caption_px, pal::kMuted, true);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_text(note, "Detected by listening only: the app sends nothing to the sensor.");

    h = make_heading(sensor_, "SETTINGS");
    lv_obj_set_width(h, lv_pct(100));
    for (int i = 0; i < kSettingCount; ++i) {
        SettingRow &r = settings_rows_[i];
        r.row = lv_obj_create(sensor_);
        plain(r.row);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(r.row, lv_pct(100), sz_.row);
        lv_obj_set_style_radius(r.row, page_layout_.esc_radius, 0);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(pal::kBar), 0);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(pal::kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(r.row, sz_.space_s, 0);
        lv_obj_set_user_data(r.row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(r.row, setting_cb, LV_EVENT_CLICKED, this);
        r.key = make_label(r.row, sz_.body_px, pal::kText, false);
        lv_label_set_text(r.key, kSettingNames[i]);
        lv_obj_align(r.key, LV_ALIGN_LEFT_MID, 0, 0);
        r.chevron = make_label(r.row, sz_.body_px, pal::kMuted, false);
        lv_label_set_text(r.chevron, LV_SYMBOL_RIGHT);
        lv_obj_align(r.chevron, LV_ALIGN_RIGHT_MID, 0, 0);
        r.value = make_label(r.row, sz_.body_px, 0x00CC66, false); /* an Edit row's value (cp0_ui_list.h) */
        lv_obj_align(r.value, LV_ALIGN_RIGHT_MID, -(text_w(LV_SYMBOL_RIGHT, sz_.body_px) + sz_.space_m), 0);
    }
}

/* ------------------------------------------------------------------------------------------------ the timer */

void RadarApp::timer_cb(lv_timer_t *t)
{
    static_cast<RadarApp *>(lv_timer_get_user_data(t))->tick();
}

const mmw::SlotState &RadarApp::slot(int i) const
{
    return paused_ ? snap_[i] : link_->tracker().slot(i);
}

std::int64_t RadarApp::view_time() const { return paused_ ? paused_at_ : now_; }

int RadarApp::effective_range() const { return settings_.range_m ? settings_.range_m : auto_range_m_; }

void RadarApp::tick()
{
    now_ = mono_ms();
    link_->poll(now_);
    /* the Bluetooth radar: windows and retries only while the Radar page is visible (never while hidden) */
    mux_->update(*link_, tab_ == Tab::Radar && !cdk::hidden(), now_);
    for (std::string l; ble_->take_log(l);) log_line("bluetooth: %s", l.c_str());
    if (std::string a, n; ble_->take_chosen(a, n)) {
        link_->set_remembered_ble(a, n);
        log_line("bluetooth: remembered %s %s", a.c_str(), n.c_str());
    }
    if (!diag_path_.empty() && now_ - diag_last_ >= 1000) {
        diag_last_ = now_;
        std::string hex;
        const std::string counts = mmw::diag_frames(*link_, now_, &hex);
        diag_write(mmw::diag_line(*link_, now_) + counts + "\n" + hex + mmw::diag_extra(*link_, diag_sent_));
        if (diag_bounded_ && ++diag_lines_ % 60 == 0) /* about once a minute: back to 200 KB once it reached twice that */
            if (struct stat st; ::stat(diag_path_.c_str(), &st) == 0 && st.st_size > static_cast<off_t>(2 * mmw::kDiagKeepBytes))
                mmw::diag_trim(diag_path_, mmw::kDiagKeepBytes);
    }
    if (link_->take_remembered_changed() && !config_dir_.empty()) {
        mmw::make_dirs(config_dir_);
        if (!mmw::write_file_atomic(config_dir_ + "/remembered.conf", link_->remembered().serialize()))
            log_line("cannot write %s/remembered.conf", config_dir_.c_str());
        if (tab_ == Tab::Sensor) update_sensor(true);
    }

    const mmw::Tracker &tr = link_->tracker();
    const mmw::LinkState state = link_->state();
    const std::uint32_t frames = tr.frames();
    unsigned slots = 0;
    std::int64_t history = 0;
    int max_mm = 0;
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        const mmw::SlotState &s = tr.slot(i);
        slots |= (s.active ? 1u : 0u) << (2 * i);
        slots |= (s.present_now ? 1u : 0u) << (2 * i + 1);
        history += static_cast<std::int64_t>(s.history.size()) + (s.history.empty() ? 0 : s.history.back().t_ms);
        if (s.active) {
            any = true;
            max_mm = std::max(max_mm, s.last.distance_mm);
        }
    }
    if (!paused_) {
        const int r = mmw::auto_range(auto_range_m_, max_mm, any, link_->driver() ? link_->driver()->max_range_m : 6);
        if (r != auto_range_m_) {
            auto_range_m_ = r;
            dirty_ = true;
            full_redraw_ = true;
        }
    }
    const bool state_changed = state != seen_state_;
    if (state_changed) {
        seen_state_ = state;
        if (state == mmw::LinkState::Found || state == mmw::LinkState::Lost)
            log_line("%s", mmw::link_state_line(*link_).c_str()); /* a Bluetooth radar has no baud */
        update_view_visibility();
        update_chips();
        update_notfound(true);
        dirty_ = true;
        full_redraw_ = true;
    }
    if (!paused_) {
        if (tab_ == Tab::Graph) {
            if (history != seen_history_) dirty_ = true;
        } else if (frames != seen_frames_ || slots != seen_slots_) {
            dirty_ = true;
        }
        if (tab_ == Tab::Radar && lay_.chart_h > 0 && history != seen_history_) {
            chart_dirty_ = true;
            dirty_ = true;
        }
    }
    seen_frames_ = frames;
    seen_slots_ = slots;
    seen_history_ = history;
    if (cdk::hidden()) return;

    /* 20 redraws a second at most (5 for the text pages), and never more than half of the time spent rendering */
    const std::int64_t min_gap = std::max<std::int64_t>(tab_ == Tab::Radar ? 50 : 200, 2 * last_render_ms_);
    if (dirty_ && tab_ != Tab::Sensor && now_ - last_draw_ >= min_gap) {
        dirty_ = false;
        last_draw_ = now_;
        if (tab_ == Tab::Radar && bg_buf_) render_background();
        if (!lv_obj_has_flag(view_, LV_OBJ_FLAG_HIDDEN)) invalidate_view();
    }
    if (tab_ == Tab::Radar && now_ - last_chip_ >= 500) {
        last_chip_ = now_;
        update_chips();
    }
    if (tab_ == Tab::Sensor && now_ - last_sensor_ >= 500) {
        last_sensor_ = now_;
        update_sensor(false);
    }
    if (!lv_obj_has_flag(nf_, LV_OBJ_FLAG_HIDDEN) && now_ - last_nf_ >= 250) {
        last_nf_ = now_;
        update_notfound(false);
    }
}

/* ------------------------------------------------------------------------------------------------ pages */

void RadarApp::set_tab(Tab t)
{
    tab_ = t;
    update_tabs();
    update_view_visibility();
    if (t == Tab::Sensor) update_sensor(true);
    if (t == Tab::Radar) update_chips();
    dirty_ = true;
    full_redraw_ = true;
    last_draw_ = 0;
}

void RadarApp::update_tabs()
{
    for (int i = 0; i < 4; ++i) {
        const bool on = static_cast<int>(tab_) == i;
        lv_obj_set_style_bg_color(tabs_[i].obj, lv_color_hex(on ? pal::kBar : 0x111111), 0);
        lv_obj_set_style_bg_opa(tabs_[i].obj, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(tabs_[i].label, lv_color_hex(on ? pal::kText : pal::kMuted), 0);
        if (on) lv_obj_remove_flag(tabs_[i].mark, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tabs_[i].mark, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(pause_.label, paused_ ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(pause_.label, lv_color_hex(paused_ ? pal::kWarn : pal::kText), 0);
    if (paused_) lv_obj_remove_flag(pause_.mark, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pause_.mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(pause_.mark, lv_color_hex(pal::kWarn), 0);
}

void RadarApp::update_view_visibility()
{
    const bool found = link_ && link_->state() == mmw::LinkState::Found;
    const bool nf = tab_ == Tab::Radar && !found && !paused_;
    auto show = [](lv_obj_t *o, bool on) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(nf_, nf);
    show(view_, tab_ != Tab::Sensor && !nf);
    show(bg_, bg_buf_ && tab_ == Tab::Radar && !nf);
    show(chip_, tab_ == Tab::Radar && !nf);
    show(sensor_, tab_ == Tab::Sensor);
    if (nf) update_notfound(true);
}

void RadarApp::update_chips()
{
    if (!link_) return;
    const mmw::LinkState s = link_->state();
    char buf[48];
    if (s == mmw::LinkState::Found) std::snprintf(buf, sizeof(buf), "%.0f fps", link_->stats().fps);
    else std::snprintf(buf, sizeof(buf), "%s", mmw::link_state_name(s));
    if (chip_shown_ != buf) {
        chip_shown_ = buf;
        lv_label_set_text(chip_label_, buf);
    }
    lv_obj_set_style_bg_color(chip_dot_, lv_color_hex(state_rgb(s)), 0);
    if (paused_) lv_obj_remove_flag(paused_chip_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(paused_chip_, LV_OBJ_FLAG_HIDDEN);
    int held = 0;
    if (settings_.hide_held) {
        for (int i = 0; i < 3; ++i) held += mmw::held_count(&slot(i), 1);
    }
    if (held > 0) {
        char hb[24];
        std::snprintf(hb, sizeof(hb), "%d held", held);
        if (held_shown_ != hb) {
            held_shown_ = hb;
            lv_label_set_text(held_chip_, hb);
        }
        lv_obj_remove_flag(held_chip_, LV_OBJ_FLAG_HIDDEN);
    } else {
        held_shown_.clear();
        lv_obj_add_flag(held_chip_, LV_OBJ_FLAG_HIDDEN);
    }
}

void RadarApp::update_notfound(bool force)
{
    if (!link_) return;
    const mmw::LinkState s = link_->state();
    const std::vector<mmw::PortReport> &reports = link_->reports();
    const std::string status = link_->status();
    std::string title;
    switch (s) {
    case mmw::LinkState::Busy: title = "The port is used by another app"; break;
    case mmw::LinkState::Lost: title = "Signal lost, searching"; break;
    case mmw::LinkState::Error: title = "The port cannot be opened"; break;
    case mmw::LinkState::Found: title = "Radar found"; break;
    default: title = reports.empty() && !status.empty() ? "Looking for the radar" : "No radar found"; break;
    }
    std::string tried;
    for (const mmw::PortReport &r : reports) {
        if (!tried.empty()) tried += "\n";
        tried += r.title + ": " + r.text;
    }
    if (tried.empty()) tried = status.empty() ? "No USB serial port (ttyUSB*, ttyACM*) is plugged in." : "-";
    std::string line = !status.empty() ? status : std::string("Plug the sensor in: it is found by itself.");
    const bool ble_page = ble_mode();
    if (ble_page) { /* Connection Bluetooth: the Bluetooth state instead of the USB scan */
        title = ble_->title();
        if (title.empty()) title = "Searching for radars over Bluetooth...";
        /* a title too long for one line breaks before its last word (a radar's name, "Bluetooth..."), never inside
         * it: LVGL would break at the "-", "_" or "." in it */
        const std::size_t sp = title.rfind(' ');
        if (sp != std::string::npos && text_w(title.c_str(), sz_.body_px) > w_ - 2 * sz_.space_m) title[sp] = '\n';
        line = !status.empty() ? status : ble_->detail(now_);
    } else if (settings_.connection == mmw::Connection::Auto && status.empty() && !link_->streaming()) {
        /* Auto, the USB scan found nothing: what the fallback to the remembered Bluetooth radar does */
        if (link_->remembered().ble_empty()) line = "No Bluetooth radar chosen yet: Connection > Bluetooth lists them.";
        else if (ble_->enabled()) line = "Bluetooth: " + ble_->title() + ".";
    }
    const std::string key = std::string(ble_page ? "B|" : "U|") + title + "|" + line + "|" + tried;
    const bool mode_changed = force || nf_shown_.compare(0, 2, ble_page ? "B|" : "U|") != 0;
    if (mode_changed)
        for (lv_obj_t *o : nf_usb_parts_) {
            if (ble_page) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    update_ble_rows(ble_page && ble_->state() != mmw::BleState::Unavailable && ble_->state() != mmw::BleState::Off);
    if (!force && key == nf_shown_) return;
    nf_shown_ = key;
    set_text_if(nf_title_, title);
    set_text_if(nf_status_, line);
    set_text_if(nf_tried_, tried);
}

/* The Bluetooth radars as rows: name, RSSI and the address's last two bytes, the action dot (blue while connecting to
 * that radar or connected to it). Hidden when not shown, so the USB page keeps its layout. */
void RadarApp::update_ble_rows(bool show)
{
    std::vector<mmw::BleCandidate> c;
    if (show) c = ble_->candidates();
    if (c.size() > static_cast<std::size_t>(kBleRows)) c.resize(kBleRows);
    const mmw::BleState st = ble_->state();
    const bool linking = st == mmw::BleState::Connecting || st == mmw::BleState::Subscribing || st == mmw::BleState::Connected;
    std::string key = show ? "1" : "0";
    for (const mmw::BleCandidate &d : c)
        key += "|" + d.address + d.name + std::to_string(d.has_rssi ? d.rssi : 1) +
               (linking && d.address == ble_->target_address() ? "*" : "");
    key += "|" + std::to_string(ble_sel_);
    if (key == ble_rows_shown_) return;
    ble_rows_shown_ = key;
    if (ble_sel_ >= static_cast<int>(c.size())) ble_sel_ = c.empty() ? -1 : static_cast<int>(c.size()) - 1;
    for (int i = 0; i < kBleRows; ++i) {
        BleRow &r = ble_rows_[i];
        if (i >= static_cast<int>(c.size())) {
            r.address.clear();
            lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const mmw::BleCandidate &d = c[static_cast<std::size_t>(i)];
        r.address = d.address;
        set_text_if(r.name, d.name.empty() ? d.address : d.name);
        char b[48];
        if (d.has_rssi) std::snprintf(b, sizeof(b), "%d dBm  %s", d.rssi, mmw::address_tail(d.address).c_str());
        else std::snprintf(b, sizeof(b), "%s", mmw::address_tail(d.address).c_str());
        set_text_if(r.detail, b);
        /* the name takes what the detail and the dot leave */
        lv_obj_set_width(r.name, std::max(sz_.target, ble_row_w_ - ble_slot_w_ - sz_.space_m - text_w(b, sz_.caption_px) -
                                                          sz_.space_s));
        const bool running = linking && d.address == ble_->target_address();
        lv_obj_set_style_bg_color(r.dot, lv_color_hex(cp0_ui_list_dot_rgb(running ? CP0_UI_LIST_AFFORDANCE_ACTION_RUNNING
                                                                                  : CP0_UI_LIST_AFFORDANCE_ACTION)), 0);
        lv_obj_set_style_bg_opa(r.row, i == ble_sel_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(r.row, LV_OBJ_FLAG_HIDDEN);
    }
}

void RadarApp::select_ble_row(int index)
{
    int n = 0;
    for (const BleRow &r : ble_rows_) n += !r.address.empty() && !lv_obj_has_flag(r.row, LV_OBJ_FLAG_HIDDEN) ? 1 : 0;
    if (n == 0) return;
    ble_sel_ = std::clamp(index, 0, n - 1);
    update_ble_rows(true);
    lv_obj_scroll_to_view(ble_rows_[ble_sel_].row, LV_ANIM_OFF);
}

void RadarApp::choose_ble_row(int index)
{
    if (index < 0 || index >= kBleRows || ble_rows_[index].address.empty()) return;
    ble_sel_ = index;
    log_line("bluetooth: chosen %s", ble_rows_[index].address.c_str());
    ble_->choose(ble_rows_[index].address, now_);
    update_notfound(true);
}

void RadarApp::set_info(InfoRow &r, const std::string &text, std::uint32_t rgb)
{
    if (r.shown == text) return;
    r.shown = text;
    lv_label_set_text(r.value, text.c_str());
    lv_obj_set_style_text_color(r.value, lv_color_hex(rgb), 0);
}

void RadarApp::update_sensor(bool force)
{
    if (!link_) return;
    if (force) {
        for (InfoRow *r : {&si_state_, &si_port_, &si_id_, &si_baud_, &si_fps_, &si_good_, &si_bad_, &si_age_, &si_model_,
                           &si_ble_})
            r->shown.clear();
        si_reports_shown_.clear();
    }
    const mmw::LinkState s = link_->state();
    const mmw::LinkStats st = link_->stats(now_);
    char buf[64];
    set_info(si_state_, mmw::link_state_name(s), state_rgb(s));
    const bool known = !link_->port_title().empty();
    set_info(si_port_, known ? link_->port_title() : std::string("-"), pal::kMuted);
    set_info(si_id_, known ? mmw::port_id(link_->port()) : std::string("-"), pal::kMuted);
    if (link_->baud() > 0) std::snprintf(buf, sizeof(buf), "%d baud", link_->baud());
    else std::snprintf(buf, sizeof(buf), "-");
    set_info(si_baud_, buf, pal::kMuted);
    std::snprintf(buf, sizeof(buf), "%.1f", st.fps);
    set_info(si_fps_, buf, pal::kMuted);
    set_info(si_good_, std::to_string(st.frames), pal::kMuted);
    set_info(si_bad_, std::to_string(st.bad_frames), pal::kMuted);
    if (st.last_frame_age_ms < 0) std::snprintf(buf, sizeof(buf), "-");
    else std::snprintf(buf, sizeof(buf), "%.1f s ago", st.last_frame_age_ms / 1000.0);
    set_info(si_age_, buf, pal::kMuted);
    set_info(si_model_, link_->model(), pal::kMuted);
    /* the remembered Bluetooth radar, under the radar line, with Connection Bluetooth or Auto */
    const bool ble_on = settings_.connection != mmw::Connection::Usb;
    if (ble_on) {
        const mmw::Remembered &rem = link_->remembered();
        set_info(si_ble_, rem.ble_empty() ? std::string("None chosen") : (rem.ble_name.empty() ? rem.ble_address : rem.ble_name),
                 pal::kMuted);
        lv_obj_remove_flag(si_ble_.row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(si_ble_.row, LV_OBJ_FLAG_HIDDEN);
    }

    std::string reports;
    for (const mmw::PortReport &r : link_->reports()) {
        if (!reports.empty()) reports += "\n";
        reports += r.title + ": " + r.text;
    }
    if (reports.empty()) reports = link_->status().empty() ? "No scan result yet." : link_->status();
    if (ble_on && ble_->enabled() && !link_->streaming()) {
        reports += "\nBluetooth: " + ble_->title();
        if (!ble_->last_error().empty()) reports += " (" + ble_->last_error() + ")";
    }
    if (reports != si_reports_shown_) {
        si_reports_shown_ = reports;
        lv_label_set_text(si_reports_, reports.c_str());
    }

    const std::string values[kSettingCount] = {mmw::units_label(settings_.units),
                                               mmw::orientation_label(settings_.orientation),
                                               mmw::range_label(settings_.range_m), settings_.trails ? "On" : "Off",
                                               settings_.hide_held ? "On" : "Off",
                                               mmw::sensor_type_label(settings_.sensor.c_str()),
                                               mmw::connection_label(settings_.connection)};
    for (int i = 0; i < kSettingCount; ++i) {
        set_text_if(settings_rows_[i].value, values[i]);
        lv_obj_set_style_bg_opa(settings_rows_[i].row, i == setting_sel_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* ------------------------------------------------------------------------------------------------ actions */

void RadarApp::toggle_pause()
{
    paused_ = !paused_;
    if (paused_) {
        paused_at_ = now_;
        for (int i = 0; i < 3; ++i) snap_[i] = link_->tracker().slot(i); /* once per pause, not per frame */
    }
    update_tabs();
    update_chips();
    update_view_visibility();
    dirty_ = true;
    full_redraw_ = true;
    last_draw_ = 0;
}

void RadarApp::rescan()
{
    link_->rescan(now_);
    if (settings_.connection != mmw::Connection::Usb) ble_->rescan(now_);
    log_line("rescan");
    update_notfound(true);
    if (tab_ == Tab::Sensor) update_sensor(true);
}

void RadarApp::save_settings()
{
    if (config_dir_.empty()) return;
    mmw::make_dirs(config_dir_);
    if (!mmw::write_file_atomic(config_dir_ + "/settings.conf", mmw::serialize_settings(settings_)))
        log_line("cannot write %s/settings.conf", config_dir_.c_str());
}

void RadarApp::change_range(int dir)
{
    settings_.range_m = mmw::step_range(settings_.range_m, effective_range(), dir);
    if (settings_.range_m) auto_range_m_ = settings_.range_m;
    save_settings();
    update_sensor(true);
    dirty_ = true;
    full_redraw_ = true;
    last_draw_ = 0;
}

void RadarApp::apply_setting(int index)
{
    switch (index) {
    case 0: settings_.units = mmw::next_units(settings_.units); break;
    case 1: settings_.orientation = mmw::next_orientation(settings_.orientation); break;
    case 2:
        settings_.range_m = mmw::next_range_setting(settings_.range_m);
        if (settings_.range_m) auto_range_m_ = settings_.range_m;
        break;
    case 3: settings_.trails = !settings_.trails; break;
    case 4: settings_.hide_held = !settings_.hide_held; break;
    case 5:
        settings_.sensor = mmw::next_sensor_type(settings_.sensor.c_str());
        link_->set_sensor_type(settings_.sensor, now_);
        break;
    case 6: /* the transport changes: the link lets go of what it holds and searches again with the new port list */
        settings_.connection = mmw::next_connection(settings_.connection);
        mux_->set_mode(settings_.connection);
        link_->start(now_, link_->remembered());
        ble_sel_ = -1;
        log_line("connection %s", mmw::connection_name(settings_.connection));
        update_view_visibility();
        update_notfound(true);
        break;
    default: return;
    }
    save_settings();
    update_sensor(true);
    update_chips();
    dirty_ = true;
    full_redraw_ = true;
}

void RadarApp::select_setting(int index)
{
    setting_sel_ = std::clamp(index, 0, kSettingCount - 1);
    update_sensor(true);
    lv_obj_scroll_to_view(settings_rows_[setting_sel_].row, LV_ANIM_OFF);
}

/* ------------------------------------------------------------------------------------------------ input */

void RadarApp::tab_cb(lv_event_t *e)
{
    auto *self = static_cast<RadarApp *>(lv_event_get_user_data(e));
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e))));
    self->set_tab(static_cast<Tab>(i));
}

void RadarApp::pause_cb(lv_event_t *e) { static_cast<RadarApp *>(lv_event_get_user_data(e))->toggle_pause(); }

void RadarApp::rescan_cb(lv_event_t *e) { static_cast<RadarApp *>(lv_event_get_user_data(e))->rescan(); }

void RadarApp::setting_cb(lv_event_t *e)
{
    auto *self = static_cast<RadarApp *>(lv_event_get_user_data(e));
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e))));
    self->setting_sel_ = i;
    self->apply_setting(i);
}

void RadarApp::ble_row_cb(lv_event_t *e)
{
    auto *self = static_cast<RadarApp *>(lv_event_get_user_data(e));
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e))));
    self->choose_ble_row(i);
}

void RadarApp::key_cb(lv_event_t *e)
{
    static_cast<RadarApp *>(lv_event_get_user_data(e))->on_key(lv_event_get_key(e));
}

void RadarApp::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<RadarApp *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(e));
    if (item) self->on_keyboard(*item);
}

void RadarApp::on_key(std::uint32_t key)
{
    switch (key) {
    case LV_KEY_ESC: cdk::request_quit(0); break;
    case LV_KEY_LEFT: set_tab(static_cast<Tab>((static_cast<int>(tab_) + 3) % 4)); break;
    case LV_KEY_RIGHT: set_tab(static_cast<Tab>((static_cast<int>(tab_) + 1) % 4)); break;
    case LV_KEY_UP:
        if (tab_ == Tab::Sensor) select_setting(setting_sel_ < 0 ? kSettingCount - 1 : setting_sel_ - 1);
        else if (tab_ == Tab::Radar && ble_mode() && !lv_obj_has_flag(nf_, LV_OBJ_FLAG_HIDDEN))
            select_ble_row(ble_sel_ < 0 ? kBleRows - 1 : ble_sel_ - 1);
        break;
    case LV_KEY_DOWN:
        if (tab_ == Tab::Sensor) select_setting(setting_sel_ < 0 ? 0 : setting_sel_ + 1);
        else if (tab_ == Tab::Radar && ble_mode() && !lv_obj_has_flag(nf_, LV_OBJ_FLAG_HIDDEN))
            select_ble_row(ble_sel_ < 0 ? 0 : ble_sel_ + 1);
        break;
    case LV_KEY_ENTER:
        if (tab_ == Tab::Sensor && setting_sel_ >= 0) apply_setting(setting_sel_);
        else if (tab_ == Tab::Radar && ble_mode() && !lv_obj_has_flag(nf_, LV_OBJ_FLAG_HIDDEN) && ble_sel_ >= 0)
            choose_ble_row(ble_sel_);
        break;
    default: break;
    }
}

void RadarApp::on_keyboard(const struct key_item &item)
{
    if (item.key_state != KBD_KEY_PRESSED && item.key_state != KBD_KEY_REPEATED) return;
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO | KBD_MOD_FN)) return;
    const bool repeat = item.key_state == KBD_KEY_REPEATED;
    /* the physical key first (letters share codes with arrows on some keyboards only through Fn); text when there is
     * no key code (typed by the on-screen keyboard) */
    int action = 0; /* 1 pause, 2 rescan, 3 plus, 4 minus, 10+n tab n */
    switch (item.key_code) {
    case KEY_SPACE: action = 1; break;
    case KEY_R: action = 2; break;
    case KEY_EQUAL:
    case KEY_KPPLUS: action = 3; break;
    case KEY_MINUS:
    case KEY_KPMINUS: action = 4; break;
    case KEY_1: action = 10; break;
    case KEY_2: action = 11; break;
    case KEY_3: action = 12; break;
    case KEY_4: action = 13; break;
    case 0:
        if (item.utf8[0] && !item.utf8[1]) {
            switch (item.utf8[0]) {
            case ' ': action = 1; break;
            case 'r':
            case 'R': action = 2; break;
            case '+':
            case '=': action = 3; break;
            case '-': action = 4; break;
            case '1': action = 10; break;
            case '2': action = 11; break;
            case '3': action = 12; break;
            case '4': action = 13; break;
            default: break;
            }
        }
        break;
    default: break;
    }
    if (repeat && action != 3 && action != 4) return;
    switch (action) {
    case 1: toggle_pause(); break;
    case 2: rescan(); break;
    case 3: change_range(1); break;
    case 4: change_range(-1); break;
    case 10:
    case 11:
    case 12:
    case 13: set_tab(static_cast<Tab>(action - 10)); break;
    default: break;
    }
}

} // namespace mmwui
