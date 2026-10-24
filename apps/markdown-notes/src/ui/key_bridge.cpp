/*
 * SPDX-License-Identifier: MIT
 */
#include "key_bridge.hpp"

namespace notesui {

namespace {

/* Text that types something: not empty, no control character (an xkb Ctrl+letter gives one, Enter gives "\r"). */
bool printable(const std::string &s)
{
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

} // namespace

bool KeyBridge::feed(uint32_t code, int state, uint32_t mods, uint32_t keysym, const std::string &utf8, KeyEvent &out)
{
    if (state != kReleased && state != kPressed && state != kRepeated) return false;
    KeyEvent t;
    const bool have = code != 0 && code < 0x300 && keys_.feed(static_cast<int>(code), state, t);
    if (state == kReleased) return false;
    const bool queued = keysym == 0;                 // not from a keyboard's xkb state: the modifiers come with the key
    if (have) {
        out = t;
    } else if (printable(utf8)) {                    // a character without a key (an accent of the on-screen keyboard)
        out = KeyEvent{};
        out.key = Key::Char;
        out.code = static_cast<int>(code);
        out.text = utf8;
        out.repeat = state == kRepeated;
    } else {
        return false;
    }
    if (mods & kModCtrl) out.ctrl = true;
    if (queued) {
        if (mods & kModAlt) out.alt = true;
        if (mods & kModShift) {
            out.shift = true;
            if (out.key == Key::Tab) out.key = Key::BackTab;
        }
        if (out.key == Key::Char && printable(utf8)) out.text = utf8;
    }
    return out.key != Key::None;
}

} // namespace notesui
