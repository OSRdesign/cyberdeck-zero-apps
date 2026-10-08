/*
 * SPDX-License-Identifier: MIT
 *
 * Firmware version check (D10): which features the connected board's firmware can do, and the notice to show when it cannot.
 * Two sources: the firmware level of DEVICE_INFO (levels verified in the references: client repeat 9, path hash mode 10) and what
 * the board answered ("unsupported" to GET_CUSTOM_VARS or GET_CHANNEL means a firmware too old for that feature). No LVGL, no I/O.
 */

#pragma once

#include "model.hpp"

#include <string>

namespace meshzero {

enum class Feature {
    BoardGps,          // SET_CUSTOM_VAR "gps" (needs the custom variables, and a board that lists the variable)
    ChannelAdmin,      // add, remove, private channels (SET_CHANNEL / GET_CHANNEL)
    ClientRepeat,      // repeat on/off (DEVICE_INFO level 9)
    PathHashMode,      // path hash size 1 / 2 / 3 bytes (DEVICE_INFO level 10, SET_PATH_HASH_MODE)
    AutoAdd,           // GET / SET_AUTOADD_CONFIG: detected from the board's answer
    Stats,             // GET_STATS: detected from the board's answer
    Discover,          // the zero-hop discover (SEND_CONTROL_DATA): detected from the board's answer
};

struct FeatureState {
    bool available = false;   // the feature works with this board
    bool hidden = false;      // nothing to show at all (a board without GPS): not an old firmware, just not there
    std::string notice;       // "firmware too old for ..." when it is the firmware that lacks it
};

/* The firmware level a feature needs when it is level based, 0 when the feature is detected from the board's answers. */
int feature_min_level(Feature f);
const char *feature_name(Feature f);
FeatureState feature_state(Feature f, const DeviceInfo *dev, const BoardCaps &caps);
/* "v1.15.0 (level 11)" for the Settings board line. */
std::string firmware_text(const DeviceInfo *dev);

/* Repeat on / off (D2): a companion may only repeat on the frequencies GET_ALLOWED_REPEAT_FREQ lists (kHz ranges, like SET_RADIO). */
struct RepeatState {
    bool shown = false;       // the row exists at all (a board that reports nothing about repeat shows no row)
    bool available = false;   // it can be switched: firmware level 9+, ranges known, the current frequency inside one
    bool on = false;          // the board reports repeat on
    std::string note;         // why it is not available ("Not allowed on this frequency ..." / "Firmware too old ...")
};
RepeatState repeat_state(const DeviceInfo *dev, const BoardCaps &caps, double freq_mhz);
/* True when freq_mhz lies in one of the ranges (inclusive, kHz). */
bool repeat_freq_allowed(const std::vector<RepeatRange> &ranges, double freq_mhz);
/* "433.000-433.000, 869.000-869.000 MHz" */
std::string repeat_ranges_text(const std::vector<RepeatRange> &ranges);

/* The path hash size in bytes (1, 2 or 3) for a mode 0, 1, 2; 0 for anything else. */
int path_hash_bytes(int mode);
const char *path_hash_text(int mode);          // "1 byte", "2 bytes", "3 bytes", "unknown"

} // namespace meshzero
