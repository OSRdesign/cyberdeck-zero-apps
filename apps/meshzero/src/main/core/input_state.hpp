/*
 * SPDX-License-Identifier: MIT
 *
 * Small input helpers that need no LVGL: which key codes mean "Delete", and whether a keyboard is attached and awake.
 */

#pragma once

#include <cstdint>
#include <string>

namespace meshzero {

/* Delete arrives as the evdev code 111 in the raw keyboard event (key_item.key_code) and as LV_KEY_DEL (127) once the
 * launcher converted it for the LVGL keypad path (cp0_keyboard_lvgl_input.c). Both mean Delete. */
constexpr uint32_t kEvdevKeyDelete = 111;
constexpr uint32_t kLvKeyDel = 127;
inline bool is_delete_key(uint32_t code)
{
    return code == kEvdevKeyDelete || code == kLvKeyDel;
}

/* True when the text of /proc/bus/input/devices lists an input device that has the letter keys (A, Z), Enter and
 * Space: a keyboard (a sleeping Bluetooth keyboard is simply not listed). */
bool keyboard_listed(const std::string &proc_devices_text);
/* Reads /proc/bus/input/devices (or the given file). When it cannot be read the answer is true: typing is not blocked on a guess. */
bool keyboard_present(const char *path = "/proc/bus/input/devices");

} // namespace meshzero
