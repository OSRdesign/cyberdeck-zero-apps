/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app: the pages (see ha_app.hpp).
 */
#include "ha_app.hpp"

#include "app.hpp"
#include "format.hpp"
#include "qr.hpp"
#include "runner.hpp"
#include "sim.hpp"
#include "store.hpp"
#include "text.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "keyboard_input.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: a new QR code in the same buffer */

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace haui {

using cp0::ui::ListRow;
using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;

namespace {

constexpr int kTimerMs = 200;
constexpr std::uint32_t kRebuildMs = 400;   /* the lists at most about twice a second */
constexpr std::uint32_t kMessageMs = 4000;
constexpr std::uint32_t kNamedMs = 1500;    /* "Connected as <user>" before Home */
constexpr std::uint32_t kSignedInMaxMs = 10000;
constexpr std::uint32_t kSlideDebounceMs = 300;
constexpr std::uint32_t kSlideHoldMs = 1500; /* the slider is not moved by the state while the user slides */
constexpr int kInfoSidePct = 60;             /* a tap right of this share of the row opens the entity page */
constexpr int kQuiet = 2;                    /* quiet zone of the QR code, in modules (on a white box) */
constexpr int kMuted = 0x9A9A9A;

const char *kConnectText = "Scan with your phone to connect to Home Assistant";

std::unique_ptr<ha::Backend> make_backend()
{
    const char *demo = std::getenv("HA_DEMO");
#ifdef CDK_APP_DIR
    /* the render harness: always the simulated Home Assistant, never the network */
    std::fprintf(stderr, "[home-assistant] simulated Home Assistant: %s\n", demo && *demo ? demo : "connect");
    return ha::make_demo_backend(demo && *demo ? demo : "connect");
#else
    if (demo && *demo) {
        std::fprintf(stderr, "[home-assistant] HA_DEMO=%s: simulated Home Assistant, no network\n", demo);
        return ha::make_demo_backend(demo);
    }
    const std::string dir = ha::store::config_dir();
    ha::Credentials creds = ha::store::load_credentials(dir);
    std::fprintf(stderr, "[home-assistant] config %s, %s\n", dir.c_str(),
                 creds.valid() ? "signed in (token present)" : "not signed in");
    return std::make_unique<ha::ClientBackend>(ha::store::load_settings(dir), std::move(creds),
                                               ha::store::load_favourites(dir), dir, ha::store::load_names(dir));
#endif
}

lv_obj_t *plain_label(lv_obj_t *parent, const lv_font_t *font, int rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(static_cast<uint32_t>(rgb)), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* the last UTF-8 character of s removed */
void pop_utf8(std::string &s)
{
    if (s.empty()) return;
    std::size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
}

bool contains_ci(const std::string &hay, const std::string &needle)
{
    return ha::lower(hay).find(ha::lower(needle)) != std::string::npos;
}

int text_w(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t p{};
    lv_text_get_size(&p, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(p.x);
}

/* The value of a row whose label must stay whole ("Server"): the toolkit's list never cuts a value and cuts the label
 * instead, so the value is chosen here: the first candidate that fits beside the label (measured in the bold face of
 * the highlighted row), else the last one shortened with "...". */
std::string fit_value(const cp0_ui_list_layout_t &l, const std::string &label, const std::vector<std::string> &candidates,
                      cp0_ui_list_affordance_t affordance)
{
    if (candidates.empty()) return std::string();
    const lv_font_t *bold = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(l.text_px));
    const lv_font_t *value = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(l.value_px));
    if (!bold || !value) return candidates.front();
    const int room = l.w - cp0_ui_list_trail_w(&l, affordance) - 2 * l.inset - l.gap - text_w(label, bold) - 2;
    for (const std::string &c : candidates)
        if (text_w(c, value) <= room) return c;
    std::string s = candidates.back();
    while (!s.empty() && text_w(s + "...", value) > room) pop_utf8(s);
    return s + "...";
}

std::uint32_t lv_key_of(std::uint32_t code)
{
    switch (code) {
    case KEY_UP: return LV_KEY_UP;
    case KEY_DOWN: return LV_KEY_DOWN;
    case KEY_LEFT: return LV_KEY_LEFT;
    case KEY_RIGHT: return LV_KEY_RIGHT;
    case KEY_ENTER:
    case KEY_KPENTER: return LV_KEY_ENTER;
    case KEY_ESC: return LV_KEY_ESC;
    case KEY_HOME: return LV_KEY_HOME;
    case KEY_END: return LV_KEY_END;
    case KEY_PAGEUP: return LV_KEY_PREV;
    case KEY_PAGEDOWN: return LV_KEY_NEXT;
    case KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case KEY_DELETE: return LV_KEY_DEL;
    default: return 0;
    }
}

} // namespace

HaApp::HaApp(lv_obj_t *screen) : screen_(screen)
{
    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());

    /* every key reaches the app as LV_EVENT_KEYBOARD on its screen; the keypad path is intercepted so that letters
     * typed into a room's filter never move the list (W, E, R, T share codes with the arrows there) */
    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    saved_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
    cp0_keyboard_set_lvgl_keypad_intercept(1);

    backend_ = make_backend();
    view_ = backend_->snapshot();
    view_version_ = backend_->version();
    message_seen_ = view_.message_seq;
    show(view_.signed_in ? Screen::Home : Screen::Connect);
    timer_ = lv_timer_create([](lv_timer_t *t) { static_cast<HaApp *>(lv_timer_get_user_data(t))->tick(); },
                             kTimerMs, this);
    std::fprintf(stderr, "[home-assistant] %s, canvas %dx%d\n", kVersion, layout_.w, layout_.h);
}

HaApp::~HaApp()
{
    lv_async_call_cancel(posted_cb, this);
    if (timer_) lv_timer_delete(timer_);
    if (slide_timer_) lv_timer_delete(slide_timer_);
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(screen_))
        lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    dialog_.reset();
    field_.reset(); /* stops editing: input context, intercept and the keyboard request restored */
    cp0_keyboard_set_lvgl_keypad_intercept(saved_intercept_);
    roller_.reset();
    list_.reset();
    page_.reset();
    backend_.reset(); /* the connection thread stops and is joined */
}

void HaApp::posted_cb(void *data)
{
    auto *self = static_cast<HaApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void HaApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void HaApp::focus(lv_obj_t *obj)
{
    focus_obj_ = obj;
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

/* --------------------------------------------------------------------------------------------- pages */

void HaApp::clear_page()
{
    close_dialog();
    list_.reset();
    roller_.reset();
    field_.reset();
    field_hint_ = nullptr;
    page_->clear_body();
    if (connect_) lv_obj_delete(connect_);
    connect_ = qr_box_ = qr_img_ = connect_text_ = connect_addr_ = connect_status_ = nullptr;
    qr_shown_.clear();
    targets_.clear();
    roller_kinds_.clear();
    roller_specs_.clear();
    focus_obj_ = nullptr;
    page_->set_caption("", false);
}

void HaApp::set_title()
{
    switch (screen_kind_) {
    case Screen::Room: page_->set_title({"Home Assistant", view_.room_name.empty() ? std::string("Room") : view_.room_name}); break;
    case Screen::Entity: page_->set_title({"Home Assistant", view_.detail.name.empty() ? entity_id_ : view_.detail.name}); break;
    case Screen::Settings: page_->set_title({"Home Assistant", "Settings"}); break;
    case Screen::ServerEdit: page_->set_title({"Home Assistant", "Server address"}); break;
    default: page_->set_title({"Home Assistant"}); break;
    }
}

void HaApp::show(Screen s)
{
    if (screen_kind_ == Screen::Room && s != Screen::Room && s != Screen::Entity) filter_.clear();
    clear_page();
    screen_kind_ = s;
    switch (s) {
    case Screen::Connect:
        backend_->send(ha::Command::set_view(false, "", ""));
        build_connect();
        break;
    case Screen::ServerEdit: build_server_edit(); break;
    case Screen::Home:
        room_shown_ = false;
        entity_id_.clear();
        backend_->send(ha::Command::set_view(false, "", ""));
        build_list(true);
        break;
    case Screen::Room:
        entity_id_.clear();
        backend_->send(ha::Command::set_view(true, room_id_, ""));
        build_list(true);
        break;
    case Screen::Settings:
        backend_->send(ha::Command::set_view(false, "", ""));
        build_list(true);
        break;
    case Screen::Entity:
        backend_->send(ha::Command::set_view(room_shown_ && entity_from_ == Screen::Room, room_id_, entity_id_));
        build_entity();
        break;
    case Screen::None: break;
    }
    /* the view as it is now (the backend answers the new view at its next turn) */
    view_ = backend_->snapshot();
    view_version_ = backend_->version();
    set_title();
    switch (s) {
    case Screen::Connect: refresh_connect(); break;
    case Screen::Home:
    case Screen::Room:
    case Screen::Settings:
        refresh_list();
        if (list_) list_->select(0, false);
        break;
    case Screen::Entity: refresh_entity(true); break;
    default: break;
    }
    update_caption();
    last_rebuild_ = lv_tick_get();
}

void HaApp::build_list(bool caption_room)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_list_layout_t ll{};
    cp0_ui_list_page_layout(m, &layout_, CP0_UI_LIST_COMFORTABLE, &ll);
    ll.separator_px = 1;
    if (caption_room) { /* the status line sits over the body's top: the list starts under it */
        const int cap = layout_.caption_y - layout_.body_y + lv_font_get_line_height(cp0_ui_font(CP0_TEXT_CAPTION)) +
                        std::max(2, m->tok[CP0_TOK_SPACE_S] / 2);
        ll.y += cap;
        ll.h = std::max(ll.row_h, ll.h - cap);
    }
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) {
        const bool by_key = key_activation_;
        const std::uint32_t key = last_key_;
        int x = -1;
        if (!by_key) {
            lv_indev_t *in = lv_indev_active();
            if (in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
                lv_point_t p;
                lv_indev_get_point(in, &p);
                x = p.x;
            }
        }
        post([this, id, by_key, key, x] { list_activate(id, by_key, key, x); });
    };
    ev.back = [this] { post([this] { back(); }); };
    cp0::ui::CompactListOptions opt;
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, std::move(ev), opt);
    list_x_ = ll.x;
    list_w_ = ll.w;
    focus(list_->focus_object());
}

void HaApp::build_connect()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int sp = m->tok[CP0_TOK_SPACE_L], sm = m->tok[CP0_TOK_SPACE_M];
    const int w = layout_.w, body_y = layout_.body_y, body_h = layout_.body_h;
    const bool portrait = layout_.h > layout_.w;

    connect_ = lv_obj_create(page_->root());
    lv_obj_remove_style_all(connect_);
    lv_obj_set_pos(connect_, 0, body_y);
    lv_obj_set_size(connect_, w, body_h);
    lv_obj_remove_flag(connect_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(connect_, LV_OBJ_FLAG_CLICKABLE);

    /* the Server address row: the list's own row, across the bottom (the column beside the code is too narrow for a
     * label and an address at the row's text size) */
    const int row_h = m->tok[CP0_TOK_ROW_H];
    const int row_y = body_h - row_h - sm; /* in connect_ */
    const int top_h = std::max(1, row_y - 2 * sm);
    int box, qx, qy, col_x, col_w, y;
    if (portrait) {
        box = std::min(w - 2 * sp, top_h * 55 / 100);
        qx = (w - box) / 2;
        qy = sm;
        col_x = sp;
        col_w = w - 2 * sp;
        y = qy + box + sm;
    } else {
        box = std::min(top_h, (w - 3 * sp) / 2);
        qx = sp;
        qy = sm;
        col_x = sp + box + sp;
        col_w = w - col_x - sp;
        y = sm;
    }
    const int col_bottom = portrait ? row_y - sm / 2 : qy + top_h;
    /* the column's text at the BODY size, or the CAPTION size when the instruction, the address line and two status
     * lines would not fit (a 480x320 screen at the deck's density) */
    const lv_font_t *body = cp0_ui_font(CP0_TEXT_BODY), *caption = cp0_ui_font(CP0_TEXT_CAPTION);
    auto text_h = [col_w](const char *text, const lv_font_t *f) {
        lv_point_t p;
        lv_text_get_size(&p, text, f, 0, 0, col_w, LV_TEXT_FLAG_NONE);
        return static_cast<int>(p.y);
    };
    const int need = text_h(kConnectText, body) + text_h(view_.server_url.c_str(), caption) +
                     2 * lv_font_get_line_height(body) + 2 * sm;
    const lv_font_t *col_font = need <= col_bottom - y ? body : caption;
    qr_box_px_ = box;
    qr_box_ = lv_obj_create(connect_);
    lv_obj_remove_style_all(qr_box_);
    lv_obj_set_pos(qr_box_, qx, qy);
    lv_obj_set_size(qr_box_, box, box);
    lv_obj_set_style_border_color(qr_box_, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_border_width(qr_box_, 1, 0);
    lv_obj_remove_flag(qr_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(qr_box_, LV_OBJ_FLAG_CLICKABLE);

    connect_text_ = plain_label(connect_, col_font, 0xFFFFFF);
    lv_obj_set_pos(connect_text_, col_x, y);
    lv_obj_set_width(connect_text_, col_w);
    lv_label_set_text(connect_text_, kConnectText);
    lv_obj_update_layout(connect_text_);
    y += lv_obj_get_height(connect_text_) + sm;

    connect_addr_ = plain_label(connect_, caption, kMuted);
    lv_obj_set_pos(connect_addr_, col_x, y);
    lv_obj_set_width(connect_addr_, col_w);
    lv_label_set_text(connect_addr_, view_.server_url.c_str());
    lv_obj_update_layout(connect_addr_);
    y += std::max(lv_obj_get_height(connect_addr_), lv_font_get_line_height(caption)) + sm;

    connect_status_ = plain_label(connect_, col_font, 0xFFFFFF);
    lv_obj_set_pos(connect_status_, col_x, y);
    lv_obj_set_size(connect_status_, col_w, std::max(1, col_bottom - y));

    /* the Server address row: one Edit row of the toolkit's list (value and chevron), the lists' column */
    cp0_ui_list_layout_t ll{};
    cp0_ui_list_layout(m, CP0_UI_LIST_COMFORTABLE, layout_.roller_x, body_y + row_y, layout_.roller_w, row_h, &ll);
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int, std::intptr_t id) { post([this, id] { list_activate(id, true, LV_KEY_ENTER, -1); }); };
    ev.back = [this] { post([this] { back(); }); };
    list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, std::move(ev));
    list_x_ = ll.x;
    list_w_ = ll.w;
    focus(list_->focus_object());
}

void HaApp::build_server_edit()
{
    lv_obj_t *col = page_->body();
    cp0::ui::TextFieldSpec spec;
    spec.label = "Server address";
    spec.text = view_.server_display;
    spec.placeholder = ha::kDefaultServer;
    spec.max_bytes = 200;
    cp0::ui::TextFieldEvents ev;
    ev.submit = [this](const std::string &t) { post([this, t] { submit_server(t); }); };
    ev.escape = [this] { post([this] { show(Screen::Connect); }); };
    ev.back = [this] { post([this] { back(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(col, layout_, spec, std::move(ev));
    field_hint_ = plain_label(col, cp0_ui_font(CP0_TEXT_CAPTION), kMuted);
    lv_obj_set_width(field_hint_, layout_.roller_w);
    lv_label_set_text(field_hint_, "homeassistant.local:8123, or Home Assistant's IP address with its port");
    focus(field_->focus_object());
    post([this] {
        if (field_ && !field_->editing()) field_->begin_edit();
    });
}

void HaApp::submit_server(const std::string &text)
{
    ha::Server s;
    std::string why;
    if (!ha::parse_server(text, s, &why)) {
        if (field_hint_) {
            lv_obj_set_style_text_color(field_hint_, lv_color_hex(CP0_UI_LIST_BAD_RGB), 0);
            lv_label_set_text(field_hint_, why.c_str());
        }
        if (field_ && !field_->editing()) field_->begin_edit();
        return;
    }
    backend_->send(ha::Command::of(ha::Command::Kind::SetServer, text));
    show(Screen::Connect);
}

/* The toolkit's slider (an 8 px bar inside the 7 mm row) is a picture of the level here: the row is the touch
 * target (a tap on the highlighted row left or right of the knob steps it by 10 %), Left / Right step it from the
 * keyboard. Not clickable, the bar is no undersized control of its own. */
static lv_obj_t *disarm_sliders(lv_obj_t *obj)
{
    if (!obj) return nullptr;
    lv_obj_t *found = nullptr;
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        found = obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (lv_obj_t *s = disarm_sliders(lv_obj_get_child(obj, static_cast<int32_t>(i)))) found = s;
    return found;
}

void HaApp::build_entity()
{
    cp0::ui::RollerEvents ev;
    ev.activate = [this](int row) {
        int x = -1;
        if (!key_activation_) {
            lv_indev_t *in = lv_indev_active();
            if (in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
                lv_point_t p;
                lv_indev_get_point(in, &p);
                x = p.x;
            }
        }
        post([this, row, x] {
            roller_tap_x_ = x;
            roller_activate(row);
            roller_tap_x_ = -1;
        });
    };
    ev.slide = [this](int row, int value) { roller_slide(row, value); };
    ev.back = [this] { post([this] { back(); }); };
    roller_ = std::make_unique<cp0::ui::Roller>(page_->root(), layout_, std::move(ev));
    focus(roller_->focus_object());
}

/* --------------------------------------------------------------------------------------------- data */

void HaApp::tick()
{
    backend_->poll();
    const std::uint32_t now = lv_tick_get();
    if (message_until_ && now >= message_until_) {
        message_until_ = 0;
        message_.clear();
        if (screen_kind_ == Screen::Connect) refresh_connect();
        update_caption();
    }
    if (screen_kind_ == Screen::Connect && view_.signed_in) {
        if (view_.paired_now && !named_at_) named_at_ = now;
        if (!connected_at_) connected_at_ = now;
        if ((named_at_ && now - named_at_ >= kNamedMs) || now - connected_at_ >= kSignedInMaxMs) {
            named_at_ = connected_at_ = 0;
            show(Screen::Home);
            return;
        }
    } else if (screen_kind_ != Screen::Connect) {
        named_at_ = connected_at_ = 0;
    }
    refresh(false);
}

void HaApp::refresh(bool force)
{
    const std::uint64_t v = backend_->version();
    if (!force && v == view_version_ && !dirty_) return;
    const std::uint32_t now = lv_tick_get();
    if (!force && now - last_rebuild_ < kRebuildMs) {
        dirty_ = true;
        return;
    }
    view_ = backend_->snapshot();
    view_version_ = v;
    dirty_ = false;
    last_rebuild_ = now;
    if (view_.message_seq != message_seen_) {
        message_seen_ = view_.message_seq;
        message_ = view_.message;
        message_error_ = view_.message_error;
        message_until_ = now + kMessageMs;
    }
    if (!view_.signed_in && screen_kind_ != Screen::Connect && screen_kind_ != Screen::ServerEdit) {
        show(Screen::Connect);
        return;
    }
    switch (screen_kind_) {
    case Screen::Connect: refresh_connect(); break;
    case Screen::Home:
    case Screen::Room:
    case Screen::Settings: refresh_list(); break;
    case Screen::Entity: refresh_entity(false); break;
    default: break;
    }
    set_title();
    update_caption();
}

void HaApp::update_caption()
{
    if (screen_kind_ == Screen::Connect || screen_kind_ == Screen::ServerEdit || screen_kind_ == Screen::None) {
        page_->set_caption("", false);
        return;
    }
    if (message_until_ && !message_.empty()) {
        page_->set_caption(message_, message_error_ ? cp0::ui::CaptionTone::Error : cp0::ui::CaptionTone::Normal);
        return;
    }
    std::string text;
    cp0::ui::CaptionTone tone = cp0::ui::CaptionTone::Normal;
    switch (view_.conn) {
    case ha::Conn::Connected: text = "Connected"; break;
    case ha::Conn::Connecting: text = "Connecting..."; tone = cp0::ui::CaptionTone::Warn; break;
    case ha::Conn::Reconnecting: text = "Reconnecting..."; tone = cp0::ui::CaptionTone::Warn; break;
    case ha::Conn::Offline: text = "Offline"; tone = cp0::ui::CaptionTone::Error; break;
    case ha::Conn::SignedOut: text = "Not signed in"; tone = cp0::ui::CaptionTone::Warn; break;
    }
    if (view_.conn != ha::Conn::Connected && view_.last_update > 0)
        text += "  last update " + ha::clock_hhmm(view_.last_update); /* two spaces: the time is kept whole */
    page_->set_caption(text, tone);
}

std::intptr_t HaApp::add_target(Target t)
{
    targets_.push_back(std::move(t));
    return static_cast<std::intptr_t>(targets_.size());
}

ListRow HaApp::entity_row(const ha::EntityRow &e)
{
    RowAffordance aff = RowAffordance::Info;
    switch (e.action) {
    case ha::RowAction::Toggle:
    case ha::RowAction::Run:
    case ha::RowAction::Confirm:
        aff = (e.running || e.pending) && !e.muted ? RowAffordance::ActionRunning : RowAffordance::Action;
        break;
    case ha::RowAction::Open: aff = RowAffordance::Open; break;
    case ha::RowAction::Info: aff = RowAffordance::Info; break;
    }
    Target t;
    t.kind = Target::Kind::Entity;
    t.id = e.id;
    t.action = e.action;
    return ListRow::item(e.name, e.value, e.muted ? ValueTone::Muted : ValueTone::Normal, add_target(t), aff);
}

std::vector<ListRow> HaApp::home_rows()
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("Favourites"));
    if (view_.favourites.empty())
        rows.push_back(ListRow::item("No favourites yet", "", ValueTone::Muted, add_target(Target()),
                                     RowAffordance::Info));
    for (const ha::EntityRow &e : view_.favourites) rows.push_back(entity_row(e));
    rows.push_back(ListRow::section("Rooms"));
    if (view_.rooms.empty()) {
        const bool waiting = !view_.registry_loaded;
        rows.push_back(ListRow::item(waiting ? "Not loaded yet" : "No rooms", "",
                                     ValueTone::Muted, add_target(Target()), RowAffordance::Info));
    }
    for (const ha::RoomRow &r : view_.rooms) {
        Target t;
        t.kind = Target::Kind::Room;
        t.id = r.id;
        rows.push_back(ListRow::item(r.name, std::to_string(r.count), ValueTone::Normal, add_target(t),
                                     RowAffordance::Open));
    }
    Target s;
    s.kind = Target::Kind::Settings;
    rows.push_back(ListRow::item("Settings", "", ValueTone::Normal, add_target(s), RowAffordance::Open));
    return rows;
}

std::vector<ListRow> HaApp::room_rows()
{
    std::vector<ListRow> rows;
    if (!filter_.empty()) rows.push_back(ListRow::section("Filter: " + filter_));
    if (!view_.room_shown || view_.room_id != room_id_) { /* the backend has not taken the new room yet */
        rows.push_back(ListRow::item("Loading...", "", ValueTone::Muted, add_target(Target()), RowAffordance::Info));
        return rows;
    }
    int shown = 0;
    for (const ha::EntityRow &e : view_.room) {
        if (!filter_.empty() && !contains_ci(e.name, filter_) && !contains_ci(e.id, filter_)) continue;
        rows.push_back(entity_row(e));
        ++shown;
    }
    if (shown == 0) {
        std::string why = !filter_.empty() ? "Nothing matches" : (view_.registry_loaded ? "Nothing here" : "Not loaded yet");
        rows.push_back(ListRow::item(why, "", ValueTone::Muted, add_target(Target()), RowAffordance::Info));
    }
    return rows;
}

std::vector<ListRow> HaApp::settings_rows()
{
    std::vector<ListRow> rows;
    /* "Server": the address with its port when it fits beside the label, else the host, else the host with "..." */
    const std::string server = list_ ? fit_value(list_->layout(), "Server", {view_.server_display, view_.server_host},
                                                 CP0_UI_LIST_AFFORDANCE_INFO)
                                     : view_.server_display;
    rows.push_back(ListRow::item("Server", server, ValueTone::Normal, add_target(Target()),
                                 RowAffordance::Info));
    rows.push_back(ListRow::item("Signed in as", view_.user.empty() ? std::string("-") : view_.user, ValueTone::Normal,
                                 add_target(Target()), RowAffordance::Info));
    Target t;
    t.kind = Target::Kind::SignOut;
    rows.push_back(ListRow::item("Sign out", view_.signing_out ? "..." : "", ValueTone::Normal, add_target(t),
                                 view_.signing_out ? RowAffordance::ActionRunning : RowAffordance::Action));
    return rows;
}

void HaApp::refresh_list()
{
    if (!list_) return;
    targets_.clear();
    std::vector<ListRow> rows;
    switch (screen_kind_) {
    case Screen::Home: rows = home_rows(); break;
    case Screen::Room: rows = room_rows(); break;
    case Screen::Settings: rows = settings_rows(); break;
    default: return;
    }
    if (list_->count() != static_cast<int>(rows.size())) {
        const int sel = std::max(0, list_->selected());
        list_->reset_rows(rows, std::min(sel, static_cast<int>(rows.size()) - 1));
    } else {
        list_->set_rows(rows);
    }
}

void HaApp::refresh_connect()
{
    if (!connect_) return;
    lv_label_set_text(connect_addr_, view_.server_url.empty() ? "-" : view_.server_url.c_str());
    targets_.clear();
    Target t;
    t.kind = Target::Kind::ServerAddress;
    /* "Server" and the host (the address line above has the whole URL), "..." only for a host too long to fit */
    const std::string host =
        list_ ? fit_value(list_->layout(), "Server", {view_.server_host}, CP0_UI_LIST_AFFORDANCE_EDIT) : view_.server_host;
    std::vector<ListRow> rows{ListRow::item("Server", host, ValueTone::Normal, add_target(t),
                                            RowAffordance::Edit)};
    if (list_) {
        if (list_->count() == 0) list_->reset_rows(rows, 0);
        else list_->set_rows(rows);
    }
    std::string status = view_.pair_status;
    int rgb = 0xFFFFFF;
    if (view_.pair_tone == ha::Tone::Warn) rgb = CP0_UI_LIST_WARN_RGB;
    if (view_.pair_tone == ha::Tone::Error) rgb = CP0_UI_LIST_BAD_RGB;
    if (view_.paired_now) rgb = CP0_UI_LIST_OK_RGB;
    if (message_until_ && !message_.empty()) {
        status = message_;
        rgb = message_error_ ? CP0_UI_LIST_BAD_RGB : 0xFFFFFF;
    }
    lv_obj_set_style_text_color(connect_status_, lv_color_hex(static_cast<uint32_t>(rgb)), 0);
    lv_label_set_text(connect_status_, status.c_str());
    if (view_.qr_url != qr_shown_) render_qr(view_.qr_url, qr_box_px_);
}

void HaApp::render_qr(const std::string &url, int box)
{
    qr_shown_ = url;
    if (qr_img_) {
        lv_obj_delete(qr_img_);
        qr_img_ = nullptr;
        lv_image_cache_drop(&qr_dsc_);
    }
    if (url.empty() || !qr_box_) return;
    const ha::QrMatrix m = ha::qr_encode(url);
    if (m.size == 0) return;
    const int px = ha::qr_module_px(m.size, kQuiet, box);
    const int side = px * (m.size + 2 * kQuiet);
    qr_pixels_.assign(static_cast<std::size_t>(side) * side * 2, 0xFF); /* white RGB565 */
    for (int y = 0; y < m.size; ++y)
        for (int x = 0; x < m.size; ++x) {
            if (!m.at(x, y)) continue;
            for (int dy = 0; dy < px; ++dy) {
                const int row = (y + kQuiet) * px + dy;
                std::uint8_t *p = &qr_pixels_[(static_cast<std::size_t>(row) * side + (x + kQuiet) * px) * 2];
                std::memset(p, 0, static_cast<std::size_t>(px) * 2);
            }
        }
    std::memset(&qr_dsc_, 0, sizeof qr_dsc_);
    qr_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    qr_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    qr_dsc_.header.w = static_cast<uint32_t>(side);
    qr_dsc_.header.h = static_cast<uint32_t>(side);
    qr_dsc_.header.stride = static_cast<uint32_t>(side * 2);
    qr_dsc_.data_size = static_cast<uint32_t>(qr_pixels_.size());
    qr_dsc_.data = qr_pixels_.data();
    qr_img_ = lv_image_create(qr_box_);
    lv_image_set_src(qr_img_, &qr_dsc_);
    lv_obj_center(qr_img_);
    lv_obj_remove_flag(qr_img_, LV_OBJ_FLAG_CLICKABLE);
}

void HaApp::refresh_entity(bool rebuild)
{
    if (!roller_) return;
    const ha::Detail &d = view_.detail;
    std::vector<cp0::ui::RowSpec> specs;
    std::vector<RollerRow> kinds;
    auto info = [&](const std::string &k, const std::string &v, RollerRow kind) {
        cp0::ui::RowSpec r;
        r.kind = cp0::ui::RowKind::Info;
        r.label = k;
        r.value = v;
        specs.push_back(r);
        kinds.push_back(kind);
    };
    auto nav = [&](const std::string &label, RollerRow kind) {
        cp0::ui::RowSpec r;
        r.kind = cp0::ui::RowKind::Nav;
        r.label = label;
        specs.push_back(r);
        kinds.push_back(kind);
    };
    std::string state = d.value.empty() ? std::string("-") : d.value;
    if (d.id != entity_id_) state = "..."; /* the backend has not taken the new page yet */
    else if (!d.known && view_.registry_loaded) state = "Not in Home Assistant";
    info("State", state, RollerRow::State);
    for (const auto &kv : d.info) info(kv.first, kv.second, RollerRow::Info);
    if (d.dimmable) {
        cp0::ui::RowSpec r;
        r.kind = cp0::ui::RowKind::Slider;
        r.label = "Brightness";
        const int pct = (slide_timer_ || lv_tick_get() - slid_at_ < kSlideHoldMs) && slide_value_ >= 0 ? slide_value_
                                                                                                         : d.brightness_pct;
        r.value = std::to_string(pct) + " %";
        r.slider_min = 0;
        r.slider_max = 100;
        r.slider_step = 10;
        r.slider_value = pct;
        specs.push_back(r);
        kinds.push_back(RollerRow::Slider);
    }
    if (d.domain == ha::Domain::Cover) {
        nav("Open", RollerRow::Open);
        nav("Close", RollerRow::Close);
    }
    nav(d.favourite ? "Remove from favourites" : "Add to favourites", RollerRow::Favourite);

    if (rebuild || kinds != roller_kinds_) {
        int sel = rebuild ? 0 : std::min(roller_->selected(), static_cast<int>(specs.size()) - 1);
        if (rebuild) { /* start on the first row that does something */
            for (std::size_t i = 0; i < kinds.size(); ++i)
                if (kinds[i] != RollerRow::State && kinds[i] != RollerRow::Info) {
                    sel = static_cast<int>(i);
                    break;
                }
        }
        roller_->set_rows(specs, std::max(0, sel), false);
    } else {
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const cp0::ui::RowSpec &a = specs[i], &b = roller_specs_[i];
            if (a.label == b.label && a.value == b.value && a.slider_value == b.slider_value) continue;
            roller_->update_row(static_cast<int>(i), a);
        }
    }
    roller_kinds_ = kinds;
    roller_specs_ = specs;
    slider_obj_ = disarm_sliders(roller_->focus_object());
}

/* --------------------------------------------------------------------------------------------- input */

void HaApp::keyboard_cb(lv_event_t *event)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(event));
    const auto *item = static_cast<const key_item *>(lv_event_get_param(event));
    if (self) self->on_key(item);
}

void HaApp::on_key(const key_item *item)
{
    if (!item || item->key_state == KBD_KEY_RELEASED) return;
    if (field_ && field_->editing()) return; /* the field reads its own keys */
    const std::uint32_t code = item->key_code;
    std::uint32_t key = lv_key_of(code);
    if (screen_kind_ == Screen::Room && !dialog_) {
        if (key == LV_KEY_BACKSPACE) {
            if (!filter_.empty()) {
                pop_utf8(filter_);
                targets_.clear();
                list_->reset_rows(room_rows(), 0);
            }
            return;
        }
        const unsigned char c0 = static_cast<unsigned char>(item->utf8[0]);
        if (!key && c0 >= 0x20 && c0 != 0x7F && !(item->mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO))) {
            if (filter_.size() < 40) filter_ += item->utf8;
            targets_.clear();
            list_->reset_rows(room_rows(), 0);
            return;
        }
    } else if (!key && item->semantic_key) {
        key = lv_key_of(item->semantic_key); /* the navigation context's letters for the arrows, as other apps */
    }
    if (!key || key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) return;
    if (screen_kind_ == Screen::ServerEdit && field_ && !dialog_) {
        if (key == LV_KEY_ENTER) {
            post([this] {
                if (field_ && !field_->editing()) field_->begin_edit();
            });
            return;
        }
        if (key == LV_KEY_ESC) {
            post([this] { back(); });
            return;
        }
    }
    forward_key(key);
}

void HaApp::forward_key(std::uint32_t lv_key)
{
    lv_obj_t *obj = dialog_ ? dialog_->focus_object() : focus_obj_;
    if (!obj || !lv_obj_is_valid(obj)) return;
    key_activation_ = true;
    last_key_ = lv_key;
    std::uint32_t k = lv_key;
    lv_obj_send_event(obj, LV_EVENT_KEY, &k);
    key_activation_ = false;
}

const ha::EntityRow *HaApp::find_row(const std::string &id) const
{
    for (const ha::EntityRow &e : view_.room)
        if (e.id == id) return &e;
    for (const ha::EntityRow &e : view_.favourites)
        if (e.id == id) return &e;
    return nullptr;
}

void HaApp::list_activate(std::intptr_t id, bool by_key, std::uint32_t key, int x)
{
    if (id <= 0 || id > static_cast<std::intptr_t>(targets_.size())) return;
    const Target t = targets_[static_cast<std::size_t>(id - 1)];
    switch (t.kind) {
    case Target::Kind::Entity: {
        const bool info_side = by_key ? key == LV_KEY_RIGHT : (x >= 0 && x >= list_x_ + list_w_ * kInfoSidePct / 100);
        if (t.action == ha::RowAction::Info || t.action == ha::RowAction::Open || info_side) {
            open_entity(t.id);
            return;
        }
        if (const ha::EntityRow *row = find_row(t.id)) act_on_entity(*row);
        return;
    }
    case Target::Kind::Room:
        room_id_ = t.id;
        room_shown_ = true;
        filter_.clear();
        show(Screen::Room);
        return;
    case Target::Kind::Settings: show(Screen::Settings); return;
    case Target::Kind::ServerAddress: show(Screen::ServerEdit); return;
    case Target::Kind::SignOut:
        if (view_.signing_out) return;
        confirm("Sign out of Home Assistant?", [this] { backend_->send(ha::Command::of(ha::Command::Kind::SignOut)); });
        return;
    case Target::Kind::None: return;
    }
}

void HaApp::act_on_entity(const ha::EntityRow &row)
{
    switch (row.action) {
    case ha::RowAction::Toggle: backend_->send(ha::Command::of(ha::Command::Kind::Toggle, row.id)); break;
    case ha::RowAction::Run: backend_->send(ha::Command::of(ha::Command::Kind::Run, row.id)); break;
    case ha::RowAction::Confirm: {
        const std::string service = row.value == "Locked" ? "unlock" : "lock";
        const std::string id = row.id;
        confirm(ha::confirm_question(service, row.name), [this, id, service] {
            backend_->send(ha::Command::of(ha::Command::Kind::Service, id, service));
        });
        break;
    }
    default: open_entity(row.id); return;
    }
    refresh(true);
}

void HaApp::open_entity(const std::string &id)
{
    entity_from_ = screen_kind_ == Screen::Room ? Screen::Room : Screen::Home;
    entity_id_ = id;
    show(Screen::Entity);
}

void HaApp::roller_activate(int row)
{
    if (row < 0 || row >= static_cast<int>(roller_kinds_.size())) return;
    const ha::Detail &d = view_.detail;
    switch (roller_kinds_[static_cast<std::size_t>(row)]) {
    case RollerRow::Open:
    case RollerRow::Close: {
        const std::string service = roller_kinds_[static_cast<std::size_t>(row)] == RollerRow::Open ? "open_cover" : "close_cover";
        const std::string id = entity_id_;
        auto run = [this, id, service] { backend_->send(ha::Command::of(ha::Command::Kind::Service, id, service)); };
        if (d.confirm) confirm(ha::confirm_question(service, d.name), run);
        else run();
        break;
    }
    case RollerRow::Favourite: backend_->send(ha::Command::of(ha::Command::Kind::ToggleFavourite, entity_id_)); break;
    case RollerRow::Slider: {
        /* a tap on the highlighted slider row: 10 % towards the side of the knob it landed on (keys: Left / Right) */
        if (roller_tap_x_ < 0 || !slider_obj_ || !lv_obj_is_valid(slider_obj_)) return;
        const cp0::ui::RowSpec &spec = roller_specs_[static_cast<std::size_t>(row)];
        lv_area_t a;
        lv_obj_get_coords(slider_obj_, &a);
        const int knob = a.x1 + (a.x2 - a.x1) * (spec.slider_value - spec.slider_min) /
                                    std::max(1, spec.slider_max - spec.slider_min);
        const int step = roller_tap_x_ < knob ? -spec.slider_step : spec.slider_step;
        roller_slide(row, std::clamp(spec.slider_value + step, spec.slider_min, spec.slider_max));
        return;
    }
    default: return;
    }
    refresh(true);
}

void HaApp::roller_slide(int row, int value)
{
    if (row < 0 || row >= static_cast<int>(roller_kinds_.size()) ||
        roller_kinds_[static_cast<std::size_t>(row)] != RollerRow::Slider)
        return;
    slide_value_ = std::clamp(value, 0, 100);
    slid_at_ = lv_tick_get();
    post([this, row] { /* the value text follows, outside the slider's own event */
        if (!roller_ || row >= static_cast<int>(roller_specs_.size()) || slide_value_ < 0) return;
        roller_specs_[static_cast<std::size_t>(row)].slider_value = slide_value_;
        roller_specs_[static_cast<std::size_t>(row)].value = std::to_string(slide_value_) + " %";
        roller_->update_row(row, roller_specs_[static_cast<std::size_t>(row)]);
        slider_obj_ = disarm_sliders(roller_->focus_object());
    });
    if (slide_timer_) lv_timer_delete(slide_timer_);
    slide_timer_ = lv_timer_create(
        [](lv_timer_t *t) {
            auto *self = static_cast<HaApp *>(lv_timer_get_user_data(t));
            self->slide_timer_ = nullptr;
            lv_timer_delete(t);
            if (!self->entity_id_.empty())
                self->backend_->send(ha::Command::of(ha::Command::Kind::SetBrightness, self->entity_id_, "",
                                                     self->slide_value_));
        },
        kSlideDebounceMs, this);
}

void HaApp::confirm(const std::string &question, std::function<void()> yes)
{
    close_dialog();
    cp0::ui::DialogSpec spec;
    spec.title = question;
    spec.page = page_.get();
    cp0::ui::DialogButton ok;
    ok.text = "Yes";
    ok.role = cp0::ui::ButtonRole::Primary;
    ok.action = [this, yes] {
        post([this, yes] {
            close_dialog();
            yes();
            refresh(true);
        });
    };
    cp0::ui::DialogButton cancel;
    cancel.text = "Cancel";
    cancel.role = cp0::ui::ButtonRole::Cancel;
    cancel.action = [this] { post([this] { close_dialog(); }); };
    spec.buttons = {ok, cancel};
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    lv_group_t *group = cdk::input_group();
    lv_obj_t *box = dialog_->focus_object();
    if (group && box) {
        if (lv_obj_get_group(box) != group) lv_group_add_obj(group, box);
        lv_group_focus_obj(box);
    }
}

void HaApp::close_dialog()
{
    if (!dialog_) return;
    dialog_.reset();
    if (focus_obj_ && lv_obj_is_valid(focus_obj_)) focus(focus_obj_);
}

void HaApp::back()
{
    if (dialog_) {
        close_dialog();
        return;
    }
    switch (screen_kind_) {
    case Screen::Connect:
    case Screen::Home:
    case Screen::None: cdk::request_quit(0); return;
    case Screen::ServerEdit: show(Screen::Connect); return;
    case Screen::Room:
        if (!filter_.empty()) {
            filter_.clear();
            targets_.clear();
            list_->reset_rows(room_rows(), 0);
            return;
        }
        room_shown_ = false;
        show(Screen::Home);
        return;
    case Screen::Entity:
        entity_id_.clear();
        slide_value_ = -1;
        show(entity_from_ == Screen::Room && room_shown_ ? Screen::Room : Screen::Home);
        return;
    case Screen::Settings: show(Screen::Home); return;
    }
}

} // namespace haui
