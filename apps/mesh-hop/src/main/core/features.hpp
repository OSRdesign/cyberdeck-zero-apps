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
    ClientRepeat,      // phase 2: repeat on/off (DEVICE_INFO level 9)
    PathHashMode,      // phase 3: path hash size (DEVICE_INFO level 10)
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

} // namespace meshzero
