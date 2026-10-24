/*
 * SPDX-License-Identifier: MIT
 *
 * The keys of the shared app platform (cp0_lvgl's key_item: the Linux evdev code, the state, the modifiers, the xkb
 * keysym and the UTF-8 text) turned into Markdown Notes' KeyEvent. No LVGL and no cp0_lvgl header: unit tested on the PC;
 * stage.cpp checks that the constants below are cp0_lvgl's. The same rules as Mesh Hop's bridge (key_bridge.hpp there):
 * a key read from a keyboard (it has an xkb keysym) goes through the KeyTranslator (US or FR layout, the modifiers
 * tracked from the modifier keys themselves); a key queued without a keysym (the on-screen keyboard, the scripted tests)
 * carries its modifiers and its text with it.
 */
#pragma once

#include "keys.hpp"

#include <cstdint>
#include <string>

namespace notesui {

class KeyBridge {
public:
    enum : int { kReleased = 0, kPressed = 1, kRepeated = 2 };                              // KBD_KEY_*
    enum : uint32_t { kModShift = 1u << 0, kModCtrl = 1u << 1, kModAlt = 1u << 2 };        // KBD_MOD_*

    explicit KeyBridge(Layout layout = Layout::US) : keys_(layout) {}

    /* One key_item. True when `out` holds a key to act on (a press or a repeat). */
    bool feed(uint32_t code, int state, uint32_t mods, uint32_t keysym, const std::string &utf8, KeyEvent &out);

private:
    KeyTranslator keys_;
};

} // namespace notesui
