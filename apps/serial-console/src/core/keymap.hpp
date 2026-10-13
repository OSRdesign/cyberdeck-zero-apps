/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: what a key of a physical keyboard does on the terminal page (no LVGL; tests/test_core.cpp).
 *
 * With a keyboard every key goes to the port at once:
 *   letters, digits, symbols   their UTF-8 text
 *   Enter                      the line ending of the port's settings (CR, LF or CR LF)
 *   Backspace                  0x7F (DEL) or 0x08 (BS), per the settings
 *   Tab, Shift+Tab             0x09, ESC [ Z
 *   Ctrl+letter                its control code (Ctrl+C = 0x03 ...), Ctrl+[ \ ] ^ _ and Ctrl+Space too
 *   arrows                     ESC [ A / B / C / D
 *   Home End Insert Delete     ESC [ H, ESC [ F, ESC [ 2 ~, ESC [ 3 ~
 *   Page Up / Down             ESC [ 5 ~ / ESC [ 6 ~
 *   F1 .. F12                  the xterm sequences
 *   Alt+key                    ESC then the key (a terminal's Meta)
 * Except the app's own keys:
 *   Esc                        leaves the terminal page (the connection is closed); a real ESC is sent by the toolbar
 *   Shift+Page Up / Down       scroll the text one page
 *   Alt+P pause, Alt+X clear, Alt+L log on/off, Alt+R reset the board, Alt+E send ESC (the toolbar's keys)
 */
#pragma once

#include "settings.hpp"

#include <cstdint>
#include <string>

namespace sc {

enum class KeyAct { None, Send, Leave, Pause, Clear, Log, Reset, SendEsc, PageUp, PageDown };

struct KeyResult {
    KeyAct act = KeyAct::None;
    std::string bytes; /* Send */
};

/* key_code: Linux KEY_*; mods: KBD_MOD_* bits (shift 1, ctrl 2, alt 4); keysym: XKB keysym (0 when unknown); utf8: the
 * text the key types ("" for none). Releases are the caller's to skip. */
KeyResult translate_key(std::uint32_t key_code, std::uint32_t mods, std::uint32_t keysym, const char *utf8,
                        Ending ending, Backspace backspace);

/* The KBD_MOD_* bits used (keyboard_input.h of cp0_lvgl; repeated so core/ needs no LVGL header). */
constexpr std::uint32_t kModShift = 1u << 0;
constexpr std::uint32_t kModCtrl = 1u << 1;
constexpr std::uint32_t kModAlt = 1u << 2;

} // namespace sc
