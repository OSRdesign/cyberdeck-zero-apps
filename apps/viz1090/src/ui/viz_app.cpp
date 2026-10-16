/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro screen (see viz_app.hpp).
 */
#include "viz_app.hpp"

#include "app.hpp"
#include "osk_policy.hpp"

#include "conf.hpp"
#include "sdr.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <unistd.h>

namespace viz {

namespace {

std::atomic<bool> g_start{false};

constexpr std::uint32_t kKeyTab = 15; /* the keyboard bridge hands Tab over as its evdev code (KEY_TAB) */
constexpr int kTickMs = 250;          /* the GPS snapshot is looked at 4 times a second, the SDR once a second */
constexpr int kSdrEveryMs = 1000;

/* the status line's colours: the launcher's palette (Net Toolbox's band); a status is never a colour alone */
constexpr std::uint32_t kGreyRgb = 0x9A9A9A;
constexpr std::uint32_t kOkRgb = 0x66CC88;
constexpr std::uint32_t kWarnRgb = 0xF0B400;
constexpr std::uint32_t kBadRgb = 0xFF6666;

std::uint32_t band_rgb(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return kOkRgb;
    case Tone::Warn: return kWarnRgb;
    case Tone::Bad: return kBadRgb;
    case Tone::Muted: return kGreyRgb; /* the same grey as Normal: a status line stays readable */
    default: return kGreyRgb;
    }
}

cp0::ui::ValueTone value_tone(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return cp0::ui::ValueTone::Ok;
    case Tone::Warn: return cp0::ui::ValueTone::Warn;
    case Tone::Bad: return cp0::ui::ValueTone::Bad;
    case Tone::Muted: return cp0::ui::ValueTone::Muted;
    default: return cp0::ui::ValueTone::Normal;
    }
}

const lv_font_t *face(const char *name, int px)
{
    if (const lv_font_t *f = cp0_fonts().get(name, static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

int text_width(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

const lv_font_t *caption_font()
{
    return face("Montserrat-Medium.ttf", cp0_ui_metrics_get()->text_px[CP0_TEXT_CAPTION]);
}

/* The headless harness (host build, CDK_APP_DIR defined): the test fixtures named relative to the app's folder, and a
 * settings file of its own in this size's state directory (seeded from VIZ_CONF_SEED), never the real ~/.config. */
void resolve_test_paths()
{
#ifdef CDK_APP_DIR
    std::setvbuf(stderr, nullptr, _IONBF, 0); /* the harness sends stderr to a file and ends with _exit(): no buffer */
    for (const char *name : {"VIZ_USB_DIR", "VIZ_DEV_DIR", "VIZ_WORLD", "VIZ_CONF_SEED", "VIZ_PROGRAM"}) {
        const char *v = std::getenv(name);
        if (v && v[0] && v[0] != '/') setenv(name, (std::string(CDK_APP_DIR) + "/" + v).c_str(), 1);
    }
#endif
}

std::string settings_file()
{
#ifdef CDK_APP_DIR
    const char *state = std::getenv("APPLAUNCH_STATE_DIR");
    const std::string path = std::string(state && state[0] ? state : "/tmp") + "/viz1090.conf";
    const char *seed = std::getenv("VIZ_CONF_SEED");
    write_text_atomic(path, seed && seed[0] ? read_text(seed) : std::string());
    return path;
#else
    return conf_path();
#endif
}

} // namespace

bool start_confirmed()
{
    return g_start.load();
}

VizApp::VizApp(lv_obj_t *screen)
{
    resolve_test_paths();
    conf_path_ = settings_file();
    const Conf conf = parse_conf(read_text(conf_path_));
    setup_.lat = conf.lat;
    setup_.lon = conf.lon;
    setup_.code = filter_code(conf.airport) == conf.airport ? conf.airport : std::string();
    recent_ = conf.recent;
    const std::string world = world_map_path();
    if (!airports_.load(world)) std::fprintf(stderr, "[%s] no airport list in %s\n", kAppId, world.c_str());
    const char *program = std::getenv("VIZ_PROGRAM");
    const std::string viz = program && program[0] ? program : "/opt/viz1090/viz1090";
    installed_ = access(viz.c_str(), X_OK) == 0;
    usb_dir_ = usb_devices_dir();
    sdr_ = find_sdr(usb_dir_);
    next_sdr_ms_ = now_ms() + kSdrEveryMs;
    gps_ = std::make_unique<GpsWatcher>(serial_dev_dir());
    gps_->start();
    Place p;
    std::fprintf(stderr, "[%s] intro: position %s, %zu recent, %zu airports, SDR %s, viz1090 %s, settings %s\n", kAppId,
                 setup_.position(p) ? place_text(p).c_str() : "not set", recent_.size(), airports_.size(),
                 sdr_.empty() ? "not found" : sdr_.c_str(), installed_ ? "installed" : "missing", conf_path_.c_str());

    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());
    inset_listener_ = cp0::ui::osk::add_inset_listener([this](int inset) { on_inset(inset); });
    show(0);
    timer_ = lv_timer_create(tick_cb, kTickMs, this);
}

VizApp::~VizApp()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    lv_async_call_cancel(posted_cb, this);
    if (inset_listener_) cp0::ui::osk::remove_inset_listener(inset_listener_);
    inset_listener_ = 0;
    if (gps_) gps_->stop(); /* the port closed, the thread joined (within about 200 ms) */
    teardown();
    page_.reset();
}

/* ---------------------------------------------------------------- plumbing */

void VizApp::posted_cb(void *data)
{
    auto *self = static_cast<VizApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

/* List, field, bar and Esc callbacks run inside an LVGL event of the widget: what replaces the widget runs after it. */
void VizApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void VizApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

std::int64_t VizApp::now_ms() const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void VizApp::tick_cb(lv_timer_t *timer)
{
    auto *self = static_cast<VizApp *>(lv_timer_get_user_data(timer));
    if (self) self->tick();
}

/* The root's rows follow the receivers: the dongle is looked for once a second (hot-plug), the GPS thread's latest
 * result four times a second. */
void VizApp::tick()
{
    if (nav_.current() != View::Root) return;
    bool changed = false;
    const std::int64_t now = now_ms();
    if (now >= next_sdr_ms_) {
        next_sdr_ms_ = now + kSdrEveryMs;
        const std::string sdr = find_sdr(usb_dir_);
        if (sdr != sdr_) {
            std::fprintf(stderr, "[%s] SDR receiver %s\n", kAppId, sdr.empty() ? "unplugged" : ("found: " + sdr).c_str());
            sdr_ = sdr;
            changed = true;
        }
    }
    const GpsSnapshot gps = gps_->snapshot();
    if (gps.seq != gps_shown_.seq) {
        if (gps.state != gps_shown_.state)
            std::fprintf(stderr, "[%s] GPS receiver: %s%s\n", kAppId,
                         gps.state == GpsState::None ? "none" : gps.state == GpsState::Waiting ? "no fix yet on " : "fix on ",
                         gps.port.c_str());
        if (!gps.denied.empty() && gps.denied != gps_shown_.denied)
            std::fprintf(stderr, "[%s] cannot read /dev/%s: the user is not in the dialout group. Run: sudo usermod -aG "
                                 "dialout $USER, then log out and in\n", kAppId, gps.denied.c_str());
        gps_shown_ = gps;
        changed = true;
    }
    if (changed) refresh_root(false);
}

VizApp::Geo VizApp::geo() const
{
    Geo g;
    g.x = layout_.roller_x;
    g.w = std::max(1, layout_.roller_w);
    g.s = cp0_ui_tok(CP0_TOK_SPACE_S);
    g.inset = cp0_ui_tok(CP0_TOK_SPACE_L);
    g.lh = static_cast<int>(lv_font_get_line_height(caption_font()));
    g.top = layout_.body_y + g.s;
    g.bottom = layout_.body_y + layout_.body_h;
    g.target = cp0_ui_tok(CP0_TOK_TARGET);
    return g;
}

/* ---------------------------------------------------------------- pages */

void VizApp::show(int select)
{
    teardown();
    page_->set_title(nav_.title_path());
    switch (nav_.current()) {
    case View::Root: build_root(select); break;
    case View::Position: build_position(select); break;
    case View::Latitude: build_degrees(true); break;
    case View::Longitude: build_degrees(false); break;
    }
}

void VizApp::teardown()
{
    list_up_ = nullptr;
    list_.reset();
    bar_.reset();
    field_.reset(); /* stops editing: the input context, the keypad intercept and the keyboard request come back */
    hint_ = nullptr; /* deleted with the body */
    if (page_) page_->clear_body();
    if (band_) lv_obj_delete(band_);
    band_ = nullptr;
}

void VizApp::back()
{
    const View from = nav_.current();
    if (!nav_.back()) {
        std::fprintf(stderr, "[%s] cancelled (Esc at the root)\n", kAppId);
        cdk::request_quit(1); /* a short Esc at the root ends the app: run_viz1090.sh goes back to the launcher */
        return;
    }
    if (from == View::Position) show(2);            /* the root, on the Position row */
    else if (from == View::Latitude) show(0);       /* the Position page, on Latitude */
    else if (from == View::Longitude) show(1);      /* ... on Longitude */
    else show(0);
}

/* The key handler in front of a CompactList: Up on its first row and Tab go back to what is above it (the action
 * bar, the field), which the list itself does not do. */
void VizApp::list_key_cb(lv_event_t *event)
{
    auto *self = static_cast<VizApp *>(lv_event_get_user_data(event));
    if (!self || !self->list_ || !self->list_up_) return;
    const std::uint32_t key = lv_event_get_key(event);
    int first = -1;
    for (int i = 0; i < self->list_->count(); ++i)
        if (self->list_->row(i).kind == cp0::ui::ListRowKind::Item) {
            first = i;
            break;
        }
    if (key == kKeyTab || (key == LV_KEY_UP && self->list_->selected() == first)) {
        const std::function<void()> up = self->list_up_;
        self->post(up);
        lv_event_stop_processing(event);
    }
}

void VizApp::build_list(int y, int h, std::vector<cp0::ui::ListRow> rows, int select)
{
    const Geo g = geo();
    cp0_ui_list_layout_t l{};
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, g.x, y, g.w, std::max(1, h), &l);
    cp0::ui::CompactListEvents events;
    events.activate = [this](int, std::intptr_t id) {
        post([this, id] {
            if (nav_.current() == View::Root) activate_root(id);
            else if (nav_.current() == View::Position) activate_position(id);
        });
    };
    events.back = [this] { post([this] { back(); }); };
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), l, std::move(events));
    lv_obj_add_event_cb(list_->focus_object(), list_key_cb,
                        static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS), this);
    list_->reset_rows(rows, select);
}

/* The check and the cross of a value are LVGL symbols, which the launcher's Montserrat (FreeType) does not carry: the
 * value font of the list falls back to the built-in Montserrat of the same size for them (this process's fonts only). */
void VizApp::prepare_fonts()
{
    const lv_font_t *value = face("Montserrat-Medium.ttf", list_layout_.value_px);
    const lv_font_t *builtin = cp0_ui_font_px(list_layout_.value_px);
    if (value && builtin && value != builtin && !value->fallback &&
        value->get_glyph_dsc != builtin->get_glyph_dsc) /* a FreeType face, not a built-in one (read-only) */
        const_cast<lv_font_t *>(value)->fallback = builtin;
}

std::string VizApp::fit_value(const RowText &text, cp0::ui::RowAffordance aff) const
{
    if (text.values.empty()) return std::string();
    const lv_font_t *label = face("Montserrat-Bold.ttf", list_layout_.text_px); /* the selected row's label */
    const lv_font_t *value = face("Montserrat-Medium.ttf", list_layout_.value_px);
    /* a chevron or a dot keeps its slot free at the right (the C enum has the same order as RowAffordance) */
    const int trail = cp0_ui_list_trail_w(&list_layout_, static_cast<cp0_ui_list_affordance_t>(aff));
    const int room = list_layout_.w - 2 * list_layout_.inset - trail - list_layout_.gap - text_width(text.label, label);
    for (const std::string &v : text.values)
        if (text_width(v, value) <= room) return v;
    return text.values.back();
}

cp0::ui::ListRow VizApp::list_row(const RowText &text, std::intptr_t id, cp0::ui::RowAffordance aff) const
{
    return cp0::ui::ListRow::item(text.label, fit_value(text, aff), value_tone(text.tone), id, aff);
}

/* ---------------------------------------------------------------- root */

void VizApp::build_root(int select)
{
    const Geo g = geo();
    band_ = lv_label_create(page_->root());
    lv_obj_remove_flag(band_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(band_, caption_font(), 0);
    lv_label_set_long_mode(band_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(band_, g.x + g.inset, g.top);
    lv_obj_set_size(band_, std::max(1, g.w - 2 * g.inset), g.lh);
    int y = g.top + g.lh + g.s;

    ui::ActionBarEvents bar_events;
    bar_events.activate = [this](int id) {
        post([this, id] {
            if (id == kStart && nav_.current() == View::Root) start();
        });
    };
    bar_events.back = [this] { post([this] { back(); }); };
    bar_events.down = [this] {
        post([this] {
            if (list_) focus(list_->focus_object());
        });
    };
    const lv_font_t *body = face("Montserrat-Medium.ttf", cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY]);
    bar_ = std::make_unique<ui::ActionBar>(page_->root(), g.x, y, g.w, g.target, body,
                                           std::vector<ui::ActionCell>{{kStart, "Start"}}, std::move(bar_events));
    y += g.target + g.s;

    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, g.x, y, g.w, std::max(1, g.bottom - y),
                       &list_layout_);
    prepare_fonts();
    gps_shown_ = gps_->snapshot();
    build_list(y, g.bottom - y, root_rows(), select);
    list_up_ = [this] {
        if (bar_) focus(bar_->cell_object(0));
    };
    /* the order of the keys: Start, then the list; Start has the focus (Enter starts, as in 0.1) */
    if (lv_group_t *group = cdk::input_group()) {
        lv_group_add_obj(group, bar_->cell_object(0));
        lv_group_add_obj(group, list_->focus_object());
    }
    focus(bar_->cell_object(0));
    show_band();
}

std::vector<cp0::ui::ListRow> VizApp::root_rows() const
{
    using cp0::ui::RowAffordance;
    /* SDR and GPS: a tap looks again / takes the fix as the position (an action, its result in the value);
     * Position opens its page */
    return {list_row(sdr_row(sdr_), kSdr, RowAffordance::Action), list_row(gps_row(gps_shown_), kGps, RowAffordance::Action),
            list_row(position_row(setup_), kPosition, RowAffordance::Open)};
}

void VizApp::refresh_root(bool force)
{
    if (!list_ || nav_.current() != View::Root) return;
    (void)force;
    list_->set_rows(root_rows());
    show_band();
}

void VizApp::show_band()
{
    if (!band_) return;
    const Band b = noted_ ? note_ : root_band(installed_, sdr_, setup_);
    const char *now = lv_label_get_text(band_);
    if (!now || b.text != now) {
        lv_label_set_text(band_, b.text.c_str());
        lv_obj_update_layout(band_);
        if (text_width(b.text, caption_font()) > lv_obj_get_width(band_)) /* cut with dots: the scenes' logs show it */
            std::fprintf(stderr, "[%s] the status line does not fit: %s\n", kAppId, b.text.c_str());
    }
    lv_obj_set_style_text_color(band_, lv_color_hex(band_rgb(b.tone)), 0);
}

void VizApp::note(const Band &band)
{
    note_ = band;
    noted_ = !band.text.empty();
    show_band();
}

void VizApp::activate_root(std::intptr_t id)
{
    noted_ = false;
    switch (id) {
    case kSdr:
        sdr_ = find_sdr(usb_dir_);
        next_sdr_ms_ = now_ms() + kSdrEveryMs;
        note(sdr_band(sdr_));
        refresh_root(true);
        break;
    case kGps: {
        const GpsSnapshot gps = gps_->snapshot();
        gps_shown_ = gps;
        if (gps.state == GpsState::Fix) {
            setup_.set_place(Place{"", gps.lat, gps.lon});
            std::fprintf(stderr, "[%s] position from the GPS: %s\n", kAppId, coords_text(gps.lat, gps.lon).c_str());
        }
        note(gps_band(gps));
        refresh_root(true);
        break;
    }
    case kPosition:
        nav_.open(View::Position);
        show(0);
        break;
    default: break;
    }
}

void VizApp::start()
{
    noted_ = false;
    const StartCheck check = check_start(installed_, setup_);
    if (check != StartCheck::Ok) {
        const Band why = refusal_band(check);
        std::fprintf(stderr, "[%s] Start refused: %s\n", kAppId, why.text.c_str());
        note(why);
        return;
    }
    Place place;
    setup_.position(place);
    const std::vector<Place> recent = push_recent(recent_, place);
    const std::string error = save_conf(conf_path_, place, recent);
    if (!error.empty()) {
        std::fprintf(stderr, "[%s] %s\n", kAppId, error.c_str());
        note(Band{error, Tone::Bad});
        return;
    }
    recent_ = recent;
    std::fprintf(stderr, "[%s] Start: %s saved in %s\n", kAppId, place_text(place).c_str(), conf_path_.c_str());
    g_start = true;
    cdk::request_quit(0);
}

/* ---------------------------------------------------------------- Position, Latitude, Longitude */

/* A text page: the field (its label beside it) and one line under it, in the page's body (it shrinks above the
 * on-screen keyboard and keeps the field in view). */
void VizApp::build_field_page(const std::string &label, const std::string &text, const std::string &placeholder,
                              std::size_t max_bytes, std::function<void(const std::string &)> changed,
                              std::function<void(const std::string &)> submit)
{
    cp0::ui::TextFieldSpec spec;
    spec.label = label;
    spec.type = cp0::ui::TextType::Text; /* letters, '-' and '.': the number pad has neither '-' nor '.' */
    spec.text = text;
    spec.placeholder = placeholder;
    spec.max_bytes = max_bytes;
    spec.label_w = layout_.roller_w * 22 / 100; /* the label beside the field: the template's form rule */
    if (const lv_font_t *font = face("Montserrat-Medium.ttf", cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY]))
        spec.label_w = std::max(spec.label_w, text_width(spec.label, font) + layout_.gap);
    cp0::ui::TextFieldEvents events;
    events.changed = std::move(changed);
    events.submit = [this, submit](const std::string &t) { post([submit, t] { submit(t); }); };
    events.back = [this] { post([this] { back(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(page_->body(), layout_, std::move(spec), std::move(events));
    hint_ = lv_label_create(page_->body());
    lv_obj_remove_flag(hint_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(hint_, caption_font(), 0);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(hint_, layout_.roller_w);
    lv_obj_set_height(hint_, lv_font_get_line_height(caption_font()));
}

void VizApp::set_hint(const Band &band)
{
    if (!hint_) return;
    const char *now = lv_label_get_text(hint_);
    if (!now || band.text != now) {
        lv_label_set_text(hint_, band.text.c_str());
        lv_obj_update_layout(hint_);
        if (text_width(band.text, caption_font()) > lv_obj_get_width(hint_))
            std::fprintf(stderr, "[%s] the field's line does not fit: %s\n", kAppId, band.text.c_str());
    }
    lv_obj_set_style_text_color(hint_, lv_color_hex(band_rgb(band.tone)), 0);
}

void VizApp::build_position(int select)
{
    code_typed_ = setup_.code;
    build_field_page("Nearest airport code", code_typed_, "CDG", kCodeMax,
                     [this](const std::string &t) { code_changed(t); },
                     [this](const std::string &t) { code_submit(t); });
    set_hint(code_hint(code_typed_, airports_));

    /* the list under the field and its line: Latitude, Longitude, Recent */
    const Geo g = geo();
    lv_obj_update_layout(page_->root());
    lv_area_t a;
    lv_obj_get_coords(hint_, &a);
    const int y = a.y2 + 1 + g.s;
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, g.x, y, g.w, std::max(1, g.bottom - y),
                       &list_layout_);
    prepare_fonts();
    build_list(y, g.bottom - y, position_rows(), select);
    list_up_ = [this] {
        if (field_) focus(field_->focus_object()); /* focus starts editing */
    };
    lv_group_t *group = cdk::input_group();
    if (group && lv_obj_get_group(field_->focus_object()) != group) lv_group_add_obj(group, field_->focus_object());
    if (group && lv_obj_get_group(list_->focus_object()) != group) lv_group_add_obj(group, list_->focus_object());
    /* A physical keyboard types at once: the field edits. When the on-screen keyboard may show (no keyboard, or the
     * rule forced), the page opens on its list so that the rows and Recent are in view; a tap on the field (or Up,
     * Tab) edits, and only then the keyboard comes, by the shared rule. */
    bool osk_may_show = false;
    if (cdk::osk_policy::Policy *policy = cdk::osk_policy::installed()) osk_may_show = policy->allowed(now_ms());
    if (osk_may_show) focus(list_->focus_object());
    else focus(field_->focus_object());
}

std::vector<cp0::ui::ListRow> VizApp::position_rows() const
{
    using cp0::ui::RowAffordance;
    /* Latitude and Longitude open a text editor (Edit); a Recent place is applied and the root comes back (Action) */
    std::vector<cp0::ui::ListRow> rows{list_row(latitude_row(setup_), kLat, RowAffordance::Edit),
                                       list_row(longitude_row(setup_), kLon, RowAffordance::Edit)};
    if (!recent_.empty()) {
        rows.push_back(cp0::ui::ListRow::section("Recent"));
        for (std::size_t i = 0; i < recent_.size(); ++i)
            rows.push_back(list_row(recent_row(recent_[i]), kRecent + static_cast<std::intptr_t>(i), RowAffordance::Action));
    }
    return rows;
}

void VizApp::activate_position(std::intptr_t id)
{
    if (id == kLat || id == kLon) {
        nav_.open(id == kLat ? View::Latitude : View::Longitude);
        show(0);
        return;
    }
    const std::intptr_t index = id - kRecent;
    if (index >= 0 && index < static_cast<std::intptr_t>(recent_.size()))
        pick(recent_[static_cast<std::size_t>(index)]);
}

/* What the code field keeps: letters, upper case (a lower-case letter typed becomes upper case at once). */
void VizApp::code_changed(const std::string &text)
{
    const std::string code = filter_code(text);
    if (code != text && field_) field_->set_text(code);
    code_typed_ = code;
    set_hint(code_hint(code, airports_));
}

void VizApp::code_submit(const std::string &text)
{
    if (nav_.current() != View::Position || !field_) return;
    const std::string code = filter_code(text);
    if (const Airport *airport = airports_.find(code)) {
        pick(Place{airport->code, airport->lat, airport->lon});
        return;
    }
    set_hint(code_hint(code, airports_));
    if (!field_->editing()) field_->begin_edit();
}

void VizApp::pick(const Place &place)
{
    setup_.set_place(place);
    std::fprintf(stderr, "[%s] position: %s\n", kAppId, place_text(place).c_str());
    nav_ = Nav();
    show(2); /* the root, on the Position row */
    note(place_set_band(place));
}

void VizApp::build_degrees(bool latitude)
{
    double v = 0;
    const std::string &current = latitude ? setup_.lat : setup_.lon;
    const std::string text = parse_degrees(current, latitude ? kLatLimit : kLonLimit, v) ? short_degrees(v) : "";
    build_field_page(latitude ? "Latitude" : "Longitude", text, latitude ? "48.8566" : "2.3522", kDegreesMax,
                     [this](const std::string &t) {
                         const std::string kept = filter_degrees(t);
                         if (kept != t && field_) field_->set_text(kept);
                     },
                     [this, latitude](const std::string &t) { degrees_submit(latitude, t); });
    set_hint(degrees_hint(latitude));
    focus(field_->focus_object()); /* focus starts editing: the on-screen keyboard shows when the rule allows it */
}

void VizApp::degrees_submit(bool latitude, const std::string &text)
{
    const View want = latitude ? View::Latitude : View::Longitude;
    if (nav_.current() != want || !field_) return;
    const std::string error = latitude ? latitude_error(text) : longitude_error(text);
    if (!error.empty()) {
        set_hint(Band{error, Tone::Bad});
        if (!field_->editing()) field_->begin_edit();
        return;
    }
    double v = 0;
    parse_degrees(text, latitude ? kLatLimit : kLonLimit, v);
    if (latitude) setup_.set_lat(v);
    else setup_.set_lon(v);
    std::fprintf(stderr, "[%s] %s typed: %s\n", kAppId, latitude ? "latitude" : "longitude", short_degrees(v).c_str());
    nav_.back();
    show(latitude ? 0 : 1);
}

/* The on-screen keyboard came or went. On the Position page the list under the field hides while it is up (the page
 * is then the header, the field and its line, which fit above it); the body loses its top padding so that the field's
 * line fits too (the template's form rule). */
void VizApp::on_inset(int inset)
{
    if (!field_ || !hint_) return;
    if (list_ && nav_.current() == View::Position) list_->set_hidden(inset > 0);
    lv_obj_t *body = page_->body();
    lv_obj_set_style_pad_top(body, inset > 0 ? 0 : layout_.gap, 0);
    lv_obj_update_layout(body);
    lv_obj_scroll_to_view(hint_, LV_ANIM_OFF);
    lv_obj_scroll_to_view(field_->row(), LV_ANIM_OFF);
}

} // namespace viz
