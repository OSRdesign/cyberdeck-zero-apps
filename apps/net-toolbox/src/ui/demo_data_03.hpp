/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the scripted answers of the demonstration backend for the speed test, Wake-on-LAN and the Bluetooth radar
 * (see backend.hpp: NET_TOOLBOX_DEMO speed=, wol=, ble=). No network, no thread, no adapter: the snapshots the core's
 * sessions publish, built by hand and through the core's own derive functions, the same every time.
 */
#pragma once

#include "backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* the demonstration's monotonic clock: the BLE devices' times are counted from it, and the radar's sweep stands still at
 * about 50 degrees */
constexpr std::int64_t kDemoClockMs = 1002420;

SpeedSnapshot demo_speed(const std::string &scenario, const SpeedConfig &cfg);
/* Stop: a running test becomes Cancelled (what was measured stays) */
SpeedSnapshot demo_speed_stop(const SpeedSnapshot &now);

WolSnapshot demo_wol(const std::string &scenario, const WolRequest &req);
WolSnapshot demo_wol_stop(const WolSnapshot &now);

BleSnapshot demo_ble(const std::string &scenario, const BleConfig &cfg);
BleSnapshot demo_ble_stop(const BleSnapshot &now);
/* the first n of the demonstration's 25 devices, the strongest first */
std::vector<BleDevice> demo_ble_devices(int n);

/* the names of the demonstration's saved machines (wol_machines=N) */
std::vector<WolMachine> demo_wol_machines(int n);

} // namespace ui
} // namespace nt
