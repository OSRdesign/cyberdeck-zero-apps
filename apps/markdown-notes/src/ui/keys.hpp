/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: the keys. A KeyEvent is what the pages act on; KeyTranslator turns evdev codes into it with the US or the
 * French AZERTY layout (RSS Reader's copy of Mesh Hop's tables: a shared copy in apps/_common is a follow-up).
 * No LVGL, no I/O except detect_layout(): unit tested on the PC (tests/unit/test_ui.cpp).
 */
#pragma once

#include <string>

namespace notesui {

enum class Key {
    None, Char, Enter, Esc, Tab, BackTab, Up, Down, Left, Right, PageUp, PageDown, Home, End, Backspace, Delete, Function,
};

struct KeyEvent {
    Key key = Key::None;
    std::string text;            /* UTF-8, for Key::Char */
    bool ctrl = false, alt = false, shift = false;
    bool repeat = false;         /* auto-repeat of a held key (Esc, Enter and Tab ignore it) */
    int fn = 0;                  /* Key::Function: 1..12 */
    int code = 0;                /* the evdev code */
};

enum class Layout { US, FR };
/* "us", "fr", "fr(azerty)"... (case insensitive); anything else: US. */
Layout layout_from_name(const std::string &name);
/* The XKBLAYOUT="..." line of /etc/default/keyboard (the text of that file) -> layout name, "" when absent. */
std::string xkb_layout_from_text(const std::string &text);
/* $MDNOTES_KEYMAP, else /etc/default/keyboard, else US. */
Layout detect_layout();

/* Feeds evdev EV_KEY events (value 0 release, 1 press, 2 repeat). True when `out` holds a key to act on (a press or a
 * repeat of a key that does something); modifier keys only update the state. */
class KeyTranslator {
public:
    explicit KeyTranslator(Layout layout = Layout::US) : layout_(layout) {}
    bool feed(int code, int value, KeyEvent &out);
    void set_layout(Layout l) { layout_ = l; }
    void reset();

private:
    Layout layout_;
    bool lshift_ = false, rshift_ = false, lctrl_ = false, rctrl_ = false, lalt_ = false, ralt_ = false, caps_ = false;
};

/* Ctrl + that letter (either case), not a repeat. */
bool is_ctrl_letter(const KeyEvent &e, char letter);
/* A character key that types something (no Ctrl / Alt). */
bool is_typing(const KeyEvent &e);

} // namespace notesui
