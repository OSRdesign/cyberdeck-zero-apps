/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the app's identity (no LVGL). kMinW x kMinH must equal X-MinSize in
 * root/usr/share/APPLaunch/applications/net-toolbox.desktop (tests/test_core.cpp checks it).
 */
#pragma once

namespace nt {

constexpr const char *kAppId = "net-toolbox";
constexpr const char *kAppTitle = "Net Toolbox";
constexpr int kMinW = 640;
constexpr int kMinH = 480;

} // namespace nt
