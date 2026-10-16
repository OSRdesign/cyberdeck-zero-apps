/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4 tests: what the Local devices, Wi-Fi networks and Wi-Fi walk test pages say and compute
 * (src/ui/view_model_04.cpp and the demonstration's snapshots, src/ui/demo_data_04.cpp, compiled in here: neither has LVGL):
 * the device rows (name, address, vendor, MAC, round trip, the tags), the status lines, the port scan words, the machine saved
 * for Wake-on-LAN, the network rows (signal bars, dBm, channel, band, security), the problems with their fix, the detail rows,
 * the channels view and its sentence, the walk's reading, window, scale, summary and weak-zone setting, the layouts.
 */
#include "check.hpp"

#include "../src/ui/demo_data_04.cpp"
#include "../src/ui/view_model_04.cpp"

#include <set>

using namespace nt;
using namespace nt::ui;

namespace {

const RowDef *row_of(const std::vector<RowDef> &rows, const std::string &label)
{
    for (const RowDef &r : rows)
        if (r.label == label) return &r;
    return nullptr;
}

bool overlaps(const Box &a, const Box &b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

DevicesRequest devices_request(const char *fixture = "deck", const char *iface = "wlan0")
{
    DevicesRequest req;
    const auto src = make_fixture_source(fixture_dir(fixture));
    req.net = read_net_state(*src);
    req.iface = iface;
    return req;
}

DevicesSnapshot devices(const char *scenario, const char *fixture = "deck", const char *iface = "wlan0")
{
    return demo_devices(scenario, devices_request(fixture, iface));
}

const Device *device_named(const DevicesSnapshot &s, const std::string &name)
{
    for (const Device &d : s.devices)
        if (d.name == name) return &d;
    return nullptr;
}

const Device *device_at(const DevicesSnapshot &s, const std::string &ip)
{
    for (const Device &d : s.devices)
        if (d.ip == ip) return &d;
    return nullptr;
}

PortScanRequest port_request(const DevicesSnapshot &d, const std::string &ip, bool full = false)
{
    PortScanRequest req;
    parse_ip(ip, req.address);
    req.range = d.range;
    req.full_range = full;
    return req;
}

WifiScanConfig wifi_cfg(const char *iface = "wlan0")
{
    WifiScanConfig c;
    c.iface = iface;
    return c;
}

WalkConfig walk_cfg(const char *iface = "wlan0")
{
    WalkConfig c;
    c.iface = iface;
    return c;
}

const WifiNetwork *network_named(const WifiSnapshot &s, const std::string &ssid, int channel = 0)
{
    for (const WifiNetwork &n : s.networks)
        if (n.ssid == ssid && (channel == 0 || n.channel == channel)) return &n;
    return nullptr;
}

/* ---------------------------------------------------------------- Local devices */

void test_device_rows()
{
    CHECK_EQ(rtt_text(0.4), std::string("<1 ms"));
    CHECK_EQ(rtt_text(2.0), std::string("2 ms"));
    CHECK_EQ(rtt_text(48.2), std::string("48 ms"));
    CHECK_EQ(rtt_text(120.0), std::string("120 ms"));

    const DevicesSnapshot s = devices("done");
    CHECK_EQ(s.devices.size(), std::size_t(14));
    CHECK_EQ(s.subnet, std::string("192.168.68.0/24"));
    CHECK(s.range.ok);
    for (std::size_t i = 1; i < s.devices.size(); ++i) CHECK(s.devices[i].ip_order > s.devices[i - 1].ip_order);

    /* the router: its name, the address at the right, the vendor and the MAC, the round trip, the diamond tag */
    const DeviceRowTexts r = device_row_texts(*device_named(s, "router"));
    CHECK_EQ(r.title, std::string("router"));
    CHECK_EQ(r.ip, std::string("192.168.68.1"));
    CHECK_EQ(r.detail, std::string("TP-Link  b0:4e:26:00:00:01"));
    CHECK_EQ(r.rtt, std::string("2 ms"));
    CHECK_EQ(r.tag, std::string("router"));
    CHECK(r.router);
    CHECK(!r.self);

    /* this device: the tag, no round trip */
    const DeviceRowTexts me = device_row_texts(*device_named(s, "cyberdeck"));
    CHECK_EQ(me.tag, std::string("this device"));
    CHECK(me.self);
    CHECK(!me.router);
    CHECK(me.rtt.empty());
    CHECK_EQ(me.ip, std::string("192.168.68.121"));

    /* a phone with a random MAC and no name: the address is the title, the vendor words say why there is no vendor */
    const DeviceRowTexts phone = device_row_texts(*device_at(s, "192.168.68.31"));
    CHECK_EQ(phone.title, std::string("192.168.68.31"));
    CHECK(phone.ip.empty());
    CHECK_EQ(phone.detail, std::string("Private (random) address  da:a1:19:7e:44:90"));
    CHECK(phone.tag.empty());
    CHECK_EQ(phone.rtt, std::string("48 ms"));

    /* an unnamed host with a vendor, and one whose company is not in the table */
    CHECK_EQ(device_row_texts(*device_at(s, "192.168.68.35")).detail, std::string("Espressif  24:6f:28:aa:01:02"));
    CHECK_EQ(device_row_texts(*device_at(s, "192.168.68.200")).detail, std::string("Vendor unknown  00:00:5e:00:53:01"));

    /* a name from mDNS and one from NetBIOS keep the address apart */
    CHECK_EQ(device_row_texts(*device_named(s, "nas")).ip, std::string("192.168.68.20"));
    CHECK_EQ(device_row_texts(*device_named(s, "printer")).ip, std::string("192.168.68.40"));

    /* nothing known: no MAC, no vendor, no round trip */
    Device bare;
    bare.ip = "192.168.68.9";
    const DeviceRowTexts b = device_row_texts(bare);
    CHECK_EQ(b.title, std::string("192.168.68.9"));
    CHECK(b.detail.empty());
    CHECK(b.rtt.empty());
    CHECK(b.tag.empty());

    /* a gateway that is also this device: this device wins (one tag, never both) */
    Device both = *device_named(s, "router");
    both.is_self = true;
    CHECK_EQ(device_row_texts(both).tag, std::string("this device"));
    CHECK(!device_row_texts(both).router);

    /* every tag is a word (the page draws its shape beside it) */
    for (const Device &d : s.devices) {
        const DeviceRowTexts t = device_row_texts(d);
        CHECK_EQ(t.tag.empty(), !(d.is_gateway || d.is_self));
        CHECK(t.title.size() <= 40);
        CHECK(t.detail.size() <= 60);
    }
}

void test_devices_status()
{
    CHECK_EQ(devices_band(DevicesSnapshot()).text, std::string("Press Scan to look for devices on your network"));
    const Band scan = devices_band(devices("scan"));
    CHECK_EQ(scan.text, std::string("Scanning 192.168.68.0/24: 96 of 254, 11 found"));
    CHECK(scan.tone == Tone::Normal);
    const Band done = devices_band(devices("done"));
    CHECK_EQ(done.text, std::string("Done: 14 devices on 192.168.68.0/24"));
    CHECK_EQ(devices_band(devices("empty")).text, std::string("Done: 1 device on 192.168.68.0/24"));
    CHECK_EQ(devices_band(devices("few")).text, std::string("Done: 3 devices on 192.168.68.0/24"));
    const Band stopped = devices_band(demo_devices_stop(devices("scan")));
    CHECK_EQ(stopped.text, std::string("Stopped: 11 devices on 192.168.68.0/24"));
    CHECK(stopped.tone == Tone::Warn);
    CHECK(demo_devices_stop(devices("done")).state == DevicesSnapshot::State::Done); /* a finished scan stays finished */

    DevicesSnapshot names = devices("scan");
    names.probed = names.total;
    names.naming = true;
    CHECK_EQ(devices_band(names).text, std::string("Looking up names: 11 devices on 192.168.68.0/24"));

    /* the core's refusals in plain words, with the fix in the list */
    const DevicesSnapshot refused = devices("refused");
    CHECK(refused.state == DevicesSnapshot::State::Failed);
    CHECK(devices_band(refused).tone == Tone::Bad);
    CHECK_EQ(devices_band(refused).text, std::string("Only private networks can be scanned"));
    CHECK(devices_empty(refused).text.find("is not a private address") != std::string::npos); /* the whole words are in the list */
    CHECK_EQ(devices_headline("This network has no other addresses to scan."), std::string("Nothing to scan on this network"));
    CHECK_EQ(devices_headline(""), std::string("The scan could not run"));
    CHECK_EQ(devices_headline("No network"), std::string("No network"));
    CHECK_EQ(devices_headline(std::string(80, 'x')), std::string("The scan could not run"));
    CHECK_EQ(devices_fix(refused.error), std::string("Connect to your home network, or choose another one under Network."));
    const DevicesSnapshot nonet = devices("done", "eth-down", "end0");
    CHECK(nonet.state == DevicesSnapshot::State::Failed);
    CHECK_EQ(nonet.error, std::string("No network"));
    CHECK(devices_band(nonet).tone == Tone::Warn);
    CHECK_EQ(devices_fix("No network"), std::string("Connect to a network first, or choose another one under Network."));
    CHECK_EQ(devices_fix("This network has no other addresses to scan."), std::string("Choose another network under Network."));
    CHECK(devices_fix("something else").empty());

    /* what the list says instead of devices */
    CHECK_EQ(devices_empty(DevicesSnapshot()).text, std::string(kDevicesHint));
    CHECK_EQ(std::string(kDevicesHint), std::string("Scans only your own network. Use it on networks you own or are allowed to test."));
    DevicesSnapshot none = devices("scan");
    none.devices.clear();
    CHECK_EQ(devices_empty(none).text, std::string("No device found yet."));
    none.state = DevicesSnapshot::State::Done;
    CHECK_EQ(devices_empty(none).text, std::string("No device answered. Some devices ignore scans."));
    CHECK(devices_empty(refused).tone == Tone::Bad);
    CHECK(!devices_empty(nonet).fix.empty());

    /* no status line is longer than the line of the smallest screen allows */
    for (const char *sc : {"scan", "done", "empty", "few", "refused"}) CHECK(devices_band(devices(sc)).text.size() <= 96);
    CHECK(done.text.size() <= 48);
}

void test_device_page()
{
    const DevicesSnapshot s = devices("done");
    const std::vector<RowDef> rows = device_info_rows(*device_named(s, "router"));
    CHECK_EQ(rows.size(), std::size_t(6));
    CHECK_EQ(row_of(rows, "IP address")->value, std::string("192.168.68.1"));
    CHECK_EQ(row_of(rows, "Name")->value, std::string("router  (from reverse DNS)"));
    CHECK_EQ(row_of(rows, "MAC address")->value, std::string("b0:4e:26:00:00:01"));
    CHECK_EQ(row_of(rows, "Vendor")->value, std::string("TP-Link"));
    CHECK_EQ(row_of(rows, "Round trip")->value, std::string("2 ms"));
    CHECK_EQ(row_of(rows, "Role")->value, std::string("Router: the gateway of this network"));
    CHECK_EQ(row_of(device_info_rows(*device_named(s, "nas")), "Name")->value, std::string("nas  (from mDNS)"));
    CHECK_EQ(row_of(device_info_rows(*device_named(s, "printer")), "Name")->value, std::string("printer  (from NetBIOS)"));

    /* a random-MAC phone: the vendor row says why it cannot be known; no name; another device */
    const std::vector<RowDef> phone = device_info_rows(*device_at(s, "192.168.68.31"));
    CHECK_EQ(row_of(phone, "Vendor")->value, std::string("Private (random) address: the vendor cannot be known"));
    CHECK_EQ(row_of(phone, "Name")->value, std::string("none found"));
    CHECK_EQ(row_of(phone, "Role")->value, std::string("Another device"));
    CHECK_EQ(row_of(device_info_rows(*device_at(s, "192.168.68.200")), "Vendor")->value, std::string("Vendor unknown"));

    /* this device: no ping round trip, no router role */
    const std::vector<RowDef> me = device_info_rows(*device_named(s, "cyberdeck"));
    CHECK_EQ(row_of(me, "Round trip")->value, std::string("this device"));
    CHECK_EQ(row_of(me, "Role")->value, std::string("This device"));
    Device bare;
    bare.ip = "192.168.68.9";
    CHECK_EQ(row_of(device_info_rows(bare), "MAC address")->value, std::string("not known"));
    CHECK_EQ(row_of(device_info_rows(bare), "Vendor")->value, std::string("not known"));
    CHECK_EQ(row_of(device_info_rows(bare), "Round trip")->value, std::string("no answer to ping"));
    CHECK_EQ(device_role(*device_named(s, "router")), std::string("Router: the gateway of this network"));

    CHECK_EQ(device_band_text(*device_named(s, "router")), std::string("192.168.68.1, router"));
    CHECK_EQ(device_band_text(*device_named(s, "cyberdeck")), std::string("192.168.68.121, this device"));
    CHECK_EQ(device_band_text(*device_named(s, "nas")), std::string("192.168.68.20"));

    /* the actions' words */
    CHECK_EQ(std::string(kActPing), std::string("Ping this device"));
    CHECK_EQ(std::string(kActPort), std::string("Check a port"));
    CHECK_EQ(std::string(kActScanCommon), std::string("Scan common ports"));
    CHECK_EQ(std::string(kActScanAll), std::string("Scan ports 1-1024"));
    CHECK_EQ(std::string(kActSaveWol), std::string("Save for Wake-on-LAN"));
}

void test_port_scan_words()
{
    const DevicesSnapshot d = devices("done");
    const int common = static_cast<int>(local_common_ports().size());
    const PortScanSnapshot scan = demo_portscan("scan", port_request(d, "192.168.68.1"));
    CHECK(scan.state == PortScanSnapshot::State::Scanning);
    CHECK_EQ(portscan_band(scan).text, "Scanning " + std::to_string(scan.scanned) + " of " + std::to_string(common) + ": 2 open");
    const PortScanSnapshot full = demo_portscan("scan", port_request(d, "192.168.68.1", true));
    CHECK_EQ(full.total, 1024);
    CHECK_EQ(portscan_band(full).text, "Scanning 307 of 1024: 2 open");

    const PortScanSnapshot done = demo_portscan("done", port_request(d, "192.168.68.1", true));
    CHECK_EQ(portscan_band(done).text, std::string("Done: 4 open ports among 1024"));
    CHECK_EQ(done.open.size(), std::size_t(4));
    CHECK_EQ(open_port_row(done.open[0]).label, std::string("Port 22  ssh"));
    CHECK_EQ(open_port_row(done.open[0]).value, std::string("open"));
    CHECK(open_port_row(done.open[0]).tone == Tone::Ok);
    CHECK_EQ(open_port_row(done.open[2]).label, std::string("Port 80  http"));
    OpenPort odd;
    odd.port = 31337;
    CHECK_EQ(open_port_row(odd).label, std::string("Port 31337"));
    CHECK(portscan_empty_text(done).empty());

    const PortScanSnapshot empty = demo_portscan("empty", port_request(d, "192.168.68.20"));
    CHECK(empty.state == PortScanSnapshot::State::Done);
    CHECK_EQ(portscan_empty_text(empty), std::string("No open port found among the common ones"));
    CHECK_EQ(portscan_band(empty).text, "Done: no open port among " + std::to_string(common));
    CHECK_EQ(portscan_empty_text(demo_portscan("empty", port_request(d, "192.168.68.20", true))), std::string("No open port found in ports 1-1024"));

    const PortScanSnapshot stopped = demo_portscan_stop(scan);
    CHECK(stopped.state == PortScanSnapshot::State::Cancelled);
    CHECK(portscan_band(stopped).tone == Tone::Warn);
    CHECK(portscan_band(stopped).text.rfind("Stopped at ", 0) == 0);
    CHECK(portscan_empty_text(stopped).empty());

    /* a target the core refuses (a public address, another subnet, the broadcast) ends in its words, before any scan */
    for (const char *ip : {"8.8.8.8", "10.1.2.3", "192.168.68.255", "192.168.68.0"}) {
        const PortScanSnapshot r = demo_portscan("done", port_request(d, ip));
        CHECK(r.state == PortScanSnapshot::State::Failed);
        CHECK_EQ(r.error, std::string(kScanTargetRefused));
        CHECK_EQ(portscan_band(r).text, std::string("Only devices on your own network can be scanned."));
        CHECK(portscan_band(r).tone == Tone::Bad);
    }
    PortScanRequest norange = port_request(d, "192.168.68.1");
    norange.range = ScanRange();
    CHECK_EQ(demo_portscan("done", norange).error, std::string("No network"));
    CHECK_EQ(demo_portscan("refused", port_request(d, "192.168.68.1")).error, std::string(kScanTargetRefused));
    CHECK(portscan_band(PortScanSnapshot()).text.empty());
}

void test_wol_of_device()
{
    const DevicesSnapshot d = devices("done");
    Settings settings;
    WolMachine m;
    std::string why;
    CHECK(wol_machine_of(*device_named(d, "nas"), settings, m, &why));
    CHECK_EQ(m.name, std::string("nas"));
    CHECK_EQ(m.mac, std::string("00:11:32:aa:bb:cc"));
    CHECK_EQ(m.host, std::string("192.168.68.20"));
    CHECK_EQ(m.port, 9);
    CHECK(settings.save_machine(m, &why));
    CHECK_EQ(wol_save_text("nas"), std::string("Saved nas for Wake-on-LAN"));
    CHECK_EQ(wol_machine_with_mac(settings, "00:11:32:AA:BB:CC"), std::string("nas"));
    CHECK(wol_machine_with_mac(settings, "00:11:32:aa:bb:dd").empty());

    /* a name already saved (any case) gets a number, so the saved machine is never replaced */
    Device twin = *device_named(d, "nas");
    twin.mac = "00:11:32:aa:bb:ee";
    twin.name = "NAS";
    CHECK(wol_machine_of(twin, settings, m, &why));
    CHECK_EQ(m.name, std::string("NAS 2"));
    CHECK(settings.save_machine(m, &why));
    CHECK(wol_machine_of(twin, settings, m, &why));
    CHECK_EQ(m.name, std::string("NAS 3"));
    CHECK_EQ(settings.wol_machines.size(), std::size_t(2));

    /* no name: the address; a long one is cut to 32 characters; the separator of the settings file is dropped */
    CHECK(wol_machine_of(*device_at(d, "192.168.68.35"), settings, m, &why));
    CHECK_EQ(m.name, std::string("192.168.68.35"));
    Device longname = *device_named(d, "nas");
    longname.name = "a-very-long-device-name-from-the-network-with-pipe|inside";
    CHECK(wol_machine_of(longname, Settings(), m, &why));
    CHECK(m.name.size() <= kWolNameMax);
    CHECK(m.name.find('|') == std::string::npos);
    CHECK(normalize_machine(m, &why)); /* the core accepts it */

    /* no MAC: nothing to save, and the words say so */
    Device nomac = *device_named(d, "nas");
    nomac.mac.clear();
    CHECK(!wol_machine_of(nomac, settings, m, &why));
    CHECK_EQ(why, std::string("This device has no MAC address to save."));
    CHECK_EQ(std::string(kWolFullText), std::string("You can save up to 20 machines: delete one first."));
}

void test_devices_demo()
{
    /* the scripted list follows the board's own subnet: every address lies inside the range, this device at its own address */
    for (const char *fixture : {"deck", "pi-3a-plus"}) {
        const char *iface = "wlan0";
        const DevicesSnapshot s = devices("done", fixture, iface);
        CHECK(s.state == DevicesSnapshot::State::Done);
        int self = 0, router = 0;
        for (const Device &d : s.devices) {
            CHECK(s.range.contains(d.ip_order));
            if (d.is_self) {
                ++self;
                CHECK_EQ(d.ip_order, s.range.self);
            }
            if (d.is_gateway) ++router;
        }
        CHECK_EQ(self, 1);
        CHECK_EQ(router, 1);
        CHECK_EQ(s.found, static_cast<int>(s.devices.size()));
    }
    const DevicesSnapshot scan = devices("scan");
    CHECK(scan.state == DevicesSnapshot::State::Scanning);
    CHECK(scan.running());
    CHECK(scan.devices.size() < devices("done").devices.size());
    CHECK_EQ(scan.total, 254);
    CHECK_EQ(devices("empty").devices.size(), std::size_t(1));
    CHECK(devices("empty").devices[0].is_self);
}

/* ---------------------------------------------------------------- Wi-Fi networks */

void test_wifi_rows()
{
    CHECK_EQ(signal_bars(-30), 4);
    CHECK_EQ(signal_bars(-50), 4);
    CHECK_EQ(signal_bars(-51), 3);
    CHECK_EQ(signal_bars(-60), 3);
    CHECK_EQ(signal_bars(-61), 2);
    CHECK_EQ(signal_bars(-70), 2);
    CHECK_EQ(signal_bars(-71), 1);
    CHECK_EQ(signal_bars(-100), 1);
    /* the bars and the words agree: Excellent 4, Good 3, Fair 2, Weak 1 */
    for (int dbm = -100; dbm <= -30; ++dbm) {
        const std::string w = signal_word(dbm);
        CHECK_EQ(signal_bars(dbm), w == "Excellent" ? 4 : w == "Good" ? 3 : w == "Fair" ? 2 : 1);
    }

    const WifiSnapshot s = demo_wifi("list", wifi_cfg(), 0);
    CHECK(s.state == WifiSnapshot::State::Ready);
    CHECK_EQ(s.networks.size(), std::size_t(11));
    for (std::size_t i = 1; i < s.networks.size(); ++i) CHECK(s.networks[i].dbm <= s.networks[i - 1].dbm);

    const WifiRowTexts home = wifi_row_texts(*network_named(s, "HomeNet", 36));
    CHECK_EQ(home.title, std::string("HomeNet"));
    CHECK_EQ(home.dbm, std::string("-48 dBm"));
    CHECK_EQ(home.bars, 4);
    CHECK(home.in_use);
    CHECK_EQ(home.connected, std::string("connected"));
    CHECK_EQ(home.caption, std::string("ch 36, 5 GHz"));
    CHECK_EQ(home.security, std::string("WPA2"));
    CHECK(!home.weak_security);

    const WifiRowTexts g24 = wifi_row_texts(*network_named(s, "HomeNet", 6));
    CHECK(!g24.in_use);
    CHECK(g24.connected.empty());
    CHECK_EQ(g24.caption, std::string("ch 6, 2.4 GHz"));

    const WifiRowTexts hidden = wifi_row_texts(*network_named(s, "", 11));
    CHECK_EQ(hidden.title, std::string("(hidden)"));
    const WifiRowTexts open = wifi_row_texts(*network_named(s, "CafeGuest"));
    CHECK_EQ(open.security, std::string("Open"));
    CHECK(open.weak_security);
    CHECK_EQ(wifi_row_texts(*network_named(s, "Neighbour-5G")).security, std::string("WPA3"));
    CHECK_EQ(wifi_row_texts(*network_named(s, "FibreBox-5G")).security, std::string("WPA2/WPA3"));
    CHECK_EQ(wifi_row_texts(*network_named(s, "Office")).security, std::string("Enterprise"));
    for (const WifiNetwork &n : s.networks) {
        const WifiRowTexts t = wifi_row_texts(n);
        CHECK(!t.title.empty());
        CHECK(t.bars >= 1 && t.bars <= 4);
        CHECK(t.caption.size() <= 24);
        CHECK(t.title.size() <= 32);
    }
    /* an ad hoc network says so */
    WifiNetwork adhoc = *network_named(s, "Livebox-4F2A");
    adhoc.adhoc = true;
    CHECK_EQ(wifi_row_texts(adhoc).caption, std::string("ch 1, 2.4 GHz, ad hoc"));
    WifiNetwork nochan;
    nochan.ssid = "x";
    CHECK(wifi_row_texts(nochan).caption.empty());
}

void test_wifi_status()
{
    CHECK_EQ(scanned_ago(10000, 10000), std::string("scanned just now"));
    CHECK_EQ(scanned_ago(13000, 10000), std::string("scanned 3 s ago"));
    CHECK_EQ(scanned_ago(75000, 10000), std::string("scanned 1 min ago"));
    CHECK_EQ(scanned_ago(5000, 10000), std::string("scanned just now")); /* a clock that went back never gives a negative age */

    const WifiSnapshot s = demo_wifi("list", wifi_cfg(), 0);
    const Band b = wifi_band(s, kDemoClockMs, "wlan0");
    CHECK_EQ(b.text, std::string("11 networks on wlan0, scanned 3 s ago"));
    CHECK(b.tone == Tone::Normal);
    WifiSnapshot one = s;
    one.networks.resize(1);
    CHECK_EQ(wifi_band(one, kDemoClockMs, "").text, std::string("1 network on wlan0, scanned 3 s ago"));
    WifiSnapshot busy = s;
    busy.busy = true;
    CHECK_EQ(wifi_band(busy, kDemoClockMs, "").text, std::string("11 networks on wlan0, scanning again"));
    WifiSnapshot stale = s;
    stale.error = "NetworkManager did not answer in time.";
    CHECK(wifi_band(stale, kDemoClockMs, "").tone == Tone::Warn);
    const WifiSnapshot empty = demo_wifi("empty", wifi_cfg(), 0);
    CHECK_EQ(wifi_band(empty, kDemoClockMs, "").text, std::string("No network found on wlan0, scanned 3 s ago"));
    WifiSnapshot first;
    first.state = WifiSnapshot::State::Scanning;
    first.iface = "wlan0";
    CHECK_EQ(wifi_band(first, 0, "").text, std::string("Scanning wlan0"));
    CHECK_EQ(wifi_band(WifiSnapshot(), 0, "").text, std::string("Looking for Wi-Fi networks"));
    WifiSnapshot stopped = s;
    stopped.state = WifiSnapshot::State::Stopped;
    CHECK(wifi_band(stopped, kDemoClockMs, "").tone == Tone::Warn);

    /* the problems, in plain words with their fix */
    const WifiSnapshot nonic = demo_wifi("list", wifi_cfg("end0"), 0);
    CHECK(nonic.state == WifiSnapshot::State::Failed);
    CHECK_EQ(nonic.error, std::string("No Wi-Fi adapter on end0"));
    CHECK_EQ(wifi_band(nonic, 0, "wlan0").text, std::string("No Wi-Fi adapter on end0: choose wlan0 under Network"));
    CHECK(wifi_band(nonic, 0, "wlan0").tone == Tone::Warn);
    CHECK_EQ(wifi_band(nonic, 0, "").text, std::string("No Wi-Fi adapter on end0: choose a Wi-Fi adapter under Network"));
    CHECK(!wifi_problem(nonic.error, "wlan0").fix.empty());
    CHECK_EQ(wifi_problem("No Wi-Fi adapter on this board", "").headline, std::string("No Wi-Fi adapter on this board"));
    CHECK(wifi_problem("No Wi-Fi adapter on this board", "").fix.empty());
    const WifiProblem off = wifi_problem(demo_wifi("off", wifi_cfg(), 0).error, "wlan0");
    CHECK_EQ(off.headline, std::string("Wi-Fi is switched off"));
    CHECK_EQ(off.fix, std::string("Turn Wi-Fi on, then try again."));
    const WifiProblem nm = wifi_problem(demo_wifi("nonm", wifi_cfg(), 0).error, "wlan0");
    CHECK_EQ(nm.headline, std::string("nmcli is missing: the scan needs NetworkManager"));
    CHECK_EQ(nm.fix, std::string("Install the package network-manager."));
    CHECK_EQ(wifi_problem("NetworkManager is not running: the Wi-Fi scan needs it.", "").headline, std::string("NetworkManager is not running"));
    CHECK_EQ(wifi_problem("NetworkManager did not answer in time.", "").headline, std::string("NetworkManager did not answer"));
    CHECK_EQ(wifi_problem("nmcli failed (exit 3): odd", "").headline, std::string("The Wi-Fi scan could not run"));
    CHECK_EQ(wifi_problem("nmcli failed (exit 3): odd", "").words, std::string("nmcli failed (exit 3): odd"));
    for (const char *sc : {"off", "nonm"}) CHECK(wifi_band(demo_wifi(sc, wifi_cfg(), 0), 0, "wlan0").text.size() <= 64);
    CHECK(wifi_band(nonic, 0, "wlan0").text.size() <= 64);
}

void test_wifi_detail()
{
    CHECK(security_meaning(WifiSecurity::Open).find("anyone near can read your traffic") != std::string::npos);
    std::set<std::string> seen;
    for (const WifiSecurity c : {WifiSecurity::Open, WifiSecurity::EnhancedOpen, WifiSecurity::Wep, WifiSecurity::Wpa, WifiSecurity::Wpa2,
                                 WifiSecurity::Wpa3, WifiSecurity::Enterprise}) {
        const std::string m = security_meaning(c);
        CHECK(!m.empty());
        CHECK(m.size() <= 56);
        seen.insert(m);
    }
    CHECK_EQ(seen.size(), std::size_t(7));
    CHECK(security_meaning(WifiSecurity::Wep).find("broken") != std::string::npos);

    const WifiSnapshot s = demo_wifi("list", wifi_cfg(), 0);
    const std::vector<RowDef> rows = wifi_detail_rows(*network_named(s, "HomeNet", 36), false);
    CHECK_EQ(row_of(rows, "Status")->value, std::string("Connected"));
    CHECK_EQ(row_of(rows, "Network name")->value, std::string("HomeNet"));
    CHECK_EQ(row_of(rows, "BSSID")->value, std::string("b0:4e:26:00:00:01"));
    CHECK_EQ(row_of(rows, "Vendor")->value, std::string("TP-Link"));
    CHECK_EQ(row_of(rows, "Signal")->value, std::string("-48 dBm, Excellent"));
    CHECK_EQ(row_of(rows, "Channel")->value, std::string("36"));
    CHECK_EQ(row_of(rows, "Frequency")->value, std::string("5180 MHz"));
    CHECK_EQ(row_of(rows, "Band")->value, std::string("5 GHz"));
    CHECK_EQ(row_of(rows, "Bit rate")->value, std::string("540 Mbit/s"));
    CHECK_EQ(row_of(rows, "Security")->value, std::string("WPA2"));
    CHECK(row_of(rows, "Security")->tone == Tone::Normal);
    CHECK_EQ(row_of(wifi_detail_rows(*network_named(s, "Livebox-4F2A"), false), "Status")->value, std::string("In range"));

    /* an open network is flagged; a hidden one is named so; a random BSSID has no vendor and says why */
    const std::vector<RowDef> open = wifi_detail_rows(*network_named(s, "CafeGuest"), false);
    CHECK(row_of(open, "Security")->tone == Tone::Warn);
    CHECK_EQ(row_of(open, "Security")->value, std::string("Open"));
    CHECK_EQ(row_of(wifi_detail_rows(*network_named(s, "", 11), false), "Network name")->value, std::string("(hidden)"));
    const std::vector<RowDef> hot = wifi_detail_rows(*network_named(s, "Pixel-Hotspot"), false);
    CHECK_EQ(row_of(hot, "Vendor")->value, std::string("Private (random) address: the vendor cannot be known"));
    WifiNetwork unknown = *network_named(s, "Livebox-4F2A");
    unknown.vendor.clear();
    CHECK_EQ(row_of(wifi_detail_rows(unknown, false), "Vendor")->value, std::string("not known"));

    /* gone: the last values stay, the status says so */
    const std::vector<RowDef> gone = wifi_detail_rows(*network_named(s, "CafeGuest"), true);
    CHECK_EQ(row_of(gone, "Status")->value, std::string("Gone: not in the last scan"));
    CHECK(row_of(gone, "Status")->tone == Tone::Warn);
    CHECK_EQ(row_of(gone, "Signal")->value, row_of(open, "Signal")->value);
    const Band gb = wifi_detail_band(*network_named(s, "CafeGuest"), true, s, kDemoClockMs, "wlan0");
    CHECK_EQ(gb.text, std::string("Gone: not in the last scan, last values shown"));
    CHECK(gb.tone == Tone::Warn);
    CHECK_EQ(wifi_detail_band(*network_named(s, "HomeNet", 36), false, s, kDemoClockMs, "wlan0").text,
             std::string("-48 dBm, Excellent, scanned 3 s ago"));

    /* the follower on the scripted lists: stage 1 of "vanish" has no CafeGuest */
    WifiFollower f;
    f.follow(*network_named(s, "CafeGuest"));
    const WifiSnapshot v0 = demo_wifi("vanish", wifi_cfg(), 0), v1 = demo_wifi("vanish", wifi_cfg(), 1);
    CHECK(network_named(v0, "CafeGuest") != nullptr);
    CHECK(network_named(v1, "CafeGuest") == nullptr);
    f.update(v0);
    CHECK(!f.gone());
    f.update(v1);
    CHECK(f.gone());
    CHECK_EQ(f.network().ssid, std::string("CafeGuest"));
    CHECK(v1.lists > v0.lists);
}

void test_channels()
{
    const WifiSnapshot s = demo_wifi("list", wifi_cfg(), 0);
    const ChannelsView g24 = channels_view(s.channels.g24, WifiBand::G24);
    CHECK_EQ(g24.title, std::string("2.4 GHz"));
    CHECK_EQ(g24.bars.size(), std::size_t(13));
    for (std::size_t i = 0; i < g24.bars.size(); ++i) CHECK_EQ(g24.bars[i].channel, static_cast<int>(i) + 1);
    CHECK(g24.empty.empty());
    int best = 0, busiest = 0;
    for (const ChannelBar &b : g24.bars) {
        best += b.best;
        busiest += b.busiest;
        CHECK(b.weight <= g24.top);
    }
    CHECK_EQ(best, 1);
    CHECK_EQ(busiest, 1);
    CHECK_EQ(g24.bars[5].networks, 2); /* HomeNet and CafeGuest on channel 6 */
    CHECK(g24.bars[5].busiest);
    CHECK_EQ(g24.busiest, std::string("Busiest: channel 6, 2 networks"));
    CHECK(g24.bars[static_cast<std::size_t>(s.channels.g24.recommended - 1)].best);

    const ChannelsView g5 = channels_view(s.channels.g5, WifiBand::G5);
    CHECK(g5.empty.empty());
    CHECK_EQ(g5.title, std::string("5 GHz"));
    CHECK(!g5.bars.empty());
    CHECK(channels_have_149(g5));
    CHECK(!channels_have_149(g24));
    CHECK(g5.busiest.rfind("Busiest: channel ", 0) == 0);

    /* the sentence names the least crowded channel of each band */
    CHECK_EQ(channels_sentence(s.channels), "Least crowded: channel " + std::to_string(s.channels.g24.recommended) + " (2.4 GHz), channel " +
                                                std::to_string(s.channels.g5.recommended) + " (5 GHz)");
    CHECK_EQ(channels_sentence(ChannelSummary()), std::string("Not enough networks seen yet to name a channel"));
    ChannelSummary only24;
    only24.g24.recommended = 11;
    CHECK_EQ(channels_sentence(only24), std::string("Least crowded: channel 11 (2.4 GHz)"));

    /* a 2.4 GHz-only board: the 5 GHz part says so instead of drawing nothing */
    const WifiSnapshot deck = demo_wifi("g24", wifi_cfg(), 0);
    const ChannelsView none5 = channels_view(deck.channels.g5, WifiBand::G5);
    CHECK_EQ(none5.empty, std::string("No 5 GHz network seen: this adapter may be 2.4 GHz only"));
    CHECK(none5.bars.empty());
    CHECK_EQ(channels_view(deck.channels.g24, WifiBand::G24).empty, std::string(""));
    CHECK_EQ(channels_view(demo_wifi("empty", wifi_cfg(), 0).channels.g24, WifiBand::G24).empty, std::string("No network seen on 2.4 GHz"));
    CHECK_EQ(channels_sentence(deck.channels), "Least crowded: channel " + std::to_string(deck.channels.g24.recommended) + " (2.4 GHz)");

    /* the busier the 2.4 GHz neighbourhood, the more it weighs: a crowd on 1, 6 and 11 */
    const WifiSnapshot many = demo_wifi("many", wifi_cfg(), 0);
    const ChannelsView crowd = channels_view(many.channels.g24, WifiBand::G24);
    CHECK(crowd.top > g24.top);
    CHECK_EQ(std::string(kChannelNote149), std::string("Channels 149 to 165 are not allowed for Wi-Fi in every country."));

    /* the layout: side by side on a wide area when both parts have bars, else one over the other; nothing overlaps */
    for (const int w : {480, 560, 640, 720, 960}) {
        for (const bool d0 : {false, true}) {
            for (const bool d1 : {false, true}) {
                const ChannelsLayout L = channels_layout(w, 20, 14, 84, 8, d0, d1);
                CHECK(!overlaps(L.first, L.second));
                CHECK(L.first.x >= 0 && L.first.x + L.first.w <= w);
                CHECK(L.second.x >= 0 && L.second.x + L.second.w <= w);
                CHECK(L.total_h >= L.first.y + L.first.h);
                CHECK(L.total_h >= L.second.y + L.second.h);
                CHECK_EQ(L.side, d0 && d1 && w >= 640);
            }
        }
    }
    CHECK(channels_layout(960, 20, 14, 84, 8, true, true).total_h < channels_layout(560, 20, 14, 84, 8, true, true).total_h);
    CHECK(channels_layout(560, 20, 14, 84, 8, true, false).total_h < channels_layout(560, 20, 14, 84, 8, true, true).total_h);
}

/* ---------------------------------------------------------------- the walk test */

void test_walk_texts()
{
    CHECK_EQ(mmss(0), std::string("00:00"));
    CHECK_EQ(mmss(83.4), std::string("01:23"));
    CHECK_EQ(mmss(599.9), std::string("09:59"));
    CHECK_EQ(mmss(725), std::string("12:05"));
    CHECK_EQ(mmss(3723), std::string("1:02:03"));
    CHECK_EQ(mmss(-5), std::string("00:00"));
    CHECK_EQ(weak_value(-75), std::string("-75 dBm"));
    CHECK_EQ(next_weak_dbm(-70), -75);
    CHECK_EQ(next_weak_dbm(-75), -80);
    CHECK_EQ(next_weak_dbm(-80), -70);
    CHECK_EQ(next_weak_dbm(-77), -75); /* a value from the settings file that is not one of the three */
    int v = -70;
    for (int i = 0; i < 3; ++i) v = next_weak_dbm(v);
    CHECK_EQ(v, -70);

    const WalkSnapshot run = demo_walk("run", walk_cfg());
    CHECK(run.state == WalkSnapshot::State::Running);
    CHECK_EQ(run.samples.size(), std::size_t(83));
    CHECK_EQ(walk_start_label(run), std::string("Stop"));
    CHECK_EQ(walk_start_label(WalkSnapshot()), std::string("Start"));
    CHECK_EQ(walk_start_label(demo_walk_stop(run)), std::string("Start"));
    CHECK_EQ(walk_band(WalkSnapshot(), "wlan0").text, std::string("Press Start, then walk around"));
    const Band rb = walk_band(run, "wlan0");
    CHECK_EQ(rb.text, "Walking: 01:23, weakest " + std::to_string(run.summary.weakest_dbm) + " dBm");
    CHECK(rb.tone == Tone::Normal);
    CHECK(run.summary.weakest_dbm <= -80);
    WalkSnapshot dip = run;
    dip.in_dip = true;
    CHECK_EQ(walk_band(dip, "").text, "Walking: 01:23, in a weak spot, weakest " + std::to_string(run.summary.weakest_dbm) + " dBm");
    CHECK(walk_band(dip, "").tone == Tone::Warn);
    WalkSnapshot nolink = run;
    nolink.linked = false;
    CHECK_EQ(walk_band(nolink, "").text, std::string("Walking: 01:23, no signal at the moment"));
    CHECK(walk_band(nolink, "").tone == Tone::Warn);
    const WalkSnapshot stopped = demo_walk_stop(run);
    CHECK_EQ(walk_band(stopped, "").text, "Stopped: 01:23, weakest " + std::to_string(run.summary.weakest_dbm) + " dBm");
    WalkSnapshot nothing;
    nothing.state = WalkSnapshot::State::Stopped;
    CHECK_EQ(walk_band(nothing, "").text, std::string("Stopped before the first reading"));
    const WalkSnapshot failed = demo_walk("run", walk_cfg("end0"));
    CHECK(failed.state == WalkSnapshot::State::Failed);
    CHECK_EQ(walk_band(failed, "wlan0").text, std::string("No Wi-Fi adapter on end0: choose wlan0 under Network"));
    CHECK(walk_band(failed, "wlan0").tone == Tone::Warn);
    CHECK_EQ(demo_walk("run", walk_cfg("")).error, std::string("No Wi-Fi adapter on this board"));
    CHECK(walk_band(run, "").text.size() <= 56);

    CHECK_EQ(std::string(kWalkNote), std::string("Walk slowly and keep the deck in your hand; stop at the dead spot to read the numbers."));
}

void test_walk_live()
{
    const WalkSnapshot run = demo_walk("run", walk_cfg());
    const WalkLive l = walk_live(run);
    CHECK_EQ(l.big, std::to_string(run.dbm));
    CHECK_EQ(l.unit, std::string("dBm"));
    CHECK_EQ(l.word, std::string(signal_word(run.dbm)));
    CHECK_EQ(l.elapsed, std::string("01:23"));
    CHECK_EQ(l.where, std::string("HomeNet on wlan0"));
    CHECK(l.zone.empty());
    CHECK(l.mark != Mark::None);

    /* every word has its shape and its tone */
    struct Case {
        int dbm;
        const char *word;
        Mark mark;
        Tone tone;
    };
    const Case cases[] = {{-45, "Excellent", Mark::Check, Tone::Ok}, {-58, "Good", Mark::Check, Tone::Ok},
                          {-66, "Fair", Mark::Bang, Tone::Warn}, {-82, "Weak", Mark::Cross, Tone::Bad}};
    for (const Case &c : cases) {
        WalkSnapshot s = run;
        s.dbm = c.dbm;
        s.linked = true;
        const WalkLive w = walk_live(s);
        CHECK_EQ(w.word, std::string(c.word));
        CHECK(w.mark == c.mark);
        CHECK(w.tone == c.tone);
        CHECK_EQ(w.big, std::to_string(c.dbm));
    }
    WalkSnapshot weak = run;
    weak.dbm = -80;
    CHECK_EQ(walk_live(weak).zone, std::string("Below -75 dBm: the weak zone"));
    weak.dbm = -75;
    CHECK(walk_live(weak).zone.empty()); /* at the threshold is not below it */
    weak.dbm = -76;
    CHECK(!walk_live(weak).zone.empty());
    WalkSnapshot nolink = run;
    nolink.linked = false;
    const WalkLive nl = walk_live(nolink);
    CHECK_EQ(nl.big, std::string("--"));
    CHECK(nl.unit.empty());
    CHECK_EQ(nl.word, std::string("No signal"));
    CHECK(nl.mark == Mark::Cross);
    const WalkLive idle = walk_live(WalkSnapshot());
    CHECK_EQ(idle.word, std::string("Ready"));
    CHECK_EQ(idle.big, std::string("--"));
    CHECK_EQ(idle.elapsed, std::string("00:00"));
    CHECK_EQ(walk_live(demo_walk_stop(run)).elapsed, std::string("01:23"));
    WalkSnapshot bare = run;
    bare.ssid.clear();
    CHECK_EQ(walk_live(bare).where, std::string("wlan0"));
}

void test_walk_chart()
{
    const WalkSnapshot run = demo_walk("run", walk_cfg());
    /* walking: the window grows from 30 s up to the last 150 s, then it moves */
    const WalkWindow w = walk_window(run);
    CHECK_NEAR(w.t0_s, 0, 1e-9);
    CHECK_NEAR(w.t1_s, 82, 1e-9);
    const WalkSnapshot lng = demo_walk("long", walk_cfg());
    CHECK_EQ(lng.samples.size(), std::size_t(200));
    const WalkWindow lw = walk_window(lng);
    CHECK_NEAR(lw.t1_s, 199, 1e-9);
    CHECK_NEAR(lw.t1_s - lw.t0_s, kWalkWindowS, 1e-9);
    WalkSnapshot early = run;
    early.samples.resize(10);
    CHECK_NEAR(walk_window(early).t1_s, 30, 1e-9);
    CHECK_NEAR(walk_window(early).t0_s, 0, 1e-9);
    CHECK_NEAR(walk_window(WalkSnapshot()).t1_s, 30, 1e-9);
    /* stopped: the whole walk that was kept */
    const WalkWindow sw = walk_window(demo_walk_stop(lng));
    CHECK_NEAR(sw.t0_s, 0, 1e-9);
    CHECK_NEAR(sw.t1_s, 199, 1e-9);

    /* the scale holds every reading and the threshold, in steps of 10, at least 30 dB, inside -100 .. -20 */
    for (const char *sc : {"run", "long", "nolink"}) {
        const WalkSnapshot s = demo_walk(sc, walk_cfg());
        const WalkYScale y = walk_y_scale(s.samples, s.weak_dbm);
        CHECK(y.top > y.bottom);
        CHECK(y.top - y.bottom >= 30);
        CHECK(y.top <= -20);
        CHECK(y.bottom >= -100);
        CHECK_EQ(y.top % 10, 0);
        CHECK_EQ(y.bottom % 10, 0);
        CHECK(s.weak_dbm < y.top && s.weak_dbm > y.bottom);
        for (const WalkSample &p : s.samples)
            if (p.linked) CHECK(p.dbm <= y.top && p.dbm >= y.bottom);
    }
    const WalkYScale empty = walk_y_scale({}, -75);
    CHECK(empty.top - empty.bottom >= 30);
    CHECK(empty.top > -75 && empty.bottom < -75);
    std::vector<WalkSample> loud(3);
    for (WalkSample &p : loud) {
        p.linked = true;
        p.dbm = -25;
    }
    CHECK(walk_y_scale(loud, -75).top <= -20);
    std::vector<WalkSample> quiet(3);
    for (WalkSample &p : quiet) {
        p.linked = true;
        p.dbm = -99;
    }
    const WalkYScale qs = walk_y_scale(quiet, -90);
    CHECK(qs.bottom >= -100);
    CHECK(qs.top - qs.bottom >= 30);

    CHECK_EQ(walk_legend(run), std::string("Dashed line: -75 dBm. Squares: weak. Ticks: access point changes."));
    CHECK_EQ(walk_legend(demo_walk("noiw", walk_cfg())), std::string("Dashed line: -75 dBm. Squares: weak. Access point changes need iw."));
    WalkConfig cfg = walk_cfg();
    cfg.weak_dbm = -80;
    CHECK(walk_legend(demo_walk("run", cfg)).find("-80 dBm") != std::string::npos);
}

void test_walk_summary()
{
    const WalkSnapshot s = demo_walk_stop(demo_walk("run", walk_cfg()));
    const std::vector<RowDef> rows = walk_summary_rows(s);
    CHECK_EQ(row_of(rows, "Duration")->value, std::string("01:23"));
    CHECK_EQ(row_of(rows, "Readings")->value, std::string("83"));
    const WalkSummary &m = s.summary;
    CHECK_EQ(row_of(rows, "Weakest")->value, std::to_string(m.weakest_dbm) + " dBm at " + mmss(m.weakest_at_s));
    CHECK(row_of(rows, "Weakest")->tone == Tone::Bad);
    CHECK(row_of(rows, "Average")->value.size() > 4);
    CHECK_EQ(row_of(rows, "Strongest")->value, std::to_string(m.strongest_dbm) + " dBm at " + mmss(m.strongest_at_s));
    const RowDef *zone = row_of(rows, "Time in the weak zone");
    CHECK(zone != nullptr);
    CHECK(zone->value.find(" s (") != std::string::npos);
    CHECK(zone->value.find(" %)") != std::string::npos);
    CHECK(zone->tone == Tone::Warn);
    CHECK_EQ(row_of(rows, "Dips under -75 dBm")->value, std::to_string(m.dips));
    CHECK(m.dips >= 1);
    CHECK_EQ(row_of(rows, "Access point changes")->value, std::string("1"));
    CHECK_EQ(row_of(rows, "At 01:02, moved to")->value, std::string("b0:4e:26:00:00:02"));
    CHECK(row_of(rows, "Without link") == nullptr);

    /* a walk that stayed strong has a green weak-zone row and no dip */
    WalkSnapshot calm = s;
    calm.summary.weak_samples = 0;
    calm.summary.weak_s = 0;
    calm.summary.weak_pct = 0;
    calm.summary.dips = 0;
    calm.summary.roams = 0;
    calm.roams.clear();
    const std::vector<RowDef> cr = walk_summary_rows(calm);
    CHECK_EQ(row_of(cr, "Time in the weak zone")->value, std::string("0 s (0 %)"));
    CHECK(row_of(cr, "Time in the weak zone")->tone == Tone::Ok);
    CHECK_EQ(row_of(cr, "Access point changes")->value, std::string("none"));

    /* with the threshold the walk was made with */
    WalkConfig cfg = walk_cfg();
    cfg.weak_dbm = -80;
    CHECK(row_of(walk_summary_rows(demo_walk_stop(demo_walk("run", cfg))), "Dips under -80 dBm") != nullptr);

    /* no iw: the changes are not known; no link for a while: said apart */
    const WalkSnapshot noiw = demo_walk_stop(demo_walk("noiw", walk_cfg()));
    CHECK_EQ(row_of(walk_summary_rows(noiw), "Access point changes")->value, std::string("not known: iw is missing"));
    const WalkSnapshot nl = demo_walk_stop(demo_walk("nolink", walk_cfg()));
    CHECK(nl.summary.nolink_samples == 4);
    CHECK_EQ(row_of(walk_summary_rows(nl), "Without link")->value, std::string("4 s"));
    CHECK_EQ(walk_nolink_note(nl), std::string("No link for 4 s"));
    CHECK(walk_nolink_note(s).empty());
    CHECK_EQ(walk_band(nl, "").text.rfind("Stopped: 01:23", 0), std::size_t(0));

    /* at most 5 changes are listed, all are counted */
    WalkSnapshot many = s;
    many.summary.roams = 9;
    many.roams.assign(9, s.roams[0]);
    int listed = 0;
    for (const RowDef &r : walk_summary_rows(many))
        if (r.label.rfind("At ", 0) == 0) ++listed;
    CHECK_EQ(listed, 5);
    CHECK_EQ(row_of(walk_summary_rows(many), "Access point changes")->value, std::string("9"));

    /* a walk with no reading at all: a plain row, nothing invented */
    WalkSnapshot dead;
    dead.state = WalkSnapshot::State::Stopped;
    dead.summary.samples = 5;
    dead.summary.duration_s = 5;
    const std::vector<RowDef> dr = walk_summary_rows(dead);
    CHECK_EQ(dr.size(), std::size_t(3));
    CHECK(row_of(dr, "Weakest") == nullptr);
    CHECK(row_of(dr, "Signal")->tone == Tone::Warn);

    for (const RowDef &r : rows) {
        CHECK(r.label.size() <= 32);
        CHECK(r.value.size() <= 40);
    }
}

void test_walk_demo()
{
    const WalkSnapshot run = demo_walk("run", walk_cfg());
    CHECK_EQ(run.summary.roams, 1);
    CHECK_EQ(run.roams.size(), std::size_t(1));
    CHECK_EQ(run.roams[0].t_ms, std::int64_t(62000));
    CHECK(run.summary.roaming_known);
    CHECK(run.summary.dips >= 1);
    CHECK(run.summary.weak_samples > 0);
    CHECK(!run.in_dip);
    CHECK(!demo_walk("noiw", walk_cfg()).summary.roaming_known);
    CHECK(demo_walk("noiw", walk_cfg()).iw_missing);
    CHECK_EQ(demo_walk("run", walk_cfg()).weak_dbm, -75);
    WalkConfig cfg = walk_cfg();
    cfg.weak_dbm = -70;
    CHECK(demo_walk("run", cfg).summary.weak_samples > run.summary.weak_samples);
    CHECK(demo_walk("stopped", walk_cfg()).state == WalkSnapshot::State::Stopped);
    CHECK(demo_walk_stop(WalkSnapshot()).state == WalkSnapshot::State::Idle);

    /* the live block's layout: the reading beside the chart on a wide area, over it on a tall one; boxes inside, none overlapping */
    for (const int w : {480, 560, 640, 720, 960}) {
        for (const int h : {200, 250, 300, 400, 500}) {
            const WalkLayout L = walk_layout(w, h, 20, 14, 40, 8);
            CHECK_EQ(L.side, w * 100 >= h * 170);
            for (const Box *b : {&L.reading, &L.chart, &L.legend}) {
                CHECK(b->x >= 0 && b->y >= 0 && b->w > 0 && b->h > 0);
                CHECK(b->x + b->w <= w);
            }
            CHECK(!overlaps(L.reading, L.chart));
            CHECK(!overlaps(L.chart, L.legend));
            CHECK(L.legend.y + L.legend.h <= h);
        }
    }
    CHECK(walk_layout(560, 250, 20, 14, 40, 8).side);
    CHECK(!walk_layout(640, 500, 20, 14, 40, 8).side);
}

} // namespace

void test_ui_model_04()
{
    test_device_rows();
    test_devices_status();
    test_device_page();
    test_port_scan_words();
    test_wol_of_device();
    test_devices_demo();
    test_wifi_rows();
    test_wifi_status();
    test_wifi_detail();
    test_channels();
    test_walk_texts();
    test_walk_live();
    test_walk_chart();
    test_walk_summary();
    test_walk_demo();
}
