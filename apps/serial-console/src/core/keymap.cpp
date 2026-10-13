/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: keys of the terminal page (see keymap.hpp).
 */
#include "keymap.hpp"

#include <linux/input-event-codes.h>

#include <cstring>

namespace sc {
namespace {

/* The letter a key code stands for on a US layout (the fallback when neither keysym nor text says it). */
char letter_of_code(std::uint32_t code)
{
    static const struct {
        std::uint32_t code;
        char letter;
    } kLetters[] = {{KEY_A, 'a'}, {KEY_B, 'b'}, {KEY_C, 'c'}, {KEY_D, 'd'}, {KEY_E, 'e'}, {KEY_F, 'f'}, {KEY_G, 'g'},
                    {KEY_H, 'h'}, {KEY_I, 'i'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'}, {KEY_M, 'm'}, {KEY_N, 'n'},
                    {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_Q, 'q'}, {KEY_R, 'r'}, {KEY_S, 's'}, {KEY_T, 't'}, {KEY_U, 'u'},
                    {KEY_V, 'v'}, {KEY_W, 'w'}, {KEY_X, 'x'}, {KEY_Y, 'y'}, {KEY_Z, 'z'}};
    for (const auto &entry : kLetters)
        if (entry.code == code) return entry.letter;
    return 0;
}

/* The character the key types, for Ctrl and Alt: the keysym (layout-aware) when it is ASCII, else its text, else the
 * US letter of the key code. */
char base_char(std::uint32_t key_code, std::uint32_t keysym, const char *utf8)
{
    if (keysym >= 0x20 && keysym < 0x7F) return static_cast<char>(keysym);
    if (utf8 && utf8[0] && !utf8[1] && static_cast<unsigned char>(utf8[0]) >= 0x20 &&
        static_cast<unsigned char>(utf8[0]) < 0x7F)
        return utf8[0];
    if (key_code == KEY_SPACE) return ' ';
    return letter_of_code(key_code);
}

KeyResult send(const std::string &bytes)
{
    KeyResult r;
    r.act = KeyAct::Send;
    r.bytes = bytes;
    return r;
}

KeyResult act(KeyAct a)
{
    KeyResult r;
    r.act = a;
    return r;
}

} // namespace

KeyResult translate_key(std::uint32_t key_code, std::uint32_t mods, std::uint32_t keysym, const char *utf8,
                        Ending ending, Backspace backspace)
{
    const bool shift = mods & kModShift, ctrl = mods & kModCtrl, alt = mods & kModAlt;
    const std::string esc = "\x1b";

    switch (key_code) {
    case KEY_ESC: return act(KeyAct::Leave);
    case KEY_ENTER:
    case KEY_KPENTER: return send(ending_bytes(ending));
    case KEY_BACKSPACE: return send(std::string(1, backspace_byte(backspace)));
    case KEY_TAB: return send(shift ? esc + "[Z" : "\t");
    case KEY_UP: return send(esc + "[A");
    case KEY_DOWN: return send(esc + "[B");
    case KEY_RIGHT: return send(esc + "[C");
    case KEY_LEFT: return send(esc + "[D");
    case KEY_HOME: return send(esc + "[H");
    case KEY_END: return send(esc + "[F");
    case KEY_INSERT: return send(esc + "[2~");
    case KEY_DELETE: return send(esc + "[3~");
    case KEY_PAGEUP: return shift ? act(KeyAct::PageUp) : send(esc + "[5~");
    case KEY_PAGEDOWN: return shift ? act(KeyAct::PageDown) : send(esc + "[6~");
    case KEY_F1: return send(esc + "OP");
    case KEY_F2: return send(esc + "OQ");
    case KEY_F3: return send(esc + "OR");
    case KEY_F4: return send(esc + "OS");
    case KEY_F5: return send(esc + "[15~");
    case KEY_F6: return send(esc + "[17~");
    case KEY_F7: return send(esc + "[18~");
    case KEY_F8: return send(esc + "[19~");
    case KEY_F9: return send(esc + "[20~");
    case KEY_F10: return send(esc + "[21~");
    case KEY_F11: return send(esc + "[23~");
    case KEY_F12: return send(esc + "[24~");
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
    case KEY_CAPSLOCK: return act(KeyAct::None); /* modifiers alone send nothing */
    default: break;
    }

    const char base = base_char(key_code, keysym, utf8);
    if (alt && !ctrl) {
        switch (base) {
        case 'p':
        case 'P': return act(KeyAct::Pause);
        case 'x':
        case 'X': return act(KeyAct::Clear);
        case 'l':
        case 'L': return act(KeyAct::Log);
        case 'r':
        case 'R': return act(KeyAct::Reset);
        case 'e':
        case 'E': return act(KeyAct::SendEsc);
        default: break;
        }
    }
    if (ctrl) {
        int code = -1; /* int: char is unsigned on the boards */
        if (base >= 'a' && base <= 'z') code = base - 'a' + 1;
        else if (base >= 'A' && base <= 'Z') code = base - 'A' + 1;
        else if (base == '[') code = 0x1B;
        else if (base == '\\') code = 0x1C;
        else if (base == ']') code = 0x1D;
        else if (base == '^' || base == '6') code = 0x1E;
        else if (base == '_' || base == '-') code = 0x1F;
        else if (base == ' ' || base == '@' || base == '2') code = 0x00;
        else if (base == '?') code = 0x7F;
        else if (utf8 && utf8[0] && !utf8[1] && static_cast<unsigned char>(utf8[0]) < 0x20)
            code = static_cast<unsigned char>(utf8[0]); /* the keyboard already made the control code */
        if (code < 0) return act(KeyAct::None);
        return send(std::string(alt ? esc : "") + std::string(1, static_cast<char>(code)));
    }
    if (!utf8 || !utf8[0]) return act(KeyAct::None);
    std::string text(utf8, strnlen(utf8, 16));
    if (static_cast<unsigned char>(text[0]) < 0x20 && text != "\t") return act(KeyAct::None);
    return send(alt ? esc + text : text);
}

} // namespace sc
