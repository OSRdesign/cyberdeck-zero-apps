/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the scripted answers of the demonstration backend for Local devices, the port scan of a device, Wi-Fi
 * networks and the walk test (see backend.hpp: NET_TOOLBOX_DEMO devices=, ports=, wifi=, walk=). No network, no thread, no
 * adapter: the snapshots the core's sessions publish, built by hand and through the core's own derive functions (the walk
 * through the core's WalkRecorder, the channels through summarize_channels), the same every time.
 */
#pragma once

#include "backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* the demonstration's monotonic clock is demo_data_03.hpp's kDemoClockMs: the Wi-Fi lists came 3 s before it */
constexpr std::int64_t kDemoWifiAgeMs = 3000;

/* devices=idle | scan | done | few | empty | refused | named: what Start shows (a request on a board without a usable address
 * gives the core's own refusal whatever the scenario); Stop turns a scan into Cancelled */
DevicesSnapshot demo_devices(const std::string &scenario, const DevicesRequest &req);
DevicesSnapshot demo_devices_stop(const DevicesSnapshot &now);
/* the 14 devices of "done" (192.168.68.0/24: the router .1, this device .121, a random-MAC phone, an unnamed host ...) */
std::vector<Device> demo_devices_list(int n);

/* ports=scan | done | empty | refused: the scan of one device (a request outside the range is refused as the core does) */
PortScanSnapshot demo_portscan(const std::string &scenario, const PortScanRequest &req);
PortScanSnapshot demo_portscan_stop(const PortScanSnapshot &now);

/* wifi=list | many | g24 | empty | off | nonm | vanish: stage 0 is what Start shows, each rescan() the next stage (the last one
 * stays); a board interface that is not a wlan one fails with the core's "No Wi-Fi adapter on <if>" */
WifiSnapshot demo_wifi(const std::string &scenario, const WifiScanConfig &cfg, int stage);
std::vector<WifiNetwork> demo_wifi_networks(int stage, bool g24_only, int extra);

/* walk=run | long | stopped | nolink | noiw | fail: Start shows the walk at 83 s (200 s for long, running); Stop turns it into
 * Stopped; reset gives Idle */
WalkSnapshot demo_walk(const std::string &scenario, const WalkConfig &cfg);
WalkSnapshot demo_walk_stop(const WalkSnapshot &now);

} // namespace ui
} // namespace nt
