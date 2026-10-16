/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the saved text of the Wi-Fi networks and of the walk test (no LVGL; tests/test_wifiscan.cpp and
 * tests/test_walktest.cpp). Same style as netlog.hpp's format_*(): the facts, one per line; the page adds log_heading()
 * and saves it with save_log().
 */
#pragma once

#include "walktest.hpp"
#include "wifiscan.hpp"

#include <string>

namespace nt {

/* the interface, the state, every network (name, BSSID, signal, channel, band, rate, security, vendor, in use) and the
 * channels summary with its least crowded channel per band */
std::string format_wifi_networks(const WifiSnapshot &s);
/* one network (the detail page's, followed by BSSID); gone: it was not in the last list (its last values) */
std::string format_wifi_network(const WifiNetwork &n, bool gone);
/* the walk: the settings, the summary, the dips, the access point changes and every sample kept (s, dBm) */
std::string format_walktest(const WalkSnapshot &s);

} // namespace nt
