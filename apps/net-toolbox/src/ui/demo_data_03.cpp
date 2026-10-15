/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the scripted answers of the demonstration backend (see demo_data_03.hpp).
 */
#include "demo_data_03.hpp"

#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nt {
namespace ui {

namespace {

/* ---------------------------------------------------------------- speed test */

/* the samples of a phase that ramps up to `peak` Mbit/s, every 0.5 s */
std::vector<SpeedSample> ramp(double peak, int n)
{
    std::vector<SpeedSample> out;
    for (int i = 0; i < n; ++i) {
        const double fill = 1.0 - std::exp(-(i + 1) / 2.2);
        const double wobble = 1.0 + 0.04 * std::sin(i * 1.7);
        out.push_back({0.5 * (i + 1), peak * fill * wobble});
    }
    return out;
}

LatencyStats loaded_stats(double idle_ms, double increase_ms)
{
    std::vector<double> v;
    for (int i = 0; i < 30; ++i) v.push_back(idle_ms + increase_ms + ((i * 7) % 11 - 5) * 0.4);
    return latency_from_values(v, 0);
}

SpeedDirection finished_direction(double mbps, const LatencyStats &idle, double increase_ms, bool upload)
{
    SpeedDirection d;
    d.tried = d.measured = true;
    d.mbps = mbps;
    d.seconds = 8.0;
    d.bytes = static_cast<std::uint64_t>(mbps * 1e6 / 8.0 * 8.0);
    d.history = ramp(mbps, 16);
    d.loaded = loaded_stats(idle.median_ms, increase_ms);
    d.has_increase = true;
    d.increase_ms = d.loaded.median_ms - idle.median_ms;
    d.grade = bufferbloat_grade(d.increase_ms);
    d.has_server_bytes = upload;
    d.server_bytes = d.bytes;
    return d;
}

SpeedResult latency_result()
{
    SpeedResult r;
    r.latency_ok = true;
    r.latency_ms = 21.0;
    r.jitter_ms = 2.4;
    r.latency_min_ms = 18.5;
    r.latency_max_ms = 29.0;
    r.latency_samples = 10;
    r.setup_ms = 92.0;
    r.colo = "CDG";
    r.ping_target = "1.1.1.1";
    r.ping_address = "1.1.1.1";
    r.idle = latency_from_values({17.2, 18.1, 19.4, 18.0, 17.6, 20.1, 18.3, 17.9, 18.8, 18.2}, 0);
    return r;
}

SpeedSnapshot running(SpeedSnapshot::Phase phase)
{
    SpeedSnapshot s;
    s.state = SpeedSnapshot::State::Running;
    s.phase = phase;
    s.result = latency_result();
    if (phase == SpeedSnapshot::Phase::Latency) s.result = SpeedResult();
    return s;
}

SpeedSnapshot downloading(double peak)
{
    SpeedSnapshot s = running(SpeedSnapshot::Phase::Download);
    s.result.down.tried = true;
    s.result.down.history = ramp(peak, 9);
    s.now_mbps = s.result.down.history.back().mbps;
    s.phase_s = 4.5;
    s.phase_bytes = static_cast<std::uint64_t>(s.now_mbps * 1e6 / 8.0 * 4.5);
    s.ping_now_ms = 18.0 + peak / 8.0;
    return s;
}

SpeedSnapshot finished(double down, double up, double down_inc, double up_inc)
{
    SpeedSnapshot s;
    s.state = SpeedSnapshot::State::Done;
    s.result = latency_result();
    s.result.down = finished_direction(down, s.result.idle, down_inc, false);
    s.result.up = finished_direction(up, s.result.idle, up_inc, true);
    s.result.grade = worse_grade(s.result.down.grade, s.result.up.grade);
    return s;
}

SpeedSnapshot failed(int exit_code, int http)
{
    SpeedSnapshot s;
    s.state = SpeedSnapshot::State::Failed;
    CommandResult r;
    r.started = true;
    r.exit_code = exit_code;
    s.error = speed_error_words(r, http, "");
    return s;
}

} // namespace

SpeedSnapshot demo_speed(const std::string &scenario, const SpeedConfig &cfg)
{
    (void)cfg;
    if (scenario == "latency") return running(SpeedSnapshot::Phase::Latency);
    if (scenario == "down-low") return downloading(4.2);
    if (scenario == "down-mid" || scenario == "stopped") return downloading(38.0);
    if (scenario == "down-high") return downloading(612.0);
    if (scenario == "up") {
        SpeedSnapshot s = running(SpeedSnapshot::Phase::Upload);
        s.result.down = finished_direction(38.4, s.result.idle, 24.0, false);
        s.result.up.tried = true;
        s.result.up.history = ramp(31.0, 6);
        s.now_mbps = s.result.up.history.back().mbps;
        s.phase_s = 3.0;
        s.ping_now_ms = 18.0 + 30.0;
        return s;
    }
    if (scenario == "done-b") return finished(23.4, 31.2, 45.0, 38.0);
    if (scenario == "done-c") return finished(23.4, 31.2, 120.0, 45.0);
    if (scenario == "done-d") return finished(23.4, 31.2, 300.0, 150.0);
    if (scenario == "done-f") return finished(23.4, 31.2, 650.0, 480.0);
    if (scenario == "done-noping") {
        SpeedSnapshot s = finished(23.4, 31.2, 0.0, 0.0);
        s.result.down.loaded = s.result.up.loaded = LatencyStats();
        s.result.down.has_increase = s.result.up.has_increase = false;
        s.result.down.grade = s.result.up.grade = s.result.grade = BloatGrade::Unknown;
        s.result.ping_note = "No ping target: the latency under load is not measured.";
        return s;
    }
    if (scenario == "done-a" || scenario == "done") return finished(23.4, 31.2, 12.0, 8.0);
    if (scenario == "e-network") return failed(7, 0);
    if (scenario == "e-tls") return failed(35, 0);
    if (scenario == "e-refused") return failed(22, 403);
    return SpeedSnapshot(); /* idle */
}

SpeedSnapshot demo_speed_stop(const SpeedSnapshot &now)
{
    SpeedSnapshot s = now;
    if (s.running()) {
        s.state = SpeedSnapshot::State::Cancelled;
        s.phase = SpeedSnapshot::Phase::None;
        s.now_mbps = 0;
    }
    return s;
}

/* ---------------------------------------------------------------- Wake-on-LAN */

namespace {

class DemoSender : public DatagramSender {
public:
    int send(const WolDestination &, const std::vector<std::uint8_t> &) override { return 0; }
};

} // namespace

WolSnapshot demo_wol(const std::string &scenario, const WolRequest &req)
{
    WolSnapshot s;
    s.machine = req.machine.name;
    s.mac = req.machine.mac;
    MacAddr mac;
    if (parse_mac(req.machine.mac, mac)) {
        DemoSender sender;
        s.send = send_magic(mac, plan_wol(req.machine, req.net, req.iface), sender);
    }
    const bool watch = req.watch && !trim(req.machine.host).empty();
    if (scenario == "failed" || s.send.sent == 0) {
        s.state = WolSnapshot::State::Failed;
        s.error = s.send.sent == 0 && scenario != "failed" ? s.send.summary : "Nothing could be sent: network unreachable.";
        if (scenario == "failed") {
            s.send.sent = 0;
            s.send.errors = 2;
            s.send.summary = s.error;
        }
        return s;
    }
    if (!watch || scenario == "sent") {
        s.state = WolSnapshot::State::Done;
        return s;
    }
    s.watch = true;
    s.watch_target = req.machine.host;
    s.watch_address = req.machine.host;
    if (scenario == "waking") {
        s.state = WolSnapshot::State::Watching;
        s.waited_s = 37;
        s.attempts = 37;
        s.watch_words = "no answer yet (37 s)";
    } else if (scenario == "once") {
        s.state = WolSnapshot::State::Done;
        s.attempts = 1;
        s.waited_s = 0;
        s.answered = s.answered_at_once = true;
        s.answered_after_s = 0.04;
        s.watch_words = "answers at once: it was probably awake already";
    } else if (scenario == "noanswer") {
        s.state = WolSnapshot::State::Done;
        s.attempts = 120;
        s.waited_s = 120;
        s.watch_words = "no answer after 120 s";
        s.hint = kWolHint;
    } else { /* awake */
        s.state = WolSnapshot::State::Done;
        s.attempts = 23;
        s.waited_s = 22;
        s.answered = true;
        s.answered_after_s = 22.4;
        s.watch_words = "answers after 22 s";
    }
    return s;
}

WolSnapshot demo_wol_stop(const WolSnapshot &now)
{
    WolSnapshot s = now;
    if (s.running()) s.state = WolSnapshot::State::Cancelled;
    return s;
}

std::vector<WolMachine> demo_wol_machines(int n)
{
    static const char *const names[] = {"Desk PC", "NAS", "Living room TV", "Laptop", "Media box", "Print server", "Workshop PC", "Garage Pi",
                                        "Server", "Mini PC", "Office", "Kitchen", "Den", "Attic", "Basement", "Guest PC",
                                        "Test rig", "Backup", "Lab", "Gaming"};
    std::vector<WolMachine> out;
    for (int i = 0; i < n && i < 20; ++i) {
        WolMachine m;
        m.name = names[i];
        char mac[32];
        std::snprintf(mac, sizeof(mac), "b8:27:eb:%02x:%02x:%02x", 0x10 + i, 0x20 + 3 * i, 0x30 + 7 * i);
        m.mac = mac;
        if (i % 3 != 2) m.host = "192.168.68." + std::to_string(20 + i);
        out.push_back(m);
    }
    return out;
}

/* ---------------------------------------------------------------- Bluetooth */

namespace {

struct DemoDevice {
    const char *name;
    const char *address;
    int dbm;
    int company; /* -1 none */
    const char *vendor;
    const char *type;
    int age_ms;
    int tx;       /* 99 none */
    int readings;
};

const DemoDevice kDemo[25] = {
    {"Galaxy Buds2", "A4:C1:38:5E:01:7B", -43, 0x0075, "Samsung", "public", 700, 0, 188},
    {"Keyboard K380", "F0:42:1C:AA:90:D3", -49, 0x01DA, "Logitech", "public", 1300, 4, 154},
    {"Pixel 8", "6C:A1:00:3F:22:9E", -54, 0x00E0, "Google", "random", 400, 8, 211},
    {"", "5B:12:F7:90:C4:08", -58, 0x004C, "Apple", "random", 2100, 99, 97},
    {"JBL Flip 6", "08:DF:1F:6A:BB:41", -63, 0x0057, "Harman", "public", 1800, 99, 120},
    {"Mi Band 7", "C8:0F:10:77:14:E2", -67, 0x038F, "Xiaomi", "public", 2600, 99, 76},
    {"", "7E:44:90:0B:D1:56", -69, 0x004C, "Apple", "random", 3400, 99, 64},
    {"TV Samsung 7", "B4:3A:28:19:CF:60", -71, 0x0075, "Samsung", "public", 900, 99, 133},
    {"Tile", "DC:6E:29:80:A3:0F", -73, 0x067C, "Tile", "random", 4800, 99, 41},
    {"", "42:9D:51:E8:36:AA", -75, 0x0006, "Microsoft", "random", 5200, 99, 38},
    {"Govee_H6159", "A4:C1:38:11:90:C7", -77, -1, "", "public", 1500, 99, 99},
    {"Fitbit Charge 5", "EC:2F:90:5D:7A:12", -78, 0x0224, "Fitbit", "public", 6100, 99, 30},
    {"", "6A:03:BB:41:09:D4", -79, -1, "", "random", 7000, 99, 26},
    {"LE-Bose QC45", "04:52:C7:C8:2E:90", -81, 0x009E, "Bose", "public", 2200, 99, 47},
    {"", "55:A0:62:71:CF:3B", -83, 0x004C, "Apple", "random", 8800, 99, 22},
    {"Ring-9F2A", "70:B3:D5:12:9F:2A", -84, -1, "", "public", 9400, 99, 19},
    {"ELK-BLEDOM", "BE:67:00:2A:11:04", -86, -1, "", "public", 3000, 99, 53},
    {"", "7C:11:89:E0:40:51", -88, 0x004C, "Apple", "random", 12000, 99, 14},
    {"Hue lamp", "00:17:88:4B:6D:C3", -89, 0x0105, "Signify", "public", 4100, 99, 33},
    {"", "63:F8:20:09:BA:7D", -91, -1, "", "random", 15000, 99, 9},
    {"SmartLock-B2", "E2:19:84:C0:5A:33", -92, -1, "", "random", 10500, 99, 12},
    {"", "4C:5D:0A:B7:73:E8", -93, 0x0006, "Microsoft", "random", 16500, 99, 7},
    {"Thermo-EA11", "A0:E6:F8:3B:EA:11", -94, -1, "", "public", 13500, 99, 8},
    {"", "5D:90:21:6F:08:CE", -96, -1, "", "random", 17500, 99, 5},
    {"MX Master 3", "D0:11:E5:4C:69:B8", -97, 0x01DA, "Logitech", "public", 18500, 99, 4},
};

} // namespace

std::vector<BleDevice> demo_ble_devices(int n)
{
    std::vector<BleDevice> out;
    for (int i = 0; i < n && i < 25; ++i) {
        const DemoDevice &x = kDemo[i];
        BleDevice d;
        d.address = x.address;
        d.address_type = x.type;
        d.name = x.name;
        d.has_rssi = true;
        d.smooth_dbm = x.dbm;
        d.rssi_dbm = x.dbm + ((i % 3) - 1) * 2;
        d.rssi_at_ms = kDemoClockMs - x.age_ms;
        d.has_tx = x.tx != 99;
        d.tx_dbm = x.tx;
        d.company = x.company;
        d.vendor = x.vendor;
        if (d.vendor.empty() && x.company >= 0) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "company 0x%04X", x.company);
            d.vendor = buf;
        }
        d.first_seen_ms = kDemoClockMs - 20000 - x.age_ms;
        d.last_seen_ms = kDemoClockMs - x.age_ms;
        d.readings = x.readings;
        d.distance = distance_class(d.smooth_dbm);
        out.push_back(d);
    }
    return out;
}

BleSnapshot demo_ble(const std::string &scenario, const BleConfig &cfg)
{
    BleSnapshot s;
    s.seconds = cfg.seconds;
    s.adapter = "PI3A (B8:27:EB:12:34:56)";
    const auto fail = [&](const char *words) {
        s.state = BleSnapshot::State::Failed;
        s.error = words;
        s.adapter.clear();
        return s;
    };
    if (scenario == "off") return fail("Bluetooth is powered off. Switch it on, for example with: bluetoothctl power on");
    if (scenario == "rfkill")
        return fail("Bluetooth is switched off (rfkill soft block). Switch it on, for example with: sudo rfkill unblock bluetooth");
    if (scenario == "none") return fail("This board has no Bluetooth adapter (or its driver is not loaded).");
    if (scenario == "bluez") return fail("bluetoothctl is missing: Bluetooth needs the bluez package.");
    if (scenario == "idle") return BleSnapshot();
    int n = 14;
    if (scenario == "scan0") n = 0;
    else if (scenario == "scan6") n = 6;
    else if (scenario == "scan25" || scenario == "done25") n = 25;
    s.devices = demo_ble_devices(n);
    s.live = n;
    s.seen = n == 0 ? 0 : n + 4;
    s.discovering = true;
    if (scenario == "done25") {
        s.state = BleSnapshot::State::Done;
        s.elapsed_s = 60.0;
        s.discovering = false;
        s.stop_note = "Scan stopped after 60 s: discovery turned off. Run it again to go on.";
    } else {
        s.state = BleSnapshot::State::Scanning;
        s.elapsed_s = 23.0;
    }
    return s;
}

BleSnapshot demo_ble_stop(const BleSnapshot &now)
{
    BleSnapshot s = now;
    if (s.running()) {
        s.state = BleSnapshot::State::Cancelled;
        s.discovering = false;
        s.stop_note = "Stopped: discovery turned off.";
    }
    return s;
}

} // namespace ui
} // namespace nt
