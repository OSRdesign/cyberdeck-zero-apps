/*
 * SPDX-License-Identifier: MIT
 *
 * 0.7.0, report 036 slice S1: the Repeater page of a repeater or room (App's methods for it) and its login box.
 *
 *   the page     a sub-page over Contacts (a tap on a repeater or room, or its Options > Manage): the header "Contacts > <name>",
 *                the line under it "Repeater  ·  direct, 2 hops  ·  heard 3 min ago"; the bar Log in (Log out once logged in),
 *                Refresh (grey: it comes with the status view of slice S2; a tap says so), More (0.7.2 order: Details, Open in
 *                CLI, Reset path, Forget password when one is saved, last); R or Ctrl+R: Reset path; the rows of ui_repeater.hpp repeater_rows (ACCESS, the clock at the login, the
 *                note of Log out, the grey row of what comes next), read-only (the keys scroll them).
 *   the login    a box (an editor: editor_open_, EditKind::Login) that opens by itself when the page opens from Contacts and this
 *                connection has no session with the node: the password field (masked, 15 bytes at most, the saved password in it,
 *                masked, when there is one), Remember on this deck (on), Cancel / Guest (a blank password) / Log in, and "No
 *                known route: the login is sent to the whole mesh" without a path. Enter logs in, Esc cancels, Tab goes to
 *                Remember (Space switches it). Without a physical keyboard the field takes the keys after a tap only (the
 *                on-screen keyboard by the shared rule; a saved password needs no typing). While the request waits, the message
 *                line says "Waiting… up to N s" (Client::remote_state()), Log in and Guest are dimmed, Cancel (or Esc) stops the
 *                wait. The result: a success closes the box (the caption line says the role, and "password saved"); a failure
 *                stays in the box's message line (a timeout: "No answer. Wrong password, or <name> is out of reach."). No
 *                automatic retry. The password goes to Client::start_login and is cleared from the field at once; the saved one
 *                comes back into the field after a failure (it is still saved). It is never logged nor shown.
 *   0.7.2        the box shows the route under its title ("Route: direct, 2 hops"; flood, in Gold: "Route: flood: the
 *                login goes to the whole mesh") and a Reset path button at the right of the Remember row (ROW high, only when a route is known;
 *                Ctrl+R): RESET_PATH,
 *                no confirm box, the answer in the box's line ("Path reset: the next message goes by flood"), the route
 *                lines follow (flood).
 *   leaving      the page's Esc, another page, another board: a wait of this page is cancelled (Client::cancel_remote).
 */

#include "app.hpp"
#include "mesh_app.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "protocol.hpp"

#include <algorithm>
#include <cstring>

using namespace meshzero;

namespace meshhop {

namespace {

constexpr uint32_t kRepText = CP0_UI_LIST_TEXT_RGB;
constexpr uint32_t kRepMuted = CP0_UI_LIST_MUTED_RGB;
constexpr uint32_t kRepWarn = CP0_UI_LIST_WARN_RGB;
constexpr uint32_t kRepSection = CP0_UI_LIST_SECTION_RGB;
constexpr uint32_t kRepButton = CP0_UI_LIST_BAR_RGB;
constexpr uint32_t kRepPressed = CP0_UI_OSK_KEY_PRESSED_RGB;
constexpr uint32_t kRepAccent = CP0_UI_LIST_ACCENT_RGB;
constexpr uint32_t kRepOnAccent = 0x101010;
constexpr uint32_t kLoginWaitPollMs = 250;
constexpr const char *kGuestHint = "A blank password logs in as a guest (Guest).";

void hide(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

int lh(const lv_font_t *f)
{
    return static_cast<int>(lv_font_get_line_height(f));
}

int tw(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t sz{};
    lv_text_get_size(&sz, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

int lines_in(const std::string &text, const lv_font_t *font, int w)
{
    if (text.empty()) return 1;
    lv_point_t sz{};
    lv_text_get_size(&sz, text.c_str(), font, 0, 0, std::max(1, w), LV_TEXT_FLAG_NONE);
    return std::max(1, (static_cast<int>(sz.y) + lh(font) - 1) / lh(font));
}

uint32_t sig_of(uint32_t h, const std::string &s)
{
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return (h ^ 0xFF) * 16777619u;
}

/* The token text sizes in the launcher's Montserrat (the Settings rows' faces), else the builtin one. */
const lv_font_t *face(int px)
{
    if (const lv_font_t *f = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

uint32_t tone_rgb(Tone t)
{
    switch (t) {
    case Tone::Gold: return kRepWarn;
    case Tone::Normal: return kRepText;
    case Tone::Green: return CP0_UI_LIST_OK_RGB;
    case Tone::Blue: return CP0_UI_LIST_ACCENT_RGB;
    case Tone::Red: return CP0_UI_LIST_BAD_RGB;
    case Tone::Muted: break;
    }
    return kRepMuted;
}

/* The Remember row of the login box (a tap switches it). */
void remember_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (App *app = static_cast<App *>(lv_event_get_user_data(e))) app->button_clicked(kTagLoginRemember);
}

bool parse_key(const std::string &hex, PubKey &out)
{
    return hex.size() == 64 && from_hex(hex, out.data(), out.size());
}

} // namespace

/* ------------------------------------------------------------------ the page */

void App::build_repeater()
{
    const Geo g = geo();
    rep_panel_ = lv_obj_create(wide_);
    lv_obj_remove_style_all(rep_panel_);
    lv_obj_remove_flag(rep_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rep_panel_, 0, 0);
    lv_obj_set_size(rep_panel_, g.w, g.h);
    lv_obj_set_style_bg_color(rep_panel_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(rep_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(rep_panel_, LV_OBJ_FLAG_CLICKABLE);         // an opaque sub-page: it takes the taps meant for the list under it
    const int bar_y = g.s / 2, list_y = bar_y + g.bar + g.s;
    rep_bar_.create(rep_panel_, g.x, bar_y, g.cw, g.bar, f_body_, f_caption_);
    RowList::Config cfg;
    cfg.max_cells = 2;
    cfg.max_boxes = 0;
    cfg.flat = true;
    cfg.marks = true;
    cfg.mark_layout = settings_list_layout(list_y, std::max(1, g.h - list_y));
    cfg.min_row_h = cfg.mark_layout.section_h;
    rep_list_.create(rep_panel_, g.lx, list_y, g.lw, std::max(1, g.h - list_y), cfg);
    hide(rep_panel_, true);
}

void App::layout_repeater()
{
    if (!rep_panel_) return;
    const Geo g = geo();
    const int list_y = g.s / 2 + g.bar + g.s;
    rep_list_.resize(std::max(1, g.h - list_y));
    rep_sig_ = 0;
}

void App::open_repeater(const std::string &key_hex, bool auto_login)
{
    const ContactRec *r = model_.find_contact(key_hex);
    if (!r) return;
    if (nav_.repeater_open && rep_key_ != key_hex) close_repeater();
    rep_key_ = key_hex;
    nav_.repeater_open = true;
    nav_.detail_open = false;
    rep_sig_ = 0;
    rep_seen_result_ = client_->remote_result().id;            // only the results of what this page asks from now on
    rep_seen_reset_ = client_->path_reset_result().id;
    rep_reset_asked_ = false;
    rep_list_.scroll_to_top();
    if (log_) log_->line("repeater page: " + key_hex.substr(0, 8) + (auto_login ? " (from Contacts)" : ""));
    // "opens by itself on entering the page when not logged in" (connected only: offline a box would only say so)
    if (auto_login && client_->ready() && !client_->remote_session(r->c.key)) post([this] {
        if (nav_.repeater_open && !editor_open_ && !nav_.popup_open) open_login_box();
    });
    invalidate();
}

void App::close_repeater()
{
    if (editor_open_ && edit_kind_ == EditKind::Login) close_login_box();
    PubKey pk{};
    if (client_->remote_busy() && parse_key(rep_key_, pk) && client_->remote_state().key == pk && client_->remote_state().op == Client::RemoteOp::Login)
        client_->cancel_remote();                                 // leaving the page stops its wait (no answer is awaited for it any more)
    nav_.repeater_open = false;
    hide(rep_panel_, true);
    rep_key_.clear();
    rep_req_ = 0;
    invalidate();
}

bool App::repeater_caption(std::string &text) const
{
    const ContactRec *r = model_.find_contact(rep_key_);
    if (!r) {
        text = "This contact is gone from the board";
        return true;
    }
    text = repeater_status_line(r->c.type, path_hops(r->c.out_path_len), r->last_heard());
    return true;
}

void App::render_repeater()
{
    const ContactRec *r = model_.find_contact(rep_key_);
    const Client::RemoteSession *ses = r ? client_->remote_session(r->c.key) : nullptr;
    std::vector<ActionBar::CellSpec> cells;
    cells.push_back({kTagRepLogin, ses ? "Log out" : "Log in", ""});
    ActionBar::CellSpec refresh{kTagRepRefresh, "Refresh", ""};
    refresh.dim = true;                                            // the status view of slice S2: not there yet
    cells.push_back(refresh);
    cells.push_back({kTagRepMore, "More", ""});
    rep_bar_.set_cells(cells);

    RepeaterView v;
    v.room = r && r->c.type == advtype::kRoom;
    v.session = ses;
    v.password_saved = passwords_ && passwords_->remembered(rep_key_);
    std::vector<RepeaterRow> rows = r ? repeater_rows(v) : std::vector<RepeaterRow>{{RepeaterRow::Note, "This contact is gone from the board.", "", Tone::Muted}};

    const Geo g = geo();
    const cp0_ui_list_layout_t l = settings_list_layout(0, g.h);
    const lv_font_t *lf = face(label_px_), *vf = face(value_px_), *sf = face(l.section_px);
    const int tw_all = g.lw - 2 * g.inset, pad = std::max(4, g.s / 2);
    uint32_t sig = sig_of(0, std::to_string(g.h) + "/" + std::to_string(tw_all));
    for (const RepeaterRow &row : rows) sig = sig_of(sig_of(sig, row.label), row.value + std::to_string(static_cast<int>(row.tone)));
    if (sig == rep_sig_ || plat_.touching()) return;
    rep_sig_ = sig;
    std::vector<RowSpec> specs;
    for (size_t i = 0; i < rows.size(); ++i) {
        const RepeaterRow &row = rows[i];
        RowSpec s;
        s.id = std::to_string(i);
        s.selectable = false;                                      // read-only rows: the keys scroll them
        if (row.kind == RepeaterRow::Section) {
            s.h = l.section_h;
            std::string up = row.label;
            for (char &c : up)
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            s.cells.push_back({up, kRepSection, g.inset, tw_all, LV_TEXT_ALIGN_LEFT, sf, (s.h - lh(sf)) / 2});
        } else if (row.kind == RepeaterRow::Note) {
            const int n = lines_in(row.label, f_caption_, tw_all);
            s.h = std::max(l.row_h, n * lh(f_caption_) + 2 * pad);
            Cell c{row.label, tone_rgb(row.tone), g.inset, tw_all, LV_TEXT_ALIGN_LEFT, f_caption_, pad};
            c.lines = n;
            s.cells.push_back(c);
        } else {
            s.h = l.row_h;
            s.aff = Aff::Info;
            const int vw = row.value.empty() ? 0 : std::min(tw(row.value, vf) + 2, tw_all / 2 + tw_all / 4);
            s.cells.push_back({row.label, kRepText, g.inset, std::max(1, tw_all - (vw ? vw + l.gap : 0)), LV_TEXT_ALIGN_LEFT, lf, (s.h - lh(lf)) / 2});
            if (vw) s.cells.push_back({row.value, tone_rgb(row.tone), g.inset + tw_all - vw, vw, LV_TEXT_ALIGN_RIGHT, vf, (s.h - lh(vf)) / 2});
        }
        specs.push_back(std::move(s));
    }
    rep_list_.set(std::move(specs), -1, true);
}

/* Log in (the box), or Log out: the session of this connection is forgotten and LOGOUT goes to the board (the node itself keeps
 * this deck in its access list: the page's note says until when). */
void App::repeater_login_or_out()
{
    const ContactRec *r = model_.find_contact(rep_key_);
    if (!r) return;
    if (!client_->remote_session(r->c.key)) {
        open_login_box();
        return;
    }
    if (client_->logout(r->c.key) == 0) {
        notice(login_error_text(client_->remote_result(), r->c.name), false);
        return;
    }
    rep_seen_result_ = client_->remote_result().id;
    rep_sig_ = 0;
    notice("Logged out on this deck (" + truncate_ellipsis(r->c.name, 20) + " still lists it)", true);
    invalidate();
}

/* More (0.7.2 order): Details, Open in CLI, Reset path, Forget password (only when one is saved) last. */
void App::repeater_more()
{
    if (nav_.popup_open || editor_open_) return;
    const ContactRec *r = model_.find_contact(rep_key_);
    if (!r) return;
    const std::vector<RepeaterMoreItem> more = repeater_more_items(passwords_ && passwords_->remembered(rep_key_));
    std::vector<std::string> items;
    std::vector<Aff> affs;
    for (const RepeaterMoreItem &m : more) {
        items.push_back(m.label);
        affs.push_back(m.aff);
    }
    const std::string key = rep_key_;
    open_menu("More", items, [this, key, more](int i) {
        if (i < 0 || i >= static_cast<int>(more.size())) return;
        switch (more[static_cast<size_t>(i)].action) {
        case RepeaterMore::Details: open_detail(key); break;
        case RepeaterMore::OpenCli: open_cli_for(key); break;
        case RepeaterMore::ResetPath:
            if (nav_.repeater_open && rep_key_ == key) repeater_reset_path();
            break;
        case RepeaterMore::ForgetPassword: {
            const bool ok = passwords_ && passwords_->forget(key);
            notice(ok ? "Password forgotten (the CLI's login asks it again)" : "The password could not be forgotten", ok);
            rep_sig_ = 0;
            invalidate();
            break;
        }
        }
    }, {}, affs);
}

/* Reset path: no confirm box (the owner's rule); the answer comes through repeater_tick (or at once when refused). */
void App::repeater_reset_path()
{
    PubKey pk{};
    if (!parse_key(rep_key_, pk) || !model_.find_contact(rep_key_)) return;
    if (log_) log_->line("repeater page: reset path of " + rep_key_.substr(0, 8));
    rep_reset_asked_ = true;
    if (!client_->reset_path(pk)) {                                // refused at once: the result says why
        rep_reset_asked_ = false;
        rep_seen_reset_ = client_->path_reset_result().id;
        show_path_reset(client_->path_reset_result());
        return;
    }
    if (editor_open_ && edit_kind_ == EditKind::Login) {
        if (login_reset_btn_) lv_obj_add_state(login_reset_btn_, LV_STATE_DISABLED);
        login_note_text("Resetting the path\xE2\x80\xA6", kRepMuted);
    }
}

void App::show_path_reset(const Client::PathResetResult &res)
{
    const ContactRec *r = model_.find_contact(rep_key_);
    const std::string name = r ? (r->c.name.empty() ? to_hex(r->prefix()) : r->c.name) : std::string("the node");
    const std::string text = path_reset_text(res, name);
    rep_sig_ = 0;
    invalidate();
    if (text.empty()) return;
    if (editor_open_ && edit_kind_ == EditKind::Login) {
        login_route_refresh();
        login_note_text(text, res.ok ? CP0_UI_LIST_OK_RGB : CP0_UI_LIST_BAD_RGB);   // in the note's place: the body keeps its height
    } else {
        notice(text, res.ok);
    }
}

/* The login box's Route line, the flood note and the Reset path button, from the contact as the model holds it now. */
void App::login_route_refresh()
{
    if (!ed_dialog_ || !login_route_) return;
    const ContactRec *r = model_.find_contact(rep_key_);
    const int hops = r ? path_hops(r->c.out_path_len) : -1;
    lv_label_set_text(login_route_, login_route_line(hops).c_str());
    lv_obj_set_style_text_color(login_route_, lv_color_hex(hops < 0 ? kRepWarn : kRepMuted), 0);
    if (login_reset_btn_) {
        hide(login_reset_btn_, !login_offers_reset(hops));
        if (client_->path_reset_busy()) lv_obj_add_state(login_reset_btn_, LV_STATE_DISABLED);
        else lv_obj_remove_state(login_reset_btn_, LV_STATE_DISABLED);
    }
    ed_dialog_->relayout();
    compact_dialog(*ed_dialog_);
}

void App::key_repeater(const KeyEvent &e)
{
    const int step = std::max(1, geo().list_row);
    switch (e.key) {
    case Key::Up: rep_list_.scroll_by(-step); return;
    case Key::Down: rep_list_.scroll_by(step); return;
    case Key::PageUp: rep_list_.scroll_by(-4 * step); return;
    case Key::PageDown: rep_list_.scroll_by(4 * step); return;
    case Key::Home: rep_list_.scroll_to_top(); return;
    default: break;
    }
    if (e.repeat) return;
    const ContactRec *r = model_.find_contact(rep_key_);
    const bool in = r && client_->remote_session(r->c.key);
    if (e.key == Key::Enter) {
        if (!in) open_login_box();
        return;
    }
    if (e.key != Key::Char || e.alt || e.text.size() != 1) return;
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
    if (c == 'r') {                                                // R or Ctrl+R (the login box's shortcut): Reset path
        repeater_reset_path();
        return;
    }
    if (e.ctrl) return;
    if (c == 'l') repeater_login_or_out();
    else if (c == 'm') repeater_more();
}

/* Every tick: the wait of the login box (its message line), the result of what this page asked. */
void App::repeater_tick(uint64_t now)
{
    if (editor_open_ && edit_kind_ == EditKind::Login && login_waiting_ && now - login_msg_ms_ >= kLoginWaitPollMs) {
        const std::string w = remote_wait_text(client_->remote_state(), now);
        if (!w.empty()) login_message(w, false);
    }
    const Client::PathResetResult &pr = client_->path_reset_result();
    if (pr.id != rep_seen_reset_) {
        rep_seen_reset_ = pr.id;
        if (rep_reset_asked_) {
            rep_reset_asked_ = false;
            show_path_reset(pr);
        }
    }
    const Client::RemoteResult &res = client_->remote_result();
    if (res.id == rep_seen_result_) return;
    rep_seen_result_ = res.id;
    rep_sig_ = 0;
    invalidate();
    if (rep_req_ == 0 || res.req != rep_req_ || res.op != Client::RemoteOp::Login) return;
    rep_req_ = 0;
    const ContactRec *r = model_.find_contact(rep_key_);
    const std::string name = r ? (r->c.name.empty() ? to_hex(r->prefix()) : r->c.name) : std::string("the node");
    const bool box = editor_open_ && edit_kind_ == EditKind::Login;
    login_waiting_ = false;
    if (res.ok) {
        if (box) close_login_box();
        notice(login_success_text(res, name), true);
        return;
    }
    const std::string why = login_error_text(res, name);
    if (box) {
        login_prefill();                                           // the saved password is still saved: it is back in the field
        login_typing_ = !osk_may_show();                           // a keyboard types again at once; by touch: a tap on the field
        login_message(why, res.error != Client::RemoteError::Cancelled);
        login_refresh();
        sync_osk();
    } else if (res.error != Client::RemoteError::Cancelled) {
        notice(why, false);
    }
}

std::string App::repeater_debug() const
{
    const ContactRec *r = nav_.repeater_open ? model_.find_contact(rep_key_) : nullptr;
    const Client::RemoteSession *ses = r ? client_->remote_session(r->c.key) : nullptr;
    const Client::RemoteState &st = client_->remote_state();
    const char *phase = st.phase == Client::RemoteState::Phase::Idle ? "idle" : st.phase == Client::RemoteState::Phase::Sending ? "sending" : "waiting";
    const bool box = editor_open_ && edit_kind_ == EditKind::Login;
    const char *pw = !box || !login_ta_ ? "-" : !lv_textarea_get_text(login_ta_)[0] ? "empty" : login_prefilled_ && login_fresh_ ? "saved" : "typed";
    std::string role = ses ? remote_role_name(ses->role) : "none";
    for (char &c : role) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return std::string(" rep=") + (nav_.repeater_open ? "1" : "0") + " rep_name='" + (r ? r->c.name : std::string()) + "'" + " rep_role=" + role +
           " pw_saved=" + (r && passwords_ && passwords_->remembered(rep_key_) ? "1" : "0") + " login=" + (box ? "1" : "0") +
           " login_wait=" + (login_waiting_ ? "1" : "0") + " login_pw=" + pw + " login_remember=" + (login_remember_ ? "1" : "0") +
           " login_focus=" + std::to_string(login_focus_) + " login_msg='" + (box ? login_msg_ : std::string()) + "'" + " remote=" + phase +
           " sessions=" + std::to_string(client_->remote_sessions().size()) +
           " rep_route='" + (r ? repeater_route_text(path_hops(r->c.out_path_len)) : std::string()) + "'" +
           " login_route='" + (box && login_route_ ? std::string(lv_label_get_text(login_route_)) : std::string()) + "'" +
           " login_reset=" + (box && login_reset_btn_ && !lv_obj_has_flag(login_reset_btn_, LV_OBJ_FLAG_HIDDEN) ? "1" : "0") +
           " path_reset=" + (client_->path_reset_busy() ? "busy" : std::to_string(client_->path_reset_result().id));
}

/* ------------------------------------------------------------------ the login box */

void App::open_login_box()
{
    if (editor_open_ || nav_.popup_open) return;
    const ContactRec *r = model_.find_contact(rep_key_);
    if (!r) return;
    if (!client_->ready()) {
        notice("Connect to a node first (Home > Node)", false);
        return;
    }
    const std::string name = r->c.name.empty() ? to_hex(r->prefix()) : r->c.name;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    {
        cp0::ui::DialogSpec spec;
        spec.title = "Log in to " + truncate_ellipsis(name, 24);
        spec.page = shell_.get();
        spec.message_line = true;
        spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { if (editor_open_ && edit_kind_ == EditKind::Login) login_cancel(); }); }});
        spec.buttons.push_back({"Guest", cp0::ui::ButtonRole::Normal, [this] { post([this] { if (editor_open_ && edit_kind_ == EditKind::Login) login_submit(true); }); }});
        spec.buttons.push_back({"Log in", cp0::ui::ButtonRole::Primary, [this] { post([this] { if (editor_open_ && edit_kind_ == EditKind::Login) login_submit(false); }); }});
        ed_dialog_ = std::make_unique<cp0::ui::Dialog>(shell_->root(), layout_, std::move(spec));
        cp0::ui::Dialog &d = *ed_dialog_;
        const int cw = d.geometry().content_w;
        const int row_h = m->tok[CP0_TOK_ROW_H];
        // 0.7.2: the route under the title (the page's own line is under the box): one caption line, grey ("Route: direct, 2 hops"),
        // Gold with no route ("Route: flood: the login goes to the whole mesh"); its Reset path button sits at the right of the Remember row
        // (ROW high, 7 mm): a row of its own would make the body scroll at 640x480
        login_route_ = dialog_label(d, "", true, kRepMuted);
        login_ta_ = make_field(d.body(), kTagLoginField, 64);
        lv_textarea_set_password_mode(login_ta_, true);
        lv_textarea_set_password_show_time(login_ta_, 0);
        lv_textarea_set_placeholder_text(login_ta_, "Password (blank: guest)");
        // Remember on this deck: a row with a check box (a tap switches it; Tab, then Space, with a keyboard); 0.7.2: Reset path at
        // its right, when a route is known
        lv_obj_t *rem_line = lv_obj_create(d.body());
        lv_obj_remove_style_all(rem_line);
        lv_obj_remove_flag(rem_line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(rem_line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(rem_line, cw, row_h);
        const std::string btn_text = "Reset path";
        const int gap = m->tok[CP0_TOK_SPACE_M];
        const int bw = std::min(cw / 2, tw(btn_text, f_ui_) + 2 * gap);
        login_reset_btn_ = make_button(rem_line, cw - bw, 0, bw, row_h, btn_text.c_str(), kTagLoginResetPath);
        lv_obj_set_style_opa(login_reset_btn_, LV_OPA_40, LV_STATE_DISABLED);
        login_check_row_ = lv_obj_create(rem_line);
        lv_obj_remove_style_all(login_check_row_);
        lv_obj_remove_flag(login_check_row_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(login_check_row_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(login_check_row_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(login_check_row_, cw - bw - gap, row_h);
        lv_obj_set_style_radius(login_check_row_, m->tok[CP0_TOK_RADIUS_S], 0);
        lv_obj_set_style_bg_color(login_check_row_, lv_color_hex(kRepPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(login_check_row_, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(login_check_row_, lv_color_hex(kRepText), 0);
        lv_obj_add_event_cb(login_check_row_, remember_cb, LV_EVENT_CLICKED, this);
        const int box = std::max(12, lh(f_body_));
        login_check_ = lv_obj_create(login_check_row_);
        lv_obj_remove_style_all(login_check_);
        lv_obj_remove_flag(login_check_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(login_check_, box, box);
        lv_obj_set_pos(login_check_, 4, (row_h - box) / 2);
        lv_obj_set_style_radius(login_check_, 4, 0);
        lv_obj_set_style_border_width(login_check_, 2, 0);
        lv_obj_set_style_border_color(login_check_, lv_color_hex(kRepAccent), 0);
        lv_obj_set_style_bg_color(login_check_, lv_color_hex(kRepAccent), 0);
        login_check_mark_ = lv_label_create(login_check_);
        lv_label_set_text(login_check_mark_, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(login_check_mark_, cp0_ui_font_px(std::max(12, box - 6)), 0);
        lv_obj_set_style_text_color(login_check_mark_, lv_color_hex(kRepOnAccent), 0);
        lv_obj_center(login_check_mark_);
        lv_obj_t *lbl = lv_label_create(login_check_row_);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(lbl, "Remember on this deck");
        lv_obj_set_style_text_font(lbl, f_body_, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kRepText), 0);
        lv_obj_set_pos(lbl, 4 + box + m->tok[CP0_TOK_SPACE_M], (row_h - lh(f_body_)) / 2);
        login_note_ = dialog_label(d, kGuestHint, true, kRepMuted);
        d.relayout();
        compact_dialog(d);
        add_scroll_cue(d);
    }
    edit_kind_ = EditKind::Login;
    editor_open_ = true;
    nav_.editor_open = true;
    edit_open_ms_ = mono_ms();
    login_remember_ = true;
    login_waiting_ = false;
    login_focus_ = 0;
    login_msg_.clear();
    login_prefill();
    login_route_refresh();
    // a keyboard types at once; by touch the field takes the keys after a tap (a saved password needs none)
    login_typing_ = !osk_may_show();
    if (!keyboard_ready() && !osk_may_show() && !login_prefilled_) login_message("Keyboard needed for a password: wake the Bluetooth keyboard (Guest needs none)", true);
    login_refresh();
    sync_osk();
    if (log_) log_->line("repeater page: the login box of " + rep_key_.substr(0, 8) + (login_prefilled_ ? " (a saved password)" : ""));
    invalidate();
}

/* The saved password into the field (masked), the first character typed replaces it; nothing when none is saved. */
void App::login_prefill()
{
    login_prefilled_ = false;
    login_fresh_ = false;
    if (!login_ta_) return;
    lv_textarea_set_text(login_ta_, "");
    if (!passwords_) return;
    std::optional<std::string> pw = passwords_->read(rep_key_);
    if (!pw || pw->size() > kMaxLoginPassword) return;
    lv_textarea_set_text(login_ta_, pw->c_str());
    lv_textarea_set_cursor_pos(login_ta_, LV_TEXTAREA_CURSOR_LAST);
    for (char &c : *pw) *static_cast<volatile char *>(&c) = 0;
    login_prefilled_ = true;
    login_fresh_ = true;
}

/* The box's status: the wait (and Cancelled) on the dialog's message line (one line); why a try failed in the note under Remember,
 * red and wrapped (the timeout's text is longer than one line at 640x480: it must be read whole); nothing: the guest hint again. */
void App::login_message(const std::string &text, bool error)
{
    login_msg_ = text;
    login_msg_ms_ = mono_ms();
    if (!ed_dialog_ || !editor_open_ || edit_kind_ != EditKind::Login) return;
    ed_dialog_->set_message(error ? std::string() : text, false);
    if (login_note_) {
        lv_label_set_text(login_note_, error ? text.c_str() : kGuestHint);
        lv_obj_set_style_text_color(login_note_, lv_color_hex(error ? CP0_UI_LIST_BAD_RGB : kRepMuted), 0);
    }
    ed_dialog_->relayout();
    compact_dialog(*ed_dialog_);
}

/* 0.7.2: Reset path's line in the box: the note under Remember (its own height, so the body does not scroll at 640x480). */
void App::login_note_text(const std::string &text, uint32_t rgb)
{
    login_msg_ = text;
    login_msg_ms_ = mono_ms();
    if (!ed_dialog_ || !login_note_) return;
    ed_dialog_->set_message(std::string(), false);
    lv_label_set_text(login_note_, text.c_str());
    lv_obj_set_style_text_color(login_note_, lv_color_hex(rgb), 0);
    ed_dialog_->relayout();
    compact_dialog(*ed_dialog_);
}

void App::login_refresh()
{
    if (!ed_dialog_ || !login_ta_) return;
    style_field(login_ta_, login_focus_ == 0 && login_typing_ && !login_waiting_);
    hide(login_check_mark_, !login_remember_);
    lv_obj_set_style_bg_opa(login_check_, login_remember_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(login_check_row_, login_focus_ == 1 ? 3 : 0, 0);
    ed_dialog_->set_button_enabled(1, !login_waiting_);                // Guest
    ed_dialog_->set_button_enabled(2, !login_waiting_);                // Log in
    ed_dialog_->relayout();
    compact_dialog(*ed_dialog_);
    if (login_typing_ && login_focus_ == 0 && osk_inset_ > 0) lv_obj_scroll_to_view_recursive(login_ta_, LV_ANIM_OFF);   // above the keyboard
}

void App::close_login_box()
{
    if (!editor_open_ || edit_kind_ != EditKind::Login) return;
    if (login_waiting_) {
        login_waiting_ = false;
        rep_req_ = 0;
        client_->cancel_remote();                                       // the box goes: its wait with it
    }
    release_osk();                                                      // before its field goes with the dialog
    if (login_ta_) lv_textarea_set_text(login_ta_, "");
    editor_open_ = false;
    nav_.editor_open = false;
    login_ta_ = login_check_ = login_check_mark_ = login_check_row_ = login_note_ = nullptr;
    login_route_ = login_reset_btn_ = nullptr;
    login_typing_ = false;
    login_prefilled_ = login_fresh_ = false;
    login_msg_.clear();
    ed_dialog_.reset();
    invalidate();
}

/* Cancel / Esc: while a login waits it stops the wait (the box stays: Cancel again closes it), else the box closes. */
void App::login_cancel()
{
    if (login_waiting_) {
        client_->cancel_remote();                                       // the result (Cancelled) comes through repeater_tick
        return;
    }
    close_login_box();
}

/* Log in (the field's password, blank = a guest login) or Guest: the request goes, the field is cleared at once. */
void App::login_submit(bool guest)
{
    if (!editor_open_ || edit_kind_ != EditKind::Login || login_waiting_ || !login_ta_) return;
    if (mono_ms() - edit_open_ms_ < 350) return;                        // an Enter already on its way must not log in
    PubKey key{};
    if (!parse_key(rep_key_, key)) return;
    std::string pw = guest ? std::string() : std::string(lv_textarea_get_text(login_ta_));
    lv_textarea_set_text(login_ta_, "");
    login_fresh_ = login_prefilled_ = false;
    rep_req_ = client_->start_login(key, pw, guest, login_remember_);
    for (char &c : pw) *static_cast<volatile char *>(&c) = 0;
    pw.clear();
    if (rep_req_ == 0) {                                                // refused at once: remote_result() says why
        const ContactRec *r = model_.find_contact(rep_key_);
        rep_seen_result_ = client_->remote_result().id;
        login_prefill();
        login_message(login_error_text(client_->remote_result(), r ? r->c.name : std::string("the node")), true);
        login_refresh();
        return;
    }
    login_waiting_ = true;
    login_message(remote_wait_text(client_->remote_state(), mono_ms()), false);
    login_refresh();
    release_osk();                                                      // the answer shows: the on-screen keyboard goes
    login_typing_ = false;
    sync_osk();
    invalidate();
}

void App::login_key(const KeyEvent &e)
{
    if (e.key == Key::Char && e.ctrl && !e.alt && (e.text == "r" || e.text == "R")) {    // Ctrl+R: Reset path (R alone is typed)
        if (!e.repeat && !login_waiting_) {
            const ContactRec *r = model_.find_contact(rep_key_);
            if (r && login_offers_reset(path_hops(r->c.out_path_len))) repeater_reset_path();
        }
        return;
    }
    switch (e.key) {
    case Key::Esc:
        if (!e.repeat) login_cancel();
        return;
    case Key::Enter:
        if (!e.repeat) login_submit(false);
        return;
    case Key::Tab:
    case Key::BackTab:
        if (e.repeat || login_waiting_) return;
        login_focus_ = login_focus_ == 0 ? 1 : 0;
        if (login_focus_ == 0) login_typing_ = true;
        login_refresh();
        sync_osk();
        return;
    default: break;
    }
    if (login_waiting_ || !login_ta_) return;
    if (login_focus_ == 1) {                                            // Remember: Space switches it
        if (e.key == Key::Char && e.text == " " && !e.repeat) {
            login_remember_ = !login_remember_;
            login_refresh();
        }
        return;
    }
    login_typing_ = true;
    switch (e.key) {
    case Key::Backspace:
        if (login_fresh_) lv_textarea_set_text(login_ta_, "");          // the saved password goes as a whole
        else lv_textarea_delete_char(login_ta_);
        login_fresh_ = login_prefilled_ = false;
        break;
    case Key::Delete:
        if (login_fresh_) lv_textarea_set_text(login_ta_, "");
        else lv_textarea_delete_char_forward(login_ta_);
        login_fresh_ = login_prefilled_ = false;
        break;
    case Key::Left: lv_textarea_cursor_left(login_ta_); break;
    case Key::Right: lv_textarea_cursor_right(login_ta_); break;
    case Key::Home: lv_textarea_set_cursor_pos(login_ta_, 0); break;
    case Key::End: lv_textarea_set_cursor_pos(login_ta_, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt) break;
        const char *cur = lv_textarea_get_text(login_ta_);
        const std::string add = filter_typed(EditMode::Free, login_fresh_ ? "" : (cur ? cur : ""), e.text, kMaxLoginPassword);
        if (!add.empty()) {
            if (login_fresh_) lv_textarea_set_text(login_ta_, "");      // like a selected text: the first character replaces it
            login_fresh_ = login_prefilled_ = false;
            lv_textarea_add_text(login_ta_, add.c_str());
        }
        if (!login_msg_.empty()) login_message("", false);
        break;
    }
    default: break;
    }
}

} // namespace meshhop
