/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl key echo (design QA of 0.2.0, D-32; the launcher's decision of 2026-10-05: "a real W/E/R/T press is recorded
 * and the matching native key within 300 ms is ignored"). After every key event (LV_EVENT_KEYBOARD, the physical key
 * Vinyl reads its shortcuts from) the toolkit's keypad also hands LVGL a key (LV_EVENT_KEY): a navigation key's own
 * LVGL key, the arrow of an F / X / Z / C alias (a small keyboard's arrows), and for every other key its raw evdev code
 * unchanged (cp0_keyboard_lvgl_input.c at the pin, `default: return code;`). Some raw codes are LVGL keys: W, E, R, T
 * (17-20) are Up, Down, Right, Left; the digits 1, 2, 7, 8, 9, 0 (2, 3, 8, 9, 10, 11) are Home, End, Backspace, Next,
 * Enter, Prev. So T opened the Sleep timer page and its echo (Left) went straight back, R toggled repeat and acted as
 * Right on a list row, 9 acted as Enter.
 *
 * KeyEcho drops such an echo: the raw code of any key that is not a navigation key, when it is an LVGL key, always; an
 * alias's arrow only when Vinyl used the letter as a shortcut (X and C on the queue page), so F / X / Z / C stay arrows
 * everywhere else. Real arrows, Enter, Esc, Backspace, Delete, Home and End are never dropped. The window: 300 ms from
 * the press or its repeat. Pure (the caller's monotonic clock), tested with a fake keypad (tests/test_menu.cpp).
 */
#pragma once

#include <cstdint>

namespace vinyl {

/* LVGL's keys (lv_group.h) */
constexpr uint32_t kLvKeyUp = 17, kLvKeyDown = 18, kLvKeyRight = 19, kLvKeyLeft = 20, kLvKeyEscape = 27, kLvKeyDel = 127,
                   kLvKeyBackspace = 8, kLvKeyEnterKey = 10, kLvKeyNext = 9, kLvKeyPrev = 11, kLvKeyHome = 2, kLvKeyEnd = 3;

/* The key the toolkit's keypad hands LVGL after a key event (key_code: evdev; semantic_key: the toolkit's alias, 0 for
 * none), as cp0_keyboard_lvgl_input.c does at the pin. */
uint32_t keypad_echo(uint32_t key_code, uint32_t semantic_key);
/* A key the keypad maps to an LVGL key on purpose (the arrows, Esc, Delete, Backspace, Enter, Next, Tab, Prev, Home,
 * End). */
bool is_navigation_key(uint32_t evdev);
/* A value LVGL treats as a key (Up, Down, Right, Left, Esc, Delete, Backspace, Enter, Next, Prev, Home, End). */
bool is_lvgl_key(uint32_t lv_key);

class KeyEcho {
public:
    static constexpr uint64_t kWindowMs = 300;
    /* a key event (state: 0 release, 1 press, 2 repeat); used: Vinyl acted on it as a shortcut */
    void seen(uint32_t key_code, uint32_t semantic_key, int state, bool used, uint64_t now_ms);
    /* the LVGL key that follows: true when it is the echo to drop */
    bool drop(uint32_t lv_key, uint64_t now_ms) const;

private:
    uint32_t key_ = 0;
    uint64_t until_ = 0;
};

}  // namespace vinyl
