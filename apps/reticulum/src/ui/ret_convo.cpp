/*
 * SPDX-License-Identifier: MIT
 *
 * The Reticulum app, phase P2: the conversation page (see ret_app.hpp). Bubbles in a scrolling column, a status line
 * under each message you sent (Sending..., Delivered 12:34, Sent, not confirmed by contact, Failed with a Retry button),
 * and a compose row at the bottom: the shared cp0::ui::TextField (it owns the keyboard rule and the on-screen keyboard),
 * the remaining-bytes counter inside it, and Send. The row follows the on-screen keyboard (osk inset listener).
 *
 * Keys: in the bubbles Up / Down / Page Up / Page Down / Home / End scroll (Down at the end moves on to the next
 * control), Enter starts typing, Right opens More, Left and Esc go back. In the field the shared TextField rules apply
 * (Enter sends, Esc stops typing, Esc again goes back, Up / Down move to the next control). With a physical keyboard the
 * field is ready for typing as soon as the page opens; without one it waits for a tap or Enter, so that the on-screen
 * keyboard does not hide the history before the user asks for it.
 */
#include "ret_app.hpp"

#include "ret_ids.hpp"

#include "app.hpp"
#include "osk_policy.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace retui {

namespace {

using ret::json::Value;

constexpr uint32_t kBubbleIn = 0x262626, kBubbleOut = 0x4A3A0A;
constexpr uint32_t kAmber = 0xF0B400, kGreen = 0x2ECC71, kRed = 0xFF6B5E, kGrey = 0x9A9A9A;
constexpr uint32_t kSendOffBg = 0x262626, kSendOffText = 0x6A6A6A;

const lv_font_t *font_px(int px, bool bold)
{
    return cp0_fonts().get(bold ? "Montserrat-Bold.ttf" : "Montserrat-Medium.ttf", static_cast<uint16_t>(px));
}

int text_w(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

uint32_t tone_rgb(ret::Tone t)
{
    switch (t) {
    case ret::Tone::Ok: return kGreen;
    case ret::Tone::Bad: return kRed;
    case ret::Tone::Warn: return kAmber;
    default: return kGrey;
    }
}

/* the focus ring of the page's own buttons: an amber outline while the key focus is on them */
void focus_ring(lv_obj_t *obj)
{
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, lv_color_hex(kAmber), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
}

/* The header's More pill: as wide as its word needs, with the room the title may use computed from the same number
 * (it was capped at the Esc button's width, and the word overflowed the pill at 800x480). */
int more_pill_w(const cp0_ui_roller_page_t &layout)
{
    return text_w("More", cp0_ui_font_px(layout.esc_text_px)) + 2 * layout.gap + layout.gap / 2;
}

void no_scroll(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

} // namespace

/* ------------------------------------------------------------------ the header and the title */

std::string App::clip_conv_title(const std::string &name) const
{
    if (layout_.title_hidden || layout_.title_px <= 0) return name;
    const lv_font_t *f = cp0_ui_font_px(layout_.title_px);
    const int more_w = more_pill_w(layout_);
    const int room = layout_.esc_x - layout_.gap - more_w - layout_.title_x - layout_.gap;
    if (room <= 0 || text_w(name, f) <= room) return name;
    std::string s = name;
    while (!s.empty()) {
        do s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80);
        if (text_w(s + "...", f) <= room) return s + "...";
    }
    return "...";
}

/* Does this title, alone, fit the header's title room (the part the shared header never drops)? Where it does not, the page gives
 * a short form of its own instead of letting the header cut it in the middle of a word. */
bool App::title_fits(const std::string &text) const
{
    if (layout_.title_hidden || layout_.title_px <= 0) return true;
    return text_w(text, cp0_ui_font_px(layout_.title_px)) <= layout_.title_max_w;
}

bool App::fire_ok()
{
    const std::int64_t now = now_ms();
    if (now - last_fire_ms_ < 400) return false;
    last_fire_ms_ = now;
    return true;
}

bool App::compose_blocked() const
{
    return send_blocked_ || ret::nothing_may_send(ifaces_);
}

/* More, left of Esc: a toolbar key of the header. It is made again with every refresh of the bubbles, so that it is the
 * last control of the input group (the Retry buttons, which are made with the bubbles, come before it). */
void App::make_more()
{
    lv_group_t *grp = cdk::input_group();
    const bool had_focus = convo_.more && grp && lv_group_get_focused(grp) == convo_.more;
    if (convo_.more) lv_obj_delete(convo_.more);
    convo_.more = nullptr;
    convo_.more = lv_obj_create(page_->header());
    lv_obj_remove_style_all(convo_.more);
    no_scroll(convo_.more);
    const int more_w = more_pill_w(layout_);
    lv_obj_set_pos(convo_.more, layout_.esc_x - layout_.gap - more_w, layout_.esc_y);
    lv_obj_set_size(convo_.more, more_w, layout_.esc_h);
    lv_obj_set_style_bg_color(convo_.more, lv_color_hex(0x262626), 0);
    lv_obj_set_style_bg_opa(convo_.more, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(convo_.more, layout_.esc_radius, 0);
    lv_obj_add_flag(convo_.more, LV_OBJ_FLAG_CLICKABLE);
    focus_ring(convo_.more);
    lv_obj_t *ml = lv_label_create(convo_.more);
    lv_obj_set_style_text_font(ml, cp0_ui_font_px(layout_.esc_text_px), 0);
    lv_obj_set_style_text_color(ml, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(ml, "More");
    lv_obj_center(ml);
    auto press_cb = [](lv_event_t *e) {
        auto *self = static_cast<App *>(lv_event_get_user_data(e));
        const lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_CLICKED) {
            if (self->fire_ok()) self->post([self] { if (!self->dialog_) self->open_conv_options(); });
        } else if (code == LV_EVENT_KEY) {
            const uint32_t key = lv_event_get_key(e);
            lv_event_stop_processing(e);
            if (key == LV_KEY_ENTER || key == LV_KEY_RIGHT) {
                if (self->fire_ok()) self->post([self] { if (!self->dialog_) self->open_conv_options(); });
            }
            else if (key == LV_KEY_ESC || key == LV_KEY_LEFT) self->post([self] { self->back(); });
            else if (key == LV_KEY_UP) lv_group_focus_prev(lv_obj_get_group(lv_event_get_current_target_obj(e)));
            else if (key == LV_KEY_DOWN) lv_group_focus_next(lv_obj_get_group(lv_event_get_current_target_obj(e)));
        }
    };
    lv_obj_add_event_cb(convo_.more, press_cb, LV_EVENT_ALL, this);
    if (lv_group_t *g = cdk::input_group()) lv_group_add_obj(g, convo_.more);
    /* made again with every refresh: the focus must not jump to another control (a key typed meanwhile would act there) */
    if (had_focus) lv_group_focus_obj(convo_.more);
}

/* ------------------------------------------------------------------ build and tear down */

void App::teardown_conversation()
{
    if (convo_.inset_listener) cp0::ui::osk::remove_inset_listener(convo_.inset_listener);
    convo_.inset_listener = 0;
    if (convo_.field && !convo_.addr.empty()) { /* what was typed and not sent stays for when the page comes back */
        draft_text_ = convo_.field->text();
        draft_for_ = convo_.addr;
    }
    convo_.field.reset(); /* stops editing, releases the keyboard, deletes its objects */
    if (convo_.root) lv_obj_delete(convo_.root);
    if (convo_.more) lv_obj_delete(convo_.more);
    convo_ = Convo();
}

void App::focus_compose()
{
    if (convo_.field) focus(convo_.field->focus_object());
}

void App::build_conversation()
{
    clear_view();
    const int w = layout_.w;
    convo_.addr = cur_addr_;
    convo_.root = lv_obj_create(page_->root());
    lv_obj_remove_style_all(convo_.root);
    no_scroll(convo_.root);
    lv_obj_remove_flag(convo_.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(convo_.root, 0, layout_.body_y);
    lv_obj_set_size(convo_.root, w, layout_.h - layout_.body_y);

    /* the bubbles */
    convo_.msgs = lv_obj_create(convo_.root);
    lv_obj_remove_style_all(convo_.msgs);
    lv_obj_set_scroll_dir(convo_.msgs, LV_DIR_VER);
    lv_obj_remove_flag(convo_.msgs, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_remove_flag(convo_.msgs, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(convo_.msgs, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(convo_.msgs, lv_color_hex(0x555555), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(convo_.msgs, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(convo_.msgs, std::max(3, layout_.gap / 3), LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(convo_.msgs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(convo_.msgs, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(convo_.msgs, std::max(2, layout_.gap / 2), 0);
    lv_obj_set_style_pad_ver(convo_.msgs, std::max(2, layout_.gap / 2), 0);
    lv_obj_add_event_cb(
        convo_.msgs,
        [](lv_event_t *e) {
            auto *self = static_cast<App *>(lv_event_get_user_data(e));
            if (!self->convo_.msgs) return;
            lv_event_stop_processing(e);
            const uint32_t key = lv_event_get_key(e);
            lv_obj_t *box = self->convo_.msgs;
            const int step = self->layout_.row_h;
            const int page = std::max(step, static_cast<int>(lv_obj_get_height(box)) * 4 / 5);
            switch (key) {
            case LV_KEY_UP: lv_obj_scroll_by(box, 0, step, LV_ANIM_OFF); break;
            case LV_KEY_DOWN:
                if (lv_obj_get_scroll_bottom(box) <= 1) lv_group_focus_next(lv_obj_get_group(box));
                else lv_obj_scroll_by(box, 0, -step, LV_ANIM_OFF);
                break;
            case LV_KEY_PREV: lv_obj_scroll_by(box, 0, page, LV_ANIM_OFF); break;
            case LV_KEY_NEXT: lv_obj_scroll_by(box, 0, -page, LV_ANIM_OFF); break;
            case LV_KEY_HOME: lv_obj_scroll_to_y(box, 0, LV_ANIM_OFF); break;
            case LV_KEY_END: lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF); break;
            case LV_KEY_ENTER: self->focus_compose(); break;
            case LV_KEY_RIGHT: self->post([self] { if (!self->dialog_) self->open_conv_options(); }); break;
            case LV_KEY_LEFT:
            case LV_KEY_ESC: self->post([self] { self->back(); }); break;
            default: break;
            }
        },
        LV_EVENT_KEY, this);

    /* the compose row */
    cp0_ui_roller_page_t lay = layout_;
    const lv_font_t *bold = font_px(layout_.text_px, true);
    const int send_w = std::max(static_cast<int>(layout_.row_h * 12 / 10), text_w("Send", bold) + 2 * layout_.inset);
    const int margin = layout_.gap;
    lay.roller_w = std::max(layout_.row_h, w - 2 * margin - send_w - layout_.gap);
    cp0::ui::TextFieldSpec spec;
    spec.placeholder = "Message";
    spec.max_bytes = static_cast<std::size_t>(limits_.max_text);
    cp0::ui::TextFieldEvents ev;
    ev.changed = [this](const std::string &) { update_compose(); };
    ev.submit = [this](const std::string &) { post([this] { send_message(); }); };
    ev.back = [this] { post([this] { back(); }); };
    convo_.field = std::make_unique<cp0::ui::TextField>(convo_.root, lay, std::move(spec), std::move(ev));
    if (draft_for_ == cur_addr_ && !draft_text_.empty()) convo_.field->set_text(draft_text_);
    lv_obj_t *label0 = lv_obj_get_child(convo_.field->row(), 0); /* the field's own label: not used here */
    if (label0) lv_obj_add_flag(label0, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *box = convo_.field->focus_object();
    lv_obj_set_style_radius(box, layout_.row_h / 2, 0);
    convo_.counter = lv_label_create(box);
    lv_obj_set_style_text_font(convo_.counter, cp0_ui_font_px(layout_.caption_px), 0);
    lv_obj_remove_flag(convo_.counter, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(convo_.counter, "");

    convo_.send = lv_obj_create(convo_.root);
    lv_obj_remove_style_all(convo_.send);
    no_scroll(convo_.send);
    lv_obj_set_size(convo_.send, send_w, layout_.row_h);
    lv_obj_set_style_radius(convo_.send, layout_.row_h / 2, 0);
    lv_obj_set_style_bg_opa(convo_.send, LV_OPA_COVER, 0);
    lv_obj_add_flag(convo_.send, LV_OBJ_FLAG_CLICKABLE);
    focus_ring(convo_.send);
    convo_.send_label = lv_label_create(convo_.send);
    lv_obj_set_style_text_font(convo_.send_label, bold, 0);
    lv_label_set_text(convo_.send_label, "Send");
    lv_obj_center(convo_.send_label);
    lv_obj_add_event_cb(
        convo_.send,
        [](lv_event_t *e) {
            auto *self = static_cast<App *>(lv_event_get_user_data(e));
            const lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_CLICKED) {
                if (self->fire_ok()) self->post([self] { self->send_message(); });
            } else if (code == LV_EVENT_KEY) {
                const uint32_t key = lv_event_get_key(e);
                lv_event_stop_processing(e);
                if (key == LV_KEY_ENTER) {
                    if (self->fire_ok()) self->post([self] { self->send_message(); });
                }
                else if (key == LV_KEY_ESC || key == LV_KEY_LEFT) self->post([self] { self->back(); });
                else if (key == LV_KEY_UP) lv_group_focus_prev(lv_obj_get_group(lv_event_get_current_target_obj(e)));
                else if (key == LV_KEY_DOWN) lv_group_focus_next(lv_obj_get_group(lv_event_get_current_target_obj(e)));
            }
        },
        LV_EVENT_ALL, this);

    convo_.warn = lv_label_create(convo_.root);
    lv_obj_set_style_text_font(convo_.warn, cp0_ui_font_px(layout_.caption_px), 0);
    lv_obj_set_style_text_color(convo_.warn, lv_color_hex(kAmber), 0);
    lv_label_set_long_mode(convo_.warn, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(convo_.warn, ret::kSlowWarning);
    lv_obj_add_flag(convo_.warn, LV_OBJ_FLAG_HIDDEN);

    convo_.panel = lv_obj_create(convo_.root);
    lv_obj_remove_style_all(convo_.panel);
    no_scroll(convo_.panel);
    lv_obj_remove_flag(convo_.panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(convo_.panel, 2, 0);
    lv_obj_set_style_border_color(convo_.panel, lv_color_hex(kAmber), 0);
    lv_obj_set_style_border_opa(convo_.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(convo_.panel, layout_.gap, 0);
    lv_obj_t *pl = lv_label_create(convo_.panel);
    lv_obj_set_style_text_font(pl, cp0_ui_font_px(layout_.caption_px), 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(pl, "No connection is allowed to send. Switch one on in Connections.");
    lv_obj_add_flag(convo_.panel, LV_OBJ_FLAG_HIDDEN);

    convo_.note = lv_label_create(convo_.root);
    lv_obj_set_style_text_font(convo_.note, cp0_ui_font_px(layout_.caption_px), 0);
    lv_obj_set_style_text_color(convo_.note, lv_color_hex(kAmber), 0);
    lv_label_set_long_mode(convo_.note, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(convo_.note, "No connection is allowed to send");
    lv_obj_add_flag(convo_.note, LV_OBJ_FLAG_HIDDEN);

    convo_.strip = lv_obj_create(convo_.root);
    lv_obj_remove_style_all(convo_.strip);
    no_scroll(convo_.strip);
    lv_obj_remove_flag(convo_.strip, LV_OBJ_FLAG_CLICKABLE);
    convo_.strip_text = lv_label_create(convo_.strip);
    lv_label_set_long_mode(convo_.strip_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(convo_.strip_text, lv_color_hex(0xE6E6E6), 0);
    convo_.strip_status = lv_label_create(convo_.strip);
    lv_label_set_long_mode(convo_.strip_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(convo_.strip_status, cp0_ui_font_px(layout_.caption_px), 0);
    lv_obj_set_style_text_align(convo_.strip_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(convo_.strip, LV_OBJ_FLAG_HIDDEN);

    convo_.goto_conn = lv_obj_create(convo_.root);
    lv_obj_remove_style_all(convo_.goto_conn);
    no_scroll(convo_.goto_conn);
    lv_obj_set_size(convo_.goto_conn, w, layout_.row_h);
    lv_obj_set_style_bg_color(convo_.goto_conn, lv_color_hex(CP0_UI_LIST_BAR_RGB), 0);
    lv_obj_set_style_bg_opa(convo_.goto_conn, LV_OPA_COVER, 0);
    lv_obj_add_flag(convo_.goto_conn, LV_OBJ_FLAG_CLICKABLE);
    focus_ring(convo_.goto_conn);
    lv_obj_t *gl = lv_label_create(convo_.goto_conn);
    lv_obj_set_style_text_font(gl, font_px(layout_.text_px, true), 0);
    lv_obj_set_style_text_color(gl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(gl, "Open Connections");
    lv_obj_align(gl, LV_ALIGN_LEFT_MID, layout_.inset, 0);
    lv_obj_t *gv = lv_label_create(convo_.goto_conn);
    lv_obj_set_style_text_font(gv, font_px(layout_.value_px, false), 0);
    lv_obj_set_style_text_color(gv, lv_color_hex(CP0_UI_LIST_MUTED_RGB), 0);
    lv_label_set_text(gv, ret::connections_value(ifaces_).c_str());
    lv_obj_align(gv, LV_ALIGN_RIGHT_MID, -(layout_.inset + layout_.chevron_px), 0);
    lv_obj_t *gc = lv_label_create(convo_.goto_conn);
    lv_obj_set_style_text_font(gc, font_px(layout_.value_px, false), 0);
    lv_obj_set_style_text_color(gc, lv_color_hex(CP0_UI_LIST_CHEVRON_RGB), 0);
    lv_label_set_text(gc, ">");
    lv_obj_align(gc, LV_ALIGN_RIGHT_MID, -layout_.inset, 0);
    lv_obj_add_flag(convo_.goto_conn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(
        convo_.goto_conn,
        [](lv_event_t *e) {
            auto *self = static_cast<App *>(lv_event_get_user_data(e));
            const lv_event_code_t code = lv_event_get_code(e);
            auto go = [self] {
                self->post([self] {
                    if (self->dialog_) return;
                    self->open(ret::View::Connections);
                });
            };
            if (code == LV_EVENT_CLICKED) {
                if (self->fire_ok()) go();
            } else if (code == LV_EVENT_KEY) {
                const uint32_t key = lv_event_get_key(e);
                lv_event_stop_processing(e);
                if (key == LV_KEY_ENTER || key == LV_KEY_RIGHT) {
                    if (self->fire_ok()) go();
                }
                else if (key == LV_KEY_ESC || key == LV_KEY_LEFT) self->post([self] { self->back(); });
                else if (key == LV_KEY_UP) lv_group_focus_prev(lv_obj_get_group(lv_event_get_current_target_obj(e)));
                else if (key == LV_KEY_DOWN) lv_group_focus_next(lv_obj_get_group(lv_event_get_current_target_obj(e)));
            }
        },
        LV_EVENT_ALL, this);

    convo_.built = true;
    convo_.osk_inset = cp0::ui::osk::bottom_inset();
    convo_.inset_listener = cp0::ui::osk::add_inset_listener([this](int inset) {
        convo_.osk_inset = inset;
        layout_conversation();
    });
    show_messages(true);
    update_compose();
    layout_conversation();
    load_history();

    /* with a physical keyboard the field is ready for typing; without one the history stays in view */
    bool typing_ready = false;
    if (cdk::osk_policy::Policy *p = cdk::osk_policy::installed()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const std::int64_t now = static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        typing_ready = !p->allowed(now) && p->input().keyboards > 0;
    }
    if (typing_ready) focus_compose();
    else focus(convo_.msgs);
}

/* ------------------------------------------------------------------ layout */

/* The room under the header, from the bottom: the compose row (above the on-screen keyboard), then what the page has to say
 * (the blocked note with its row that opens Connections, the warning about a long message), then the bubbles. When the
 * keyboard takes most of the screen the bubbles give way to ONE line with the latest message: the owner's question 2 of
 * board 29, "the compose row and the latest message only". */
void App::layout_conversation()
{
    if (!convo_.built) return;
    const int w = layout_.w;
    const int pad = std::max(2, layout_.gap / 2);
    const int fh = layout_.row_h;
    const int area_h = layout_.h - layout_.body_y;
    const lv_font_t *cap = cp0_ui_font_px(layout_.caption_px);
    const int lh = lv_font_get_line_height(cap);
    const bool blocked = compose_blocked();
    const bool slow = convo_.field && ret::counter_for(ret::text_bytes(ret::trim_text(convo_.field->text())), limits_).slow;

    int y = area_h - convo_.osk_inset - pad - fh; /* the compose row's top, relative to the area under the header */
    const int margin = layout_.gap;
    lv_obj_set_pos(convo_.send, w - margin - lv_obj_get_width(convo_.send), y);
    lv_obj_set_pos(convo_.field->row(), margin, y);
    int top_free = y - pad; /* what is above is stacked from here upwards */

    auto hide = [](lv_obj_t *o) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); };
    auto show = [](lv_obj_t *o) { lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); };
    hide(convo_.goto_conn);
    hide(convo_.panel);
    hide(convo_.note);
    hide(convo_.warn);
    if (blocked) {
        lv_obj_t *label = lv_obj_get_child(convo_.panel, 0);
        lv_point_t size{};
        const lv_font_t *pf = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        const int pw = w - 2 * margin;
        lv_text_get_size(&size, "No connection is allowed to send. Switch one on in Connections.", pf, 0, 0,
                         std::max(1, pw - 2 * layout_.gap - 4), LV_TEXT_FLAG_NONE);
        const int ph = size.y + 2 * pad + 4;
        const int gh = layout_.row_h;
        if (top_free >= ph + pad + gh) { /* the whole note and the row that opens Connections */
            top_free -= gh;
            lv_obj_set_pos(convo_.goto_conn, 0, top_free);
            lv_obj_set_width(convo_.goto_conn, w);
            show(convo_.goto_conn);
            top_free -= ph + pad;
            lv_obj_set_pos(convo_.panel, margin, top_free);
            lv_obj_set_size(convo_.panel, pw, ph);
            lv_obj_t *l = lv_obj_get_child(convo_.panel, 0);
            lv_obj_set_width(l, std::max(1, pw - 2 * layout_.gap - 4));
            lv_obj_set_pos(l, layout_.gap - 2, pad);
            show(convo_.panel);
        } else if (top_free >= lh + pad) { /* the keyboard is up and takes the room: one line (Esc brings the whole note back) */
            top_free -= lh + pad;
            lv_obj_set_pos(convo_.note, margin, top_free);
            lv_obj_set_width(convo_.note, w - 2 * margin);
            show(convo_.note);
        }
    } else if (slow && top_free >= lh + pad) {
        /* the warning wins the room over the history: with the keyboard up at 480x320 both do not fit, and the history line
         * is dropped below (board QA of 0.3.2, D3: the warning left the message a cut sliver) */
        top_free -= lh + pad;
        lv_obj_set_pos(convo_.warn, margin, top_free);
        lv_obj_set_width(convo_.warn, w - 2 * margin);
        show(convo_.warn);
    }

    /* the bubbles, or the latest message alone when less than two rows are left for them */
    const int slh = lv_font_get_line_height(font_px(layout_.caption_px, false));
    const bool compact = top_free < 2 * layout_.row_h;
    if (compact && top_free >= slh + pad) {
        hide(convo_.msgs);
        lv_obj_set_pos(convo_.strip, 0, std::max(0, top_free - slh - pad));
        lv_obj_set_size(convo_.strip, w, slh + pad);
        show(convo_.strip);
    } else {
        hide(convo_.strip);
        /* less than two rows and not even the one line: no history at all, never a cut-off sliver of it */
        if (!compact && top_free >= 1) show(convo_.msgs);
        else hide(convo_.msgs);
    }
    lv_obj_set_pos(convo_.msgs, 0, 0);
    lv_obj_set_size(convo_.msgs, w, std::max(1, top_free));
    update_strip();
    regroup();
    lv_obj_update_layout(convo_.root);
    /* the latest message stays in view when the room under it changed */
    lv_obj_scroll_to_y(convo_.msgs, LV_COORD_MAX, LV_ANIM_OFF);
}

/* The one line that stands for the bubbles while the keyboard is up: "You: Be there at 13" and, for a message you sent,
 * where it stands at the right. */
void App::update_strip()
{
    if (!convo_.strip || !convo_.strip_text) return;
    std::string text, status;
    ret::Tone tone = ret::Tone::Muted;
    if (!msgs_.empty()) {
        const ret::Message &m = msgs_.back();
        text = (m.out() ? "You: " : "") + m.text;
        const ret::MsgStatus st = ret::message_status(m);
        status = st.text;
        tone = st.tone;
    }
    const lv_font_t *f = font_px(layout_.caption_px, false);
    const int w = layout_.w, margin = layout_.gap;
    const int sw = status.empty() ? 0 : std::min(w / 2, text_w(status, cp0_ui_font_px(layout_.caption_px)) + 1);
    lv_label_set_text(convo_.strip_text, text.c_str());
    lv_obj_set_style_text_font(convo_.strip_text, f, 0);
    lv_obj_set_width(convo_.strip_text, std::max(1, w - 2 * margin - (sw ? sw + layout_.gap : 0)));
    lv_obj_set_pos(convo_.strip_text, margin, 0);
    lv_label_set_text(convo_.strip_status, status.c_str());
    lv_obj_set_style_text_color(convo_.strip_status, lv_color_hex(tone_rgb(tone)), 0);
    lv_obj_set_width(convo_.strip_status, std::max(1, sw));
    lv_obj_align(convo_.strip_status, LV_ALIGN_TOP_RIGHT, -margin, 0);
}

/* ------------------------------------------------------------------ the bubbles */

void App::show_messages(bool force)
{
    if (!convo_.built || !convo_.msgs) return;
    lv_obj_t *box = convo_.msgs;
    update_strip();
    /* the poll refreshes the page every 3 s: rebuild the bubbles (and the Retry buttons, which may have the focus) only
     * when a message, its state or the screen changed */
    std::string sig = std::to_string(layout_.w) + "x" + std::to_string(layout_.h) + ":";
    for (const ret::Message &m : msgs_)
        sig += std::to_string(m.id) + m.state + m.detail + std::to_string(m.text.size()) + (m.unread ? "u" : "r") + ",";
    if (!force && sig == convo_.sig) return;
    convo_.sig = sig;
    const bool first = lv_obj_get_child_count(box) == 0;
    const bool at_bottom = first || lv_obj_get_scroll_bottom(box) <= 2;
    const int keep_y = static_cast<int>(lv_obj_get_scroll_y(box));
    /* a rebuild deletes the Retry buttons: the one that has the focus gets it again (same message), instead of the focus
     * jumping to the first control of the group */
    std::int64_t refocus_retry = -1;
    if (lv_group_t *grp = cdk::input_group()) {
        lv_obj_t *fo = lv_group_get_focused(grp);
        for (lv_obj_t *r : convo_.retries)
            if (r == fo) refocus_retry = static_cast<std::int64_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(r)));
    }
    convo_.retries.clear();
    lv_obj_clean(box);

    const int vw = layout_.w;
    const int margin = layout_.gap;
    const int pad_h = layout_.gap, pad_v = std::max(3, layout_.gap / 2);
    const int max_bw = (vw - 2 * margin) * 78 / 100;
    const lv_font_t *bf = font_px(layout_.value_px, false);
    const lv_font_t *sf = cp0_ui_font_px(layout_.caption_px);
    const int sl = lv_font_get_line_height(sf);

    if (msgs_.empty()) {
        lv_obj_t *e = lv_label_create(box);
        lv_obj_set_style_text_font(e, sf, 0);
        lv_obj_set_style_text_color(e, lv_color_hex(kGrey), 0);
        lv_obj_set_width(e, vw - 2 * margin);
        lv_obj_set_style_pad_left(e, margin, 0);
        lv_label_set_long_mode(e, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(e, "No messages yet. Write the first one below.");
    }
    for (const ret::Message &m : msgs_) {
        lv_point_t size{};
        lv_text_get_size(&size, m.text.c_str(), bf, 0, 0, std::max(1, max_bw - 2 * pad_h), LV_TEXT_FLAG_NONE);
        const int bw = size.x + 2 * pad_h + 1, bh = size.y + 2 * pad_v;
        lv_obj_t *wrap = lv_obj_create(box);
        lv_obj_remove_style_all(wrap);
        no_scroll(wrap);
        lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(wrap, vw, bh);
        lv_obj_t *b = lv_obj_create(wrap);
        lv_obj_remove_style_all(b);
        no_scroll(b);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, m.out() ? vw - margin - bw : margin, 0);
        lv_obj_set_style_radius(b, layout_.gap * 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(m.out() ? kBubbleOut : kBubbleIn), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(l, bf, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(l, m.text.c_str());
        lv_obj_set_width(l, size.x + 1);
        lv_obj_set_pos(l, pad_h, pad_v);

        const ret::MsgStatus st = ret::message_status(m);
        if (st.text.empty()) continue;
        const int rh = st.retry ? layout_.row_h : sl + 2;
        lv_obj_t *line = lv_obj_create(box);
        lv_obj_remove_style_all(line);
        no_scroll(line);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(line, vw, rh);
        int right = vw - margin;
        if (st.retry) {
            const int pw = text_w("Retry", sf) + 2 * layout_.inset;
            lv_obj_t *pill = lv_obj_create(line);
            lv_obj_remove_style_all(pill);
            no_scroll(pill);
            lv_obj_set_size(pill, pw, rh);
            lv_obj_set_pos(pill, right - pw, 0);
            lv_obj_set_style_radius(pill, rh / 2, 0);
            lv_obj_set_style_bg_color(pill, lv_color_hex(0x262626), 0);
            lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
            lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
            focus_ring(pill);
            lv_obj_set_user_data(pill, reinterpret_cast<void *>(static_cast<intptr_t>(m.id)));
            lv_obj_t *pl = lv_label_create(pill);
            lv_obj_set_style_text_font(pl, sf, 0);
            lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
            lv_label_set_text(pl, "Retry");
            lv_obj_center(pl);
            lv_obj_add_event_cb(
                pill,
                [](lv_event_t *e) {
                    auto *self = static_cast<App *>(lv_event_get_user_data(e));
                    lv_obj_t *target = lv_event_get_current_target_obj(e);
                    const std::int64_t id = static_cast<std::int64_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                    const lv_event_code_t code = lv_event_get_code(e);
                    if (code == LV_EVENT_CLICKED) {
                        if (self->fire_ok()) self->post([self, id] { self->retry_message(id); });
                    } else if (code == LV_EVENT_KEY) {
                        const uint32_t key = lv_event_get_key(e);
                        lv_event_stop_processing(e);
                        if (key == LV_KEY_ENTER) {
                            if (self->fire_ok()) self->post([self, id] { self->retry_message(id); });
                        }
                        else if (key == LV_KEY_ESC || key == LV_KEY_LEFT) self->post([self] { self->back(); });
                        else if (key == LV_KEY_UP) lv_group_focus_prev(lv_obj_get_group(target));
                        else if (key == LV_KEY_DOWN) lv_group_focus_next(lv_obj_get_group(target));
                    }
                },
                LV_EVENT_ALL, this);
            convo_.retries.push_back(pill);
            right -= pw + layout_.gap;
        }
        lv_obj_t *sl_obj = lv_label_create(line);
        lv_obj_set_style_text_font(sl_obj, sf, 0);
        lv_obj_set_style_text_color(sl_obj, lv_color_hex(tone_rgb(st.tone)), 0);
        lv_label_set_long_mode(sl_obj, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(sl_obj, std::max(1, right - margin));
        lv_obj_set_style_text_align(sl_obj, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(sl_obj, st.text.c_str());
        lv_obj_set_pos(sl_obj, margin, (rh - sl) / 2);
    }

    regroup();
    if (refocus_retry >= 0)
        for (lv_obj_t *r : convo_.retries)
            if (static_cast<std::int64_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(r))) == refocus_retry)
                lv_group_focus_obj(r);
    lv_obj_update_layout(box);
    if (at_bottom) lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(box, keep_y, LV_ANIM_OFF);
}

/* The input group: the bubbles, the field and Send are added once, in that order; the Retry buttons (made with the
 * bubbles), the open-Connections row (only while it shows) and More follow. Nothing that is focused is ever removed and
 * added again: that would move the focus, and stop typing in the field. Up and Down in the field, and Down at the end of
 * the bubbles, move along it. */
void App::regroup()
{
    lv_group_t *group = cdk::input_group();
    if (!group || !convo_.built) return;
    auto add = [group](lv_obj_t *o) {
        if (o && lv_obj_get_group(o) != group) lv_group_add_obj(group, o);
    };
    add(convo_.msgs);
    if (convo_.field) add(convo_.field->focus_object());
    add(convo_.send);
    for (lv_obj_t *r : convo_.retries) add(r);
    if (lv_obj_has_flag(convo_.goto_conn, LV_OBJ_FLAG_HIDDEN)) {
        if (lv_obj_get_group(convo_.goto_conn)) lv_group_remove_obj(convo_.goto_conn);
    } else {
        add(convo_.goto_conn);
    }
    make_more(); /* always the last */
}

/* ------------------------------------------------------------------ the compose row */

void App::update_compose()
{
    if (!convo_.built || !convo_.field) return;
    const std::string text = ret::trim_text(convo_.field->text());
    const ret::Counter c = ret::counter_for(ret::text_bytes(text), limits_);
    const bool blocked = compose_blocked();
    uint32_t rgb = kGrey;
    bool bold = false;
    if (c.level == ret::Level::Near) rgb = 0xFFFFFF, bold = true;
    if (c.level == ret::Level::Over) rgb = kAmber, bold = true;
    lv_label_set_text(convo_.counter, c.text.c_str());
    lv_obj_set_style_text_color(convo_.counter, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_font(convo_.counter, bold ? font_px(layout_.caption_px, true) : cp0_ui_font_px(layout_.caption_px), 0);
    /* the counter sits at the right inside the field; the text area gives it room */
    lv_obj_t *box = convo_.field->focus_object();
    const int fh = layout_.row_h;
    const int inset = std::max(layout_.gap, fh / 5);
    const lv_font_t *cf = font_px(layout_.caption_px, true);
    const int cw = std::max(text_w("9999 over", cf), text_w(c.text, cf)) + 2;
    lv_obj_set_width(convo_.counter, cw);
    lv_obj_set_style_text_align(convo_.counter, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(convo_.counter, LV_ALIGN_RIGHT_MID, -inset, 0);
    lv_obj_t *area = lv_obj_get_child(box, 0);
    if (area && area != convo_.counter) {
        const int field_w = lv_obj_get_width(box);
        lv_obj_set_width(area, std::max(1, field_w - 3 * inset - cw));
    }
    const bool can_send = !c.empty && !c.too_long && !blocked && !convo_.sending;
    lv_obj_set_style_bg_color(convo_.send, lv_color_hex(can_send ? kAmber : kSendOffBg), 0);
    lv_obj_set_style_text_color(convo_.send_label, lv_color_hex(can_send ? 0x000000 : kSendOffText), 0);
    const bool want_warn = c.slow && !blocked;
    const bool shown_warn = !lv_obj_has_flag(convo_.warn, LV_OBJ_FLAG_HIDDEN);
    const bool shown_panel = !lv_obj_has_flag(convo_.panel, LV_OBJ_FLAG_HIDDEN);
    if (want_warn != shown_warn || blocked != shown_panel) layout_conversation();
}

void App::load_history()
{
    Value p = Value::object();
    p.set("address", Value::string(cur_addr_));
    p.set("limit", Value::number(60));
    const std::string address = cur_addr_;
    request_ifaces(); /* the blocked state follows the connections as they are now */
    link_->request("messages.history", p, 8.0, [this, address](const ret::Response &r) {
        if (svc_ != Svc::Up || address != cur_addr_ || nav_.current() != ret::View::Conversation) return;
        std::vector<ret::Message> list;
        bool more = false;
        if (r.ok && ret::parse_history(r.result, list, &more)) {
            for (const ret::Message &m : msgs_) ret::merge_message(list, m); /* what arrived meanwhile */
            msgs_ = std::move(list);
            msgs_more_ = more;
            bool unread = false;
            for (const ret::Message &m : msgs_)
                if (!m.out() && m.unread) unread = true;
            if (unread) mark_read(address);
        } else if (!r.ok) {
            request_failed(r, "Could not load the messages");
        }
        show_messages();
        update_compose();
    });
}

/* ------------------------------------------------------------------ Send and Retry: the user's action */

void App::send_message()
{
    if (!convo_.built || !convo_.field || convo_.sending) return;
    const std::string text = ret::trim_text(convo_.field->text());
    const ret::Counter c = ret::counter_for(ret::text_bytes(text), limits_);
    if (c.empty || c.too_long) return;
    if (compose_blocked()) { /* the draft stays; the note above the field says why */
        update_compose();
        layout_conversation();
        return;
    }
    convo_.sending = true;
    update_compose();
    Value p = Value::object();
    p.set("address", Value::string(cur_addr_));
    p.set("text", Value::string(text));
    const std::string address = cur_addr_;
    link_->request(
        "messages.send", p, 15.0,
        [this, address](const ret::Response &r) {
            convo_.sending = false;
            if (r.ok) {
                ret::Message m;
                if (ret::parse_message(r.result, m) && address == cur_addr_ && convo_.built) {
                    ret::merge_message(msgs_, m);
                    send_blocked_ = false;
                    if (convo_.field) convo_.field->set_text("");
                    show_messages();
                    lv_obj_scroll_to_y(convo_.msgs, LV_COORD_MAX, LV_ANIM_OFF);
                }
                std::fprintf(stderr, "[reticulum] message handed to the service\n");
                convs_dirty_ = true;
            } else if (r.transport && r.code == "no_transmit") {
                send_blocked_ = true;
            } else {
                request_failed(r, "Could not send it");
            }
            if (convo_.built) {
                update_compose();
                layout_conversation();
            }
        },
        /*user_action=*/true);
}

void App::retry_message(std::int64_t id)
{
    if (!convo_.built) return;
    if (compose_blocked()) {
        update_compose();
        layout_conversation();
        return;
    }
    Value p = Value::object();
    p.set("id", Value::number(static_cast<double>(id)));
    link_->request(
        "messages.retry", p, 15.0,
        [this](const ret::Response &r) {
            if (r.ok) {
                ret::Message m;
                if (ret::parse_message(r.result, m)) {
                    ret::merge_message(msgs_, m);
                    send_blocked_ = false;
                }
                std::fprintf(stderr, "[reticulum] message sent again by hand\n");
                convs_dirty_ = true;
            } else if (r.transport && r.code == "no_transmit") {
                send_blocked_ = true;
            } else {
                request_failed(r, "Could not send it again");
            }
            if (nav_.current() == ret::View::Conversation) {
                show_messages();
                update_compose();
                layout_conversation();
            }
        },
        /*user_action=*/true);
}

void App::retry_all_failed()
{
    std::vector<std::int64_t> ids;
    for (const ret::Message &m : msgs_)
        if (m.out() && m.state == "failed") ids.push_back(m.id);
    if (ids.empty()) return;
    /* Each goes by hand from this button; back on the conversation the user sees them sending */
    nav_.back();
    if (!saved_sel_.empty()) saved_sel_.pop_back();
    render();
    for (std::int64_t id : ids) retry_message(id);
}

} // namespace retui
