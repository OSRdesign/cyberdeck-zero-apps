/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the screen, the tabs, the map, the timer and the input (see atlas_app.hpp). The Sources tab is
 * atlas_sources.cpp, the browser and the import atlas_import.cpp, the Settings tab atlas_settings.cpp.
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "app.hpp"
#include "attribution.hpp"
#include "format.hpp"
#include "keyboard_input.h"

#include "cp0_map_widget.h"
#include "cp0_ui_metrics_lvgl.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <pwd.h>
#include <unistd.h>

namespace atlasui {

namespace {
/* demonstration mode only (the scenes): a crash leaves its stack in the log (addresses: addr2line -f -e <binary>) */
void crash_handler(int sig)
{
    void *frames[48];
    const int n = backtrace(frames, 48);
    static const char head[] = "[deck-atlas] crash, stack:\n";
    if (write(2, head, sizeof head - 1) < 0) {}
    backtrace_symbols_fd(frames, n, 2);
    std::signal(sig, SIG_DFL);
    raise(sig);
}
} // namespace

/* ------------------------------------------------------------------------------------------------ construction */

AtlasApp::AtlasApp(lv_obj_t *screen) : screen_(screen)
{
    compute_sizes();

    /* demonstration mode (scenes, screenshots): a store and settings of their own under the harness's runtime folder,
     * a fixture map imported at the start, never any network (DECKATLAS_DEMO=1 | empty | import-hold) */
    if (const char *d = std::getenv("DECKATLAS_DEMO"); d && *d && std::strcmp(d, "0") != 0) {
        demo_ = true;
        demo_mode_ = d;
        std::signal(SIGABRT, crash_handler);
        std::signal(SIGSEGV, crash_handler);
        std::signal(SIGBUS, crash_handler);
    }
    const char *home = std::getenv("HOME");
    home_ = home && *home ? home : "";
    if (struct passwd *pw = ::getpwuid(::getuid())) user_ = pw->pw_name ? pw->pw_name : "";
    if (demo_) {
        const char *state = std::getenv("APPLAUNCH_STATE_DIR");
        const std::string base = std::string(state && *state ? state : "/tmp") + "/deck-atlas-demo";
        config_dir_ = base + "/config";
        store_dir_ = base + "/maps";
    } else {
        config_dir_ = atlas::config_dir();
    }
    std::string text;
    if (!config_dir_.empty() && atlas::read_small_file(config_dir_ + "/settings.conf", text))
        settings_ = atlas::parse_settings(text);
    if (demo_) {
        if (const char *s = std::getenv("DECKATLAS_SCALE"); s && *s) {
            atlas::Settings t = atlas::parse_settings(std::string("scale=") + s);
            settings_.scale = t.scale;
        }
        if (const char *v = std::getenv("DECKATLAS_VIEW"); v && *v) {
            atlas::Settings t = atlas::parse_settings(std::string("view=") + v);
            if (t.view.set) settings_.view = t.view;
        }
        if (const char *u = std::getenv("DECKATLAS_UNITS"); u && *u) settings_.units = std::strcmp(u, "mi") == 0 ? atlas::Units::Mi : atlas::Units::Km;
        if (const char *g = std::getenv("DECKATLAS_GPS"); g && *g) settings_.gps = std::strcmp(g, "auto") == 0; /* the GPS scenes */
    }

    open_store();

    /* the launcher's page header, as in every app (the narrow portrait shell on 320x480 comes from the toolkit) */
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen_, layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());
    w_ = layout_.w;
    h_ = layout_.h;
    body_y_ = layout_.body_y;
    view_h_ = h_ - body_y_ - tab_h_;

    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);

    build_map();
    build_tabs();
    worker_ = std::make_unique<Worker>();

    first_view();
    /* after the first view: the callback's first call would only repeat it */
    if (map_) cp0_map_on_view_change(map_, view_cb, this);
    update_chip();
    gps_start();
    set_tab(Tab::Map);
    log_line("%s %dx%d%s scale %s, store %s%s, %d map files, network %s", kVersion, w_, h_, narrow_ ? " narrow portrait" : "",
             atlas::scale_label(settings_.scale, map_ ? cp0_map_get_scale(map_) : 0).c_str(), store_dir_.c_str(),
             store_ ? "" : " (not open)", static_cast<int>(archives_.size()),
             demo_ ? "off (demo)" : settings_.offline ? "off (offline mode)" : "on");
    timer_ = lv_timer_create(timer_cb, 100, this);
}

AtlasApp::~AtlasApp()
{
    /* the order matters: the import is told to stop and the worker joined; the view saved; the widgets (the map
     * widget and its tile thread) deleted; then the store flushed and closed */
    import_cancel_ = 1;
    if (map_) cp0_map_on_view_change(map_, nullptr, nullptr);
    if (worker_) worker_->shutdown();
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    gps_stop(); /* the port closed, position.state removed */
    lv_async_call_cancel(posted_cb, this);
    if (map_ && store_) {
        double lat = 0, lon = 0;
        cp0_map_get_center(map_, &lat, &lon);
        settings_.view.set = true;
        settings_.view.lat = lat;
        settings_.view.lon = lon;
        settings_.view.zoom = cp0_map_get_zoom(map_);
        save_settings();
    }
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(screen_)) lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    dialog_.reset();
    field_.reset();
    text_.reset();
    list_.reset();
    page_.reset(); /* the map widget goes with the page's objects */
    map_ = nullptr;
    if (store_) {
        const int rc = cp0_map_store_flush(store_);
        if (rc != CP0_MAP_OK) log_line("flush: %s", cp0_map_strerror(rc));
        cp0_map_store_close(store_);
        store_ = nullptr;
    }
    worker_.reset();
}

void AtlasApp::compute_sizes()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    narrow_ = m->h > m->w && m->w < 480;
    space_s_ = m->tok[CP0_TOK_SPACE_S];
    space_m_ = m->tok[CP0_TOK_SPACE_M];
    row_ = m->tok[CP0_TOK_ROW_H];
    target_ = m->tok[CP0_TOK_TARGET];
    body_px_ = m->text_px[CP0_TEXT_BODY];
    caption_px_ = m->text_px[CP0_TEXT_CAPTION];
    tab_h_ = std::max(row_, target_);
}

void AtlasApp::log_cb(void *, const char *line) { log_line("map: %s", line ? line : ""); }

void AtlasApp::open_store()
{
    char ua[256] = "";
    if (cp0_map_user_agent(ua, sizeof ua, "deck-atlas", kVersion) != CP0_MAP_OK) ua[0] = '\0';
    if (store_dir_.empty()) {
        char dir[CP0_MAP_PATH_MAX];
        if (cp0_map_store_default_dir(dir, sizeof dir) == CP0_MAP_OK) store_dir_ = dir;
    }
    cp0_map_store_cfg_t cfg{};
    cfg.store_dir = store_dir_.empty() ? nullptr : store_dir_.c_str();
    cfg.user_agent = ua;
    cfg.ram_cache_bytes = 0; /* from board.state ram_mb: 8 MiB (64 tiles RGB565) on the 512 MB boards */
    cfg.read_only = 0;
    cfg.network = (!demo_ && !settings_.offline) ? 1 : 0;
    cfg.cache_cap_bytes = static_cast<std::uint64_t>(settings_.cache_mb) << 20;
    cfg.log = log_cb;
    const int rc = cp0_map_store_open(&cfg, &store_);
    if (rc != CP0_MAP_OK) {
        store_ = nullptr;
        store_error_ = std::string("Cannot open the map folder: ") + cp0_map_strerror(rc);
        log_line("%s (%s)", store_error_.c_str(), store_dir_.c_str());
        return;
    }
    store_dir_ = cp0_map_store_dir(store_);

    if (demo_) {
        /* the scenes' map: a real archive in place (the optional real-tile check), else the fixture folder converted
         * by the engine's own import (a few tiles: instant; demonstration mode only, before the UI exists) */
        list_store(store_, archives_, online_);
        const char *real = std::getenv("DECKATLAS_MBTILES");
        if (archives_.empty() && real && *real && demo_mode_ != "empty") {
            cp0_map_import_opts_t o{};
            o.copy = 0;
            o.id = "real-tiles";
            o.title = "Real tiles";
            const int r = cp0_map_store_import_ex(store_, real, &o);
            log_line("demo: %s registered in place: %s", real, r == CP0_MAP_OK ? "ok" : cp0_map_strerror(r));
        } else if (archives_.empty() && demo_mode_ != "empty") {
            std::string fixture;
            if (const char *f = std::getenv("DECKATLAS_FIXTURE"); f && *f) fixture = f;
#ifdef CDK_APP_DIR
            if (fixture.empty()) fixture = std::string(CDK_APP_DIR) + "/tests/fixtures/tiles";
#endif
            if (!fixture.empty()) {
                cp0_map_import_opts_t o{};
                o.id = "test-tiles";
                o.title = "Test map";
                const char *attr = std::getenv("DECKATLAS_ATTRIBUTION"); /* the scenes of a long credit */
                if (attr && *attr) o.attribution = attr;
                const int r = cp0_map_store_import_ex(store_, fixture.c_str(), &o);
                log_line("demo: fixture %s imported: %s", fixture.c_str(), r == CP0_MAP_OK ? "ok" : cp0_map_strerror(r));
            }
        }
        log_line("demo: network forced off, settings in %s", config_dir_.c_str());
    }
    list_store(store_, archives_, online_);
    for (OnlineItem &o : online_) o.facts.network_on = !settings_.offline;
    listed_once_ = true;
}

/* ------------------------------------------------------------------------------------------------ building */

lv_obj_t *AtlasApp::make_label(lv_obj_t *parent, int px, std::uint32_t rgb, bool wrap)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, cp0_ui_font_px(px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *AtlasApp::make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (lv_obj_get_group(b)) lv_group_remove_obj(b);
    lv_obj_set_height(b, target_);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, target_ * 2, 0);
    lv_obj_set_style_pad_hor(b, space_m_, 0);
    lv_obj_set_style_radius(b, layout_.esc_radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pal::kPanel), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pal::kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = make_label(b, body_px_, pal::kText, false);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    return b;
}

void AtlasApp::build_map()
{
    map_box_ = lv_obj_create(page_->root());
    plain(map_box_);
    lv_obj_set_pos(map_box_, 0, body_y_);
    lv_obj_set_size(map_box_, w_, view_h_);
    lv_obj_set_style_bg_color(map_box_, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(map_box_, LV_OPA_COVER, 0);
    if (store_) {
        map_ = cp0_map_create(map_box_, store_);
        if (!map_) log_line("map widget: out of memory");
    }
    if (map_) {
        lv_obj_set_size(map_, w_, view_h_);
        lv_obj_add_event_cb(map_, map_key_cb, LV_EVENT_KEY, this);
        cp0_map_set_units(map_, settings_.units == atlas::Units::Mi ? CP0_MAP_UNITS_IMPERIAL : CP0_MAP_UNITS_METRIC);
        cp0_map_set_scale(map_, settings_.scale);
        apply_map_source();
        /* the seam for the next version (GPS): the position dot, the follow mode and position.state come with the GPS
         * reader; until then no fix is ever set */
        cp0_map_set_position(map_, nullptr);
    }
    build_gps_layer(); /* the dot's layer over the map, under the credit plate and the chips */

    /* the credit the engine would cut: a plate of ours over its box with the whole text (update_attribution) */
    attr_plate_ = lv_obj_create(map_box_);
    plain(attr_plate_);
    lv_obj_set_style_bg_color(attr_plate_, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_bg_opa(attr_plate_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(attr_plate_, cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S], 0);
    attr_label_ = lv_label_create(attr_plate_);
    lv_obj_set_style_text_font(attr_label_, cp0_ui_font(CP0_TEXT_CAPTION), 0);
    lv_obj_set_style_text_color(attr_label_, lv_color_hex(pal::kText), 0);
    lv_label_set_long_mode(attr_label_, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_remove_flag(attr_label_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(attr_plate_, LV_OBJ_FLAG_HIDDEN);

    /* "No map here": a plate in the middle of the map when no imported file covers the view */
    chip_ = lv_obj_create(map_box_);
    plain(chip_);
    lv_obj_set_size(chip_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(chip_, w_ - 2 * space_m_, 0);
    lv_obj_set_style_bg_color(chip_, lv_color_hex(pal::kPlate), 0);
    lv_obj_set_style_bg_opa(chip_, LV_OPA_90, 0);
    lv_obj_set_style_radius(chip_, layout_.esc_radius, 0);
    lv_obj_set_style_pad_hor(chip_, space_m_, 0);
    lv_obj_set_style_pad_ver(chip_, space_s_, 0);
    chip_label_ = make_label(chip_, body_px_, pal::kText, true);
    lv_obj_set_width(chip_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(chip_label_, w_ - 4 * space_m_, 0);
    lv_obj_set_style_text_align(chip_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(chip_);
    lv_obj_add_flag(chip_, LV_OBJ_FLAG_HIDDEN);
    build_gps_controls();
}

void AtlasApp::build_tabs()
{
    static const char *const kNames[3] = {"Map", "Sources", "Settings"};
    tabbar_ = lv_obj_create(page_->root());
    plain(tabbar_);
    lv_obj_set_pos(tabbar_, 0, h_ - tab_h_);
    lv_obj_set_size(tabbar_, w_, tab_h_);
    lv_obj_set_style_bg_color(tabbar_, lv_color_hex(pal::kStrip), 0);
    lv_obj_set_style_bg_opa(tabbar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(tabbar_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(tabbar_, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_border_width(tabbar_, 1, 0);
    const int bar = std::max(3, tab_h_ / 14);
    for (int i = 0; i < 3; ++i) {
        const int x0 = i * w_ / 3, x1 = (i + 1) * w_ / 3;
        Cell &c = tabs_[i];
        c.obj = lv_obj_create(tabbar_);
        plain(c.obj);
        lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(c.obj, x0, 0);
        lv_obj_set_size(c.obj, x1 - x0, tab_h_);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(pal::kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(c.obj, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_user_data(c.obj, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(c.obj, tab_cb, LV_EVENT_CLICKED, this);
        c.label = make_label(c.obj, body_px_, pal::kMuted, false);
        lv_label_set_text(c.label, kNames[i]);
        lv_obj_center(c.label);
        c.mark = lv_obj_create(c.obj);
        plain(c.mark);
        lv_obj_set_size(c.mark, x1 - x0 - 2 * space_s_, bar);
        lv_obj_set_pos(c.mark, space_s_, 1);
        lv_obj_set_style_bg_color(c.mark, lv_color_hex(pal::kAccent), 0);
        lv_obj_set_style_bg_opa(c.mark, LV_OPA_COVER, 0);
        lv_obj_add_flag(c.mark, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------------------------------------ pages */

void AtlasApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

void AtlasApp::posted_cb(void *data)
{
    auto *self = static_cast<AtlasApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void AtlasApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void AtlasApp::set_title()
{
    static const char *const kTab[3] = {"Map", "Sources", "Settings"};
    std::vector<std::string> path = {"Deck Atlas"};
    switch (sub_) {
    case Sub::None: break;
    case Sub::FileDetail: path = {"Deck Atlas", kTab[1], "Map file"}; break;
    case Sub::Browser: path = {"Deck Atlas", kTab[1], "Import"}; break;
    case Sub::Import: path = {"Deck Atlas", kTab[1], "Import"}; break;
    case Sub::Key: path = {"Deck Atlas", kTab[1], "API key"}; break;
    case Sub::Shown: path = {"Deck Atlas", kTab[1], "Map shows"}; break;
    case Sub::About: path = {"Deck Atlas", kTab[2], "About"}; break;
    }
    page_->set_title(path);
}

void AtlasApp::clear_page_objects()
{
    list_.reset();
    field_.reset();
    text_.reset();
    page_->clear_body();
    if (page_box_) lv_obj_delete(page_box_);
    page_box_ = nullptr;
    key_status_ = nullptr;
    imp_title_ = imp_bar_ = imp_count_ = imp_rate_ = imp_result_ = imp_note_ = imp_cancel_ = imp_show_ = imp_done_ = nullptr;
    imp_shown_.clear();
}

void AtlasApp::make_list(bool with_tabs, cp0::ui::CompactListEvents ev)
{
    list_.reset();
    cp0_ui_list_layout_t ll{};
    const int h = with_tabs ? view_h_ : h_ - body_y_;
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, layout_.roller_x, body_y_, layout_.roller_w,
                       std::max(1, h), &ll);
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, std::move(ev));
    focus(list_->focus_object());
}

void AtlasApp::show_tabs(bool shown)
{
    if (shown) lv_obj_remove_flag(tabbar_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(tabbar_, LV_OBJ_FLAG_HIDDEN);
}

void AtlasApp::set_tab(Tab t)
{
    if (dialog_) close_dialog();
    if (tab_ == Tab::Map && t != Tab::Map && view_dirty_) {
        save_settings();
        view_dirty_ = false;
    }
    if (list_ && sub_ == Sub::None) {
        if (tab_ == Tab::Sources) sources_sel_ = list_->selected();
        if (tab_ == Tab::Settings) settings_sel_ = list_->selected();
    }
    clear_page_objects();
    sub_ = Sub::None;
    tab_ = t;
    show_tabs(true);
    if (t == Tab::Map) {
        lv_obj_remove_flag(map_box_, LV_OBJ_FLAG_HIDDEN);
        if (map_) focus(map_);
        update_chip();
    } else {
        lv_obj_add_flag(map_box_, LV_OBJ_FLAG_HIDDEN);
        if (t == Tab::Sources) {
            build_sources();
            refresh_store();
        } else {
            build_settings();
            refresh_stats();
        }
    }
    update_tabs();
    set_title();
}

void AtlasApp::update_tabs()
{
    for (int i = 0; i < 3; ++i) {
        const bool on = static_cast<int>(tab_) == i;
        lv_obj_set_style_bg_color(tabs_[i].obj, lv_color_hex(on ? pal::kBar : pal::kStrip), 0);
        lv_obj_set_style_bg_opa(tabs_[i].obj, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(tabs_[i].label, lv_color_hex(on ? pal::kText : pal::kMuted), 0);
        if (on) lv_obj_remove_flag(tabs_[i].mark, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tabs_[i].mark, LV_OBJ_FLAG_HIDDEN);
    }
}

void AtlasApp::close_sub()
{
    const Sub was = sub_;
    if (was == Sub::About) {
        set_tab(Tab::Settings);
        return;
    }
    set_tab(Tab::Sources);
}

void AtlasApp::back()
{
    if (dialog_) {
        dialog_->cancel();
        return;
    }
    switch (sub_) {
    case Sub::None:
        cdk::request_quit(0); /* a short Esc at a tab ends the app (owner decision 2026-10-13) */
        return;
    case Sub::Browser: browser_back(); return;
    case Sub::Import:
        if (importing_) {
            import_cancel_ = 1;
            return;
        }
        close_sub();
        return;
    default: close_sub(); return;
    }
}

void AtlasApp::confirm(const std::string &title, const std::string &text, const std::string &yes, std::function<void()> on_yes)
{
    close_dialog();
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    auto fn = std::make_shared<std::function<void()>>(std::move(on_yes));
    spec.buttons.push_back({yes, cp0::ui::ButtonRole::Primary, [this, fn] {
                                post([this, fn] {
                                    close_dialog();
                                    if (*fn) (*fn)();
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    if (!text.empty()) dialog_->add_text(text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void AtlasApp::message(const std::string &title, const std::string &text)
{
    close_dialog();
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({"OK", cp0::ui::ButtonRole::Primary, [this] { post([this] { close_dialog(); }); }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    if (!text.empty()) dialog_->add_text(text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void AtlasApp::close_dialog()
{
    if (!dialog_) return;
    dialog_.reset();
    if (field_) focus(field_->focus_object());
    else if (list_) focus(list_->focus_object());
    else if (sub_ == Sub::None && tab_ == Tab::Map && map_) focus(map_);
}

/* ------------------------------------------------------------------------------------------------ the timer */

void AtlasApp::timer_cb(lv_timer_t *t) { static_cast<AtlasApp *>(lv_timer_get_user_data(t))->tick(); }

void AtlasApp::tick()
{
    const std::int64_t now = mono_ms();
    if (worker_) worker_->poll();
    gps_tick(now);
    if (importing_ || sub_ == Sub::Import) update_import_page(false);
    if (view_dirty_ && now - view_saved_at_ >= 30000) {
        view_saved_at_ = now;
        view_dirty_ = false;
        save_settings();
    }
    if (((sub_ == Sub::None && tab_ == Tab::Sources) || sub_ == Sub::FileDetail) && now - last_refresh_ >= 3000)
        refresh_store();
    if (sub_ == Sub::None && tab_ == Tab::Settings && now - last_stats_ >= 5000) refresh_stats();
}

void AtlasApp::save_settings()
{
    if (config_dir_.empty()) return;
    if (map_) {
        double lat = 0, lon = 0;
        cp0_map_get_center(map_, &lat, &lon);
        settings_.view.set = true;
        settings_.view.lat = lat;
        settings_.view.lon = lon;
        settings_.view.zoom = cp0_map_get_zoom(map_);
    }
    atlas::make_dirs(config_dir_);
    if (!atlas::write_file_atomic(config_dir_ + "/settings.conf", atlas::serialize_settings(settings_)))
        log_line("cannot write %s/settings.conf", config_dir_.c_str());
}

/* ------------------------------------------------------------------------------------------------ the map */

void AtlasApp::apply_map_source()
{
    if (!map_) return;
    const std::string &id = settings_.source;
    if (!id.empty()) {
        const bool known = find_archive(id) || (find_online(id) && find_online(id)->facts.enabled);
        widget_by_id_ = true;
        if (known && cp0_map_set_source(map_, id.c_str()) == CP0_MAP_OK) return;
        log_line("map source %s gone: every map file", id.c_str());
        settings_.source.clear();
    }
    widget_by_id_ = false;
    cp0_map_set_source(map_, nullptr);
}

void AtlasApp::store_changed()
{
    if (!store_ || !map_) return;
    double lat = 0, lon = 0;
    cp0_map_get_center(map_, &lat, &lon);
    const int z = cp0_map_get_zoom(map_);
    apply_map_source();
    cp0_map_set_center(map_, lat, lon);
    cp0_map_set_zoom(map_, z);
    update_chip();
}

void AtlasApp::fit_archive(const ArchiveItem &a)
{
    if (!map_ || !a.has_bounds) return;
    const atlas::Fit f = atlas::fit_box(a.bounds, w_, view_h_, cp0_map_get_scale(map_), a.minzoom >= 0 ? a.minzoom : 0,
                                        a.maxzoom >= 0 ? a.maxzoom : 18);
    cp0_map_set_zoom(map_, f.zoom);
    cp0_map_set_center(map_, f.lat, f.lon);
}

void AtlasApp::first_view()
{
    if (!map_) return;
    if (settings_.view.set) {
        cp0_map_set_zoom(map_, settings_.view.zoom);
        cp0_map_set_center(map_, settings_.view.lat, settings_.view.lon);
        return;
    }
    const ArchiveItem *a = settings_.source.empty() ? (archives_.empty() ? nullptr : &archives_.front())
                                                    : find_archive(settings_.source);
    if (a && a->has_bounds) {
        fit_archive(*a);
        return;
    }
    const atlas::Fit f = atlas::world_view(w_, view_h_, cp0_map_get_scale(map_));
    cp0_map_set_zoom(map_, f.zoom);
    cp0_map_set_center(map_, f.lat, f.lon);
}

/* The widget says when the centre or the zoom changed (LVGL thread, at most once per its tick): the view is worth
 * saving, and "No map here" is decided again. A change of the display scale is not reported: the Settings row asks. */
void AtlasApp::view_cb(void *user, double lat, double lon, int zoom)
{
    AtlasApp *self = static_cast<AtlasApp *>(user);
    self->view_dirty_ = true;
    self->settings_.view.set = true;
    self->settings_.view.lat = lat;
    self->settings_.view.lon = lon;
    self->settings_.view.zoom = zoom;
    if (self->follow_.on_view_change(lat, lon, zoom, cp0_map_get_scale(self->map_))) self->on_follow_off();
    if (self->gps_layer_ && self->settings_.gps) lv_obj_invalidate(self->gps_layer_); /* the dot moves with the map */
    self->update_chip();
}

/* What the engine draws as the credit, as far as the app can tell (the same source choice as its update_attribution:
 * the source's own, or the first two archives' joined with " | "). */
std::string AtlasApp::engine_attribution() const
{
    std::string raw;
    if (const OnlineItem *o = settings_.source.empty() ? nullptr : find_online(settings_.source)) {
        raw = o->attribution;
    } else if (const ArchiveItem *a = settings_.source.empty() ? nullptr : find_archive(settings_.source)) {
        raw = a->attribution;
    } else {
        int n = 0;
        for (const ArchiveItem &a : archives_) {
            const std::string clean = atlas::attribution_plain(a.attribution);
            if (clean.empty() || raw.find(clean) != std::string::npos) continue;
            raw += (raw.empty() ? "" : " | ") + clean;
            if (++n == 2) break;
        }
    }
    return atlas::attribution_plain(raw);
}

void AtlasApp::update_attribution()
{
    if (!attr_plate_ || !map_) return;
    const std::string text = engine_attribution();
    const std::string key = text + "|" + std::to_string(w_) + "x" + std::to_string(view_h_);
    if (key == attr_key_) return;
    attr_key_ = key;
    const lv_font_t *font = cp0_ui_font(CP0_TEXT_CAPTION);
    atlas::AttrInput in;
    in.w = w_;
    in.h = view_h_;
    in.target = target_;
    in.space = space_m_;
    in.text_h = lv_font_get_line_height(font);
    const atlas::AttrPlan plan = atlas::plan_attribution(in, text, [font](const std::string &s) {
        if (s.empty()) return 0;
        lv_point_t p;
        lv_text_get_size(&p, s.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return static_cast<int>(p.x);
    });
    attr_row_top_ = plan.row_top;
    gps_layout(); /* the follow button keeps clear of a credit in the bottom row at the right */
    if (!plan.cover) {
        lv_obj_add_flag(attr_plate_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    std::string joined;
    for (const std::string &l : plan.lines) joined += (joined.empty() ? "" : "\n") + l;
    lv_obj_set_pos(attr_plate_, plan.x, plan.y);
    lv_obj_set_size(attr_plate_, plan.w, plan.h);
    lv_label_set_text(attr_label_, joined.c_str());
    lv_obj_set_pos(attr_label_, plan.pad, plan.pad);
    lv_obj_set_size(attr_label_, plan.w - 2 * plan.pad, plan.h - 2 * plan.pad);
    lv_obj_remove_flag(attr_plate_, LV_OBJ_FLAG_HIDDEN);
}

void AtlasApp::update_chip()
{
    update_attribution();
    if (!chip_) return;
    std::string text;
    if (!store_) {
        text = store_error_;
    } else if (map_) {
        const OnlineItem *on = settings_.source.empty() ? nullptr : find_online(settings_.source);
        const ArchiveItem *one = !settings_.source.empty() ? find_archive(settings_.source)
                                 : (widget_by_id_ && archives_.size() == 1) ? &archives_.front() : nullptr;
        bool any_online = false;
        for (const OnlineItem &o : online_) any_online = any_online || o.facts.enabled;
        if (on) {
            text.clear(); /* the engine's own strip says why an online source shows nothing */
        } else if (archives_.empty()) {
            text = any_online ? "No map files. Pick an online source in Sources > Map shows."
                              : "No map yet. Import a map file in Sources.";
        } else if (one && one->state == CP0_MAP_E_ARCHIVE_MISSING) {
            text.clear(); /* the widget's red strip: "Map archive missing" */
        } else {
            std::vector<atlas::Coverage> cov;
            for (const ArchiveItem &a : archives_) {
                if (one && a.id != one->id) continue;
                atlas::Coverage c;
                c.has_bounds = a.has_bounds;
                c.bounds = a.bounds;
                c.minzoom = a.minzoom;
                c.maxzoom = a.maxzoom;
                c.missing = a.state == CP0_MAP_E_ARCHIVE_MISSING;
                c.bounds_deepest_only = atlas::bounds_deepest_only(widget_by_id_, a.has_zoom_extents);
                cov.push_back(c);
            }
            double lat = 0, lon = 0;
            cp0_map_get_center(map_, &lat, &lon);
            const int z = cp0_map_get_zoom(map_);
            const atlas::Box box = atlas::view_box(lat, lon, z, w_, view_h_, cp0_map_get_scale(map_));
            if (!atlas::view_covered(cov, box, z)) text = "No map here";
        }
    }
    if (text == chip_shown_) return;
    chip_shown_ = text;
    if (text.empty()) {
        lv_obj_add_flag(chip_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(chip_label_, text.c_str());
    lv_obj_remove_flag(chip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(chip_);
}

void AtlasApp::show_on_map(const std::string &id)
{
    set_shown(id, true);
    set_tab(Tab::Map);
}

/* ------------------------------------------------------------------------------------------------ input */

void AtlasApp::tab_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e))));
    self->post([self, i] { self->set_tab(static_cast<Tab>(i)); });
}

void AtlasApp::map_key_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    if (lv_event_get_key(e) == LV_KEY_ESC) self->post([self] { self->back(); });
}

void AtlasApp::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(e));
    if (item) self->on_keyboard(*item);
}

void AtlasApp::on_keyboard(const key_item &item)
{
    if (item.key_state != KBD_KEY_PRESSED) return;
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO)) return;
    /* + and - typed without a key code (the on-screen keyboard's path): the widget reads key codes only */
    if (item.key_code == 0 && map_ && sub_ == Sub::None && tab_ == Tab::Map && !dialog_ && item.utf8[0] && !item.utf8[1]) {
        const char c = item.utf8[0];
        if (gps_key(0, c)) return;
        if (c == '+' || c == '=') cp0_map_handle_key(map_, KEY_EQUAL, 0);
        else if (c == '-') cp0_map_handle_key(map_, KEY_MINUS, 0);
        return;
    }
    if (map_ && sub_ == Sub::None && tab_ == Tab::Map && !dialog_ && gps_key(item.key_code, 0)) return;
    if (item.key_code != KEY_TAB) return;
    /* Tab / Shift+Tab: the next / previous tab, only at a tab (a form uses Tab between its fields) */
    if (sub_ != Sub::None || dialog_) return;
    const int dir = (item.mods & KBD_MOD_SHIFT) ? 2 : 1;
    const Tab next = static_cast<Tab>((static_cast<int>(tab_) + dir) % 3);
    post([this, next] { set_tab(next); });
}

} // namespace atlasui
