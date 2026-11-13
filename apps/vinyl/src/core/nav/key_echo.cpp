/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl key echo (see key_echo.hpp).
 */
#include "key_echo.hpp"

namespace vinyl {

namespace {
/* linux/input-event-codes.h */
constexpr uint32_t kEvEsc = 1, kEvBackspace = 14, kEvTab = 15, kEvEnter = 28, kEvKpEnter = 96, kEvHome = 102, kEvUp = 103,
                   kEvLeft = 105, kEvRight = 106, kEvEnd = 107, kEvDown = 108, kEvDelete = 111, kEvNext = 407,
                   kEvPrevious = 412;
}  // namespace

bool is_navigation_key(uint32_t c) {
    switch (c) {
    case kEvUp: case kEvDown: case kEvLeft: case kEvRight: case kEvEsc: case kEvDelete: case kEvBackspace: case kEvEnter:
    case kEvKpEnter: case kEvNext: case kEvTab: case kEvPrevious: case kEvHome: case kEvEnd: return true;
    default: return false;
    }
}

uint32_t keypad_echo(uint32_t key_code, uint32_t semantic_key) {
    const uint32_t c = semantic_key ? semantic_key : key_code;
    switch (c) {
    case kEvUp: return kLvKeyUp;
    case kEvDown: return kLvKeyDown;
    case kEvRight: return kLvKeyRight;
    case kEvLeft: return kLvKeyLeft;
    case kEvEsc: return kLvKeyEscape;
    case kEvDelete: return kLvKeyDel;
    case kEvBackspace: return kLvKeyBackspace;
    case kEvEnter:
    case kEvKpEnter: return kLvKeyEnterKey;
    case kEvNext: return kLvKeyNext;
    case kEvTab: return kEvTab;
    case kEvPrevious: return kLvKeyPrev;
    case kEvHome: return kLvKeyHome;
    case kEvEnd: return kLvKeyEnd;
    default: return c;
    }
}

bool is_lvgl_key(uint32_t k) {
    switch (k) {
    case kLvKeyUp: case kLvKeyDown: case kLvKeyRight: case kLvKeyLeft: case kLvKeyEscape: case kLvKeyDel:
    case kLvKeyBackspace: case kLvKeyEnterKey: case kLvKeyNext: case kLvKeyPrev: case kLvKeyHome: case kLvKeyEnd: return true;
    default: return false;
    }
}

void KeyEcho::seen(uint32_t key_code, uint32_t semantic_key, int state, bool used, uint64_t now_ms) {
    if (state != 1 && state != 2) return; /* a release hands LVGL no key event of its own to act on */
    if (is_navigation_key(key_code)) return; /* meant */
    const uint32_t echo = keypad_echo(key_code, semantic_key);
    const bool alias = semantic_key != 0 && semantic_key != key_code;
    const bool drop_it = alias ? used : is_lvgl_key(echo); /* an alias stays an arrow unless Vinyl used the letter */
    if (!drop_it) return;
    key_ = echo;
    until_ = now_ms + kWindowMs;
}

bool KeyEcho::drop(uint32_t lv_key, uint64_t now_ms) const { return key_ != 0 && lv_key == key_ && now_ms <= until_; }

}  // namespace vinyl
