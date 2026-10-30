/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the terminal page (see terminal_view.hpp). Geometry from the layout service only: the page layout,
 * the SPACE / ROW / TARGET tokens, the toolbar key size (cp0_ui_toolbar_key_px), the text sizes and the fonts' own
 * metrics.
 */
#include "terminal_view.hpp"

#include "app.hpp"
#include "osk_policy.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace sc {
namespace {

/* the launcher's palette (report 024 section 5): text, labels, status colours, the toolbar keys and the primary */
constexpr std::uint32_t kText = 0xE0E0E0;
constexpr std::uint32_t kLabel = 0x9A9A9A;
constexpr std::uint32_t kOk = 0x66CC88;
constexpr std::uint32_t kWarn = 0xF0B400;
constexpr std::uint32_t kError = 0xFF6666;
constexpr std::uint32_t kKey = 0x2A2A2A;
constexpr std::uint32_t kKeyPressed = 0x2D9CDB;
constexpr std::uint32_t kPrimary = 0x1F5F8A;
constexpr std::uint32_t kPrimaryPressed = 0x2A7AB0;
constexpr std::size_t kTakeMax = 64 * 1024;  /* received bytes drained per tick (50 ms): 1.3 MB/s */
constexpr int kRenderMs = 100;                /* at most 10 redraws a second */
constexpr int kStatusMs = 200;
constexpr int kModeMs = 500;

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

int text_width(const char *text, const lv_font_t *font)
{
    if (!text || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

int caption_px()
{
    return cp0_ui_metrics_get()->text_px[CP0_TEXT_CAPTION];
}

constexpr int kDeckColumns = 55;    /* the deck's terminal: 608 px at 11 px a character (18 px) */
constexpr int kDeckTextMmX10 = 16;  /* the deck's CAPTION: 18 px at 11.3 px/mm */

/* The key bar's keys: the launcher's 9 mm toolbar key (as the header Esc, the stock-app toolbars and the on-screen
 * keyboard), not TARGET (7 mm), which the line field's Send keeps. */
int toolbar_key()
{
    return cp0_ui_toolbar_key_px(cp0_ui_metrics_get()->ppmm_x100);
}

/* What local echo shows for bytes sent: a line end as a new line, DEL / BS as an erase. */
std::string echo_of(const std::string &bytes)
{
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '\r') out += (i + 1 < bytes.size() && bytes[i + 1] == '\n') ? "\r" : "\r\n";
        else if (c == '\x7f' || c == '\b') out += "\b \b";
        else out += c;
    }
    return out;
}

} // namespace

const lv_font_t *mono_font(int px)
{
    const lv_font_t *font = cp0_fonts().get("JetBrainsMono-Regular.ttf", static_cast<uint16_t>(px));
    return font ? font : cp0_ui_font_px(px);
}

int board_text_px(const cp0_ui_roller_page_t &layout)
{
    const int caption = caption_px();
    if (layout.h <= layout.w) return caption; /* landscape and square: as always */
    const int smallest = std::min(caption, cp0_ui_font_snap(cp0_ui_mm_px(cp0_ui_metrics_get()->ppmm_x100,
                                                                         kDeckTextMmX10, 0)));
    const int view_w = layout.w - 2 * layout.gap; /* relayout()'s text width */
    for (int px = caption; px > smallest; px -= 2) {
        const int advance = static_cast<int>(lv_font_get_glyph_width(mono_font(px), 'M', 0));
        if (advance > 0 && view_w / advance >= kDeckColumns) return px;
    }
    return smallest;
}

TerminalView::TerminalView(cp0::ui::Page &page, const cp0_ui_roller_page_t &layout, TerminalSpec spec)
    : page_(page), layout_(layout), spec_(std::move(spec))
{
    mono_ = mono_font(board_text_px(layout_));
    char_w_ = std::max(1, static_cast<int>(lv_font_get_glyph_width(mono_, 'M', 0)));
    line_h_ = std::max(1, static_cast<int>(lv_font_get_line_height(mono_)));

    build();

    // the header: "ttyUSB0 115200 8N1"; where the title has no room for it (the Esc button and the status strip with
    // its SSH slot take the right half: 640x480 shows "ttyUSB0 115200"), the frame opens the status line; where even
    // that has no room (a 480 px wide portrait screen whose status strip shows a battery), "ttyUSB0" and the speed
    // opens it too
    const std::string full = terminal_title(spec_.port.tty, spec_.baud, spec_.settings.frame);
    const std::string with_speed = spec_.port.tty + " " + std::to_string(spec_.baud);
    const lv_font_t *title_font = cp0_ui_font_px(layout_.title_px);
    if (text_width(full.c_str(), title_font) <= layout_.title_max_w) {
        page_.set_title({full});
    } else if (text_width(with_speed.c_str(), title_font) <= layout_.title_max_w) {
        page_.set_title({with_speed});
        status_prefix_ = spec_.settings.frame + "  ";
    } else {
        page_.set_title({spec_.port.tty});
        status_prefix_ = std::to_string(spec_.baud) + " " + spec_.settings.frame + "  ";
    }

    SessionOptions o;
    o.id = spec_.port.id;
    o.path = spec_.port.path;
    o.baud = spec_.baud;
    parse_frame(spec_.settings.frame, o.frame);
    o.roots = spec_.roots;
    o.log = spec_.settings.log;
    o.log_path = spec_.log_dir + "/" + log_file_name(spec_.port.tty, std::time(nullptr));
    session_ = std::make_unique<Session>(o);
    session_->start();

    cdk::osk_policy::Policy *policy = cdk::osk_policy::installed();
    set_mode(policy && policy->allowed(now_ms()) ? Mode::Field : Mode::Raw);
    inset_listener_ = cp0::ui::osk::add_inset_listener([this](int) { relayout(); });
    update_status(true);
    render(true);
}

TerminalView::~TerminalView()
{
    if (key_screen_ && LV_EVENT_KEYBOARD != 0)
        lv_obj_remove_event_cb_with_user_data(key_screen_, keyboard_event_cb, this);
    if (inset_listener_) cp0::ui::osk::remove_inset_listener(inset_listener_);
    set_mode(Mode::None); /* the keyboard state restored, the field (and its keyboard request) gone */
    session_.reset();     /* the port and the log closed, the worker joined */
    if (box_) {
        lv_obj_remove_event_cb_with_user_data(box_, box_delete_cb, this);
        lv_obj_delete(box_);
    }
}

void TerminalView::box_delete_cb(lv_event_t *event)
{
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(event));
    if (!self) return;
    self->box_ = self->status_ = self->pill_ = self->info_ = self->out_ = self->text_ = self->bar_ = nullptr;
    self->input_ = self->send_ = nullptr;
    self->tools_.clear();
}

int TerminalView::button_w(const char *text, int key) const
{
    return std::max(key, text_width(text, cp0_ui_font_px(layout_.esc_text_px)) + 2 * layout_.gap);
}

lv_obj_t *TerminalView::make_button(lv_obj_t *parent, const char *text, bool primary, int key)
{
    const lv_font_t *font = cp0_ui_font_px(layout_.esc_text_px);
    lv_obj_t *button = lv_button_create(parent);
    if (lv_obj_get_group(button)) lv_group_remove_obj(button); /* touch only: the keyboard has its own keys */
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);   /* a tap never takes the focus from the field */
    lv_obj_set_size(button, button_w(text, key), key);
    lv_obj_set_style_radius(button, layout_.esc_radius, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? kPrimary : kKey), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? kPrimaryPressed : kKeyPressed), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return button;
}

void TerminalView::build()
{
    box_ = plain(page_.root());
    lv_obj_set_pos(box_, 0, layout_.body_y);
    lv_obj_add_event_cb(box_, box_delete_cb, LV_EVENT_DELETE, this);

    const lv_font_t *small = cp0_ui_font_px(caption_px());
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    status_ = plain(box_);
    pill_ = lv_label_create(status_);
    lv_obj_set_style_text_font(pill_, small, 0);
    lv_obj_set_style_text_color(pill_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(pill_, s, 0);
    lv_obj_set_style_pad_ver(pill_, std::max(1, s / 4), 0);
    info_ = lv_label_create(status_);
    lv_obj_set_style_text_font(info_, small, 0);
    lv_obj_set_style_text_color(info_, lv_color_hex(kLabel), 0);
    lv_label_set_long_mode(info_, LV_LABEL_LONG_MODE_DOTS);

    out_ = plain(box_);
    lv_obj_add_flag(out_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(out_, out_event_cb, LV_EVENT_ALL, this);
    text_ = lv_label_create(out_);
    lv_label_set_long_mode(text_, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(text_, mono_, 0);
    lv_obj_set_style_text_color(text_, lv_color_hex(kText), 0);
    lv_obj_set_style_text_line_space(text_, 0, 0);
    lv_label_set_text(text_, "");

    bar_ = lv_obj_create(box_);
    lv_obj_remove_style_all(bar_);
    lv_obj_set_flex_flow(bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar_, s, 0);
    lv_obj_set_scroll_dir(bar_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(bar_, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    const int key = toolbar_key();
    for (int i = 0; i <= static_cast<int>(Tool::Reset); ++i) {
        lv_obj_t *button = make_button(bar_, tool_label(static_cast<Tool>(i)).c_str(), false, key);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(button, tool_event_cb, LV_EVENT_CLICKED, this);
        tools_.push_back(button);
    }

    key_screen_ = lv_obj_get_screen(box_);
    if (key_screen_ && LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(key_screen_, keyboard_event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

std::string TerminalView::tool_label(Tool which) const
{
    switch (which) {
    case Tool::CtrlC: return "Ctrl-C";
    case Tool::CtrlD: return "Ctrl-D";
    case Tool::CtrlZ: return "Ctrl-Z";
    case Tool::Tab: return "Tab";
    case Tool::Esc: return "Send Esc";
    case Tool::Enter: return "Enter";
    case Tool::Pause: return follow_ ? "Pause" : "Resume";
    case Tool::Clear: return "Clear";
    case Tool::Log: return log_on_ ? "Stop log" : "Start log";
    case Tool::Reset: return "Reset board";
    }
    return std::string();
}

bool TerminalView::editing() const
{
    return field_ && field_->editing();
}

/* Raw keys (a physical keyboard): every key to the port, read as LV_EVENT_KEYBOARD with the keypad path intercepted,
 * as a text field does; the previous input state comes back when the page goes or the mode changes. */
void TerminalView::raw_keys(bool on)
{
    if (on == raw_on_) return;
    raw_on_ = on;
    if (on) {
        saved_context_ = cp0_keyboard_get_input_context();
        saved_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
        cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
        cp0_keyboard_set_lvgl_keypad_intercept(1);
    } else {
        cp0_keyboard_set_input_context(saved_context_);
        cp0_keyboard_set_lvgl_keypad_intercept(saved_intercept_);
    }
}

void TerminalView::set_mode(Mode mode)
{
    if (mode == mode_) return;
    if (mode_ == Mode::Raw) raw_keys(false);
    if (mode_ == Mode::Field) {
        field_.reset();
        if (input_) lv_obj_delete(input_);
        input_ = send_ = nullptr;
    }
    mode_ = mode;
    if (mode == Mode::Raw) raw_keys(true);
    if (mode == Mode::Field && box_) {
        input_ = plain(box_);
        send_ = make_button(input_, "Send", true, cp0_ui_tok(CP0_TOK_TARGET));
        lv_obj_add_event_cb(send_, send_event_cb, LV_EVENT_CLICKED, this);
        // the field: the label column is a single pixel (no label: the page says what the line is for), the field
        // takes the row up to Send
        const int field_w = layout_.w - 2 * layout_.gap - button_w("Send", cp0_ui_tok(CP0_TOK_TARGET)) - cp0_ui_tok(CP0_TOK_SPACE_S);
        cp0_ui_roller_page_t field_layout = layout_;
        field_layout.roller_w = std::max(layout_.row_h, field_w) + 1 + layout_.gap;
        cp0::ui::TextFieldSpec spec;
        spec.label = "";
        spec.label_w = 1;
        spec.type = cp0::ui::TextType::Text;
        spec.placeholder = "Type a line, Enter sends";
        spec.max_bytes = 512;
        cp0::ui::TextFieldEvents events;
        events.submit = [this](const std::string &text) { submit_line(text); };
        field_ = std::make_unique<cp0::ui::TextField>(input_, field_layout, std::move(spec), std::move(events));
    }
    if (mode != Mode::None) relayout();
}

void TerminalView::relayout()
{
    if (!box_) return;
    const int w = layout_.w;
    const int m = layout_.gap;
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int target = cp0_ui_tok(CP0_TOK_TARGET);
    const int key = toolbar_key();
    const int row = layout_.row_h;
    const int h = std::max(0, layout_.h - layout_.body_y - cp0::ui::osk::bottom_inset());
    const bool ed = editing();
    lv_obj_set_size(box_, w, h);

    int top = 0;
    if (ed) {
        lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
        top = s;
    } else {
        const int line = lv_font_get_line_height(cp0_ui_font_px(caption_px()));
        const int status_h = line + 2 * std::max(1, s / 4);
        lv_obj_remove_flag(status_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(status_, m, s);
        lv_obj_set_size(status_, w - 2 * m, status_h);
        lv_obj_align(pill_, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_update_layout(pill_);
        const int pill_w = lv_obj_get_width(pill_);
        lv_obj_set_width(info_, std::max(0, w - 2 * m - pill_w - s));
        lv_obj_set_height(info_, line);
        lv_obj_align(info_, LV_ALIGN_LEFT_MID, pill_w + s, 0);
        top = s + status_h + s;
    }

    int bottom = h;
    if (mode_ == Mode::Field && input_ && field_) {
        const int input_h = ed ? row : std::max(row, target);
        const int input_y = bottom - input_h - s;
        lv_obj_set_pos(input_, 0, std::max(0, input_y));
        lv_obj_set_size(input_, w, input_h);
        lv_obj_set_pos(field_->row(), 0, (input_h - row) / 2);
        if (ed) {
            lv_obj_add_flag(send_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(send_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(send_, w - m - button_w("Send", target), (input_h - target) / 2);
        }
        bottom = std::max(0, input_y);
    }
    if (ed) {
        lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        const int bar_y = bottom - key - s;
        lv_obj_set_pos(bar_, m, std::max(0, bar_y));
        lv_obj_set_size(bar_, w - 2 * m, key);
        bottom = std::max(0, bar_y);
    }

    const int out_h = std::max(0, bottom - s - top);
    const int out_w = w - 2 * m;
    lv_obj_set_pos(out_, m, top);
    lv_obj_set_size(out_, out_w, out_h);
    lv_obj_set_pos(text_, 0, 0);
    lv_obj_set_size(text_, out_w, out_h);
    // a drag scrolls the text; a strip too small to drag (the keyboard is up at 640x480) takes no touch
    if (out_h >= row) lv_obj_add_flag(out_, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(out_, LV_OBJ_FLAG_CLICKABLE);
    const int cols = std::max(1, out_w / char_w_);
    const int rows = std::max(1, out_h / line_h_);
    if (cols != cols_ || rows != rows_) {
        cols_ = cols;
        rows_ = rows;
        drawn_version_ = ~0ull;
    }
    was_editing_ = ed;
    render(true);
}

void TerminalView::render(bool force)
{
    if (!text_) return;
    const std::int64_t now = now_ms();
    if (!force && (buffer_.version() == drawn_version_ || now - last_render_ < kRenderMs)) return;
    last_render_ = now;
    drawn_version_ = buffer_.version();
    const std::vector<std::string> rows =
        follow_ ? view_bottom(buffer_, cols_, rows_, &top_) : view_at(buffer_, cols_, rows_, &top_);
    std::string text;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) text += '\n';
        text += rows[i];
    }
    if (text != shown_) {
        shown_ = text;
        lv_label_set_text(text_, shown_.c_str());
    }
}

void TerminalView::update_status(bool force)
{
    if (!session_ || !pill_) return;
    const SessionStatus st = session_->status();
    if (st.state == LinkState::Failed && !failed_reported_) {
        failed_reported_ = true;
        if (spec_.failed) spec_.failed(st.error);
        return;
    }
    const std::string pill = pill_text(st);
    const std::string info = status_prefix_ + status_text(st, !follow_);
    if (force || pill != pill_shown_) {
        pill_shown_ = pill;
        lv_label_set_text(pill_, pill.c_str());
        const std::uint32_t colour = st.state == LinkState::Open ? kOk
                                     : st.state == LinkState::Lost || st.state == LinkState::Failed ? kError
                                                                                                     : kWarn;
        lv_obj_set_style_bg_color(pill_, lv_color_hex(colour), 0);
        relayout();
    }
    if (force || info != info_shown_) {
        info_shown_ = info;
        lv_label_set_text(info_, info.c_str());
    }
    if (st.log_on != log_on_) {
        log_on_ = st.log_on;
        const int index = static_cast<int>(Tool::Log);
        if (index < static_cast<int>(tools_.size()))
            lv_label_set_text(lv_obj_get_child(tools_[static_cast<std::size_t>(index)], 0), tool_label(Tool::Log).c_str());
    }
}

void TerminalView::tick()
{
    if (!session_) return;
    std::string chunk;
    if (session_->take(chunk, kTakeMax) > 0) buffer_.feed(chunk);
    const std::int64_t now = now_ms();
    if (now - last_status_ >= kStatusMs) {
        last_status_ = now;
        update_status(false);
        if (failed_reported_) return;
    }
    if (now - last_mode_ >= kModeMs) {
        last_mode_ = now;
        cdk::osk_policy::Policy *policy = cdk::osk_policy::installed();
        set_mode(policy && policy->allowed(now) ? Mode::Field : Mode::Raw);
    }
    if (editing() != was_editing_) relayout();
    render(false);
}

void TerminalView::send(const std::string &bytes)
{
    if (bytes.empty() || !session_) return;
    session_->send(bytes);
    if (spec_.settings.echo) buffer_.feed(echo_of(bytes));
}

void TerminalView::submit_line(const std::string &text)
{
    const std::string line = text; /* the field's own string: copied before it is cleared */
    send(line + ending_bytes(spec_.settings.ending));
    if (field_) field_->set_text("");
}

void TerminalView::set_follow(bool follow)
{
    follow_ = follow;
    const int index = static_cast<int>(Tool::Pause);
    if (index < static_cast<int>(tools_.size()))
        lv_label_set_text(lv_obj_get_child(tools_[static_cast<std::size_t>(index)], 0), tool_label(Tool::Pause).c_str());
    update_status(false);
    render(true);
}

void TerminalView::scroll(int delta)
{
    if (delta == 0) return;
    const bool bottom = scroll_view(buffer_, cols_, rows_, &top_, delta);
    if (bottom != follow_) set_follow(bottom);
    else render(true);
}

void TerminalView::act(KeyAct action)
{
    switch (action) {
    case KeyAct::Leave:
        if (spec_.leave) spec_.leave();
        break;
    case KeyAct::Pause: set_follow(!follow_); break;
    case KeyAct::Clear:
        buffer_.clear();
        set_follow(true);
        break;
    case KeyAct::Log: tool(Tool::Log); break;
    case KeyAct::Reset: tool(Tool::Reset); break;
    case KeyAct::SendEsc: send("\x1b"); break;
    case KeyAct::PageUp: scroll(-std::max(1, rows_ - 1)); break;
    case KeyAct::PageDown: scroll(std::max(1, rows_ - 1)); break;
    case KeyAct::Send:
    case KeyAct::None: break;
    }
}

void TerminalView::tool(Tool which)
{
    switch (which) {
    case Tool::CtrlC: send("\x03"); break;
    case Tool::CtrlD: send("\x04"); break;
    case Tool::CtrlZ: send("\x1a"); break;
    case Tool::Tab: send("\t"); break;
    case Tool::Esc: send("\x1b"); break;
    case Tool::Enter: send(ending_bytes(spec_.settings.ending)); break;
    case Tool::Pause: act(KeyAct::Pause); break;
    case Tool::Clear: act(KeyAct::Clear); break;
    case Tool::Log:
        if (log_on_) session_->set_log(false, std::string());
        else session_->set_log(true, spec_.log_dir + "/" + log_file_name(spec_.port.tty, std::time(nullptr)));
        break;
    case Tool::Reset: session_->reset_board(); break;
    }
}

void TerminalView::on_key(const struct key_item &item)
{
    const KeyResult r = translate_key(item.key_code, item.mods, item.keysym, item.utf8, spec_.settings.ending,
                                      spec_.settings.backspace);
    if (r.act == KeyAct::Send) send(r.bytes);
    else if (r.act == KeyAct::Leave && item.key_state != KBD_KEY_PRESSED) return; /* a held Esc leaves once */
    else act(r.act);
}

void TerminalView::keyboard_event_cb(lv_event_t *event)
{
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(event));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(event));
    if (!self || !item || item->key_state == KBD_KEY_RELEASED) return;
    if (self->mode_ == Mode::Raw) {
        self->on_key(*item);
        return;
    }
    // the on-screen keyboard's one-shot Ctrl while the line is edited: the control code goes to the port (the field
    // itself types no text for a Ctrl key)
    if (self->mode_ == Mode::Field && self->editing() && (item->mods & KBD_MOD_CTRL)) {
        const KeyResult r = translate_key(item->key_code, item->mods, item->keysym, item->utf8,
                                          self->spec_.settings.ending, self->spec_.settings.backspace);
        if (r.act == KeyAct::Send) self->send(r.bytes);
    }
}

void TerminalView::out_event_cb(lv_event_t *event)
{
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(event));
    if (!self) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: self->drag_ = 0; break;
    case LV_EVENT_PRESSING: {
        lv_indev_t *indev = lv_indev_active();
        if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) break;
        lv_point_t v{};
        lv_indev_get_vect(indev, &v);
        self->drag_ += v.y;
        int steps = 0;
        while (self->drag_ >= self->line_h_) {
            self->drag_ -= self->line_h_;
            --steps; /* the finger goes down: older text */
        }
        while (self->drag_ <= -self->line_h_) {
            self->drag_ += self->line_h_;
            ++steps;
        }
        self->scroll(steps);
        break;
    }
    case LV_EVENT_KEY: { /* the keypad path (no physical keyboard, or the on-screen keyboard's keys) */
        const uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_ESC && self->spec_.leave) self->spec_.leave();
        else if (key == LV_KEY_UP) self->scroll(-1);
        else if (key == LV_KEY_DOWN) self->scroll(1);
        break;
    }
    default: break;
    }
}

void TerminalView::tool_event_cb(lv_event_t *event)
{
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(event));
    auto *button = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (!self || !button) return;
    self->tool(static_cast<Tool>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button))));
}

void TerminalView::send_event_cb(lv_event_t *event)
{
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(event));
    if (self && self->field_) self->submit_line(self->field_->text());
}

} // namespace sc
