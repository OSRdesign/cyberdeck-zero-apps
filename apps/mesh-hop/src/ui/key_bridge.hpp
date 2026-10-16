/*
 * SPDX-License-Identifier: MIT
 *
 * The keys of the shared app platform (cp0_lvgl's key_item: the Linux evdev code, the state, the modifiers, the xkb
 * keysym and the UTF-8 text) turned into Mesh Hop's KeyEvent. No LVGL and no cp0_lvgl header: unit tested on the PC
 * (tests/unit/test_ui.cpp); stage.cpp checks that the constants below are cp0_lvgl's.
 *
 * A key read from a keyboard (it has an xkb keysym) goes through the same KeyTranslator as the app's old evdev reader:
 * the US or FR layout of detect_layout() (cp0_lvgl's xkb keymap is always "us"), the modifiers tracked from the
 * modifier keys themselves (AltGr is the right Alt, which xkb's "us" map reports as plain Alt), Caps Lock, Tab /
 * Shift+Tab, the keypad. A key queued without a keysym (cp0_keyboard_inject_key: the on-screen keyboard, the scripted
 * tests) carries its modifiers and its text with it: the given text wins for a character key, the modifiers are
 * added. A key the translator does not map that still has a printable text (a character without a key code, the ISO
 * key next to the left Shift) types that text.
 */

#pragma once

#include "ui_logic.hpp"

#include <cstdint>
#include <string>

namespace meshhop {

class KeyBridge {
public:
    enum : int { kReleased = 0, kPressed = 1, kRepeated = 2 };                              // KBD_KEY_*
    enum : uint32_t { kModShift = 1u << 0, kModCtrl = 1u << 1, kModAlt = 1u << 2 };        // KBD_MOD_*

    explicit KeyBridge(Layout layout = Layout::US) : keys_(layout) {}

    /* One key_item. True when `out` holds a key to act on (a press or a repeat); releases and modifier keys only
     * update the state. */
    bool feed(uint32_t code, int state, uint32_t mods, uint32_t keysym, const std::string &utf8, KeyEvent &out);

private:
    KeyTranslator keys_;
};

} // namespace meshhop
