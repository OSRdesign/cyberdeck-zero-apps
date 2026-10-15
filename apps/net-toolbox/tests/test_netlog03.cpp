/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): the saved texts of the speed test, Wake-on-LAN and the BLE scan, and the 0.3 settings (the
 * saved machines, at most 20, a 0.2 file read with the defaults, hostile values ignored).
 */
#include "check.hpp"

#include "netlog.hpp"
#include "settings.hpp"
#include "text.hpp"

using namespace nt;

namespace {

bool has(const std::string &text, const std::string &part)
{
    const bool ok = text.find(part) != std::string::npos;
    if (!ok) std::fprintf(stderr, "missing [%s] in:\n%s\n", part.c_str(), text.c_str());
    return ok;
}

void test_format_03()
{
    // the speed test
    {
        SpeedSnapshot s;
        s.state = SpeedSnapshot::State::Done;
        SpeedResult &r = s.result;
        r.colo = "CDG";
        r.latency_ok = true;
        r.latency_ms = 13.7;
        r.jitter_ms = 18.4;
        r.latency_min_ms = 9.3;
        r.latency_max_ms = 41.9;
        r.latency_samples = 10;
        r.setup_ms = 189;
        r.ping_target = "1.1.1.1";
        r.idle = latency_from_values({10, 11, 12});
        r.down.tried = r.down.measured = true;
        r.down.mbps = 24.04;
        r.down.bytes = 24000000;
        r.down.seconds = 8.0;
        r.down.loaded = latency_from_values({80, 90, 86}, 1);
        r.down.has_increase = true;
        r.down.increase_ms = 75;
        r.down.grade = BloatGrade::C;
        r.down.history = {{0.5, 14.4}, {1.0, 24.0}};
        r.up.tried = r.up.measured = true;
        r.up.mbps = 12;
        r.up.bytes = 12000000;
        r.up.seconds = 8.0;
        r.up.capped = true;
        r.up.has_server_bytes = true;
        r.up.server_bytes = 12000000;
        r.up.note = "Cloudflare did not confirm the upload; the speed is what curl sent.";
        r.grade = BloatGrade::C;
        const std::string t = format_speedtest(s);
        CHECK(has(t, "Server: speed.cloudflare.com (Cloudflare site CDG)\n"));
        CHECK(has(t, "Latency: 14 ms (median of 10), jitter 18 ms, min 9.3 ms, max 42 ms; connection setup 189 ms\n"));
        CHECK(has(t, "Ping target: 1.1.1.1\n"));
        CHECK(has(t, "Latency idle (ping): median 11 ms, min 10 ms, max 12 ms, jitter 1.0 ms, 3 replies, 0% loss\n"));
        CHECK(has(t, "Download: 24.0 Mbit/s (24.0 MB in 8.0 s)\n"));
        CHECK(has(t, "  latency under load: median 86 ms, min 80 ms, max 90 ms, jitter 7.0 ms, 3 replies, 25% loss\n"));
        CHECK(has(t, "  increase: 75 ms, grade C\n"));
        CHECK(has(t, "  samples (s, Mbit/s): 0.5/14.4 1.0/24.0\n"));
        CHECK(has(t, "Upload: 12 Mbit/s (12.0 MB in 8.0 s, every byte allowed moved)\n"));
        CHECK(has(t, "  received by the server: 12.0 MB\n"));
        CHECK(has(t, "  note: Cloudflare did not confirm"));
        CHECK(has(t, "Bufferbloat grade: C - Fair"));
        SpeedSnapshot f;
        f.state = SpeedSnapshot::State::Failed;
        f.error = "speed.cloudflare.com cannot be found: no DNS answer. Is the network up?";
        f.detail = "curl: (6) Could not resolve host: speed.cloudflare.com";
        f.result.ping_note = "Ping is not allowed for this user";
        const std::string ft = format_speedtest(f);
        CHECK(has(ft, "Error: speed.cloudflare.com cannot be found"));
        CHECK(has(ft, "Note: Ping is not allowed"));
        CHECK(has(ft, "curl: curl: (6) Could not resolve host"));
        CHECK(ft.find("Download") == std::string::npos);
        SpeedSnapshot c;
        c.state = SpeedSnapshot::State::Cancelled;
        c.result.down.tried = true;
        c.result.down.error = "Stopped.";
        CHECK(has(format_speedtest(c), "Stopped before the end.\nDownload: Stopped.\n"));
    }
    // Wake-on-LAN
    {
        WolSnapshot s;
        s.state = WolSnapshot::State::Done;
        s.machine = "Desk PC";
        s.mac = "aa:bb:cc:dd:ee:ff";
        WolPacket p;
        p.dest.iface = "wlan0";
        p.dest.source = "192.168.68.121";
        p.dest.address = "255.255.255.255";
        p.dest.port = 9;
        p.words = "sent";
        s.send.packets = {p};
        s.send.sent = 1;
        s.send.summary = "1 packet sent on wlan0.";
        s.watch = true;
        s.watch_target = "desk.lan";
        s.watch_address = "192.168.68.20";
        s.attempts = 120;
        s.watch_words = "no answer after 120 s";
        s.hint = kWolHint;
        const std::string t = format_wol(s);
        CHECK(has(t, "Machine: Desk PC (aa:bb:cc:dd:ee:ff)\nState: done\n"));
        CHECK(has(t, "Sent: 1 packet sent on wlan0.\n  255.255.255.255 port 9 on wlan0: sent\n"));
        CHECK(has(t, "Watch: desk.lan (192.168.68.20), 120 pings: no answer after 120 s\n"));
        CHECK(has(t, "Note: Wake-on-LAN works only when"));
        WolSnapshot f;
        f.state = WolSnapshot::State::Failed;
        f.machine = "x\x01y";
        f.error = "No interface is up with an IPv4 address: there is nothing to send on.";
        const std::string ft = format_wol(f);
        CHECK(has(ft, "Machine: x\\x01y\nState: failed\nError: No interface is up"));
    }
    // the BLE scan
    {
        BleSnapshot s;
        s.state = BleSnapshot::State::Done;
        s.adapter = "PI3A (B8:27:EB:00:00:01)";
        s.elapsed_s = 60;
        s.seconds = 60;
        s.live = 1;
        s.seen = 4;
        s.stop_note = "Scan stopped after 60 s: discovery turned off. Run it again to go on.";
        BleDevice d;
        d.address = "4A:00:00:00:00:04";
        d.address_type = "random";
        d.rssi_dbm = -72;
        d.smooth_dbm = -70.4;
        d.distance = DistanceClass::Medium;
        d.has_tx = true;
        d.tx_dbm = 8;
        d.vendor = "Apple";
        BleDevice e;
        e.address = "0A:00:00:00:00:05";
        e.rssi_dbm = -50;
        e.smooth_dbm = -51;
        e.distance = DistanceClass::Near;
        e.name = "Meshtastic_0a0a";
        s.devices = {e, d};
        const std::string t = format_ble(s);
        CHECK(has(t, "Adapter: PI3A (B8:27:EB:00:00:01)\nScan: 60.0 s of 60 s; 1 devices in range, 4 seen in all\n"));
        CHECK(has(t, "Scan stopped after 60 s"));
        CHECK(has(t, "0A:00:00:00:00:05  -  -50 dBm (-51)  near  -  -  Meshtastic_0a0a\n"));
        CHECK(has(t, "4A:00:00:00:00:04  random  -72 dBm (-70)  medium  8 dBm  Apple  -\n"));
        CHECK(has(t, "Distance: A rough guess from the signal strength only, not metres"));
        BleSnapshot f;
        f.state = BleSnapshot::State::Failed;
        f.error = "This board has no Bluetooth adapter (or its driver is not loaded).";
        CHECK(has(format_ble(f), "Error: This board has no Bluetooth adapter"));
    }
}

void test_settings_03()
{
    // a 0.2 file reads with the 0.3 defaults
    const std::string v02 = "v=1\ntarget=1.1.1.1\ntrace_target=example.com\nhttp_insecure=1\nwhois_query=example.com\n";
    Settings old = Settings::parse(v02);
    CHECK(old.wol_machines.empty() && old.wol_iface.empty() && old.wol_watch && old.speed_ping == "1.1.1.1" &&
          old.ble_seconds == 60);
    CHECK(old.trace_target == "example.com" && old.http_insecure);
    // machines: saved, replaced by name (any case), removed, found
    Settings s;
    WolMachine m;
    m.name = "Desk PC";
    m.mac = "AA-BB-CC-DD-EE-FF";
    m.host = "192.168.68.20";
    std::string why;
    CHECK(s.save_machine(m, &why));
    m.name = "NAS";
    m.mac = "001122334455";
    m.host = "";
    m.broadcast = "192.168.68.255";
    m.port = 7;
    CHECK(s.save_machine(m));
    WolMachine again;
    again.name = "desk pc";
    again.mac = "aa:bb:cc:dd:ee:01";
    CHECK(s.save_machine(again) && s.wol_machines.size() == 2 && s.wol_machines[0].mac == "aa:bb:cc:dd:ee:01");
    CHECK(s.find_machine("NAS") && s.find_machine(" nas ")->port == 7 && !s.find_machine("TV"));
    WolMachine bad;
    bad.name = "TV";
    bad.mac = "nope";
    CHECK(!s.save_machine(bad, &why) && !why.empty());
    s.wol_iface = "wlan0";
    s.wol_watch = false;
    s.speed_ping = "gateway";
    s.ble_seconds = 30;
    const std::string text = s.serialize();
    CHECK(has(text, "wol=desk pc|aa:bb:cc:dd:ee:01|||9\nwol=NAS|00:11:22:33:44:55||192.168.68.255|7\n"));
    CHECK(has(text, "wol_iface=wlan0\nwol_watch=0\nspeed_ping=gateway\nble_seconds=30\n"));
    const Settings round = Settings::parse(text);
    CHECK(round.wol_machines.size() == 2 && round.wol_machines[1].broadcast == "192.168.68.255" && !round.wol_watch);
    CHECK(round.wol_iface == "wlan0" && round.speed_ping == "gateway" && round.ble_seconds == 30);
    CHECK(s.remove_machine("NAS") && s.wol_machines.size() == 1 && !s.remove_machine("NAS"));
    // at most 20; the 21st is refused with words, a replacement still works
    Settings full;
    for (int i = 0; i < 20; ++i) {
        WolMachine x;
        x.name = "PC " + std::to_string(i);
        char mac[24];
        std::snprintf(mac, sizeof(mac), "00:11:22:33:44:%02x", i);
        x.mac = mac;
        CHECK(full.save_machine(x));
    }
    WolMachine extra;
    extra.name = "PC 20";
    extra.mac = "00:11:22:33:44:99";
    CHECK(!full.save_machine(extra, &why) && why.find("20") != std::string::npos);
    extra.name = "PC 3";
    CHECK(full.save_machine(extra) && full.wol_machines.size() == 20);
    std::string many = "v=1\n";
    for (int i = 0; i < 30; ++i) many += "wol=M" + std::to_string(i) + "|00:11:22:33:44:55|||9\n";
    CHECK(Settings::parse(many).wol_machines.size() == 20);
    // hostile values are ignored
    const Settings hostile = Settings::parse(
        "wol=PC|zz:zz:zz:zz:zz:zz|||9\nwol=|aa:bb:cc:dd:ee:ff|||9\nwol=A|aa:bb:cc:dd:ee:ff|bad host||9\n"
        "wol=B|aa:bb:cc:dd:ee:ff|||0\nwol=C|aa:bb:cc:dd:ee:ff||::1|9\nwol=D|aa:bb:cc:dd:ee:ff\n"
        "wol=E|aa:bb:cc:dd:ee:ff|||9\nwol=e|aa:bb:cc:dd:ee:01|||9\n"
        "wol_iface=wlan0; rm -rf /\nwol_watch=yes\nspeed_ping=$(reboot)\nble_seconds=0\nble_seconds=9999\n");
    CHECK(hostile.wol_machines.size() == 1 && hostile.wol_machines[0].name == "E"); /* "e" is the same name */
    CHECK(hostile.wol_iface.empty() && hostile.wol_watch && hostile.speed_ping == "1.1.1.1" && hostile.ble_seconds == 60);
    CHECK(Settings::parse("speed_ping=off\n").speed_ping == "off");
    CHECK(Settings::parse("speed_ping=8.8.8.8\n").speed_ping == "8.8.8.8");
    CHECK(Settings::parse("ble_seconds=5\n").ble_seconds == 5);
    // the defaults written
    const std::string def = Settings().serialize();
    CHECK(has(def, "wol_watch=1\nspeed_ping=1.1.1.1\nble_seconds=60\n") && def.find("wol=") == std::string::npos);
}

} // namespace

void test_netlog_03()
{
    test_format_03();
    test_settings_03();
}
