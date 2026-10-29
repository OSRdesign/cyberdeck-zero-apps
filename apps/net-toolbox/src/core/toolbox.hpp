/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the app's identity (no LVGL). kMinW x kMinH must equal X-MinSize in
 * root/usr/share/APPLaunch/applications/net-toolbox.desktop, kPortraitW x kPortraitH its X-Portrait (the smallest portrait
 * canvas of PDA mode, 0.5.0) (tests/test_core.cpp checks both).
 */
#pragma once

namespace nt {

constexpr const char *kAppId = "net-toolbox";
constexpr const char *kAppTitle = "Net Toolbox";
constexpr int kMinW = 640;
constexpr int kMinH = 480;
constexpr int kPortraitW = 480;
constexpr int kPortraitH = 640;

} // namespace nt
