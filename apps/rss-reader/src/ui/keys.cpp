/*
 * SPDX-License-Identifier: MIT
 */
#include "keys.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rssui {

namespace {

enum : int {
    kEsc = 1, kBackspace = 14, kTab = 15, kEnter = 28, kLCtrl = 29, kLShift = 42, kRShift = 54, kKpAsterisk = 55, kLAlt = 56,
    kSpace = 57, kCaps = 58, kF1 = 59, kF10 = 68, kF11 = 87, kF12 = 88, kKpEnter = 96, kRCtrl = 97, kKpSlash = 98, kRAlt = 100,
    kHome = 102, kUp = 103, kPageUp = 104, kLeft = 105, kRight = 106, kEnd = 107, kDown = 108, kPageDown = 109, kDelete = 111,
};

struct KeyChars {
    const char *base, *shift, *altgr;
};

/* evdev codes 2..53 (the digit row to the slash key); 86 (the extra key next to left shift) is not mapped. */
KeyChars us_chars(int code)
{
    static const char *row_digit_b = "1234567890-=";
    static const char *row_digit_s = "!@#$%^&*()_+";
    static const char *row_q_b = "qwertyuiop[]";
    static const char *row_q_s = "QWERTYUIOP{}";
    static const char *row_a_b = "asdfghjkl;'`";
    static const char *row_a_s = "ASDFGHJKL:\"~";
    static const char *row_z_b = "zxcvbnm,./";
    static const char *row_z_s = "ZXCVBNM<>?";
    static thread_local char buf[3][2];
    auto one = [&](int slot, char c) { buf[slot][0] = c; buf[slot][1] = 0; return buf[slot]; };
    char b = 0, s = 0;
    if (code >= 2 && code <= 13) { b = row_digit_b[code - 2]; s = row_digit_s[code - 2]; }
    else if (code >= 16 && code <= 27) { b = row_q_b[code - 16]; s = row_q_s[code - 16]; }
    else if (code >= 30 && code <= 41) { b = row_a_b[code - 30]; s = row_a_s[code - 30]; }
    else if (code == 43) { b = '\\'; s = '|'; }
    else if (code >= 44 && code <= 53) { b = row_z_b[code - 44]; s = row_z_s[code - 44]; }
    else return {"", "", ""};
    return {one(0, b), one(1, s), ""};
}

KeyChars fr_chars(int code)
{
    switch (code) {
    case 2: return {"&", "1", ""};
    case 3: return {"\xC3\xA9", "2", "~"};
    case 4: return {"\"", "3", "#"};
    case 5: return {"'", "4", "{"};
    case 6: return {"(", "5", "["};
    case 7: return {"-", "6", "|"};
    case 8: return {"\xC3\xA8", "7", "`"};
    case 9: return {"_", "8", "\\"};
    case 10: return {"\xC3\xA7", "9", "^"};
    case 11: return {"\xC3\xA0", "0", "@"};
    case 12: return {")", "\xC2\xB0", "]"};
    case 13: return {"=", "+", "}"};
    case 16: return {"a", "A", ""};
    case 17: return {"z", "Z", ""};
    case 18: return {"e", "E", "\xE2\x82\xAC"};
    case 26: return {"^", "\xC2\xA8", ""};
    case 27: return {"$", "\xC2\xA3", "\xC2\xA4"};
    case 30: return {"q", "Q", ""};
    case 39: return {"m", "M", ""};
    case 40: return {"\xC3\xB9", "%", ""};
    case 41: return {"\xC2\xB2", "", ""};
    case 43: return {"*", "\xC2\xB5", ""};
    case 44: return {"w", "W", ""};
    case 50: return {",", "?", ""};
    case 51: return {";", ".", ""};
    case 52: return {":", "/", ""};
    case 53: return {"!", "\xC2\xA7", ""};
    default: return us_chars(code);
    }
}

} // namespace

Layout layout_from_name(const std::string &name)
{
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("fr", 0) == 0) return Layout::FR;
    return Layout::US;
}

std::string xkb_layout_from_text(const std::string &text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("XKBLAYOUT=", 0) != 0) continue;
        std::string v = line.substr(10);
        v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
        v.erase(std::remove(v.begin(), v.end(), '\''), v.end());
        return v;
    }
    return "";
}

Layout detect_layout()
{
    if (const char *e = std::getenv("RSSREADER_KEYMAP"); e && *e) return layout_from_name(e);
    std::ifstream f("/etc/default/keyboard");
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        return layout_from_name(xkb_layout_from_text(ss.str()));
    }
    return Layout::US;
}

void KeyTranslator::reset()
{
    lshift_ = rshift_ = lctrl_ = rctrl_ = lalt_ = ralt_ = false;
}

bool KeyTranslator::feed(int code, int value, KeyEvent &out)
{
    const bool down = value != 0;
    switch (code) {
    case kLShift: lshift_ = down; return false;
    case kRShift: rshift_ = down; return false;
    case kLCtrl: lctrl_ = down; return false;
    case kRCtrl: rctrl_ = down; return false;
    case kLAlt: lalt_ = down; return false;
    case kRAlt: ralt_ = down; return false;
    case kCaps:
        if (value == 1) caps_ = !caps_;
        return false;
    default: break;
    }
    if (value == 0) return false;
    out = KeyEvent{};
    out.code = code;
    out.repeat = value == 2;
    out.shift = lshift_ || rshift_;
    out.ctrl = lctrl_ || rctrl_;
    out.alt = lalt_;
    const bool altgr = ralt_;
    switch (code) {
    case kEsc: out.key = Key::Esc; return true;
    case kEnter:
    case kKpEnter: out.key = Key::Enter; return true;
    case kTab: out.key = out.shift ? Key::BackTab : Key::Tab; return true;
    case kBackspace: out.key = Key::Backspace; return true;
    case kDelete: out.key = Key::Delete; return true;
    case kUp: out.key = Key::Up; return true;
    case kDown: out.key = Key::Down; return true;
    case kLeft: out.key = Key::Left; return true;
    case kRight: out.key = Key::Right; return true;
    case kPageUp: out.key = Key::PageUp; return true;
    case kPageDown: out.key = Key::PageDown; return true;
    case kHome: out.key = Key::Home; return true;
    case kEnd: out.key = Key::End; return true;
    default: break;
    }
    if ((code >= kF1 && code <= kF10) || code == kF11 || code == kF12) {
        out.key = Key::Function;
        out.fn = code <= kF10 ? code - kF1 + 1 : code - kF11 + 11;
        return true;
    }
    out.key = Key::Char;
    if (code == kSpace) {
        out.text = " ";
        return true;
    }
    static const char *const kp = "789-456+1230.";
    if (code >= 71 && code <= 83) {
        out.text = std::string(1, kp[code - 71]);
        return true;
    }
    if (code == kKpAsterisk) {
        out.text = "*";
        return true;
    }
    if (code == kKpSlash) {
        out.text = "/";
        return true;
    }
    const KeyChars kc = layout_ == Layout::FR ? fr_chars(code) : us_chars(code);
    std::string t;
    if (altgr && kc.altgr[0]) {
        t = kc.altgr;
    } else {
        bool shift = out.shift;
        const unsigned char b0 = static_cast<unsigned char>(kc.base[0]);
        if (caps_ && kc.base[1] == 0 && std::islower(b0)) shift = !shift;     // caps lock only acts on letters
        t = shift ? kc.shift : kc.base;
    }
    if (t.empty()) {
        out.key = Key::None;
        return false;
    }
    out.text = t;
    return true;
}

bool is_ctrl_letter(const KeyEvent &e, char letter)
{
    if (e.key != Key::Char || !e.ctrl || e.alt || e.repeat || e.text.size() != 1) return false;
    return std::tolower(static_cast<unsigned char>(e.text[0])) == std::tolower(static_cast<unsigned char>(letter));
}

bool is_typing(const KeyEvent &e)
{
    return e.key == Key::Char && !e.ctrl && !e.alt && !e.text.empty();
}

} // namespace rssui
