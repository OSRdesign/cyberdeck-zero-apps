/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the settings file (no LVGL; tests/test_stats.cpp).
 *
 * ${XDG_CONFIG_HOME:-$HOME/.config}/cyberdeckzero/system-monitor.conf (SYSTEM_MONITOR_CONFIG overrides the path), one
 * key=value per line:
 *   v=1
 *   period=1        sampling period in seconds: 1, 2 or 5
 * Parsed, never executed; unknown keys and bad values are ignored; written through a temporary file and a rename.
 */
#pragma once

#include <string>

namespace sm {

struct Settings {
    int period_s = 1;
    std::string serialize() const;
    static Settings parse(const std::string &text);
};

/* "" when neither SYSTEM_MONITOR_CONFIG nor HOME is set */
std::string settings_path();
Settings load_settings(const std::string &path);
bool save_settings(const std::string &path, const Settings &settings);

} // namespace sm
