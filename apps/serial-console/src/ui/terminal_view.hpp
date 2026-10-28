/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the terminal page (LVGL thread only). It fills the page under the header (the header's title is
 * "ttyUSB0 115200 8N1"):
 *   status line   a pill (Connected / Port lost: waiting / Opening) and RX / TX counters, Paused, the log file
 *   text          the received text in the launcher's monospace face at the CAPTION size (on a portrait canvas, PDA
 *                 mode: smaller where CAPTION leaves fewer columns than the deck has, never under the deck's size in
 *                 millimetres, board_text_px), cut into rows of the view's width (core/term.hpp); it follows new
 *                 text, a drag (or Shift+Page Up / Down) scrolls back and pauses it, scrolling to the bottom follows
 *                 again; redrawn at most 10 times a second
 *   toolbar       one row of TARGET-high keys that scrolls sideways: Ctrl-C, Ctrl-D, Ctrl-Z, Tab, Send Esc, Enter,
 *                 Pause / Resume, Clear, Start / Stop log, Reset board
 *   input         only without a physical keyboard (the shared on-screen keyboard rule, _common/osk_policy): a
 *                 one-line TextField and Send; a tap on the field brings the on-screen keyboard, Enter sends the line
 *                 with the port's line ending and keeps the keyboard up; while it is up the status line, the toolbar
 *                 and Send give their room to the text
 * With a physical keyboard every key goes to the port at once (core/keymap.hpp); Esc leaves the page (the connection
 * closes). The serial I/O runs in a core Session (worker thread); the view drains it on the app's timer.
 */
#pragma once

#include "console_core.hpp"
#include "keymap.hpp"
#include "serial_io.hpp"
#include "term.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "keyboard_input.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sc {

/* The launcher's monospace face at px (the builtin font when it is missing). */
const lv_font_t *mono_font(int px);
/* The size of the board's text (the terminal, the boot output and evidence pages). Landscape and square: CAPTION. A
 * portrait canvas (PDA mode) gets the Standard class's CAPTION (18 px) on panels of about 8.5 px/mm: a third larger than
 * on the deck, and only 40 columns at 480 wide. There the text stays at CAPTION while the terminal keeps at least the
 * deck's 55 columns, else it goes down 2 px at a time to the deck's own size in millimetres (1.6 mm) and never smaller:
 * 14 px and 56 columns on the 480 px wide panels, CAPTION and 69 columns on the 800 px wide tablet. */
int board_text_px(const cp0_ui_roller_page_t &layout);

struct TerminalSpec {
    PortInfo port;
    PortSettings settings;
    int baud = kDefaultBaud;
    Roots roots;
    std::string log_dir;                 /* where "Log to file" writes */
    /* Called inside an LVGL event or the app's timer: the app must not delete the view in them (it posts). */
    std::function<void()> leave;         /* Esc: back to the port page */
    std::function<void(int err)> failed; /* the open was refused for good (no permission, busy) */
};

class TerminalView {
public:
    TerminalView(cp0::ui::Page &page, const cp0_ui_roller_page_t &layout, TerminalSpec spec);
    ~TerminalView();
    TerminalView(const TerminalView &) = delete;
    TerminalView &operator=(const TerminalView &) = delete;

    /* The app's timer (every 50 ms): received text, status, redraw, keyboard mode. */
    void tick();
    /* The object for the page's input group. */
    lv_obj_t *focus_object() const { return out_; }

private:
    enum class Mode { None, Raw, Field };
    enum class Tool { CtrlC, CtrlD, CtrlZ, Tab, Esc, Enter, Pause, Clear, Log, Reset };

    static void out_event_cb(lv_event_t *event);
    static void keyboard_event_cb(lv_event_t *event);
    static void tool_event_cb(lv_event_t *event);
    static void send_event_cb(lv_event_t *event);
    static void box_delete_cb(lv_event_t *event);

    void build();
    /* key = the button's height and minimum width: the 9 mm toolbar key for the key bar, TARGET (7 mm) for Send */
    int button_w(const char *text, int key) const;
    lv_obj_t *make_button(lv_obj_t *parent, const char *text, bool primary, int key);
    void set_mode(Mode mode);
    void raw_keys(bool on);
    void relayout();
    void render(bool force);
    void update_status(bool force);
    void on_key(const struct key_item &item);
    void act(KeyAct action);
    void send(const std::string &bytes);
    void submit_line(const std::string &text);
    void tool(Tool which);
    void scroll(int delta);
    void set_follow(bool follow);
    bool editing() const;
    std::string tool_label(Tool which) const;

    cp0::ui::Page &page_;
    cp0_ui_roller_page_t layout_;
    TerminalSpec spec_;
    std::unique_ptr<Session> session_;
    TermBuffer buffer_;

    lv_obj_t *box_ = nullptr;     /* everything under the header */
    lv_obj_t *status_ = nullptr, *pill_ = nullptr, *info_ = nullptr;
    lv_obj_t *out_ = nullptr, *text_ = nullptr;
    lv_obj_t *bar_ = nullptr;
    std::vector<lv_obj_t *> tools_;
    lv_obj_t *input_ = nullptr, *send_ = nullptr;
    std::unique_ptr<cp0::ui::TextField> field_;
    lv_obj_t *key_screen_ = nullptr;
    int inset_listener_ = 0;

    const lv_font_t *mono_ = nullptr;
    int char_w_ = 1, line_h_ = 1, cols_ = 1, rows_ = 1;
    bool follow_ = true;
    ViewPos top_;
    std::uint64_t drawn_version_ = ~0ull;
    std::string shown_;
    std::int64_t last_render_ = 0, last_status_ = 0, last_mode_ = 0;
    std::string pill_shown_, info_shown_;
    std::string status_prefix_; /* the frame ("8N1  ") when the header has no room for it */
    bool log_on_ = false;
    Mode mode_ = Mode::None;
    bool was_editing_ = false;
    bool raw_on_ = false;
    cp0_keyboard_input_context_t saved_context_ = KBD_INPUT_CONTEXT_NAVIGATION;
    int saved_intercept_ = 0;
    int drag_ = 0;
    bool failed_reported_ = false;
};

} // namespace sc
