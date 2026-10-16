/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the scripted answers of the demonstration backend (see demo_data_04.hpp).
 */
#include "demo_data_04.hpp"

#include "demo_data_03.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace nt {
namespace ui {

namespace {

/* ---------------------------------------------------------------- Local devices */

struct DemoDevice {
    int host;               /* the last byte */
    const char *name;
    NameSource source;
    const char *vendor;
    bool is_private;
    const char *mac;
    double rtt;             /* < 0: no answer to ping */
};

const DemoDevice kDevices[] = {
    {1, "router", NameSource::ReverseDns, "TP-Link", false, "b0:4e:26:00:00:01", 2.0},
    {20, "nas", NameSource::Mdns, "Synology", false, "00:11:32:aa:bb:cc", 3.0},
    {23, "laptop-olivier", NameSource::Mdns, "Intel", false, "3c:f0:11:12:34:56", 12.0},
    {31, "", NameSource::None, "", true, "da:a1:19:7e:44:90", 48.0},
    {35, "", NameSource::None, "Espressif", false, "24:6f:28:aa:01:02", 31.0},
    {40, "printer", NameSource::Netbios, "Brother", false, "00:80:77:5a:6b:7c", 9.0},
    {52, "tv-salon", NameSource::Mdns, "LG", false, "a8:23:fe:01:02:03", 22.0},
    {60, "hue-bridge", NameSource::Mdns, "Signify", false, "00:17:88:21:43:65", 6.0},
    {77, "pi-zero-w", NameSource::Mdns, "Raspberry Pi", false, "b8:27:eb:10:20:30", 14.0},
    {88, "", NameSource::None, "Apple", false, "f0:18:98:77:88:99", 35.0},
    {101, "desktop-pc", NameSource::Netbios, "Gigabyte", false, "1c:1b:0d:44:55:66", 1.0},
    {121, "cyberdeck", NameSource::None, "Raspberry Pi", false, "b8:27:eb:aa:bb:cc", -1.0}, /* this device */
    {130, "chromecast", NameSource::Mdns, "Google", false, "f4:f5:d8:12:12:12", 18.0},
    {200, "", NameSource::None, "", false, "00:00:5e:00:53:01", 60.0},
};
constexpr int kDemoDeviceCount = static_cast<int>(sizeof(kDevices) / sizeof(kDevices[0]));

} // namespace

std::vector<Device> demo_devices_list(int n)
{
    std::vector<Device> out;
    for (int i = 0; i < kDemoDeviceCount && static_cast<int>(out.size()) < n; ++i) {
        const DemoDevice &s = kDevices[i];
        Device d;
        d.ip = "192.168.68." + std::to_string(s.host);
        d.ip_order = (192u << 24) | (168u << 16) | (68u << 8) | static_cast<std::uint32_t>(s.host);
        d.mac = s.mac;
        d.vendor = s.vendor;
        d.vendor_private = s.is_private;
        d.name = s.name;
        d.name_source = s.source;
        d.has_rtt = s.rtt >= 0;
        d.rtt_ms = s.rtt > 0 ? s.rtt : 0.0;
        d.is_gateway = s.host == 1;
        d.is_self = s.host == 121;
        out.push_back(d);
    }
    return out;
}

DevicesSnapshot demo_devices(const std::string &scenario, const DevicesRequest &req)
{
    DevicesSnapshot s;
    s.interface = req.iface;
    const ScanRange range = scan_range_of(req.net, req.iface);
    s.range = range;
    s.subnet = subnet_text(range);
    if (scenario == "refused") {
        s.state = DevicesSnapshot::State::Failed;
        s.error = "8.8.8.8 is not a private address: Local devices only scans your own private network.";
        return s;
    }
    if (!range.ok) {
        s.state = DevicesSnapshot::State::Failed;
        s.error = range.problem.empty() ? std::string("No network") : range.problem;
        return s;
    }
    s.total = range.count();
    const std::uint32_t net24 = range.first & 0xFFFFFF00u;
    s.gateway = ipv4_text(net24 | 1u);
    std::vector<Device> all = demo_devices_list(kDemoDeviceCount);
    for (Device &d : all) { /* the scripted list follows the board's own subnet and address */
        const std::uint32_t host = d.ip_order & 0xFFu;
        d.ip_order = d.is_self ? range.self : (net24 | host);
        d.ip = ipv4_text(d.ip_order);
    }
    std::sort(all.begin(), all.end(), [](const Device &a, const Device &b) { return a.ip_order < b.ip_order; });
    if (scenario == "scan") {
        s.state = DevicesSnapshot::State::Scanning;
        s.probed = 96;
        for (const Device &d : all)
            if (d.is_self || d.ip_order - net24 <= 96) s.devices.push_back(d);
        s.found = static_cast<int>(s.devices.size());
        return s;
    }
    s.state = DevicesSnapshot::State::Done;
    s.probed = s.total;
    for (const Device &d : all) {
        if (scenario == "empty" && !d.is_self) continue;
        if (scenario == "few" && !(d.is_self || d.is_gateway || d.name == "nas")) continue;
        s.devices.push_back(d);
    }
    s.found = static_cast<int>(s.devices.size());
    return s;
}

DevicesSnapshot demo_devices_stop(const DevicesSnapshot &now)
{
    DevicesSnapshot s = now;
    if (s.state == DevicesSnapshot::State::Scanning) s.state = DevicesSnapshot::State::Cancelled;
    return s;
}

/* ---------------------------------------------------------------- the port scan of a device */

PortScanSnapshot demo_portscan(const std::string &scenario, const PortScanRequest &req)
{
    PortScanSnapshot s;
    s.address = req.address.text();
    s.full_range = req.full_range;
    const std::string problem = req.range.ok ? scan_target_problem(req.address, req.range) : std::string("No network");
    if (scenario == "refused" || !problem.empty()) {
        s.state = PortScanSnapshot::State::Failed;
        s.error = problem == "No network" ? problem : std::string(kScanTargetRefused);
        return s;
    }
    s.total = req.full_range ? 1024 : static_cast<int>(local_common_ports().size());
    const bool router = req.address.bytes[3] == 1;
    std::vector<std::uint16_t> ports;
    if (scenario == "scan" || scenario == "done")
        ports = router ? std::vector<std::uint16_t>{22, 53, 80, 443} : std::vector<std::uint16_t>{22, 445};
    const std::size_t shown = scenario == "scan" ? std::min<std::size_t>(2, ports.size()) : ports.size();
    for (std::size_t i = 0; i < shown; ++i) {
        OpenPort p;
        p.port = ports[i];
        p.service = local_port_service(p.port);
        p.connect_ms = 2 + static_cast<int>(i);
        s.open.push_back(p);
    }
    s.open_count = static_cast<int>(s.open.size());
    if (scenario == "scan") {
        s.state = PortScanSnapshot::State::Scanning;
        s.scanned = s.total * 3 / 10;
    } else {
        s.state = PortScanSnapshot::State::Done;
        s.scanned = s.total;
    }
    return s;
}

PortScanSnapshot demo_portscan_stop(const PortScanSnapshot &now)
{
    PortScanSnapshot s = now;
    if (s.state == PortScanSnapshot::State::Scanning) s.state = PortScanSnapshot::State::Cancelled;
    return s;
}

/* ---------------------------------------------------------------- Wi-Fi networks */

namespace {

WifiNetwork wnet(const char *ssid, const char *bssid, int channel, int mhz, int rate, int dbm, const char *security, const char *vendor,
                 bool in_use = false)
{
    WifiNetwork n;
    n.ssid = ssid;
    n.hidden = ssid[0] == '\0';
    n.bssid = bssid;
    n.local_bssid = bssid_is_local(bssid);
    n.vendor = n.local_bssid ? "" : vendor;
    n.channel = channel;
    n.freq_mhz = mhz;
    n.band = band_of_freq(mhz);
    n.rate_mbit = rate;
    n.signal_pct = nm_dbm_to_signal(dbm);
    n.dbm = nm_signal_to_dbm(n.signal_pct);
    n.security_raw = security;
    n.security = security_class(security);
    n.security_text = security_words(security);
    n.in_use = in_use;
    return n;
}

} // namespace

std::vector<WifiNetwork> demo_wifi_networks(int stage, bool g24_only, int extra)
{
    const int drift = stage % 2 == 0 ? 0 : 3;
    std::vector<WifiNetwork> v;
    if (!g24_only) v.push_back(wnet("HomeNet", "b0:4e:26:00:00:01", 36, 5180, 540, -48 - drift, "WPA2", "TP-Link", true));
    v.push_back(wnet("HomeNet", "b0:4e:26:00:00:02", 6, 2437, 144, -55 - drift, "WPA2", "TP-Link", g24_only));
    v.push_back(wnet("Livebox-4F2A", "5c:8f:e0:11:22:33", 1, 2412, 130, -62, "WPA2", "Sagemcom"));
    v.push_back(wnet("", "e8:94:f6:44:55:66", 11, 2462, 130, -71, "WPA2", "TP-Link"));
    v.push_back(wnet("Pixel-Hotspot", "6a:5c:b1:00:12:34", 3, 2422, 72, -69, "WPA2", ""));
    if (stage < 1) v.push_back(wnet("CafeGuest", "00:1a:2b:3c:4d:5e", 6, 2437, 54, -74, "", "Ubiquiti"));
    v.push_back(wnet("Bbox-91C2", "f4:ca:e5:61:62:63", 11, 2462, 130, -79, "WPA2", "Sagemcom"));
    v.push_back(wnet("TP-Link_9A3E", "c0:06:c3:9a:3e:01", 4, 2427, 144, -83, "WPA2", "TP-Link"));
    if (!g24_only) {
        v.push_back(wnet("Neighbour-5G", "88:c3:97:20:30:40", 44, 5220, 866, -67, "WPA3", "Netgear"));
        v.push_back(wnet("FibreBox-5G", "ac:84:c6:50:60:70", 149, 5745, 866, -76, "WPA2 WPA3", "Huawei"));
        v.push_back(wnet("Office", "d4:6e:0e:11:12:13", 100, 5500, 650, -81, "WPA2 802.1X", "Cisco"));
    }
    for (int i = 0; i < extra; ++i) {
        char ssid[24], bssid[40];
        std::snprintf(ssid, sizeof(ssid), "Neighbour-%02d", i + 1);
        std::snprintf(bssid, sizeof(bssid), "9c:3d:cf:%02x:%02x:%02x", i & 0xff, (10 + i) & 0xff, (20 + i) & 0xff);
        const int ch = (i % 3 == 0) ? 1 : (i % 3 == 1) ? 6 : 11;
        v.push_back(wnet(ssid, bssid, ch, 2407 + 5 * ch, 130, -84 - (i % 8), "WPA2", "Arris"));
    }
    sort_networks(v);
    return v;
}

WifiSnapshot demo_wifi(const std::string &scenario, const WifiScanConfig &cfg, int stage)
{
    WifiSnapshot s;
    s.iface = cfg.iface;
    s.interval_ms = cfg.interval_ms;
    if (cfg.iface.rfind("wlan", 0) != 0) {
        s.state = WifiSnapshot::State::Failed;
        s.error = cfg.iface.empty() ? std::string("No Wi-Fi adapter on this board") : "No Wi-Fi adapter on " + cfg.iface;
        return s;
    }
    if (scenario == "off") {
        s.state = WifiSnapshot::State::Failed;
        s.error = "Wi-Fi is switched off.";
        return s;
    }
    if (scenario == "nonm") {
        s.state = WifiSnapshot::State::Failed;
        s.error = "nmcli is missing: the Wi-Fi scan needs NetworkManager (package network-manager).";
        return s;
    }
    s.state = WifiSnapshot::State::Ready;
    s.rescan = stage == 0 ? "yes" : "auto";
    s.lists = static_cast<std::uint64_t>(stage + 1);
    s.list_at_ms = kDemoClockMs - kDemoWifiAgeMs;
    if (scenario != "empty") s.networks = demo_wifi_networks(scenario == "vanish" ? stage : 0, scenario == "g24", scenario == "many" ? 14 : 0);
    s.channels = summarize_channels(s.networks);
    return s;
}

/* ---------------------------------------------------------------- the walk test */

namespace {

double walk_dbm_at(double t, bool long_walk)
{
    double v;
    if (t < 20) v = -52 - t * 0.15;
    else if (t < 38) v = -55 - (t - 20) * (17.0 / 18.0);
    else if (t < 44) v = -72 - (t - 38) * (10.0 / 6.0);
    else if (t < 52) v = -82 + (t - 44) * (4.0 / 8.0);
    else if (t < 62) v = -78 + (t - 52) * 1.4;
    else v = -62 - 2.0 * std::sin((t - 62) * 0.35);
    if (long_walk && t >= 120) { /* a second dead spot: a lift, a thick wall */
        if (t < 135) v = -62 - (t - 120) * 1.2;
        else if (t < 150) v = -80 - 2.0 * std::sin((t - 135) * 0.8);
        else v = -80 + std::min(18.0, (t - 150) * 1.1);
    }
    return v + 1.5 * std::sin(t * 1.3);
}

} // namespace

WalkSnapshot demo_walk(const std::string &scenario, const WalkConfig &cfg)
{
    WalkSnapshot s;
    s.iface = cfg.iface;
    s.weak_dbm = std::min(kWalkWeakMaxDbm, std::max(kWalkWeakMinDbm, cfg.weak_dbm));
    s.interval_ms = cfg.interval_ms;
    if (cfg.iface.rfind("wlan", 0) != 0 || scenario == "fail") {
        s.state = WalkSnapshot::State::Failed;
        s.error = cfg.iface.empty() ? std::string("No Wi-Fi adapter on this board") : "No Wi-Fi adapter on " + cfg.iface;
        return s;
    }
    const bool long_walk = scenario == "long";
    const int n = long_walk ? 200 : 83;
    WalkRecorder rec(s.weak_dbm, cfg.interval_ms, cfg.hysteresis_ms, cfg.max_samples);
    const bool noiw = scenario == "noiw";
    WalkReading last;
    for (int t = 0; t < n; ++t) {
        WalkReading r;
        r.linked = true;
        r.dbm = static_cast<int>(std::lround(walk_dbm_at(t, long_walk)));
        if (scenario == "nolink" && t >= 44 && t < 48) r.linked = false;
        if (!noiw) {
            r.has_link = true;
            r.ssid = "HomeNet";
            r.bssid = t < 62 ? "b0:4e:26:00:00:01" : "b0:4e:26:00:00:02";
        }
        rec.add(static_cast<std::int64_t>(t) * 1000, r);
        last = r;
    }
    s.state = WalkSnapshot::State::Running;
    s.samples.assign(rec.samples().begin(), rec.samples().end());
    s.dropped = rec.dropped();
    s.summary = rec.summary();
    s.dips = rec.dips();
    s.roams = rec.roams();
    s.in_dip = rec.in_dip();
    s.linked = last.linked;
    s.dbm = last.linked ? last.dbm : 0;
    s.bssid = rec.bssid();
    s.ssid = rec.ssid();
    s.source = last.linked ? "/proc/net/wireless" : "";
    s.iw_missing = noiw;
    s.elapsed_s = static_cast<double>(n);
    if (scenario == "stopped") s.state = WalkSnapshot::State::Stopped;
    return s;
}

WalkSnapshot demo_walk_stop(const WalkSnapshot &now)
{
    WalkSnapshot s = now;
    if (s.state == WalkSnapshot::State::Running) s.state = WalkSnapshot::State::Stopped;
    return s;
}

} // namespace ui
} // namespace nt
