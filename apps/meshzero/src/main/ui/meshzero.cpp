/*
 * SPDX-License-Identifier: MIT
 */

#include "meshzero.hpp"

#include "cp0_font_service.hpp"
#include "input_state.hpp"
#include "nav.hpp"
#include "cp0_keyboard_navigation_contract.h"
#include "input_keys.h"
#include "keyboard_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <utility>

using namespace meshzero;

namespace {

constexpr uint32_t kBackground = 0x101214;
constexpr uint32_t kSelected = 0x2A2F35;
constexpr uint32_t kPanel = 0x1A1D20;
constexpr uint32_t kText = 0xF4F4F5;
constexpr uint32_t kMuted = 0x8A929B;
constexpr uint32_t kGold = 0xF0B400;
constexpr uint32_t kGreen = 0x33CC33;
constexpr uint32_t kBlue = 0x3B9DFF;
constexpr uint32_t kRed = 0xE5604D;

constexpr int kWidth = 320;
constexpr int kContentH = 150;
constexpr int kHeaderH = 18;
constexpr int kFooterH = 16;
constexpr int kRowH = 17;
constexpr int kColHdrH = 14;
constexpr uint32_t kStartGuardMs = 700;   // ignore Enter / taps this long after start (the one that launched us)
constexpr uint32_t kNoticeMs = 4000;
constexpr uint32_t kMinPlausibleTime = 1735689600;

std::atomic<bool> g_quit{false};

uint64_t mono_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w, int h,
                     lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);                       // fixed size: "dots" mode wraps when the height is automatic
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, uint32_t color, lv_obj_t **label_out = nullptr)
{
    lv_obj_t *b = make_box(parent, x, y, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4048), LV_STATE_PRESSED);
    lv_obj_t *l = make_label(b, &lv_font_montserrat_12, color, 2, (h - 14) / 2, w - 4, 14, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(l, text);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (label_out) *label_out = l;
    return b;
}

/* Message text: DejaVu Sans (accents, more symbols) when the launcher can load it, else Montserrat. */
const lv_font_t *text_font(uint16_t size)
{
    // The launcher ships DejaVuSans.ttf in its font folder and its own pages use that file. Try the absolute paths first
    // (cp0_fonts() resolves a bare name through the launcher's filesystem service, which an app may not have), then the
    // bare name; cp0_fonts() itself falls back to Montserrat (no accents) when nothing loads.
    static const char *const paths[] = {"/usr/share/APPLaunch/share/font/DejaVuSans.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char *p : paths)
        if (::access(p, R_OK) == 0) {
            const lv_font_t *f = cp0_fonts().get(p, size);
            if (f != cp0_fonts().fallback(size)) return f;
        }
    return cp0_fonts().get("DejaVuSans.ttf", size);
}

std::string fmt_clock(uint32_t ts)
{
    if (ts < kMinPlausibleTime) return "--:--";
    const time_t t = static_cast<time_t>(ts);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::string fmt_age(uint32_t when)
{
    const uint32_t now = now_unix();
    if (when == 0 || now < kMinPlausibleTime || when < kMinPlausibleTime) return "-";
    if (when > now) return "now";
    return format_age(static_cast<int64_t>(now - when));
}

std::string fmt_freq(double mhz)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f MHz", mhz);
    return b;
}

std::string fmt_bw(double khz)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%g kHz", khz);
    return b;
}

std::string short_link(const Client &c)
{
    switch (c.link()) {
    case Link::Ready: return "Connected";
    case Link::Searching: return "No board";
    case Link::Denied: return "Denied";
    case Link::Busy: return "Port busy";
    case Link::Error: return "Port error";
    case Link::Handshake: return "Connecting";
    case Link::NotCompanion: return "Wrong fw";
    }
    return "";
}

uint32_t link_color(const Client &c)
{
    switch (c.link()) {
    case Link::Ready: return kGreen;
    case Link::Handshake: return kGold;
    case Link::Searching: return kMuted;
    default: return kRed;
    }
}

} // namespace

bool meshzero_quit_requested()
{
    return g_quit.load();      // never set: the app ends only through the launcher's hold-Esc
}

/* ------------------------------------------------------------------ construction */

UIMeshZeroPage::UIMeshZeroPage() : serial_(Store::default_dir())
{
    set_page_title("MeshZero");

    store_ = std::make_unique<Store>(Store::default_dir());
    store_->attach(model_);
    client_ = std::make_unique<Client>(serial_, model_);
    client_->set_deck_clock([this] { return deck_clock_.state(); }, [this] { deck_clock_.refresh(); });
    deck_clock_.refresh();      // is the deck clock NTP synchronised? answered by a worker thread

    root_ = make_box(ui_APP_Container, 0, 0, kWidth, kContentH);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    build_header();
    build_home();
    build_list();
    build_chat();
    build_detail();
    build_edit();

    footer_ = make_label(root_, &lv_font_montserrat_12, kMuted, 6, kContentH - kFooterH + 1, kWidth - 8, 14);

    lv_obj_add_event_cb(root_screen_, key_event_cb, LV_EVENT_KEY, this);
    // Raw evdev keys: letters (their codes collide with LVGL's arrow keys for W/E/R/T, so none of those is used) and
    // everything typed in the editor. The keyboard may be asleep and absent: touch does everything.
    // Must be the page's own screen: it is only made active after this constructor returns.
    keyboard_root_ = root_screen_;
    if (keyboard_root_ && LV_EVENT_KEYBOARD != 0)
        keyboard_dsc_ = lv_obj_add_event_cb(keyboard_root_, &UIMeshZeroPage::keyboard_event_cb,
                                            static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);

    start_tick_ = lv_tick_get();
    edit_ = model_.radio_settings();
    timer_ = lv_timer_create(timer_cb, 50, this);
    set_view(View::Home);
    tick();
}

UIMeshZeroPage::~UIMeshZeroPage()
{
    if (timer_) lv_timer_delete(timer_);
    if (keyboard_root_ && keyboard_dsc_) lv_obj_remove_event_dsc(keyboard_root_, keyboard_dsc_);
    leave_text_mode();
    if (client_) client_->transport().close();
    model_.set_listener(nullptr);
}

void UIMeshZeroPage::build_header()
{
    static const char *const names[4] = {"Status", "Chats", "Contacts", "Settings"};
    static const int widths[4] = {56, 52, 70, 66};
    int x = 0;
    for (int i = 0; i < 4; ++i) {
        tab_box_[i] = make_box(root_, x, 0, widths[i], kHeaderH);
        lv_obj_add_flag(tab_box_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tab_box_[i], button_event_cb, LV_EVENT_CLICKED, this);
        tab_label_[i] = make_label(tab_box_[i], &lv_font_montserrat_14, kMuted, 6, 1, widths[i] - 6, 16);
        lv_label_set_text(tab_label_[i], names[i]);
        x += widths[i];
    }
    status_ = make_label(root_, &lv_font_montserrat_12, kMuted, x + 2, 0, kWidth - x - 4, kHeaderH, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_pad_top(status_, 3, 0);

    back_btn_ = make_button(root_, 2, 1, 56, kHeaderH - 2, "< Back", kText, &back_label_);
    lv_obj_add_event_cb(back_btn_, button_event_cb, LV_EVENT_CLICKED, this);
    title_ = make_label(root_, &lv_font_montserrat_14, kGold, 62, 1, 176, 16, LV_TEXT_ALIGN_CENTER);
    remove_btn_ = make_button(root_, 168, 1, 70, kHeaderH - 2, "Remove", kRed, &remove_label_);
    lv_obj_add_event_cb(remove_btn_, button_event_cb, LV_EVENT_CLICKED, this);
    action_btn_ = make_button(root_, kWidth - 80, 1, 78, kHeaderH - 2, "Message", kText, &action_label_);
    lv_obj_add_event_cb(action_btn_, button_event_cb, LV_EVENT_CLICKED, this);
}

void UIMeshZeroPage::build_home()
{
    home_box_ = make_box(root_, 0, kHeaderH, kWidth, kContentH - kHeaderH - kFooterH);
    for (size_t i = 0; i < home_key_.size(); ++i) {
        home_key_[i] = make_label(home_box_, &lv_font_montserrat_12, kMuted, 6, static_cast<int>(i) * 13 + 2, 62, 13);
        home_val_[i] = make_label(home_box_, &lv_font_montserrat_12, kText, 70, static_cast<int>(i) * 13 + 2, kWidth - 74, 13);
    }
    static const char *const keys[7] = {"Status", "Board", "Node", "Radio", "Battery", "Clock", "Mesh"};
    for (size_t i = 0; i < home_key_.size(); ++i) lv_label_set_text(home_key_[i], keys[i]);
    advert_flood_ = make_button(home_box_, 6, 94, 150, 20, "Advert: flood (A)", kText);
    advert_direct_ = make_button(home_box_, 164, 94, 150, 20, "Advert: zero-hop (D)", kText);
    lv_obj_add_event_cb(advert_flood_, button_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(advert_direct_, button_event_cb, LV_EVENT_CLICKED, this);
}

void UIMeshZeroPage::build_list()
{
    list_box_ = make_box(root_, 0, kHeaderH, kWidth, kColHdrH + kRows * kRowH);
    static const int xs[3] = {14, 146, 248};
    static const int ws[3] = {130, 100, 68};
    for (int i = 0; i < 3; ++i)
        col_hdr_[i] = make_label(list_box_, &lv_font_montserrat_12, kMuted, xs[i], 0, ws[i], kColHdrH, i == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
    for (int i = 0; i < kRows; ++i) {
        lv_obj_t *row = make_box(list_box_, 0, kColHdrH + i * kRowH, kWidth, kRowH);
        rows_[i] = row;
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kSelected), 0);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_ALL, this);
        row_mark_[i] = make_label(row, &lv_font_montserrat_14, kGold, 2, 1, 12, 16);
        row_left_[i] = make_label(row, text_font(14), kText, 14, 0, ws[0], 16);
        row_mid_[i] = make_label(row, text_font(12), kMuted, xs[1], 2, ws[1], 14);
        row_right_[i] = make_label(row, &lv_font_montserrat_12, kMuted, xs[2], 2, ws[2], 14, LV_TEXT_ALIGN_RIGHT);
    }
    empty_msg_ = make_label(root_, &lv_font_montserrat_14, kMuted, 10, 58, kWidth - 20, 60, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(empty_msg_, LV_LABEL_LONG_MODE_WRAP);
}

void UIMeshZeroPage::build_chat()
{
    chat_box_ = lv_obj_create(root_);
    lv_obj_remove_style_all(chat_box_);
    lv_obj_set_pos(chat_box_, 0, kHeaderH);
    lv_obj_set_size(chat_box_, kWidth, kContentH - kHeaderH - kFooterH);
    lv_obj_set_flex_flow(chat_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(chat_box_, 4, 0);
    lv_obj_set_style_pad_row(chat_box_, 3, 0);
    lv_obj_set_scrollbar_mode(chat_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(chat_box_, LV_OBJ_FLAG_SCROLLABLE);
}

void UIMeshZeroPage::build_detail()
{
    det_box_ = make_box(root_, 0, kHeaderH, kWidth, kContentH - kHeaderH - kFooterH);
    static const char *const keys[7] = {"Name", "Type", "Key", "Route", "Heard", "Position", "Last SNR"};
    for (size_t i = 0; i < det_key_.size(); ++i) {
        det_key_[i] = make_label(det_box_, &lv_font_montserrat_12, kMuted, 6, static_cast<int>(i) * 16 + 2, 62, 14);
        lv_label_set_text(det_key_[i], keys[i]);
        det_val_[i] = make_label(det_box_, text_font(14), kText, 70, static_cast<int>(i) * 16, kWidth - 74, 16);
    }
}

void UIMeshZeroPage::build_edit()
{
    // The text is typed on the Bluetooth keyboard only (no on-screen keyboard): the whole area is the text.
    edit_box_ = make_box(root_, 0, kHeaderH, kWidth, kContentH - kHeaderH - kFooterH);
    edit_ta_ = lv_textarea_create(edit_box_);
    lv_obj_set_pos(edit_ta_, 2, 2);
    lv_obj_set_size(edit_ta_, kWidth - 4, kContentH - kHeaderH - kFooterH - 4);
    lv_textarea_set_one_line(edit_ta_, false);
    lv_obj_set_style_text_font(edit_ta_, text_font(14), 0);
    lv_obj_set_style_text_color(edit_ta_, lv_color_hex(kText), 0);
    lv_obj_set_style_bg_color(edit_ta_, lv_color_hex(kPanel), 0);
    lv_obj_set_style_border_color(edit_ta_, lv_color_hex(kGold), 0);
    lv_obj_set_style_border_width(edit_ta_, 1, 0);
    lv_obj_set_style_pad_all(edit_ta_, 4, 0);
    lv_obj_remove_flag(edit_ta_, LV_OBJ_FLAG_CLICKABLE);
}

/* ------------------------------------------------------------------ views */

void UIMeshZeroPage::set_view(View v)
{
    if (text_mode_ && v != View::Edit) leave_text_mode();
    view_ = v;
    if (is_tab(v)) tab_ = v;
    const bool tabs = is_tab(v);
    for (int i = 0; i < 4; ++i) set_hidden(tab_box_[i], !tabs);
    set_hidden(status_, !tabs);
    set_hidden(back_btn_, tabs);
    set_hidden(title_, tabs);
    set_hidden(action_btn_, !(v == View::Chat || v == View::Detail || v == View::Edit));
    set_hidden(remove_btn_, v != View::Chat || !conv_removable());
    if (v != View::Chat) remove_pending_ = false;
    set_hidden(home_box_, v != View::Home);
    set_hidden(list_box_, !(v == View::Chats || v == View::Contacts || v == View::Settings));
    set_hidden(chat_box_, v != View::Chat);
    set_hidden(det_box_, v != View::Detail);
    set_hidden(edit_box_, v != View::Edit);
    for (int i = 0; i < 4; ++i) {
        const View tv = i == 0 ? View::Home : i == 1 ? View::Chats : i == 2 ? View::Contacts : View::Settings;
        lv_obj_set_style_text_color(tab_label_[i], lv_color_hex(tv == v ? kGold : kMuted), 0);
    }
    if (v == View::Chats || v == View::Contacts || v == View::Settings) {
        selected_ = 0;
        offset_ = 0;
    }
    last_nav_tick_ = lv_tick_get();
    if (v == View::Chat) chat_conv_built_.clear();
    if (v == View::Settings && !dirty_) edit_ = model_.radio_settings();
    refresh();
}

void UIMeshZeroPage::next_tab(int delta)
{
    if (!is_tab(view_)) return;
    static const View order[4] = {View::Home, View::Chats, View::Contacts, View::Settings};
    int i = 0;
    for (int k = 0; k < 4; ++k)
        if (order[k] == view_) i = k;
    set_view(order[(i + delta + 4) % 4]);
}

void UIMeshZeroPage::tick()
{
    client_->poll(mono_ms());
    bool changed = false;
    if (model_.revision() != last_revision_) changed = true;
    const uint32_t link = static_cast<uint32_t>(client_->link());
    if (link != last_link_) changed = true;
    if (client_->notice().id != last_notice_) {
        last_notice_ = client_->notice().id;
        notice_text_ = client_->notice().text;
        notice_ok_ = client_->notice().ok;
        notice_tick_ = lv_tick_get();
        changed = true;
    }
    if (!notice_text_.empty() && lv_tick_elaps(notice_tick_) >= kNoticeMs) {
        notice_text_.clear();
        changed = true;
    }
    if (lv_tick_elaps(last_tick_) >= 1000) changed = true;   // ages ("5 min") move on
    if (holding_ && lv_tick_elaps(hold_tick_) >= 4000) holding_ = false;
    if (!dirty_ && !holding_ && view_ != View::Edit) {
        const RadioSettings cur = model_.radio_settings();
        if (cur.name != edit_.name || !cur.same_radio(edit_)) {
            edit_ = cur;
            changed = true;
        }
    }
    if (client_->clock_prompt_pending() && view_ != View::Edit && (prompt_tick_ == 0 || lv_tick_elaps(prompt_tick_) >= 500)) {
        // the board has no GPS and the deck clock is not synchronised: ask for the start date and time (keyboard only)
        prompt_tick_ = lv_tick_get();
        if (prompt_tick_ == 0) prompt_tick_ = 1;
        if (!keyboard_ready()) {
            client_->clock_skip();
            notice("Keyboard needed to set the board clock: Settings > Sync clock now", false);
        } else {
            start_edit(EditKind::Clock, format_local_datetime(now_unix()), "Board clock: start date and time (local)");
        }
    }
    if (view_ == View::Chat) model_.mark_read(conv_);
    if (changed) refresh();
}

void UIMeshZeroPage::refresh()
{
    last_revision_ = model_.revision();
    last_link_ = static_cast<uint32_t>(client_->link());
    last_tick_ = lv_tick_get();

    // header
    const int unread = model_.unread_total();
    lv_label_set_text(tab_label_[1], unread > 0 ? ("Chats " + std::to_string(unread)).c_str() : "Chats");
    lv_label_set_text(status_, short_link(*client_).c_str());
    lv_obj_set_style_text_color(status_, lv_color_hex(link_color(*client_)), 0);

    switch (view_) {
    case View::Home: render_home(); break;
    case View::Chats:
    case View::Contacts:
    case View::Settings:
        build_items();
        render_list();
        break;
    case View::Chat: render_chat(); break;
    case View::Detail: render_detail(); break;
    case View::Edit: update_edit_title(); break;
    }
    render_footer();
}

void UIMeshZeroPage::render_footer()
{
    if (!notice_text_.empty()) {
        lv_label_set_text(footer_, notice_text_.c_str());
        lv_obj_set_style_text_color(footer_, lv_color_hex(notice_ok_ ? kGreen : kRed), 0);
        return;
    }
    const char *t = "";
    switch (view_) {
    case View::Home: t = "A: advert   D: zero-hop advert   Tab: next   Hold Esc: exit"; break;
    case View::Chats: t = "Enter: open / add channel   Tab: next   Hold Esc: exit"; break;
    case View::Contacts: t = "Enter: open   U: refresh   A: advert   Tab: next"; break;
    case View::Settings:
        t = "Left/Right: change  Enter: edit  S: save  V: undo";
        if (selected_ == 1 && client_->ready()) {
            const bool in_band = edit_.freq_mhz >= 863.0 && edit_.freq_mhz <= 870.0;
            lv_label_set_text(footer_, in_band ? "EU 868 band (863-870 MHz), step 0.025 MHz" : "Outside the EU 868 band (863-870 MHz)!");
            lv_obj_set_style_text_color(footer_, lv_color_hex(in_band ? kMuted : kRed), 0);
            return;
        }
        break;
    case View::Chat: t = conv_removable() ? "Enter or M: write   Del: remove channel   Esc: back" : "Up/Down: scroll   Enter or M: write   Esc: back"; break;
    case View::Detail: t = "Enter: message   Esc: back"; break;
    case View::Edit:
        t = edit_kind_ == EditKind::Clock ? "Digits - :   Enter: set the board clock   Esc: skip"
            : edit_kind_ == EditKind::Number ? "Digits . -   Enter: accept   Esc: cancel" : edit_kind_ == EditKind::Message
                ? "Type on the keyboard   Enter: send   Esc: cancel" : "Type on the keyboard   Enter: OK   Esc: cancel";
        break;
    }
    lv_label_set_text(footer_, t);
    lv_obj_set_style_text_color(footer_, lv_color_hex(kMuted), 0);
}

/* ------------------------------------------------------------------ Status */

void UIMeshZeroPage::render_home()
{
    const Client &c = *client_;
    std::string v[7];
    v[0] = c.link_text();
    const auto &dev = model_.device();
    const auto &self = model_.self();
    if (c.ready()) {
        v[1] = dev ? dev->model + " " + dev->version : serial_.info().path;
    } else {
        v[1] = c.link_hint();
        if (v[1].empty()) v[1] = serial_.info().path;
    }
    v[2] = self ? self->name : "-";
    if (self) {
        char b[96];
        std::snprintf(b, sizeof(b), "%.3f MHz  BW %g  SF%d  CR 4/%d  %d dBm", self->freq_mhz(), self->bw_khz(), self->sf, self->cr,
                      self->tx_power);
        v[3] = b;
    } else {
        v[3] = "-";
    }
    if (const auto &bat = model_.battery()) {
        const int mv = bat->millivolts;
        const int pct = std::clamp((mv - 3300) * 100 / 900, 0, 100);   // rough: 3.3 V empty, 4.2 V full
        char b[48];
        std::snprintf(b, sizeof(b), "%.2f V  (about %d%%)", mv / 1000.0, pct);
        v[4] = b;
    } else {
        v[4] = "-";
    }
    if (c.ready()) {
        const ClockInfo &ci = model_.clock();
        v[5] = std::string(clock_source_name(ci.source)) + "  " + format_local_datetime(now_unix()) +
               (ci.gps_known ? (ci.gps ? "  GPS on" : "  GPS off") : "  GPS ?");
    } else {
        v[5] = "-";
    }
    int channels = 0;
    for (const auto &ch : model_.channels())
        if (!ch.empty) ++channels;
    char b[96];
    std::snprintf(b, sizeof(b), "%zu contacts   %d channels   %d unread", model_.contact_count(), channels, model_.unread_total());
    v[6] = b;
    for (size_t i = 0; i < 7; ++i) {
        lv_label_set_text(home_val_[i], v[i].c_str());
        uint32_t col = kText;
        if (i == 0) col = link_color(c);
        else if (i == 1 && !c.ready()) col = kMuted;
        else if (i == 5 && c.ready()) col = model_.clock().source == ClockSource::NotSet ? kGold : kText;
        lv_obj_set_style_text_color(home_val_[i], lv_color_hex(col), 0);
    }
    lv_label_set_text(home_key_[1], c.ready() ? "Board" : "Hint");
}

/* ------------------------------------------------------------------ lists */

int UIMeshZeroPage::settings_row_count() const
{
    const ChannelRec *ch0 = model_.find_channel(0);
    const bool offer_public = client_->ready() && (!ch0 || ch0->empty);
    return offer_public ? kSettingsRows : kSettingsRows - 1;
}

void UIMeshZeroPage::build_items()
{
    items_.clear();
    item_keys_.clear();
    if (view_ == View::Chats) {
        for (const ConvSummary &s : model_.conversations()) {
            Item it;
            it.mark = s.unread > 0 ? "*" : "";
            it.mark_color = kGold;
            it.left = s.title;
            it.left_color = s.unread > 0 ? kGold : kText;
            it.mid = truncate_utf8(s.last_text, 22);
            it.right = s.last_ts ? fmt_age(s.last_ts) : "";
            if (s.unread > 0) it.right = std::to_string(s.unread) + " new  " + it.right;
            it.right_color = s.unread > 0 ? kGold : kMuted;
            it.mid_color = kMuted;
            items_.push_back(it);
            item_keys_.push_back(s.key);
        }
        Item add;
        add.left = "+ Add hashtag channel";
        add.left_color = kBlue;
        items_.push_back(add);
        item_keys_.push_back("+");
    } else if (view_ == View::Contacts) {
        for (const ContactRec *r : model_.contacts_sorted()) {
            Item it;
            it.left = r->c.name.empty() ? to_hex(r->prefix()) : r->c.name;
            it.left_color = kText;
            char b[48];
            if (r->has_snr) std::snprintf(b, sizeof(b), "%s  %.1f dB", contact_type_name(r->c.type), r->snr);
            else std::snprintf(b, sizeof(b), "%s", contact_type_name(r->c.type));
            it.mid = b;
            it.right = fmt_age(r->last_heard());
            items_.push_back(it);
            item_keys_.push_back(r->key_hex());
        }
    } else {
        const bool changed_name = edit_.name != model_.radio_settings().name;
        const RadioSettings cur = model_.radio_settings();
        auto row = [&](const char *label, const std::string &value, bool changed, bool adjustable) {
            Item it;
            it.left = label;
            it.left_color = kText;
            it.mid = value;
            it.mid_color = changed ? kGold : kText;
            it.mark = changed ? "*" : "";
            it.mark_color = kGold;
            if (adjustable) {
                it.right = "-      +";
                it.right_color = kBlue;
            }
            items_.push_back(it);
            item_keys_.push_back("");
        };
        const bool connected = client_->ready();
        row("Node name", edit_.name.empty() ? "-" : edit_.name, connected && changed_name, false);
        row("Frequency", connected ? fmt_freq(edit_.freq_mhz) : "-", connected && std::fabs(edit_.freq_mhz - cur.freq_mhz) > 0.0004, true);
        if (connected && (edit_.freq_mhz < 863.0 || edit_.freq_mhz > 870.0)) items_.back().mid_color = kRed;   // outside the EU 868 band
        row("Bandwidth", connected ? fmt_bw(edit_.bw_khz) : "-", connected && std::fabs(edit_.bw_khz - cur.bw_khz) > 0.0004, true);
        row("Spreading factor", connected ? "SF" + std::to_string(edit_.sf) : "-", connected && edit_.sf != cur.sf, true);
        row("Coding rate", connected ? "4/" + std::to_string(edit_.cr) : "-", connected && edit_.cr != cur.cr, true);
        row("TX power", connected ? std::to_string(edit_.tx_power) + " dBm" : "-", connected && edit_.tx_power != cur.tx_power, true);
        Item save;
        save.left = "Save to radio";
        save.left_color = dirty_ ? kGreen : kMuted;
        save.mid = dirty_ ? "unsaved changes" : "";
        save.mid_color = kGold;
        items_.push_back(save);
        item_keys_.push_back("");
        Item undo;
        undo.left = "Undo changes";
        undo.left_color = dirty_ ? kText : kMuted;
        items_.push_back(undo);
        item_keys_.push_back("");
        Item eu;
        eu.left = "Preset EU 868";
        eu.left_color = connected ? kText : kMuted;
        eu.mid = "frequency 869.525 MHz";
        eu.mid_color = kMuted;
        items_.push_back(eu);
        item_keys_.push_back("");
        Item sync;
        sync.left = "Sync clock now";
        sync.left_color = connected ? kText : kMuted;
        sync.mid = connected ? clock_source_name(model_.clock().source) : "";
        sync.mid_color = kMuted;
        items_.push_back(sync);
        item_keys_.push_back("");
        if (settings_row_count() == kSettingsRows) {
            Item pub;
            pub.left = "Add the Public channel";
            pub.left_color = kText;
            pub.mid = "slot 0 is empty";
            pub.mid_color = kMuted;
            items_.push_back(pub);
            item_keys_.push_back("");
        }
    }
    const int n = static_cast<int>(items_.size());
    selected_ = std::clamp(selected_, 0, std::max(0, n - 1));
    if (selected_ < offset_) offset_ = selected_;
    if (selected_ >= offset_ + kRows) offset_ = selected_ - kRows + 1;
    offset_ = std::max(0, std::min(offset_, std::max(0, n - kRows)));
}

void UIMeshZeroPage::render_list()
{
    static const char *const hdr[3][3] = {{"Chat", "Last message", "Age"}, {"Name", "Type / last SNR", "Heard"}, {"Setting", "Value", ""}};
    const int h = view_ == View::Chats ? 0 : view_ == View::Contacts ? 1 : 2;
    for (int i = 0; i < 3; ++i) lv_label_set_text(col_hdr_[i], hdr[h][i]);
    for (int i = 0; i < kRows; ++i) {
        const int index = offset_ + i;
        if (index >= static_cast<int>(items_.size())) {
            set_hidden(rows_[i], true);
            continue;
        }
        set_hidden(rows_[i], false);
        const Item &it = items_[static_cast<size_t>(index)];
        lv_obj_set_style_bg_opa(rows_[i], index == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_label_set_text(row_mark_[i], it.mark.c_str());
        lv_obj_set_style_text_color(row_mark_[i], lv_color_hex(it.mark_color), 0);
        lv_label_set_text(row_left_[i], it.left.c_str());
        lv_obj_set_style_text_color(row_left_[i], lv_color_hex(it.left_color), 0);
        lv_label_set_text(row_mid_[i], it.mid.c_str());
        lv_obj_set_style_text_color(row_mid_[i], lv_color_hex(it.mid_color ? it.mid_color : kMuted), 0);
        lv_label_set_text(row_right_[i], it.right.c_str());
        lv_obj_set_style_text_color(row_right_[i], lv_color_hex(it.right_color ? it.right_color : kMuted), 0);
    }
    std::string msg;
    if (view_ == View::Chats && items_.size() == 1) msg = "No conversations yet.";
    if (items_.empty()) {
        if (view_ == View::Contacts)
            msg = client_->ready() ? "No contacts yet.\nSend an advert (A) and wait for others." : "No contacts cached.\nConnect the radio board.";
    }
    lv_label_set_text(empty_msg_, msg.c_str());
    set_hidden(empty_msg_, msg.empty() || !(view_ == View::Chats || view_ == View::Contacts));
}

void UIMeshZeroPage::move_selection(int delta)
{
    if (view_ == View::Chat) {
        chat_scroll(delta * 40);
        return;
    }
    const int n = static_cast<int>(items_.size());
    if (n == 0) return;
    selected_ = std::clamp(selected_ + delta, 0, n - 1);
    if (selected_ < offset_) offset_ = selected_;
    if (selected_ >= offset_ + kRows) offset_ = selected_ - kRows + 1;
    refresh();
}

/* ------------------------------------------------------------------ chat */

void UIMeshZeroPage::notice(const std::string &text, bool ok)
{
    notice_text_ = text;
    notice_ok_ = ok;
    notice_tick_ = lv_tick_get();
    refresh();
}

/* Is a keyboard attached and awake (a sleeping Bluetooth keyboard is not listed by the kernel)? Checked at most once a second. */
bool UIMeshZeroPage::keyboard_ready()
{
    if (kbd_check_tick_ == 0 || lv_tick_elaps(kbd_check_tick_) >= 1000) {
        kbd_present_ = keyboard_present();
        kbd_check_tick_ = lv_tick_get();
        if (kbd_check_tick_ == 0) kbd_check_tick_ = 1;
    }
    return kbd_present_;
}

bool UIMeshZeroPage::conv_removable() const
{
    if (conv_.rfind("c:", 0) != 0) return false;
    const ChannelRec *ch = model_.find_channel(std::atoi(conv_.c_str() + 2));
    return ch && !ch->empty && ch->app_added;
}

void UIMeshZeroPage::request_remove()
{
    if (!conv_removable()) return;
    if (!remove_pending_ || lv_tick_elaps(remove_armed_) > 4000) {      // a second tap within 4 s confirms
        remove_pending_ = true;
        remove_armed_ = lv_tick_get();
        lv_label_set_text(remove_label_, "Sure?");
        return;
    }
    remove_pending_ = false;
    const int idx = std::atoi(conv_.c_str() + 2);
    if (client_->remove_channel(idx)) set_view(View::Chats);
}

std::string UIMeshZeroPage::target_title() const
{
    return model_.conv_title(conv_);
}

void UIMeshZeroPage::open_chat(const std::string &conv)
{
    conv_ = conv;
    set_view(View::Chat);
    lv_label_set_text(title_, truncate_utf8(target_title(), 24).c_str());
    lv_label_set_text(action_label_, "Message");
    model_.mark_read(conv_);
}

void UIMeshZeroPage::chat_scroll(int dy)
{
    lv_obj_scroll_by(chat_box_, 0, -dy, LV_ANIM_OFF);
}

void UIMeshZeroPage::render_chat()
{
    const bool removable = conv_removable();
    set_hidden(remove_btn_, !removable);
    lv_obj_set_width(title_, removable ? 100 : 176);
    lv_label_set_text(title_, truncate_utf8(target_title(), removable ? 12 : 24).c_str());
    lv_obj_set_style_text_color(title_, lv_color_hex(kGold), 0);
    lv_label_set_text(action_label_, "Message");
    if (remove_pending_ && lv_tick_elaps(remove_armed_) > 4000) remove_pending_ = false;
    lv_label_set_text(remove_label_, remove_pending_ ? "Sure?" : "Remove");
    if (chat_conv_built_ == conv_ && chat_seq_ == model_.revision()) return;
    const bool first = chat_conv_built_ != conv_;
    const bool at_bottom = first || lv_obj_get_scroll_bottom(chat_box_) <= 6;
    chat_conv_built_ = conv_;
    chat_seq_ = model_.revision();
    lv_obj_clean(chat_box_);
    const auto msgs = model_.messages(conv_);
    const size_t start = msgs.size() > 40 ? msgs.size() - 40 : 0;
    lv_obj_t *last = nullptr;
    auto add = [&](const std::string &text, uint32_t color) {
        lv_obj_t *l = lv_label_create(chat_box_);
        lv_obj_set_width(l, kWidth - 10);
        lv_obj_set_height(l, LV_SIZE_CONTENT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(l, text_font(12), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_label_set_text(l, text.c_str());
        last = l;
    };
    for (size_t i = start; i < msgs.size(); ++i) {
        const Message &m = *msgs[i];
        std::string line = fmt_clock(m.ts) + " " + m.sender + ": " + m.text;
        uint32_t color = kText;
        if (m.dir == Dir::Out) {
            switch (m.state) {
            case MsgState::Pending: line += m.note.empty() ? "  (sending)" : "  (" + m.note + ")"; color = kGold; break;
            case MsgState::Sent: line += conv_.rfind("c:", 0) == 0 ? "  (sent)" : "  (sent, waiting for ack)"; color = kBlue; break;
            case MsgState::Delivered: line += "  (delivered)"; color = kGreen; break;
            case MsgState::Failed: line += "  (failed: " + m.note + ")"; color = kRed; break;
            case MsgState::NoAck: line += "  (no ack)"; color = kRed; break;
            default: break;
            }
        }
        add(line, color);
    }
    if (msgs.empty()) add("No messages yet. Press Enter or tap Message to write.", kMuted);
    lv_obj_update_layout(chat_box_);
    if (at_bottom && last) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
}

/* ------------------------------------------------------------------ contact detail */

void UIMeshZeroPage::open_detail(const std::string &key_hex)
{
    detail_key_ = key_hex;
    set_view(View::Detail);
    lv_label_set_text(action_label_, "Message");
}

void UIMeshZeroPage::render_detail()
{
    const ContactRec *r = model_.find_contact(detail_key_);
    if (!r) {
        lv_label_set_text(title_, "(gone)");
        for (auto *v : det_val_) lv_label_set_text(v, "");
        return;
    }
    lv_label_set_text(title_, truncate_utf8(r->c.name, 24).c_str());
    lv_obj_set_style_text_color(title_, lv_color_hex(kGold), 0);
    lv_label_set_text(action_label_, "Message");
    lv_label_set_text(det_val_[0], r->c.name.c_str());
    lv_label_set_text(det_val_[1], contact_type_name(r->c.type));
    lv_label_set_text(det_val_[2], (to_hex(r->c.key.data(), 8) + "...").c_str());
    char b[96];
    const int hops = path_hops(r->c.out_path_len);
    if (hops < 0) std::snprintf(b, sizeof(b), "flood (no stored path)");
    else if (hops == 0) std::snprintf(b, sizeof(b), "direct");
    else std::snprintf(b, sizeof(b), "%d hop%s", hops, hops == 1 ? "" : "s");
    lv_label_set_text(det_val_[3], b);
    lv_label_set_text(det_val_[4], (fmt_age(r->last_heard()) + " ago").c_str());
    if (r->c.lat == 0 && r->c.lon == 0) std::snprintf(b, sizeof(b), "not shared");
    else std::snprintf(b, sizeof(b), "%.5f, %.5f", r->c.lat, r->c.lon);
    lv_label_set_text(det_val_[5], b);
    if (r->has_snr) std::snprintf(b, sizeof(b), "%.1f dB", r->snr);
    else std::snprintf(b, sizeof(b), "-");
    lv_label_set_text(det_val_[6], b);
}

/* ------------------------------------------------------------------ settings actions */

void UIMeshZeroPage::settings_adjust(int row, int delta)
{
    if (!client_->ready() || delta == 0) return;
    RadioSettings &e = edit_;
    switch (row) {
    case 1: e.freq_mhz = std::clamp(std::round((e.freq_mhz + delta * 0.025) * 1000.0) / 1000.0, 137.0, 2500.0); break;
    case 2: {
        const auto &bws = lora_bandwidths();
        int idx = 0;
        for (size_t i = 0; i < bws.size(); ++i)
            if (std::fabs(bws[i] - e.bw_khz) < 0.05) idx = static_cast<int>(i);
        idx = std::clamp(idx + delta, 0, static_cast<int>(bws.size()) - 1);
        e.bw_khz = bws[static_cast<size_t>(idx)];
        break;
    }
    case 3: e.sf = std::clamp(e.sf + delta, 5, 12); break;
    case 4: e.cr = std::clamp(e.cr + delta, 5, 8); break;
    case 5: e.tx_power = std::clamp(e.tx_power + delta, -9, e.max_tx_power > 0 ? e.max_tx_power : 30); break;
    default: return;
    }
    dirty_ = true;
    refresh();
}

void UIMeshZeroPage::settings_activate(int row)
{
    switch (row) {
    case 0: start_edit(EditKind::Name, edit_.name, "Node name"); break;
    case 1:
    case 2:
    case 3:
    case 4:
    case 5: {
        if (!client_->ready()) break;
        char b[32];
        const char *title = "";
        if (row == 1) { std::snprintf(b, sizeof(b), "%.3f", edit_.freq_mhz); title = "Frequency (MHz)"; }
        else if (row == 2) { std::snprintf(b, sizeof(b), "%g", edit_.bw_khz); title = "Bandwidth (kHz)"; }
        else if (row == 3) { std::snprintf(b, sizeof(b), "%d", edit_.sf); title = "Spreading factor"; }
        else if (row == 4) { std::snprintf(b, sizeof(b), "%d", edit_.cr); title = "Coding rate (5-8 = 4/5-4/8)"; }
        else { std::snprintf(b, sizeof(b), "%d", edit_.tx_power); title = "TX power (dBm)"; }
        num_row_ = row;
        start_edit(EditKind::Number, b, title);
        break;
    }
    case 6:
        if (dirty_ && client_->save_settings(edit_)) {
            dirty_ = false;
            holding_ = true;
            hold_tick_ = lv_tick_get();
        }
        refresh();
        break;
    case 7:
        edit_ = model_.radio_settings();
        dirty_ = false;
        refresh();
        break;
    case 8:
        if (client_->ready()) {        // only the frequency: bandwidth, SF and CR stay as the board reports them
            edit_.freq_mhz = 869.525;
            dirty_ = true;
        }
        refresh();
        break;
    case 9: client_->sync_clock(); break;
    case 10: client_->add_public_channel(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ editor */

void UIMeshZeroPage::enter_text_mode()
{
    if (text_mode_) return;
    saved_context_ = static_cast<int>(cp0_keyboard_get_input_context());
    saved_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
    cp0_keyboard_set_lvgl_keypad_intercept(1);
    text_mode_ = true;
}

void UIMeshZeroPage::leave_text_mode()
{
    if (!text_mode_) return;
    cp0_keyboard_set_input_context(static_cast<cp0_keyboard_input_context_t>(saved_context_));
    cp0_keyboard_set_lvgl_keypad_intercept(saved_intercept_);
    text_mode_ = false;
}

void UIMeshZeroPage::start_edit(EditKind kind, const std::string &initial, const std::string &title)
{
    if (lv_tick_elaps(start_tick_) < kStartGuardMs) return;
    if (!keyboard_ready()) {       // no dead editor: say what is missing
        notice("Keyboard needed: wake the Bluetooth keyboard", false);
        return;
    }
    edit_kind_ = kind;
    edit_title_ = title;
    edit_limit_ = kind == EditKind::Message ? client_->max_text(conv_) : kind == EditKind::Name ? kMaxNameBytes : kind == EditKind::Channel ? 30 : 16;
    lv_textarea_set_text(edit_ta_, initial.c_str());
    lv_textarea_set_max_length(edit_ta_, static_cast<uint32_t>(edit_limit_));
    set_view(View::Edit);
    edit_open_tick_ = lv_tick_get();
    enter_text_mode();
    lv_label_set_text(title_, title.c_str());
    lv_label_set_text(action_label_, kind == EditKind::Message ? "Send" : kind == EditKind::Channel ? "Add" : kind == EditKind::Clock ? "Set" : "OK");
    lv_label_set_text(back_label_, "Cancel");
    update_edit_title();
}

void UIMeshZeroPage::update_edit_title()
{
    if (view_ != View::Edit) return;
    std::string t;
    uint32_t color = kGold;
    if (!notice_text_.empty() && !notice_ok_) {
        t = notice_text_;
        color = kRed;
    } else {
        const char *text = lv_textarea_get_text(edit_ta_);
        const size_t bytes = text ? std::strlen(text) : 0;
        const size_t left = bytes >= edit_limit_ ? 0 : edit_limit_ - bytes;
        if (left <= 10 && edit_kind_ != EditKind::Number && edit_kind_ != EditKind::Clock) color = kRed;
        if (edit_kind_ == EditKind::Message) t = truncate_utf8(target_title(), 16) + "  " + std::to_string(left) + " left";
        else if (edit_kind_ == EditKind::Name) t = "Node name  " + std::to_string(left) + " left";
        else if (edit_kind_ == EditKind::Channel) t = "Hashtag channel  " + std::to_string(left) + " left";
        else t = edit_title_;
    }
    lv_label_set_text(title_, truncate_utf8(t, 30).c_str());
    lv_obj_set_style_text_color(title_, lv_color_hex(color), 0);
}

void UIMeshZeroPage::finish_edit(bool accept)
{
    if (view_ != View::Edit) return;
    const std::string text = lv_textarea_get_text(edit_ta_);
    View back_to = edit_kind_ == EditKind::Message ? View::Chat : edit_kind_ == EditKind::Channel ? View::Chats : edit_kind_ == EditKind::Clock ? (is_tab(tab_) ? tab_ : View::Home) : View::Settings;
    if (!accept && edit_kind_ == EditKind::Clock) client_->clock_skip();      // Esc: the board clock stays as it is
    if (accept) {
        if (edit_kind_ == EditKind::Message) {
            if (text.empty()) return;
            const uint32_t seq = conv_.rfind("c:", 0) == 0 ? client_->send_channel(std::atoi(conv_.c_str() + 2), text)
                                                          : [&]() -> uint32_t {
                KeyPrefix p{};
                const ContactRec *c = from_hex(conv_.substr(2), p.data(), 6) ? model_.find_by_prefix(p) : nullptr;
                return c ? client_->send_direct(c->key_hex(), text) : 0;
            }();
            if (seq == 0) {      // refused: keep the text, the reason is in the notice
                if (notice_text_.empty()) { notice_text_ = "Message not sent"; notice_ok_ = false; notice_tick_ = lv_tick_get(); }
                update_edit_title();
                return;
            }
        } else if (edit_kind_ == EditKind::Clock) {
            uint32_t ts = 0;
            if (!parse_local_datetime(text, ts)) {
                notice_text_ = "Use YYYY-MM-DD HH:MM (2025 or later)";
                notice_ok_ = false;
                notice_tick_ = lv_tick_get();
                update_edit_title();
                return;
            }
            client_->clock_set_user(ts);
        } else if (edit_kind_ == EditKind::Channel) {
            std::string name = text;
            if (name.empty() || name[0] != '#') name = "#" + name;
            std::string bad = validate_hashtag(name);
            if (bad.empty() && !client_->add_hashtag_channel(name)) bad = client_->notice().text;
            if (!bad.empty()) {
                notice_text_ = bad;
                notice_ok_ = false;
                notice_tick_ = lv_tick_get();
                update_edit_title();
                return;
            }
        } else if (edit_kind_ == EditKind::Name) {
            const std::string bad = validate_name(text);
            if (!bad.empty()) { notice_text_ = bad; notice_ok_ = false; notice_tick_ = lv_tick_get(); update_edit_title(); return; }
            edit_.name = text;
            dirty_ = true;
        } else {
            // a typed number: parsed, checked with the same rules as the steppers (freq 137-2500, LoRa bandwidths, SF 5-12, CR 5-8, TX power)
            char *end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            RadioSettings t = edit_;
            std::string bad;
            if (end == text.c_str() || *end != '\0') bad = "Type a number";
            else if (num_row_ == 1) t.freq_mhz = std::round(v * 1000.0) / 1000.0;
            else if (num_row_ == 2) {
                bool found = false;
                for (double bw : lora_bandwidths())
                    if (std::fabs(bw - v) < 0.05) { t.bw_khz = bw; found = true; }
                if (!found) bad = "Bandwidth: 7.8 10.4 15.6 20.8 31.25 41.7 62.5 125 250 500";
            } else if (num_row_ == 3) t.sf = static_cast<int>(std::lround(v));
            else if (num_row_ == 4) t.cr = static_cast<int>(std::lround(v));
            else t.tx_power = static_cast<int>(std::lround(v));
            if (bad.empty()) bad = validate_radio(t);
            if (!bad.empty()) {
                notice_text_ = bad;
                notice_ok_ = false;
                notice_tick_ = lv_tick_get();
                update_edit_title();
                return;
            }
            edit_ = t;
            dirty_ = true;
        }
    }
    leave_text_mode();
    lv_label_set_text(back_label_, "< Back");
    set_view(back_to);
    if (back_to == View::Settings) {
        selected_ = edit_kind_ == EditKind::Name ? 0 : num_row_;
        refresh();
    }
}

/* ------------------------------------------------------------------ actions */

void UIMeshZeroPage::activate()
{
    // the Enter / tap that started the app can still be in flight: it must not open anything by itself
    if (lv_tick_elaps(start_tick_) < kStartGuardMs) return;
    // Settings > Touch can turn a tap into the Enter key as well as my own tap handler: the second one is ignored
    if (view_ != View::Edit && last_nav_tick_ != 0 && lv_tick_elaps(last_nav_tick_) < 350) return;
    if (view_ == View::Chats) {
        if (selected_ >= static_cast<int>(item_keys_.size())) return;
        const std::string &key = item_keys_[static_cast<size_t>(selected_)];
        if (key == "+") {
            if (!client_->ready()) {
                notice_text_ = "Connect the radio first";
                notice_ok_ = false;
                notice_tick_ = lv_tick_get();
                refresh();
                return;
            }
            start_edit(EditKind::Channel, "#", "New hashtag channel");
        } else {
            open_chat(key);
        }
    } else if (view_ == View::Contacts) {
        if (selected_ >= static_cast<int>(item_keys_.size())) return;
        const std::string &key = item_keys_[static_cast<size_t>(selected_)];
        const ContactRec *r = model_.find_contact(key);
        if (r && (r->c.type == advtype::kChat || r->c.type == advtype::kNone)) open_chat(Model::conv_direct(r->prefix()));
        else open_detail(key);
    } else if (view_ == View::Settings) {
        settings_activate(selected_);
    } else if (view_ == View::Chat) {
        start_edit(EditKind::Message, "", target_title());
    } else if (view_ == View::Detail) {
        const ContactRec *r = model_.find_contact(detail_key_);
        if (r && r->c.type != advtype::kRepeater && r->c.type != advtype::kSensor) open_chat(Model::conv_direct(r->prefix()));
        else {
            notice_text_ = "Repeaters and sensors take no text messages";
            notice_ok_ = false;
            notice_tick_ = lv_tick_get();
            refresh();
        }
    }
}

/* A short Esc is always Back and never ends the app (nav.hpp). Holding Esc for 3 s is handled by the launcher itself. */
void UIMeshZeroPage::back()
{
    const EscResult r = esc_pressed(static_cast<Screen>(view_), static_cast<Screen>(tab_));
    if (r.cancel_editor) {
        finish_edit(false);
    } else if (r.show_exit_hint) {
        notice(kExitHint, true);
    } else if (r.next != static_cast<Screen>(view_)) {
        set_view(static_cast<View>(r.next));
    }
}

/* ------------------------------------------------------------------ input */

void UIMeshZeroPage::handle_key(lv_event_t *event)
{
    if (text_mode_) return;
    uint32_t key = lv_event_get_key(event);
    key = cp0_keyboard_navigation_alias(key);
    if (key == LV_KEY_UP || key == KEY_UP) move_selection(-1);
    else if (key == LV_KEY_DOWN || key == KEY_DOWN) move_selection(1);
    else if (key == LV_KEY_RIGHT || key == KEY_RIGHT || key == LV_KEY_LEFT || key == KEY_LEFT) {
        const int d = (key == LV_KEY_RIGHT || key == KEY_RIGHT) ? 1 : -1;
        if (view_ == View::Settings) settings_adjust(selected_, d);
        else next_tab(d);
    } else if (key == LV_KEY_ENTER || key == KEY_ENTER) activate();
    else if (key == LV_KEY_ESC || key == KEY_ESC) back();
    else if (key == KEY_TAB || key == '\t') next_tab(1);
}

void UIMeshZeroPage::handle_text_key(const struct key_item *item)
{
    if (item->key_state == KBD_KEY_RELEASED) return;
    const uint32_t code = item->key_code;
    // an Enter that was already on its way when the editor opened (a double Enter) must not accept the empty editor
    if ((code == KEY_ENTER || code == KEY_KPENTER) && lv_tick_elaps(edit_open_tick_) < 350) return;
    if (code == KEY_ESC) {
        finish_edit(false);
    } else if (code == KEY_ENTER || code == KEY_KPENTER) {
        if (item->key_state == KBD_KEY_PRESSED) finish_edit(true);
    } else if (code == KEY_BACKSPACE || is_delete_key(code)) {
        lv_textarea_delete_char(edit_ta_);
    } else if (code == KEY_LEFT) {
        lv_textarea_cursor_left(edit_ta_);
    } else if (code == KEY_RIGHT) {
        lv_textarea_cursor_right(edit_ta_);
    } else if (item->utf8[0] && static_cast<unsigned char>(item->utf8[0]) >= 0x20) {
        std::string add = item->utf8;
        if (edit_kind_ == EditKind::Channel && add == " ") return;      // no spaces in a channel name
        if (edit_kind_ == EditKind::Clock && !(add.size() == 1 && (std::isdigit(static_cast<unsigned char>(add[0])) || add[0] == '-' || add[0] == ':' || add[0] == ' '))) return;
        if (edit_kind_ == EditKind::Number) {         // digits, a decimal point (a comma too) and a minus sign
            if (add == ",") add = ".";
            if (!(add.size() == 1 && (std::isdigit(static_cast<unsigned char>(add[0])) || add[0] == '.' || add[0] == '-'))) return;
        }
        const char *cur = lv_textarea_get_text(edit_ta_);
        if ((cur ? std::strlen(cur) : 0) + add.size() > edit_limit_) return;
        lv_textarea_add_text(edit_ta_, add.c_str());
    }
    update_edit_title();
}

void UIMeshZeroPage::handle_raw_key(lv_event_t *event)
{
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(event));
    if (!item) return;
    if (text_mode_ && view_ == View::Edit) {
        handle_text_key(item);
        lv_event_stop_processing(event);
        return;
    }
    // Delete: the launcher sends the raw evdev code 111 here (LVGL's own LV_KEY_DEL, 127, only reaches the keypad path and is not
    // used: acting on both would confirm the two-tap removal by itself). Letters are matched by raw code.
    if (item->key_state == KBD_KEY_PRESSED && is_delete_key(item->key_code) && view_ == View::Chat) {
        request_remove();
        return;
    }
    if (item->key_state != KBD_KEY_PRESSED || !is_tab(view_)) return;
    switch (item->key_code) {      // letters by raw code: W/E/R/T (17-20) are never used, they equal the arrow keys
    case KEY_A: client_->send_advert(true); break;
    case KEY_D: client_->send_advert(false); break;
    case KEY_U: if (view_ == View::Contacts) client_->refresh_contacts(); break;
    case KEY_S: if (view_ == View::Settings) settings_activate(6); break;
    case KEY_V: if (view_ == View::Settings) settings_activate(7); break;
    default: break;
    }
    if (view_ == View::Chat && item->key_code == KEY_M) activate();
}

void UIMeshZeroPage::key_event_cb(lv_event_t *event)
{
    static_cast<UIMeshZeroPage *>(lv_event_get_user_data(event))->handle_key(event);
}

void UIMeshZeroPage::keyboard_event_cb(lv_event_t *event)
{
    static_cast<UIMeshZeroPage *>(lv_event_get_user_data(event))->handle_raw_key(event);
}

void UIMeshZeroPage::row_event_cb(lv_event_t *event)
{
    auto *self = static_cast<UIMeshZeroPage *>(lv_event_get_user_data(event));
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    lv_point_t point = {0, 0};
    if (indev) lv_indev_get_point(indev, &point);
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (code == LV_EVENT_PRESSED) {
        self->press_y_ = point.y;
        self->swiped_ = false;
        self->valid_press_ = lv_tick_elaps(self->start_tick_) >= kStartGuardMs;
        lv_area_t a;
        lv_obj_get_coords(target, &a);
        const int w = std::max(1, a.x2 - a.x1 + 1);
        self->press_fx_ = static_cast<float>(point.x - a.x1) / static_cast<float>(w);
    } else if (code == LV_EVENT_RELEASED) {
        const int dy = point.y - self->press_y_;
        if (std::abs(dy) >= 10) {   // a vertical drag scrolls the list
            self->swiped_ = true;
            const int rows = std::max(1, std::abs(dy) / kRowH);
            const int max_offset = std::max(0, static_cast<int>(self->items_.size()) - kRows);
            self->offset_ = std::clamp(self->offset_ + (dy < 0 ? rows : -rows), 0, max_offset);
            self->refresh();
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (self->swiped_ || !self->valid_press_) {
            self->swiped_ = false;
            return;
        }
        const int row = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
        const int index = self->offset_ + row;
        if (index >= static_cast<int>(self->items_.size())) return;
        self->selected_ = index;
        if (self->view_ == View::Settings) {
            if (index >= 1 && index <= 5 && self->press_fx_ >= 0.72f) self->settings_adjust(index, self->press_fx_ >= 0.86f ? 1 : -1);
            else self->settings_activate(index);       // a number or the name: typed on the keyboard; the rest are actions
        } else {
            self->activate();
            if (self->is_tab(self->view_)) self->refresh();
        }
    }
}

void UIMeshZeroPage::button_event_cb(lv_event_t *event)
{
    auto *self = static_cast<UIMeshZeroPage *>(lv_event_get_user_data(event));
    lv_obj_t *t = lv_event_get_target_obj(event);
    if (lv_tick_elaps(self->start_tick_) < kStartGuardMs) return;
    for (int i = 0; i < 4; ++i)
        if (t == self->tab_box_[static_cast<size_t>(i)])
            self->set_view(i == 0 ? View::Home : i == 1 ? View::Chats : i == 2 ? View::Contacts : View::Settings);
    if (t == self->remove_btn_) {
        self->request_remove();
        return;
    }
    if (t == self->back_btn_) self->back();
    else if (t == self->action_btn_) {
        if (self->view_ == View::Edit) self->finish_edit(true);
        else self->activate();
    } else if (t == self->advert_flood_) self->client_->send_advert(true);
    else if (t == self->advert_direct_) self->client_->send_advert(false);
}

void UIMeshZeroPage::timer_cb(lv_timer_t *timer)
{
    static_cast<UIMeshZeroPage *>(lv_timer_get_user_data(timer))->tick();
}
