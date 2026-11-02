/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app: the pages (see ha_app.hpp).
 */
#include "ha_app.hpp"
#include "ha_ui.hpp"

#include "app.hpp"
#include "format.hpp"
#include "mdi.hpp"
#include "qr.hpp"
#include "runner.hpp"
#include "sim.hpp"
#include "store.hpp"
#include "text.hpp"
#include "tiles.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "keyboard_input.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: a new QR code in the same buffer */

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace haui {

using cp0::ui::ListRow;
using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;

namespace {

constexpr int kTimerMs = 200;
constexpr std::uint32_t kRebuildMs = 500;   /* the pages at most twice a second */
constexpr std::uint32_t kMessageMs = 4000;
constexpr std::uint32_t kNamedMs = 1500;    /* "Connected as <user>" before the dashboard */
constexpr std::uint32_t kSignedInMaxMs = 10000;
constexpr std::uint32_t kSlideDebounceMs = 300;
constexpr std::uint32_t kSlideHoldMs = 1500; /* the slider is not moved by the state while the user slides */
constexpr int kInfoSidePct = 60;             /* a tap right of this share of the row opens the entity page */
constexpr int kQuiet = 2;                    /* quiet zone of the QR code, in modules (on a white box) */

const char *kConnectText = "Scan with your phone to connect to Home Assistant";
const char *kChevron = "\xE2\x80\xBA"; /* U+203A, the lists' chevron */

std::string data_dir()
{
#ifdef CDK_APP_DIR
    return std::string(CDK_APP_DIR) + "/root/usr/share/APPLaunch/share/home-assistant";
#else
    if (const char *d = std::getenv("HA_DATA_DIR"))
        if (*d) return d;
    return "/usr/share/APPLaunch/share/home-assistant";
#endif
}

std::string mdi_font_path()
{
    return data_dir() + "/materialdesignicons-webfont.ttf";
}

std::unique_ptr<ha::Backend> make_backend()
{
    ha::set_mdi_path(data_dir() + "/mdi-names.bin");
    std::fprintf(stderr, "[home-assistant] icons: %zu names (MDI %s)\n", ha::mdi().size(), ha::mdi().version().c_str());
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

/* The line of the connection: a dot and a word, the launcher's status colours (not the tiles' HA colours). */
void conn_word(const ha::View &v, std::string &word, std::uint32_t &rgb)
{
    switch (v.conn) {
    case ha::Conn::Connected: word = "Online"; rgb = CP0_UI_LIST_OK_RGB; break;
    case ha::Conn::Connecting: word = "Connecting..."; rgb = CP0_UI_LIST_WARN_RGB; break;
    case ha::Conn::Reconnecting: word = "Reconnecting..."; rgb = CP0_UI_LIST_WARN_RGB; break;
    case ha::Conn::Offline:
        word = "Offline";
        if (v.last_update > 0) word += " " + ha::clock_hhmm(v.last_update);
        rgb = CP0_UI_LIST_BAD_RGB;
        break;
    case ha::Conn::SignedOut: word = "Signed out"; rgb = CP0_UI_LIST_WARN_RGB; break;
    }
}

} // namespace

HaApp::HaApp(lv_obj_t *screen) : screen_(screen)
{
    ha::set_mdi_path(data_dir() + "/mdi-names.bin"); /* before the connection thread starts (make_backend) */
    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen, layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());

    /* the tiles' sizes and faces from the layout service: one ROW high, the name (BODY) and the state word (CAPTION)
     * on two lines inside it (both a size smaller while they do not fit), the icon circle 5.5 mm, its glyph 60 % */
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    row_h_ = m->tok[CP0_TOK_ROW_H];
    gap_ = m->tok[CP0_TOK_SPACE_S];
    margin_ = m->tok[CP0_TOK_SPACE_M];
    int name_px = m->text_px[CP0_TEXT_BODY], word_px = m->text_px[CP0_TEXT_CAPTION];
    /* a name of 12 wide characters ("Reading lamp") whole in a tile of this screen (study 053 section 6.2: the tile
     * keeps room for 10-12 characters), never smaller than the deck's CAPTION in millimetres (1.6 mm, the guidelines'
     * floor); the state word never larger than the name */
    {
        const int cols = ha::grid_columns(layout_.w, m->ppmm_x100, margin_, gap_);
        const int tile_w = (layout_.w - 2 * margin_ - (cols - 1) * gap_) / cols;
        const int pad = std::max(2, gap_ / 2);
        const int floor_px = std::max(10, cp0_ui_mm_px(m->ppmm_x100, 16, 10) & ~1);
        const lv_font_t *cap = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(word_px));
        const int zone = tile_w - row_h_ - 3 * pad - text_w(kChevron, cap);
        while (name_px > floor_px &&
               text_w("Reading lamp", cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(name_px))) > zone)
            name_px -= 2;
        word_px = std::max(floor_px, std::min(word_px, name_px - 2));
    }
    const int text_room = row_h_ - 2 * std::max(2, gap_ / 2);
    for (;;) {
        name_font_ = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(name_px));
        word_font_ = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(word_px));
        if (line_h(name_font_) + line_h(word_font_) <= text_room || name_px <= 12) break;
        name_px -= 2;
        if (word_px > 10) word_px -= 2;
    }
    tab_font_ = name_font_;
    tab_bold_ = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(name_px));
    circle_ = std::min(cp0_ui_mm_px(m->ppmm_x100, 55, 24), row_h_ - 4);
    const std::string mdi = mdi_font_path();
    const bool have_mdi = ::access(mdi.c_str(), R_OK) == 0 && ha::mdi().loaded();
    auto even = [](int px) { return std::max(8, px & ~1); };
    glyph_font_ = have_mdi ? cp0_fonts().get(mdi.c_str(), static_cast<uint16_t>(even(circle_ * 60 / 100))) : nullptr;
    hero_glyph_font_ = have_mdi ? cp0_fonts().get(mdi.c_str(), static_cast<uint16_t>(even(row_h_ * 55 / 100))) : nullptr;
    lv_font_glyph_dsc_t g{};
    chevron_text_ = name_font_ && lv_font_get_glyph_dsc(name_font_, &g, CP0_UI_LIST_CHEVRON_CODEPOINT, 0) ? kChevron : ">";
    /* the stored dashboard's cards (0.3.0): a sensor's or the weather's value 3 mm high, the cards' inner padding the
     * small space token, the rows' switch 2.5 mm high */
    big_font_ = cp0_fonts().get("Montserrat-Medium.ttf",
                                static_cast<uint16_t>(even(std::max(cp0_ui_mm_px(m->ppmm_x100, 30, 20), name_px + 6))));
    cpad_ = std::max(4, gap_);
    sw_h_ = even(std::max(16, std::min(row_h_ * 36 / 100, cp0_ui_mm_px(m->ppmm_x100, 25, 16))));
    sw_w_ = sw_h_ * 9 / 5;

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
    show(view_.signed_in ? Screen::Dashboard : Screen::Connect);
    timer_ = lv_timer_create([](lv_timer_t *t) { static_cast<HaApp *>(lv_timer_get_user_data(t))->tick(); },
                             kTimerMs, this);
    std::fprintf(stderr, "[home-assistant] %s, canvas %dx%d, tiles: row %d px, %s\n", kVersion, layout_.w, layout_.h,
                 row_h_, have_mdi ? "MDI font" : "no icon font (initials)");
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
    field_.reset();
    field_hint_ = nullptr;
    page_->clear_body();
    if (connect_) lv_obj_delete(connect_);
    connect_ = qr_box_ = qr_img_ = connect_text_ = connect_addr_ = connect_status_ = nullptr;
    qr_shown_.clear();
    clear_content();
    if (bar_) lv_obj_delete(bar_);
    bar_ = tabs_box_ = conn_dot_ = conn_word_ = nullptr;
    tab_btns_.clear();
    bar_conn_.clear();
    if (hero_) lv_obj_delete(hero_);
    hero_ = hero_circle_ = hero_glyph_ = hero_word_ = hero_meta_ = nullptr;
    hero_shown_.clear();
    if (dbox_) lv_obj_delete(dbox_);
    dbox_ = nullptr;
    drows_.clear();
    dsel_ = -1;
    targets_.clear();
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
    case Screen::Dashboard:
        entity_id_.clear();
        build_dashboard();
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
        backend_->send(ha::Command::set_view(room_shown_, room_id_, entity_id_, dash_shown_));
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
    case Screen::Dashboard: refresh_dashboard(); break;
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
    int bx, qx, qy, col_x, col_w, y;
    if (portrait) {
        bx = std::min(w - 2 * sp, top_h * 55 / 100);
        qx = (w - bx) / 2;
        qy = sm;
        col_x = sp;
        col_w = w - 2 * sp;
        y = qy + bx + sm;
    } else {
        bx = std::min(top_h, (w - 3 * sp) / 2);
        qx = sp;
        qy = sm;
        col_x = sp + bx + sp;
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
    qr_box_px_ = bx;
    qr_box_ = lv_obj_create(connect_);
    lv_obj_remove_style_all(qr_box_);
    lv_obj_set_pos(qr_box_, qx, qy);
    lv_obj_set_size(qr_box_, bx, bx);
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

/* The entity page (study 053 section 7.6): the tile's icon, its word and "room - changed HH:MM" across the top, then the
 * rows straight under it, from the top (refresh_entity builds them). The rows' sizes, faces and colours are the
 * toolkit's list ones (cp0_ui_list_layout, COMFORTABLE: the ROW token), in the lists' column. */
void HaApp::build_entity()
{
    const int hero_h = row_h_ + gap_;
    hero_ = box(page_->root());
    lv_obj_set_pos(hero_, layout_.roller_x, layout_.body_y);
    lv_obj_set_size(hero_, layout_.roller_w, hero_h);
    const int d = row_h_ - gap_;
    hero_circle_ = box(hero_);
    lv_obj_set_size(hero_circle_, d, d);
    lv_obj_set_pos(hero_circle_, layout_.inset, (hero_h - d) / 2);
    lv_obj_set_style_radius(hero_circle_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hero_circle_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hero_circle_, lv_color_hex(ha::rgb::kCircle), 0);
    hero_glyph_ = lv_label_create(hero_circle_);
    lv_obj_set_style_text_font(hero_glyph_, hero_glyph_font_ ? hero_glyph_font_ : name_font_, 0);
    lv_obj_center(hero_glyph_);
    const int tx = layout_.inset + d + margin_;
    const int tw = std::max(1, layout_.roller_w - tx - layout_.inset);
    const int th = line_h(tab_bold_) + line_h(word_font_);
    hero_word_ = line_label(hero_, tab_bold_, ha::rgb::kText, tw);
    lv_obj_set_pos(hero_word_, tx, (hero_h - th) / 2);
    hero_meta_ = line_label(hero_, word_font_, ha::rgb::kSecondary, tw);
    lv_obj_set_pos(hero_meta_, tx, (hero_h - th) / 2 + line_h(tab_bold_));
    lv_obj_update_layout(hero_); /* the labels' widths before their texts (dots are decided then) */

    /* the rows: the toolkit list's geometry and faces, starting right under the top */
    const int y = layout_.body_y + hero_h;
    const int h = std::max(row_h_, layout_.h - y);
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, layout_.roller_x, y, layout_.roller_w, h, &dl_);
    dl_.separator_px = 1;
    dlabel_font_ = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(dl_.text_px));
    dlabel_bold_ = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(dl_.text_px));
    dvalue_font_ = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(dl_.value_px));
    dbox_ = lv_obj_create(page_->root());
    lv_obj_remove_style_all(dbox_);
    lv_obj_remove_flag(dbox_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(dbox_, dl_.x, dl_.y);
    lv_obj_set_size(dbox_, dl_.w, dl_.h);
    lv_obj_set_scroll_dir(dbox_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(dbox_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(dbox_, dl_.scrollbar_w, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(dbox_, dl_.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(dbox_, lv_color_hex(CP0_UI_LIST_SCROLLBAR_RGB), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(dbox_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_remove_flag(dbox_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    focus_obj_ = nullptr; /* the page reads its keys itself (detail_key) */
}

/* --------------------------------------------------------------------------------------------- the dashboard */

const ha::Tab *HaApp::current_tab() const
{
    if (tabs_.empty()) return nullptr;
    return &tabs_[static_cast<std::size_t>(ha::tab_index(tabs_, tab_key_))];
}

void HaApp::build_dashboard()
{
    tabs_ = view_.tabs.empty() ? ha::make_tabs({}) : view_.tabs;
    if (ha::tab_index(tabs_, tab_key_) == 0) tab_key_ = tabs_[0].key();
    content_y_ = layout_.body_y + row_h_;
    build_tab_bar();
    build_content();
}

void HaApp::build_tab_bar()
{
    if (bar_) lv_obj_delete(bar_);
    tab_btns_.clear();
    bar_conn_.clear();
    const int w = layout_.w;
    bar_ = box(page_->root());
    lv_obj_set_pos(bar_, 0, layout_.body_y);
    lv_obj_set_size(bar_, w, row_h_);

    /* the connection at the right end: a dot and a word, as wide as its longest word */
    const int dot = std::max(6, line_h(word_font_) / 2);
    int word_w = 0;
    for (const char *s : {"Online", "Connecting...", "Reconnecting...", "Offline 00:00", "Signed out"})
        word_w = std::max(word_w, text_w(s, word_font_));
    const int conn_w = dot + gap_ + word_w;
    conn_word_ = line_label(bar_, word_font_, ha::rgb::kSecondary, word_w);
    lv_obj_set_pos(conn_word_, w - margin_ - word_w, (row_h_ - line_h(word_font_)) / 2);
    conn_dot_ = box(bar_);
    lv_obj_set_size(conn_dot_, dot, dot);
    lv_obj_set_style_radius(conn_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(conn_dot_, LV_OPA_COVER, 0);
    lv_obj_set_pos(conn_dot_, w - margin_ - conn_w, (row_h_ - dot) / 2);

    /* the tabs, scrolling sideways */
    tabs_box_ = lv_obj_create(bar_);
    lv_obj_remove_style_all(tabs_box_);
    lv_obj_remove_flag(tabs_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(tabs_box_, 0, 0);
    lv_obj_set_size(tabs_box_, std::max(row_h_, w - margin_ - conn_w - gap_), row_h_);
    lv_obj_set_scroll_dir(tabs_box_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tabs_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(tabs_box_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    int x = std::max(0, margin_ - gap_);
    const int tab_max = std::max(row_h_, (w - conn_w) * 55 / 100); /* a long area name ends with dots */
    tab_icon_.assign(tabs_.size(), false);
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        /* a view of the stored dashboard: its title, or its icon when the title is missing or does not fit */
        const bool icon = tabs_[i].kind == ha::Tab::Kind::View && tabs_[i].glyph && glyph_font_ &&
                          (tabs_[i].title.empty() || text_w(tabs_[i].title, tab_bold_) + 2 * margin_ > tab_max);
        tab_icon_[i] = icon;
        const int tw = icon ? row_h_
                            : std::clamp(std::max(text_w(tabs_[i].title, tab_bold_), text_w(tabs_[i].title, tab_font_)) +
                                             2 * margin_,
                                         row_h_, tab_max);
        lv_obj_t *b = lv_obj_create(tabs_box_);
        lv_obj_remove_style_all(b);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(b, x, 0);
        lv_obj_set_size(b, tw, row_h_);
        lv_obj_set_style_radius(b, m_radius(), 0);
        lv_obj_set_user_data(b, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(b, tab_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_t *label = line_label(b, icon ? glyph_font_ : tab_font_, ha::rgb::kSecondary, 0);
        lv_label_set_text(label, icon ? ha::utf8_of(tabs_[i].glyph).c_str() : tabs_[i].title.c_str());
        if (!icon && text_w(tabs_[i].title, tab_bold_) > tw - 2 * margin_)
            lv_obj_set_size(label, tw - 2 * margin_, std::max(line_h(tab_font_), line_h(tab_bold_)));
        lv_obj_center(label);
        lv_obj_t *under = box(b); /* the active tab's underline */
        lv_obj_set_size(under, tw - 2 * gap_, std::max(2, gap_ / 3));
        lv_obj_align(under, LV_ALIGN_BOTTOM_MID, 0, -std::max(1, gap_ / 4));
        lv_obj_set_style_bg_color(under, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
        lv_obj_add_flag(under, LV_OBJ_FLAG_HIDDEN);
        tab_btns_.push_back(b);
        x += tw;
    }
    bar_focus_drawn_ = false;
    bar_styled_tab_ = -1;
    style_bar();
}

int HaApp::m_radius() const
{
    return cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S];
}

void HaApp::refresh_bar()
{
    if (!bar_) return;
    /* the areas changed (loaded, renamed, one added): the tabs again, the current one kept by its key */
    bool same = view_.tabs.size() == tabs_.size() || view_.tabs.empty();
    if (same && !view_.tabs.empty())
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            same = same && tabs_[i].key() == view_.tabs[i].key() && tabs_[i].title == view_.tabs[i].title;
    if (!same) {
        const std::string before = tab_key_;
        tabs_ = view_.tabs;
        if (ha::tab_index(tabs_, tab_key_) == 0) tab_key_ = tabs_[0].key();
        /* the stored dashboard arrived: the app opens on its first view, unless a tab was chosen meanwhile */
        if (!tab_chosen_ && tabs_[0].kind == ha::Tab::Kind::View) tab_key_ = tabs_[0].key();
        build_tab_bar(); /* styles it too */
        if (tab_key_ != before) build_content();
        return;
    }
    style_bar();
}

void HaApp::style_bar()
{
    if (!bar_) return;
    const int cur = ha::tab_index(tabs_, tab_key_);
    const bool bar_focus = focus_visible_ && zone_ == Zone::Bar;
    /* only when the active tab or the bar's focus changed: a restyle repaints the whole bar */
    const bool restyle = cur != bar_styled_tab_ || bar_focus != bar_focus_drawn_;
    bar_styled_tab_ = cur;
    bar_focus_drawn_ = bar_focus;
    for (std::size_t i = 0; restyle && i < tab_btns_.size(); ++i) {
        lv_obj_t *b = tab_btns_[i];
        lv_obj_t *label = lv_obj_get_child(b, 0);
        lv_obj_t *under = lv_obj_get_child(b, 1);
        const bool active = static_cast<int>(i) == cur;
        if (i >= tab_icon_.size() || !tab_icon_[i]) lv_obj_set_style_text_font(label, active ? tab_bold_ : tab_font_, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(active ? ha::rgb::kText : ha::rgb::kSecondary), 0);
        if (active) lv_obj_remove_flag(under, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(under, LV_OBJ_FLAG_HIDDEN);
        const bool focused = active && focus_visible_ && zone_ == Zone::Bar;
        lv_obj_set_style_bg_color(b, lv_color_hex(kHighlight), 0);
        lv_obj_set_style_bg_opa(b, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        set_focus_ring(b, focused);
    }
    std::string word;
    std::uint32_t rgb = ha::rgb::kSecondary;
    conn_word(view_, word, rgb);
    if (word != bar_conn_) {
        bar_conn_ = word;
        lv_label_set_text(conn_word_, word.c_str());
        lv_obj_set_style_bg_color(conn_dot_, lv_color_hex(rgb), 0);
    }
}

void HaApp::clear_content()
{
    if (grid_) lv_obj_delete(grid_);
    grid_ = grid_note_ = nullptr;
    tiles_.clear();
    sliders_.clear();
    rows_.clear();
    grid_ids_.clear();
    grid_dims_.clear();
    focus_drawn_ = slider_focus_drawn_ = -1;
    clear_dash();
    if (screen_kind_ == Screen::Dashboard) list_.reset();
}

void HaApp::build_content()
{
    clear_content();
    const ha::Tab *t = current_tab();
    if (!t) return;
    const int h = std::max(row_h_, layout_.h - content_y_);
    if (t->kind == ha::Tab::Kind::All) {
        backend_->send(ha::Command::set_view(false, "", ""));
        const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
        cp0_ui_list_layout_t ll{};
        cp0_ui_list_layout(m, CP0_UI_LIST_COMFORTABLE, layout_.roller_x, content_y_, layout_.roller_w, h, &ll);
        ll.separator_px = 1;
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
        list_ = std::make_unique<cp0::ui::CompactList>(page_->root(), ll, std::move(ev));
        list_x_ = ll.x;
        list_w_ = ll.w;
        focus(list_->focus_object());
        refresh_list();
        list_->select(1, false); /* the first item (row 0 is the FAVOURITES heading) */
        return;
    }
    if (t->kind == ha::Tab::Kind::View) { /* a view of the stored dashboard (ha_dash.cpp) */
        room_shown_ = false;
        backend_->send(ha::Command::set_view(false, "", "", t->view));
    } else if (t->kind == ha::Tab::Kind::Area) {
        room_id_ = t->area;
        room_shown_ = true;
        backend_->send(ha::Command::set_view(true, t->area, ""));
    } else {
        room_shown_ = false;
        backend_->send(ha::Command::set_view(false, "", ""));
    }
    focus_obj_ = nullptr;
    grid_ = lv_obj_create(page_->root());
    lv_obj_remove_style_all(grid_);
    lv_obj_set_pos(grid_, 0, content_y_);
    lv_obj_set_size(grid_, layout_.w, h);
    lv_obj_set_scroll_dir(grid_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(grid_, std::max(3, margin_ / 3), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(grid_, lv_color_hex(CP0_UI_LIST_SCROLLBAR_RGB), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(grid_, std::max(2, gap_ / 2), LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(grid_, margin_, 0);
    lv_obj_remove_flag(grid_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(grid_, grid_event_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(grid_, grid_event_cb, LV_EVENT_CLICKED, this);
    grid_cols_ = ha::grid_columns(layout_.w, cp0_ui_metrics_get()->ppmm_x100, margin_, gap_);
    tile_w_ = (layout_.w - 2 * margin_ - (grid_cols_ - 1) * gap_) / grid_cols_;
}

const std::vector<ha::EntityRow> *HaApp::grid_rows_source(bool &loading) const
{
    loading = false;
    const ha::Tab *t = current_tab();
    if (!t) return nullptr;
    if (t->kind == ha::Tab::Kind::Favourites) return &view_.favourites;
    if (t->kind == ha::Tab::Kind::Area) {
        if (!view_.room_shown || view_.room_id != t->area) { /* the backend has not taken the new area yet */
            loading = true;
            return nullptr;
        }
        return &view_.room;
    }
    return nullptr;
}

void HaApp::refresh_dashboard()
{
    if (!bar_) return;
    refresh_bar();
    const ha::Tab *t = current_tab();
    if (!t) return;
    if (t->kind == ha::Tab::Kind::All) {
        refresh_list();
        return;
    }
    if (t->kind == ha::Tab::Kind::View) {
        refresh_dash_view();
        return;
    }
    bool loading = false;
    const std::vector<ha::EntityRow> *rows = grid_rows_source(loading);
    static const std::vector<ha::EntityRow> kNone;
    refresh_grid(rows ? *rows : kNone, loading);
}

void HaApp::refresh_grid(const std::vector<ha::EntityRow> &rows, bool loading)
{
    if (!grid_) return;
    std::string note;
    if (loading) note = "Loading...";
    else if (rows.empty()) {
        const ha::Tab *t = current_tab();
        if (t && t->kind == ha::Tab::Kind::Favourites)
            note = view_.registry_loaded || view_.conn == ha::Conn::Connected
                       ? "No favourites yet. Open a device (in All, or by its name here) and add it."
                       : "Not loaded yet";
        else note = view_.registry_loaded ? "Nothing here" : "Not loaded yet";
    }
    if (!note.empty()) {
        if (!tiles_.empty() || !sliders_.empty()) {
            lv_obj_clean(grid_);
            grid_note_ = nullptr;
            tiles_.clear();
            sliders_.clear();
            rows_.clear();
            grid_ids_.clear();
            grid_dims_.clear();
            focus_drawn_ = slider_focus_drawn_ = -1;
        }
        if (!grid_note_) {
            grid_note_ = plain_label(grid_, word_font_, kMuted);
            lv_obj_set_pos(grid_note_, margin_, margin_);
            lv_obj_set_width(grid_note_, layout_.w - 2 * margin_);
            lv_obj_remove_flag(grid_note_, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_label_set_text(grid_note_, note.c_str());
        return;
    }
    std::vector<std::string> ids;
    std::vector<bool> dims;
    for (const ha::EntityRow &r : rows) {
        ids.push_back(r.id);
        dims.push_back(r.dimmable);
    }
    if (ids != grid_ids_ || dims != grid_dims_ || grid_note_ != nullptr) {
        build_grid(rows);
        return;
    }
    const std::uint32_t now = lv_tick_get();
    for (std::size_t i = 0; i < tiles_.size() && i < rows.size(); ++i) {
        draw_tile(tiles_[i], rows[i], false);
        if (tiles_[i].slider >= 0) {
            SliderUi &s = sliders_[static_cast<std::size_t>(tiles_[i].slider)];
            const bool holding = s.id == slide_id_ && slide_value_ >= 0 && (slide_timer_ || dragging_ || now - slid_at_ < kSlideHoldMs);
            draw_slider(s, holding ? slide_value_ : rows[i].brightness_pct, rows[i].on && !rows[i].muted, false);
        }
    }
}

void HaApp::build_grid(const std::vector<ha::EntityRow> &rows)
{
    lv_obj_clean(grid_);
    grid_note_ = nullptr;
    tiles_.clear();
    sliders_.clear();
    focus_drawn_ = slider_focus_drawn_ = -1;
    grid_ids_.clear();
    grid_dims_.clear();
    for (const ha::EntityRow &r : rows) {
        grid_ids_.push_back(r.id);
        grid_dims_.push_back(r.dimmable);
    }
    rows_ = ha::grid_rows(grid_dims_, grid_cols_);
    tiles_.resize(rows.size());
    const int radius = m_radius();
    const int text_w0 = tile_w_ - row_h_;
    const int chev_w = text_w(chevron_text_, word_font_) + std::max(2, gap_ / 2);
    const int pad = std::max(2, gap_ / 2);
    const int lines = line_h(name_font_) + line_h(word_font_);
    const int y_name = (row_h_ - lines) / 2;
    int y = gap_;
    for (const ha::GridRow &gr : rows_) {
        if (!gr.slider) {
            for (std::size_t c = 0; c < gr.tiles.size(); ++c) {
                const int i = gr.tiles[c];
                TileUi &t = tiles_[static_cast<std::size_t>(i)];
                t.id = rows[static_cast<std::size_t>(i)].id;
                t.tile = box(grid_);
                lv_obj_set_pos(t.tile, margin_ + static_cast<int>(c) * (tile_w_ + gap_), y);
                lv_obj_set_size(t.tile, tile_w_, row_h_);
                lv_obj_set_style_radius(t.tile, radius, 0);
                lv_obj_set_style_bg_opa(t.tile, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(t.tile, lv_color_hex(ha::rgb::kTile), 0);
                /* the icon square: the action */
                t.icon = box(t.tile);
                lv_obj_add_flag(t.icon, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_pos(t.icon, 0, 0);
                lv_obj_set_size(t.icon, row_h_, row_h_);
                lv_obj_set_user_data(t.icon, reinterpret_cast<void *>(static_cast<std::intptr_t>(i * 2)));
                lv_obj_add_event_cb(t.icon, zone_event_cb, LV_EVENT_PRESSED, this);
                lv_obj_add_event_cb(t.icon, zone_event_cb, LV_EVENT_CLICKED, this);
                t.circle = box(t.icon);
                lv_obj_set_size(t.circle, circle_, circle_);
                lv_obj_center(t.circle);
                lv_obj_set_style_radius(t.circle, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_opa(t.circle, LV_OPA_COVER, 0);
                t.glyph = lv_label_create(t.circle);
                lv_obj_set_style_text_font(t.glyph, glyph_font_ ? glyph_font_ : word_font_, 0);
                lv_obj_remove_flag(t.glyph, LV_OBJ_FLAG_CLICKABLE);
                /* the name and the word: the entity page */
                t.text = box(t.tile);
                lv_obj_add_flag(t.text, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_pos(t.text, row_h_, 0);
                lv_obj_set_size(t.text, text_w0, row_h_);
                lv_obj_set_user_data(t.text, reinterpret_cast<void *>(static_cast<std::intptr_t>(i * 2 + 1)));
                lv_obj_add_event_cb(t.text, zone_event_cb, LV_EVENT_PRESSED, this);
                lv_obj_add_event_cb(t.text, zone_event_cb, LV_EVENT_CLICKED, this);
                const int lw = std::max(1, text_w0 - pad - chev_w - pad);
                t.name = line_label(t.text, name_font_, ha::rgb::kText, lw);
                lv_obj_set_pos(t.name, pad, y_name);
                t.word = line_label(t.text, word_font_, ha::rgb::kSecondary, lw);
                lv_obj_set_pos(t.word, pad, y_name + line_h(name_font_));
                /* the chevron in the tile's top right corner, on the name's line: the name opens a page */
                t.chevron = line_label(t.text, word_font_, CP0_UI_LIST_CHEVRON_RGB, 0);
                lv_label_set_text(t.chevron, chevron_text_.c_str());
                lv_obj_set_pos(t.chevron, text_w0 - pad - text_w(chevron_text_, word_font_),
                               y_name + (line_h(name_font_) - line_h(word_font_)) / 2);
                t.drawn = false;
            }
        } else {
            const int i = gr.tiles[0];
            SliderUi s;
            s.id = rows[static_cast<std::size_t>(i)].id;
            s.tile = i;
            const int w = layout_.w - 2 * margin_;
            s.row = box(grid_);
            lv_obj_add_flag(s.row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(s.row, margin_, y);
            lv_obj_set_size(s.row, w, row_h_);
            lv_obj_set_style_radius(s.row, radius, 0);
            lv_obj_set_style_bg_opa(s.row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s.row, lv_color_hex(ha::rgb::kTile), 0);
            lv_obj_set_user_data(s.row, reinterpret_cast<void *>(static_cast<std::intptr_t>(sliders_.size())));
            for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED})
                lv_obj_add_event_cb(s.row, slider_event_cb, code, this);
            /* the light's name (two lights in one row of tiles each get a slider row: the name says which) */
            const int name_w = std::min(text_w(rows[static_cast<std::size_t>(i)].name, word_font_) + 4, w * 30 / 100);
            s.name = line_label(s.row, word_font_, ha::rgb::kSecondary, std::max(1, name_w));
            lv_label_set_text(s.name, rows[static_cast<std::size_t>(i)].name.c_str());
            lv_obj_set_pos(s.name, margin_, (row_h_ - line_h(word_font_)) / 2);
            const int pct_w = text_w("100 %", word_font_);
            s.pct = line_label(s.row, word_font_, ha::rgb::kText, pct_w);
            lv_obj_set_style_text_align(s.pct, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(s.pct, w - margin_ - pct_w, (row_h_ - line_h(word_font_)) / 2);
            const int knob = std::max(10, row_h_ * 30 / 100);
            s.x0 = margin_ + name_w + margin_ + knob / 2;
            s.w = std::max(row_h_, w - margin_ - pct_w - margin_ - knob / 2 - s.x0);
            const int th = std::max(6, line_h(word_font_) / 4);
            s.track = box(s.row);
            lv_obj_set_pos(s.track, s.x0, (row_h_ - th) / 2);
            lv_obj_set_size(s.track, s.w, th);
            lv_obj_set_style_radius(s.track, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(s.track, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s.track, lv_color_hex(kTrack), 0);
            s.fill = box(s.row);
            lv_obj_set_pos(s.fill, s.x0, (row_h_ - th) / 2);
            lv_obj_set_size(s.fill, th, th);
            lv_obj_set_style_radius(s.fill, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(s.fill, LV_OPA_COVER, 0);
            s.knob = box(s.row);
            lv_obj_set_size(s.knob, knob, knob);
            lv_obj_set_style_radius(s.knob, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(s.knob, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s.knob, lv_color_hex(kKnob), 0);
            tiles_[static_cast<std::size_t>(i)].slider = static_cast<int>(sliders_.size());
            sliders_.push_back(s);
        }
        y += row_h_ + gap_;
    }
    const std::uint32_t now = lv_tick_get();
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        draw_tile(tiles_[i], rows[i], true);
        if (tiles_[i].slider >= 0) {
            SliderUi &s = sliders_[static_cast<std::size_t>(tiles_[i].slider)];
            const bool holding = s.id == slide_id_ && slide_value_ >= 0 && (slide_timer_ || now - slid_at_ < kSlideHoldMs);
            draw_slider(s, holding ? slide_value_ : rows[i].brightness_pct, rows[i].on && !rows[i].muted, true);
        }
    }
    ha::clamp_focus(rows_, focus_);
    style_focus();
    lv_obj_update_layout(grid_); /* the new tiles' coordinates now: the focus' scroll into view uses them */
    /* the labels laid out again at their real width (dots are decided when a text is set) */
    for (TileUi &t : tiles_) {
        lv_label_set_text(t.name, t.shown.name.c_str());
        lv_label_set_text(t.word, t.shown.tile_value.c_str());
    }
    for (SliderUi &s : sliders_) {
        const ha::EntityRow &row = tiles_[static_cast<std::size_t>(s.tile)].shown;
        lv_label_set_text(s.name, row.name.c_str());
    }
}

/* Only what changed is touched: LVGL redraws just those objects (a state change repaints one tile). */
void HaApp::draw_tile(TileUi &t, const ha::EntityRow &row, bool force)
{
    const ha::EntityRow &old = t.shown;
    const bool grey = row.muted && !row.pending && !row.done;
    const bool was_grey = old.muted && !old.pending && !old.done;
    const bool more = ha::tile_has_more(row.domain, row.tile_action, row.dimmable);
    if (force || more != ha::tile_has_more(old.domain, old.tile_action, old.dimmable)) {
        /* the chevron only where the page has more than the action; without it the name and the word take its room */
        const int pad = std::max(2, gap_ / 2);
        const int zone = tile_w_ - row_h_; /* the name zone's width (its coordinates may not be laid out yet) */
        const int lw = std::max(1, zone - 2 * pad - (more ? text_w(chevron_text_, word_font_) + pad : 0));
        if (more) lv_obj_remove_flag(t.chevron, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(t.chevron, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(t.name, lw);
        lv_obj_set_width(t.word, lw);
    }
    if (force || row.name != old.name) lv_label_set_text(t.name, row.name.c_str());
    if (force || row.tile_value != old.tile_value) lv_label_set_text(t.word, row.tile_value.c_str());
    if (force || row.glyph != old.glyph || (!row.glyph && row.name != old.name)) {
        const std::string g = row.glyph ? ha::utf8_of(row.glyph) : initial(row.name);
        lv_label_set_text(t.glyph, glyph_font_ ? g.c_str() : initial(row.name).c_str());
        lv_obj_center(t.glyph);
    }
    const bool look_changed = force || grey != was_grey || row.unavailable != old.unavailable ||
                              row.glyph_rgb != old.glyph_rgb || row.circle_rgb != old.circle_rgb;
    if (look_changed) {
        std::uint32_t name_rgb = ha::rgb::kText, word_rgb = ha::rgb::kSecondary;
        std::uint32_t glyph_rgb = row.glyph_rgb, circle_rgb = row.circle_rgb;
        if (row.unavailable) { /* dimmed */
            name_rgb = word_rgb = glyph_rgb = ha::rgb::kDisabled;
            circle_rgb = ha::rgb::kCircle;
        } else if (grey) { /* offline, or no state yet: the last layout greyed */
            name_rgb = ha::rgb::kSecondary;
            word_rgb = glyph_rgb = ha::rgb::kDisabled;
            circle_rgb = ha::rgb::kCircle;
        }
        lv_obj_set_style_text_color(t.name, lv_color_hex(name_rgb), 0);
        lv_obj_set_style_text_color(t.word, lv_color_hex(word_rgb), 0);
        lv_obj_set_style_text_color(t.glyph, lv_color_hex(glyph_rgb), 0);
        lv_obj_set_style_bg_color(t.circle, lv_color_hex(circle_rgb), 0);
    }
    t.shown = row;
    t.drawn = true;
}

void HaApp::draw_slider(SliderUi &s, int value, bool on, bool force)
{
    value = std::clamp(value, 0, 100);
    if (!force && value == s.value && on == s.on) return;
    s.value = value;
    s.on = on;
    const int th = std::max(6, line_h(word_font_) / 4); /* as built (the coordinates may not be laid out yet) */
    const int fill = std::max(th, s.w * value / 100);
    lv_obj_set_width(s.fill, fill);
    lv_obj_set_style_bg_color(s.fill, lv_color_hex(on ? kAccent : kTrack), 0);
    const int knob = std::max(10, row_h_ * 30 / 100);
    lv_obj_set_pos(s.knob, s.x0 + s.w * value / 100 - knob / 2, (row_h_ - knob) / 2);
    lv_label_set_text(s.pct, (std::to_string(value) + " %").c_str());
}

void HaApp::style_focus()
{
    if (focus_drawn_ >= 0 && focus_drawn_ < static_cast<int>(tiles_.size())) {
        lv_obj_t *o = tiles_[static_cast<std::size_t>(focus_drawn_)].tile;
        lv_obj_set_style_bg_color(o, lv_color_hex(ha::rgb::kTile), 0);
        set_focus_ring(o, false);
    }
    if (slider_focus_drawn_ >= 0 && slider_focus_drawn_ < static_cast<int>(sliders_.size())) {
        lv_obj_t *o = sliders_[static_cast<std::size_t>(slider_focus_drawn_)].row;
        lv_obj_set_style_bg_color(o, lv_color_hex(ha::rgb::kTile), 0);
        set_focus_ring(o, false);
    }
    focus_drawn_ = slider_focus_drawn_ = -1;
    if (focus_visible_ && zone_ == Zone::Grid && !rows_.empty()) {
        const int tile = ha::focused_tile(rows_, focus_);
        const bool on_slider = rows_[static_cast<std::size_t>(focus_.row)].slider;
        if (tile >= 0 && tile < static_cast<int>(tiles_.size())) {
            lv_obj_t *o = nullptr;
            if (on_slider && tiles_[static_cast<std::size_t>(tile)].slider >= 0) {
                slider_focus_drawn_ = tiles_[static_cast<std::size_t>(tile)].slider;
                o = sliders_[static_cast<std::size_t>(slider_focus_drawn_)].row;
            } else {
                focus_drawn_ = tile;
                o = tiles_[static_cast<std::size_t>(tile)].tile;
            }
            lv_obj_set_style_bg_color(o, lv_color_hex(kHighlight), 0);
            set_focus_ring(o, true);
        }
    }
    style_dash_focus();
    style_bar();
}

void HaApp::focus_tile_into_view()
{
    if (zone_ == Zone::Bar) {
        const int cur = ha::tab_index(tabs_, tab_key_);
        if (cur < static_cast<int>(tab_btns_.size())) lv_obj_scroll_to_view(tab_btns_[static_cast<std::size_t>(cur)], LV_ANIM_OFF);
        return;
    }
    if (on_dash_tab()) {
        if (dstop_drawn_ >= 0 && dstop_drawn_ < static_cast<int>(dstops_.size()))
            lv_obj_scroll_to_view_recursive(dstops_[static_cast<std::size_t>(dstop_drawn_)].obj, LV_ANIM_OFF);
        return;
    }
    if (slider_focus_drawn_ >= 0) lv_obj_scroll_to_view(sliders_[static_cast<std::size_t>(slider_focus_drawn_)].row, LV_ANIM_OFF);
    else if (focus_drawn_ >= 0) lv_obj_scroll_to_view(tiles_[static_cast<std::size_t>(focus_drawn_)].tile, LV_ANIM_OFF);
}

void HaApp::switch_tab(int index)
{
    if (tabs_.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(tabs_.size()) - 1);
    if (tabs_[static_cast<std::size_t>(index)].key() == tab_key_) return;
    tab_key_ = tabs_[static_cast<std::size_t>(index)].key();
    tab_chosen_ = true;
    focus_ = ha::Focus();
    dstop_ = 0;
    close_dialog();
    build_content();
    refresh_dashboard();
    if (index < static_cast<int>(tab_btns_.size())) lv_obj_scroll_to_view(tab_btns_[static_cast<std::size_t>(index)], LV_ANIM_OFF);
    dirty_ = true; /* the area's tiles as soon as the backend has them */
}

bool HaApp::swipe(int dx, int dy)
{
    const int min = cp0_ui_mm_px(cp0_ui_metrics_get()->ppmm_x100, kSwipeMmX10, 40);
    if (std::abs(dx) < min || std::abs(dx) < 2 * std::abs(dy)) return false;
    switch_tab(ha::tab_index(tabs_, tab_key_) + (dx < 0 ? 1 : -1));
    return true;
}

void HaApp::tab_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(e));
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj)));
    self->post([self, index] {
        self->focus_visible_ = false;
        self->zone_ = Zone::Grid;
        self->switch_tab(index);
        self->style_focus();
    });
}

void HaApp::zone_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(e));
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_point_t p{};
    if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->press_at_ = p;
        return;
    }
    const int dx = p.x - self->press_at_.x, dy = p.y - self->press_at_.y;
    const int code = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj)));
    const int tile = code / 2;
    const bool icon = code % 2 == 0;
    self->post([self, tile, icon, dx, dy] {
        if (self->swipe(dx, dy)) return;
        if (tile < 0 || tile >= static_cast<int>(self->tiles_.size())) return;
        /* a tap: the focus goes (touch-only boards show none) and the tile acts or opens its page */
        self->focus_visible_ = false;
        self->style_focus();
        const ha::EntityRow row = self->tiles_[static_cast<std::size_t>(tile)].shown;
        if (icon) self->act_on_tile(row);
        else self->open_entity(row.id);
    });
}

void HaApp::grid_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(e));
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return; /* a tile's own press bubbled up */
    lv_point_t p{};
    if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->press_at_ = p;
        return;
    }
    const int dx = p.x - self->press_at_.x, dy = p.y - self->press_at_.y;
    self->post([self, dx, dy] { self->swipe(dx, dy); });
}

void HaApp::slider_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(e));
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj)));
    self->slider_pointer(index, lv_event_get_code(e));
}

/* A drag sets the level under the finger; a tap without a drag steps it by 10 % towards the side of the knob it
 * landed on (the 0.1.0 slider row's rule). The value goes to Home Assistant debounced. */
void HaApp::slider_pointer(int index, lv_event_code_t code)
{
    if (index < 0 || index >= static_cast<int>(sliders_.size())) return;
    SliderUi &s = sliders_[static_cast<std::size_t>(index)];
    lv_point_t p{};
    if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(s.row, &a);
    const int x = p.x - a.x1;
    if (code == LV_EVENT_PRESSED) {
        press_at_ = p;
        dragging_ = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (!dragging_ && std::abs(p.x - press_at_.x) > std::max(4, gap_)) dragging_ = true;
        if (dragging_) slider_set(index, std::clamp((x - s.x0) * 100 / std::max(1, s.w), 0, 100));
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        dragging_ = false;
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        const bool dragged = dragging_;
        dragging_ = false;
        if (dragged) return;
        const int knob = s.x0 + s.w * std::max(0, s.value) / 100;
        post([this, index, step = x < knob ? -10 : 10] {
            focus_visible_ = false;
            style_focus();
            slider_step(index, step);
        });
    }
}

void HaApp::slider_step(int index, int delta)
{
    if (index < 0 || index >= static_cast<int>(sliders_.size())) return;
    const SliderUi &s = sliders_[static_cast<std::size_t>(index)];
    /* 10 % steps from the level shown (72 % -> 82 %), as the entity page's slider row */
    slider_set(index, std::clamp(std::max(0, s.value) + delta, 0, 100));
}

void HaApp::slider_set(int index, int value)
{
    if (index < 0 || index >= static_cast<int>(sliders_.size())) return;
    SliderUi &s = sliders_[static_cast<std::size_t>(index)];
    const ha::EntityRow &row = tiles_[static_cast<std::size_t>(s.tile)].shown;
    draw_slider(s, value, value > 0 && !row.muted, false);
    send_brightness(s.id, value);
}

/* The brightness of a light, debounced (the entity page's slider and the tiles' rows). */
void HaApp::send_brightness(const std::string &id, int value)
{
    slide_id_ = id;
    slide_value_ = std::clamp(value, 0, 100);
    slid_at_ = lv_tick_get();
    if (slide_timer_) lv_timer_delete(slide_timer_);
    slide_timer_ = lv_timer_create(
        [](lv_timer_t *t) {
            auto *self = static_cast<HaApp *>(lv_timer_get_user_data(t));
            self->slide_timer_ = nullptr;
            lv_timer_delete(t);
            self->slid_at_ = lv_tick_get();
            if (!self->slide_id_.empty())
                self->backend_->send(ha::Command::of(ha::Command::Kind::SetBrightness, self->slide_id_, "",
                                                     self->slide_value_));
        },
        kSlideDebounceMs, this);
}

/* What the icon (or Enter) does: toggle, run, lock / unlock and garage-type covers after a confirmation, else the
 * entity page. An unavailable tile opens its page. */
void HaApp::act_on_tile(const ha::EntityRow &row)
{
    if (row.unavailable) {
        open_entity(row.id);
        return;
    }
    switch (row.tile_action) {
    case ha::TileAction::Toggle: backend_->send(ha::Command::of(ha::Command::Kind::Toggle, row.id)); break;
    case ha::TileAction::Run: backend_->send(ha::Command::of(ha::Command::Kind::Run, row.id)); break;
    case ha::TileAction::Lock:
    case ha::TileAction::Cover: {
        const bool lock = row.tile_action == ha::TileAction::Lock;
        const std::string service = lock ? (row.on ? "lock" : "unlock") : (row.on ? "close_cover" : "open_cover");
        const std::string id = row.id;
        auto run = [this, id, service] { backend_->send(ha::Command::of(ha::Command::Kind::Service, id, service)); };
        /* not connected: no question for a command that cannot go (the engine says so on the screen) */
        if (row.confirm && view_.conn == ha::Conn::Connected) confirm(ha::confirm_question(service, row.name), run);
        else run();
        break;
    }
    case ha::TileAction::Details: open_entity(row.id); return;
    }
    refresh(true);
}

void HaApp::dashboard_key(ha::NavKey k)
{
    const ha::Tab *t = current_tab();
    if (!t) return;
    const int cur = ha::tab_index(tabs_, tab_key_);
    if (k == ha::NavKey::Back) {
        back();
        return;
    }
    if (k == ha::NavKey::PrevView || k == ha::NavKey::NextView) {
        switch_tab(cur + (k == ha::NavKey::NextView ? 1 : -1));
        style_focus();
        focus_tile_into_view();
        return;
    }
    const bool list_tab = t->kind == ha::Tab::Kind::All;
    if (!focus_visible_) { /* the first key shows where the focus is, and does nothing else */
        focus_visible_ = true;
        if (list_tab) zone_ = Zone::Grid;
        style_focus();
        focus_tile_into_view();
        if (!list_tab) return;
    }
    if (zone_ == Zone::Bar) {
        switch (k) {
        case ha::NavKey::Left:
        case ha::NavKey::Right:
            switch_tab(cur + (k == ha::NavKey::Right ? 1 : -1));
            break;
        case ha::NavKey::Down:
        case ha::NavKey::Enter:
            zone_ = Zone::Grid;
            focus_ = ha::Focus();
            dstop_ = 0;
            if (list_ && current_tab()->kind == ha::Tab::Kind::All) list_->select(1, false);
            break;
        default: break;
        }
        style_focus();
        focus_tile_into_view();
        return;
    }
    if (t->kind == ha::Tab::Kind::View) {
        dash_key(k);
        return;
    }
    if (list_tab) {
        if (!list_) return;
        if (k == ha::NavKey::Up && list_->selected() <= 1) { /* the first item: up to the tab bar */
            zone_ = Zone::Bar;
            style_focus();
            focus_tile_into_view();
            return;
        }
        std::uint32_t key = 0;
        switch (k) {
        case ha::NavKey::Up: key = LV_KEY_UP; break;
        case ha::NavKey::Down: key = LV_KEY_DOWN; break;
        case ha::NavKey::Home: key = LV_KEY_HOME; break;
        case ha::NavKey::End: key = LV_KEY_END; break;
        case ha::NavKey::Enter: key = LV_KEY_ENTER; break;
        case ha::NavKey::Right:
        case ha::NavKey::Details: key = LV_KEY_RIGHT; break; /* the entity page of an entity row */
        default: break;
        }
        if (key) forward_key(key);
        return;
    }
    if (tiles_.empty()) {
        if (k == ha::NavKey::Up) {
            zone_ = Zone::Bar;
            style_focus();
        }
        return;
    }
    const int tile = ha::focused_tile(rows_, focus_);
    if (k == ha::NavKey::Enter) {
        if (tile < 0) return;
        act_on_tile(tiles_[static_cast<std::size_t>(tile)].shown);
        return;
    }
    if (k == ha::NavKey::Details) {
        if (tile >= 0) open_entity(tiles_[static_cast<std::size_t>(tile)].id);
        return;
    }
    switch (ha::move_focus(rows_, focus_, k)) {
    case ha::Move::Moved: break;
    case ha::Move::Top: zone_ = Zone::Bar; break;
    case ha::Move::Slider:
        if (tile >= 0 && tiles_[static_cast<std::size_t>(tile)].slider >= 0)
            slider_step(tiles_[static_cast<std::size_t>(tile)].slider, k == ha::NavKey::Right ? 10 : -10);
        return;
    case ha::Move::Stayed: return;
    }
    style_focus();
    focus_tile_into_view();
}

void HaApp::show_toast(const std::string &text, bool error)
{
    if (toast_) {
        lv_obj_delete(toast_);
        toast_ = nullptr;
    }
    if (text.empty()) return;
    const int w = layout_.w - 2 * margin_;
    toast_ = box(page_->root());
    lv_obj_set_width(toast_, w);
    lv_obj_set_height(toast_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast_, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast_, m_radius(), 0);
    lv_obj_set_style_pad_all(toast_, gap_, 0);
    lv_obj_set_style_border_color(toast_, lv_color_hex(error ? CP0_UI_LIST_BAD_RGB : 0x3A3A3A), 0);
    lv_obj_set_style_border_width(toast_, 1, 0);
    lv_obj_t *l = plain_label(toast_, word_font_, error ? CP0_UI_LIST_BAD_RGB : ha::rgb::kText);
    lv_obj_set_width(l, w - 2 * gap_);
    lv_label_set_text(l, text.c_str());
    lv_obj_update_layout(toast_);
    lv_obj_set_pos(toast_, margin_, layout_.h - margin_ - lv_obj_get_height(toast_));
    toast_until_ = lv_tick_get() + kMessageMs;
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
    if (toast_ && now >= toast_until_) {
        lv_obj_delete(toast_);
        toast_ = nullptr;
    }
    if (screen_kind_ == Screen::Connect && view_.signed_in) {
        if (view_.paired_now && !named_at_) named_at_ = now;
        if (!connected_at_) connected_at_ = now;
        if ((named_at_ && now - named_at_ >= kNamedMs) || now - connected_at_ >= kSignedInMaxMs) {
            named_at_ = connected_at_ = 0;
            show(Screen::Dashboard);
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
        if (screen_kind_ == Screen::Dashboard || screen_kind_ == Screen::Entity) show_toast(message_, message_error_);
    }
    if (!view_.signed_in && screen_kind_ != Screen::Connect && screen_kind_ != Screen::ServerEdit) {
        show(Screen::Connect);
        return;
    }
    switch (screen_kind_) {
    case Screen::Connect: refresh_connect(); break;
    case Screen::Dashboard: refresh_dashboard(); break;
    case Screen::Room:
    case Screen::Settings: refresh_list(); break;
    case Screen::Entity: refresh_entity(false); break;
    default: break;
    }
    set_title();
    update_caption();
}

/* The status line under the title: the Room and Settings pages (0.1.0's look). The dashboard shows the connection in
 * its tab bar and the entity page at its top; their messages are the toast. */
void HaApp::update_caption()
{
    if (screen_kind_ != Screen::Room && screen_kind_ != Screen::Settings) {
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

/* Settings > Dashboard's value: "Automatic (Home)", a dashboard's title, "Off (tiles only)"; shorter when it does not
 * fit beside the label */
std::string HaApp::dash_setting_value()
{
    const std::string &s = view_.dash_setting;
    std::vector<std::string> candidates;
    if (s == ha::kDashOff) {
        candidates = {"Off (tiles only)", "Off"};
    } else {
        std::string title;
        for (const auto &c : view_.dash_choices)
            if (c.first == s) title = c.second;
        if (!title.empty() && s != ha::kDashAuto) candidates = {title};
        else candidates = {"Automatic (" + (view_.dash_auto_title.empty() ? std::string("tiles only") : view_.dash_auto_title) + ")",
                           "Automatic"};
    }
    return list_ ? fit_value(list_->layout(), "Dashboard", candidates, CP0_UI_LIST_AFFORDANCE_EDIT) : candidates.front();
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
    /* 0.3.0: which stored dashboard the tabs start with; Enter (or a tap on the highlighted row) moves to the next */
    Target dash;
    dash.kind = Target::Kind::Dashboard;
    rows.push_back(ListRow::item("Dashboard", dash_setting_value(), ValueTone::Normal, add_target(dash),
                                 RowAffordance::Edit));
    /* the tiles' keys, where a keyboard user looks for them (touch: the icon acts, the name opens the page) */
    rows.push_back(ListRow::item("Tile action", "Enter", ValueTone::Normal, add_target(Target()), RowAffordance::Info));
    rows.push_back(ListRow::item("Tile page", "Ctrl+O", ValueTone::Normal, add_target(Target()), RowAffordance::Info));
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
    case Screen::Dashboard: rows = home_rows(); break;
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

void HaApp::render_qr(const std::string &url, int bx)
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
    const int px = ha::qr_module_px(m.size, kQuiet, bx);
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

void HaApp::refresh_hero()
{
    if (!hero_) return;
    const ha::Detail &d = view_.detail;
    const ha::EntityRow &t = d.tile;
    const bool current = d.id == entity_id_;
    std::string word = t.tile_value.empty() ? std::string("-") : t.tile_value;
    if (!current) word = "...";
    else if (!d.known && view_.registry_loaded) word = "Not in Home Assistant";
    const bool grey = !current || (t.muted && !t.pending && !t.done);
    std::uint32_t glyph_rgb = t.glyph_rgb, circle_rgb = t.circle_rgb;
    if (t.unavailable || grey) {
        glyph_rgb = ha::rgb::kDisabled;
        circle_rgb = ha::rgb::kCircle;
    }
    const std::string name = d.name.empty() ? entity_id_ : d.name;
    const std::string g = current && t.glyph && hero_glyph_font_ ? ha::utf8_of(t.glyph) : initial(name);
    const std::string meta = current ? d.meta : std::string();
    const std::string key = word + "\n" + meta + "\n" + g + "\n" + std::to_string(glyph_rgb) + "/" + std::to_string(circle_rgb);
    if (key == hero_shown_) return;
    hero_shown_ = key;
    lv_label_set_text(hero_word_, word.c_str());
    lv_label_set_text(hero_meta_, meta.c_str());
    lv_obj_set_style_bg_color(hero_circle_, lv_color_hex(circle_rgb), 0);
    lv_obj_set_style_text_color(hero_glyph_, lv_color_hex(glyph_rgb), 0);
    lv_label_set_text(hero_glyph_, g.c_str());
    lv_obj_center(hero_glyph_);
}

void HaApp::refresh_entity(bool rebuild)
{
    if (!dbox_) return;
    refresh_hero();
    const ha::Detail &d = view_.detail;
    const bool current = d.id == entity_id_;
    std::vector<DetailRow> want;
    auto add = [&want](DetailRow::Kind kind, DetailRow::Does does, const std::string &label, const std::string &value,
                       bool running, int slider) {
        DetailRow r;
        r.kind = kind;
        r.does = does;
        r.label = label;
        r.value = value;
        r.running = running;
        r.slider_value = slider;
        want.push_back(r);
    };
    using K = DetailRow::Kind;
    using D = DetailRow::Does;
    /* the room and the last change are at the top ("Living room - changed 19:58") */
    if (current)
        for (const auto &kv : d.info)
            if (kv.first != "Room" && kv.first != "Last changed") add(K::Info, D::None, kv.first, kv.second, false, -1);
    if (current && d.dimmable) {
        const bool holding = slide_id_ == entity_id_ && slide_value_ >= 0 &&
                             (slide_timer_ || dragging_ || lv_tick_get() - slid_at_ < kSlideHoldMs);
        const int pct = holding ? slide_value_ : d.brightness_pct;
        add(K::Slider, D::Brightness, "Brightness", std::to_string(pct) + " %", false, pct);
    }
    const bool waiting = d.tile.pending && !d.tile.muted;
    if (current) {
        switch (d.tile.tile_action) {
        case ha::TileAction::Toggle: add(K::Action, D::Act, d.tile.on ? "Turn off" : "Turn on", "", waiting, -1); break;
        case ha::TileAction::Run:
            add(K::Action, D::Act, d.domain == ha::Domain::Scene ? "Activate" : "Run", "", waiting || d.tile.running, -1);
            break;
        case ha::TileAction::Lock: add(K::Action, D::Act, d.tile.on ? "Lock" : "Unlock", "", waiting, -1); break;
        case ha::TileAction::Cover:
            add(K::Action, D::Open, "Open", "", waiting, -1);
            add(K::Action, D::Close, "Close", "", waiting, -1);
            break;
        case ha::TileAction::Details: break;
        }
    }
    add(K::Action, D::Favourite, d.favourite && current ? "Remove from favourites" : "Add to favourites", "", false, -1);

    bool same = !rebuild && want.size() == drows_.size();
    /* an action row's new words ("Turn off" -> "Turn on", "Add to" -> "Remove from favourites") are laid out again:
     * its face is fitted to its words */
    for (std::size_t i = 0; same && i < want.size(); ++i)
        same = want[i].kind == drows_[i].kind && want[i].does == drows_[i].does &&
               (want[i].kind != DetailRow::Kind::Action || want[i].label == drows_[i].label);
    if (!same) {
        build_detail_rows(want);
        return;
    }
    for (std::size_t i = 0; i < want.size(); ++i) draw_detail_row(drows_[i], want[i], false);
}

/* The rows from the top of their box, one ROW each, a hairline between them (the lists' look). */
void HaApp::build_detail_rows(const std::vector<DetailRow> &rows)
{
    const int keep = dsel_;
    lv_obj_clean(dbox_);
    drows_ = rows;
    const int w = dl_.w, rh = dl_.row_h;
    const int value_w = text_w("100 %", dvalue_font_);
    const int dot = cp0_ui_list_dot_px(&dl_);
    const int knob = std::max(10, rh * 30 / 100);
    for (std::size_t i = 0; i < drows_.size(); ++i) {
        DetailRow &r = drows_[i];
        const DetailRow want = r;
        r.font = dlabel_font_;
        r.bold = dlabel_bold_;
        r.row = box(dbox_);
        lv_obj_set_pos(r.row, 0, static_cast<int>(i) * rh);
        lv_obj_set_size(r.row, w, rh);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(CP0_UI_LIST_BAR_RGB), 0);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_TRANSP, 0);
        if (i + 1 < drows_.size()) {
            lv_obj_set_style_border_side(r.row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r.row, dl_.separator_px, 0);
            lv_obj_set_style_border_color(r.row, lv_color_hex(CP0_UI_LIST_SEPARATOR_RGB), 0);
        }
        if (r.kind != DetailRow::Kind::Info) {
            lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(r.row, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
            for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED})
                lv_obj_add_event_cb(r.row, detail_event_cb, code, this);
        }
        const int label_y = (rh - line_h(dlabel_bold_)) / 2;
        switch (r.kind) {
        case DetailRow::Kind::Info: {
            const int vw = std::min(text_w(r.value, dvalue_font_), w - 2 * dl_.inset - dl_.gap - rh);
            const int lw = cp0_ui_list_label_w(w, dl_.inset, dl_.gap, vw);
            r.label_obj = line_label(r.row, dlabel_font_, CP0_UI_LIST_TEXT_RGB, std::max(1, lw));
            lv_obj_set_pos(r.label_obj, dl_.inset, label_y);
            r.value_obj = line_label(r.row, dvalue_font_,
                                     cp0_ui_list_value_rgb(CP0_UI_LIST_TONE_NORMAL, CP0_UI_LIST_AFFORDANCE_INFO),
                                     std::max(1, vw));
            lv_obj_set_style_text_align(r.value_obj, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(r.value_obj, w - dl_.inset - vw, (rh - line_h(dvalue_font_)) / 2);
            break;
        }
        case DetailRow::Kind::Action: {
            const int lw = std::max(1, w - 2 * dl_.inset - cp0_ui_list_trail_w(&dl_, CP0_UI_LIST_AFFORDANCE_ACTION));
            r.label_obj = line_label(r.row, dlabel_font_, cp0_ui_list_label_rgb(CP0_UI_LIST_AFFORDANCE_ACTION), lw);
            lv_obj_set_pos(r.label_obj, dl_.inset, label_y);
            /* a label too long for the row at the list's size ("Remove from favourites" on a 480x320 screen at the
             * deck's density) gets a smaller face (2 px steps, at most down to the value's size less 4), not dots */
            if (text_w(r.label, dlabel_bold_) > lw) {
                const int floor_px = std::max(10, dl_.value_px - 4);
                for (int px = dl_.text_px - 2; px >= floor_px; px -= 2) {
                    const lv_font_t *fb = cp0_fonts().get("Montserrat-Bold.ttf", static_cast<uint16_t>(px));
                    if (text_w(r.label, fb) > lw && px - 2 >= floor_px) continue;
                    r.font = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(px));
                    r.bold = fb;
                    lv_obj_set_style_text_font(r.label_obj, r.font, 0);
                    lv_obj_set_height(r.label_obj, line_h(fb));
                    lv_obj_set_y(r.label_obj, (rh - line_h(fb)) / 2);
                    break;
                }
            }
            r.dot = box(r.row);
            lv_obj_set_size(r.dot, dot, dot);
            lv_obj_set_pos(r.dot, cp0_ui_list_dot_x(&dl_, w), cp0_ui_list_chevron_y(rh, 0, dot));
            lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
            break;
        }
        case DetailRow::Kind::Slider: {
            /* the label whole (in the bold face of the selected row), the value as wide as "100 %", the track between */
            /* as wide as the wider of its two faces, with a few px of slack (the label's own measure wraps at the
             * exact width), and as high as the taller one: never dots */
            const int lw = std::max(text_w(r.label, dlabel_bold_), text_w(r.label, dlabel_font_)) + std::max(4, dl_.gap / 2);
            r.label_obj = line_label(r.row, dlabel_font_, CP0_UI_LIST_TEXT_RGB, std::max(1, lw));
            lv_obj_set_pos(r.label_obj, dl_.inset, label_y);
            r.value_obj = line_label(r.row, dvalue_font_, CP0_UI_LIST_VALUE_RGB, value_w);
            lv_obj_set_style_text_align(r.value_obj, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(r.value_obj, w - dl_.inset - value_w, (rh - line_h(dvalue_font_)) / 2);
            r.x0 = dl_.inset + lw + dl_.gap + knob / 2;
            r.w = std::max(knob, w - dl_.inset - value_w - dl_.gap - knob / 2 - r.x0);
            const int th = std::max(6, dl_.text_px / 4); /* the toolkit's slider height */
            r.track = box(r.row);
            lv_obj_set_pos(r.track, r.x0, (rh - th) / 2);
            lv_obj_set_size(r.track, r.w, th);
            lv_obj_set_style_radius(r.track, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(r.track, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r.track, lv_color_hex(kTrack), 0);
            r.fill = box(r.row);
            lv_obj_set_pos(r.fill, r.x0, (rh - th) / 2);
            lv_obj_set_size(r.fill, th, th);
            lv_obj_set_style_radius(r.fill, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(r.fill, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r.fill, lv_color_hex(kAccent), 0);
            r.knob = box(r.row);
            lv_obj_set_size(r.knob, knob, knob);
            lv_obj_set_style_radius(r.knob, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(r.knob, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r.knob, lv_color_hex(kKnob), 0);
            break;
        }
        }
        if (r.label_obj) { /* as high as the taller of its two faces (a selected row's label turns bold) */
            const int lh = std::max(line_h(r.font), line_h(r.bold));
            lv_obj_set_height(r.label_obj, lh);
            lv_obj_set_y(r.label_obj, (rh - lh) / 2);
        }
        r.slider_value = -1;
        draw_detail_row(r, want, true);
    }
    lv_obj_update_layout(dbox_);
    /* the labels laid out again at their real width: a label's dots are decided when its text is set, and a new
     * label's width is only applied by the layout */
    for (DetailRow &r : drows_) {
        if (r.label_obj) lv_label_set_text(r.label_obj, r.label.c_str());
        if (r.value_obj) lv_label_set_text(r.value_obj, r.value.c_str());
    }
    /* the selection: the same row again after a rebuild of the same page, else the first that does something */
    dsel_ = -1;
    int sel = keep >= 0 && keep < static_cast<int>(drows_.size()) && drows_[static_cast<std::size_t>(keep)].kind != DetailRow::Kind::Info
                  ? keep
                  : detail_step(-1, 1);
    select_detail(sel, true);
}

void HaApp::draw_detail_row(DetailRow &r, const DetailRow &want, bool force)
{
    if (r.label_obj && (force || want.label != r.label)) lv_label_set_text(r.label_obj, want.label.c_str());
    if (r.value_obj && r.kind != DetailRow::Kind::Slider && (force || want.value != r.value))
        lv_label_set_text(r.value_obj, want.value.c_str());
    if (r.dot && (force || want.running != r.running))
        lv_obj_set_style_bg_color(r.dot, lv_color_hex(cp0_ui_list_dot_rgb(want.running ? CP0_UI_LIST_AFFORDANCE_ACTION_RUNNING
                                                                                     : CP0_UI_LIST_AFFORDANCE_ACTION)),
                                  0);
    r.label = want.label;
    r.value = want.value;
    r.running = want.running;
    if (r.kind == DetailRow::Kind::Slider && (force || want.slider_value != r.slider_value)) draw_detail_slider(r, want.slider_value);
}

void HaApp::draw_detail_slider(DetailRow &r, int value)
{
    value = std::clamp(value, 0, 100);
    r.slider_value = value;
    const int th = std::max(6, dl_.text_px / 4); /* as built (the coordinates may not be laid out yet) */
    lv_obj_set_width(r.fill, std::max(th, r.w * value / 100));
    const int knob = std::max(10, dl_.row_h * 30 / 100);
    lv_obj_set_pos(r.knob, r.x0 + r.w * value / 100 - knob / 2, (dl_.row_h - knob) / 2);
    r.value = std::to_string(value) + " %";
    lv_label_set_text(r.value_obj, r.value.c_str());
}

/* The next row that does something (Info rows are skipped: they are read in passing), from `from` by `delta`; the
 * same row when there is none. */
int HaApp::detail_step(int from, int delta) const
{
    const int n = static_cast<int>(drows_.size());
    for (int i = from + delta; i >= 0 && i < n; i += delta)
        if (drows_[static_cast<std::size_t>(i)].kind != DetailRow::Kind::Info) return i;
    return from;
}

void HaApp::select_detail(int index, bool scroll)
{
    if (index < 0 || index >= static_cast<int>(drows_.size())) return;
    if (dsel_ >= 0 && dsel_ < static_cast<int>(drows_.size()) && dsel_ != index) {
        DetailRow &o = drows_[static_cast<std::size_t>(dsel_)];
        lv_obj_set_style_bg_opa(o.row, LV_OPA_TRANSP, 0);
        if (o.label_obj) lv_obj_set_style_text_font(o.label_obj, o.font, 0);
    }
    dsel_ = index;
    DetailRow &r = drows_[static_cast<std::size_t>(index)];
    lv_obj_set_style_bg_opa(r.row, LV_OPA_COVER, 0);
    if (r.label_obj) lv_obj_set_style_text_font(r.label_obj, r.bold, 0);
    if (scroll) lv_obj_scroll_to_view(r.row, LV_ANIM_OFF);
}

/* Up / Down move over the rows that do something, Home / End; Left / Right step a slider by 10 %, elsewhere Right
 * acts and Left goes back (the lists' keys); Enter acts; Esc goes back. */
void HaApp::detail_key(std::uint32_t key)
{
    const bool on_slider = dsel_ >= 0 && dsel_ < static_cast<int>(drows_.size()) &&
                           drows_[static_cast<std::size_t>(dsel_)].kind == DetailRow::Kind::Slider;
    switch (key) {
    case LV_KEY_UP: select_detail(detail_step(dsel_, -1), true); return;
    case LV_KEY_DOWN: select_detail(detail_step(dsel_, 1), true); return;
    case LV_KEY_HOME: select_detail(detail_step(-1, 1), true); return;
    case LV_KEY_END: select_detail(detail_step(static_cast<int>(drows_.size()), -1), true); return;
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
        if (on_slider) {
            DetailRow &r = drows_[static_cast<std::size_t>(dsel_)];
            const int v = std::clamp(std::max(0, r.slider_value) + (key == LV_KEY_RIGHT ? 10 : -10), 0, 100);
            draw_detail_slider(r, v);
            send_brightness(entity_id_, v);
            return;
        }
        if (key == LV_KEY_LEFT) back();
        else detail_activate(dsel_, -1);
        return;
    case LV_KEY_ENTER:
        if (!on_slider) detail_activate(dsel_, -1);
        return;
    case LV_KEY_ESC: back(); return;
    default: return;
    }
}

void HaApp::detail_activate(int index, int tap_x)
{
    if (index < 0 || index >= static_cast<int>(drows_.size())) return;
    const ha::Detail &d = view_.detail;
    DetailRow &r = drows_[static_cast<std::size_t>(index)];
    switch (r.does) {
    case DetailRow::Does::Brightness: {
        if (tap_x < 0) return;
        /* a tap on the selected slider row: 10 % towards the side of the knob it landed on */
        const int knob = r.x0 + r.w * std::max(0, r.slider_value) / 100;
        const int v = std::clamp(std::max(0, r.slider_value) + (tap_x < knob ? -10 : 10), 0, 100);
        draw_detail_slider(r, v);
        send_brightness(entity_id_, v);
        return;
    }
    case DetailRow::Does::Act: act_on_tile(d.tile); return;
    case DetailRow::Does::Open:
    case DetailRow::Does::Close: {
        const std::string service = r.does == DetailRow::Does::Open ? "open_cover" : "close_cover";
        const std::string id = entity_id_;
        auto run = [this, id, service] { backend_->send(ha::Command::of(ha::Command::Kind::Service, id, service)); };
        if (d.confirm && view_.conn == ha::Conn::Connected) confirm(ha::confirm_question(service, d.name), run);
        else run();
        break;
    }
    case DetailRow::Does::Favourite: backend_->send(ha::Command::of(ha::Command::Kind::ToggleFavourite, entity_id_)); break;
    case DetailRow::Does::None: return;
    }
    refresh(true);
}

void HaApp::detail_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(e));
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    self->detail_pointer(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj))),
                         lv_event_get_code(e));
}

/* Touch, the lists' rule: a tap selects a row, a tap on the selected row acts. A slider row also follows a drag at
 * once (the level under the finger), selected or not. */
void HaApp::detail_pointer(int index, lv_event_code_t code)
{
    if (index < 0 || index >= static_cast<int>(drows_.size())) return;
    lv_point_t p{};
    if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    DetailRow &r = drows_[static_cast<std::size_t>(index)];
    lv_area_t a;
    lv_obj_get_coords(r.row, &a);
    const int x = p.x - a.x1;
    switch (code) {
    case LV_EVENT_PRESSED:
        press_at_ = p;
        dragging_ = false;
        return;
    case LV_EVENT_PRESSING:
        if (r.kind != DetailRow::Kind::Slider) return;
        if (!dragging_ && std::abs(p.x - press_at_.x) > std::max(4, gap_)) {
            dragging_ = true;
            select_detail(index, false);
        }
        if (dragging_) {
            const int v = std::clamp((x - r.x0) * 100 / std::max(1, r.w), 0, 100);
            if (v != r.slider_value) {
                draw_detail_slider(r, v);
                send_brightness(entity_id_, v);
            }
        }
        return;
    case LV_EVENT_PRESS_LOST: dragging_ = false; return;
    case LV_EVENT_CLICKED: {
        const bool dragged = dragging_;
        dragging_ = false;
        if (dragged) return;
        const bool selected = index == dsel_;
        post([this, index, selected, x] {
            if (index >= static_cast<int>(drows_.size())) return;
            if (!selected) select_detail(index, false);
            else detail_activate(index, x);
        });
        return;
    }
    default: return;
    }
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
    if (screen_kind_ == Screen::Dashboard && !dialog_) {
        const bool ctrl = (item->mods & KBD_MOD_CTRL) != 0;
        ha::NavKey k = ha::nav_key(code, ctrl);
        if (k == ha::NavKey::None && item->semantic_key) k = ha::nav_key(item->semantic_key, false);
        if (k == ha::NavKey::None) return;
        key_activation_ = true;
        last_key_ = lv_key_of(code);
        dashboard_key(k);
        key_activation_ = false;
        return;
    }
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
    if (screen_kind_ == Screen::Entity && !dialog_) {
        detail_key(key);
        return;
    }
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
    case Target::Kind::Dashboard: {
        /* the next of Automatic, each dashboard with supported cards, Off */
        std::vector<ha::DashChoice> choices;
        for (const auto &c : view_.dash_choices) {
            ha::DashChoice d;
            d.key = c.first;
            d.title = c.second;
            d.loaded = true;
            d.supported = 1;
            choices.push_back(d);
        }
        backend_->send(ha::Command::of(ha::Command::Kind::SetDashboard, ha::next_dashboard_setting(view_.dash_setting, choices)));
        tab_chosen_ = false; /* back on the dashboard: its first tab */
        tab_key_.clear();
        refresh(true);
        return;
    }
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
    entity_from_ = screen_kind_ == Screen::Room ? Screen::Room : Screen::Dashboard;
    dash_shown_ = -1;
    if (entity_from_ == Screen::Dashboard) {
        /* keep the area's tiles (or the dashboard view's cards) subscribed under the page (back shows them at once) */
        const ha::Tab *t = current_tab();
        room_shown_ = t && t->kind == ha::Tab::Kind::Area;
        if (room_shown_) room_id_ = t->area;
        if (t && t->kind == ha::Tab::Kind::View) dash_shown_ = t->view;
    }
    entity_id_ = id;
    show(Screen::Entity);
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
    lv_obj_t *b = dialog_->focus_object();
    if (group && b) {
        if (lv_obj_get_group(b) != group) lv_group_add_obj(group, b);
        lv_group_focus_obj(b);
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
    case Screen::Dashboard:
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
        show(Screen::Dashboard);
        return;
    case Screen::Entity:
        entity_id_.clear();
        slide_value_ = -1;
        show(entity_from_ == Screen::Room && room_shown_ ? Screen::Room : Screen::Dashboard);
        return;
    case Screen::Settings: show(Screen::Dashboard); return;
    }
}

} // namespace haui
