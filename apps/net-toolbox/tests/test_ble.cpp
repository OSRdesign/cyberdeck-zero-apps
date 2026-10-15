/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): the BLE scan. bluetoothctl's output recorded on the Pi 3A+ (tests/fixtures/ble, sanitized):
 * the line cleaner (prompt redraws, colours, backspaces), the event parser (every RSSI line of the recording, the
 * `info` answers), hostile lines; `bluetoothctl show` and the /sys state in plain words; the device table on a fake clock
 * (smoothing, expiry and the seen count, names and aliases, the `info` context, company names, distance classes, the
 * bound); the whole scan on a scripted bluetoothctl (the recording replayed) with discovery turned off on every exit path
 * (the time is up, cancel, BlueZ refuses, no adapter, the scan never starts, the runner's timeout, the session destroyed);
 * the same on a fake bluetoothctl program run by the real stream runner.
 */
#include "check.hpp"
#include "stream_fakes.hpp"

#include "ble.hpp"
#include "text.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

using namespace nt;

namespace {

const std::int64_t kSec = 1000000;

void write_file(const std::string &path, const std::string &text)
{
    std::ofstream(path) << text;
}

std::string slurp(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/* a /sys with an adapter (or not) and its rfkill switch */
std::string make_sys(bool adapter, const char *soft = "0", const char *hard = "0")
{
    const std::string dir = test_tmpdir("blesys");
    for (const char *d : {"/sys", "/sys/class", "/sys/class/bluetooth", "/sys/class/rfkill", "/sys/class/rfkill/rfkill0",
                          "/sys/class/rfkill/rfkill1"})
        mkdir((dir + d).c_str(), 0755);
    if (adapter) mkdir((dir + "/sys/class/bluetooth/hci0").c_str(), 0755);
    write_file(dir + "/sys/class/rfkill/rfkill0/type", "bluetooth\n");
    write_file(dir + "/sys/class/rfkill/rfkill0/soft", std::string(soft) + "\n");
    write_file(dir + "/sys/class/rfkill/rfkill0/hard", std::string(hard) + "\n");
    write_file(dir + "/sys/class/rfkill/rfkill1/type", "wlan\n");
    write_file(dir + "/sys/class/rfkill/rfkill1/soft", "1\n"); /* Wi-Fi blocked: not Bluetooth's business */
    write_file(dir + "/sys/class/rfkill/rfkill1/hard", "0\n");
    return dir;
}

CommandResult shown(const std::string &text, int exit_code = 0)
{
    CommandResult r;
    r.started = true;
    r.exit_code = exit_code;
    r.output = text;
    return r;
}

/* the recording replayed, a line every 15 ms from 1 s; then ticks until the client ends it */
fake::Script replay(const char *file, std::int64_t step_us = 15000)
{
    fake::Script s;
    std::int64_t at = kSec;
    for (const std::string &l : fake::raw_lines(fixture("ble", file))) {
        at += step_us;
        s.steps.push_back(fake::Step{at, 0, 0, {l}});
    }
    s.hang = true;
    s.after_end = {fake::Step{at + 10 * kSec, 0, 0, {"\x1b[0;94m[bluetoothctl]> \x1b[0mscan off", "Discovery stopped",
                                                      "\x1b[0;94m[bluetoothctl]> \x1b[0mquit"}}};
    return s;
}

bool ends_with_off(const std::string &log)
{
    return ends_with(log, "scan off\nquit\n");
}

void test_ble_lines()
{
    const std::vector<std::string> lines = fake::raw_lines(fixture("ble", "scan-3a-plus.txt"));
    CHECK(lines.size() > 500);
    CHECK_EQ(clean_btctl_line(lines[0]), std::string("menu scan")); /* "Waiting...\r", prompt, blanks, 8 backspaces */
    CHECK_EQ(clean_btctl_line(lines.back()), std::string(""));       /* the last prompt alone */
    CHECK_EQ(clean_btctl_line("\x1b[0;94m[bluetoothctl]> \x1b[0m\r              \r[\x1b[0;93mCHG\x1b[0m] Device "
                              "4A:00:00:00:00:04 RSSI: 0xffffffb8 (-72)"),
             std::string("[CHG] Device 4A:00:00:00:00:04 RSSI: 0xffffffb8 (-72)"));
    CHECK_EQ(clean_btctl_line("[PI3A]> scan on"), std::string("scan on"));
    CHECK_EQ(clean_btctl_line("[bluetooth]# show"), std::string("show"));
    CHECK_EQ(clean_btctl_line("[CHG] Device X"), std::string("[CHG] Device X"));
    CHECK_EQ(clean_btctl_line("\tName: x  "), std::string("\tName: x"));
    CHECK_EQ(clean_btctl_line("a\x1b" "cb\x1b[2Kc\x07"), std::string("abc"));
    CHECK_EQ(clean_btctl_line("\x1b[0;9"), std::string(""));         /* a sequence cut at the end */

    // every line of the recording: the events, counted against the raw text
    int raw_rssi = 0, raw_new = 0, raw_company = 0;
    for (const std::string &l : lines) {
        if (l.find("RSSI: 0x") != std::string::npos && l.find("CHG") != std::string::npos) ++raw_rssi;
        if (l.find("NEW\x1b[0m] ") != std::string::npos) ++raw_new;
        if (l.find("CHG") != std::string::npos && l.find("ManufacturerData.Key:") != std::string::npos) ++raw_company;
    }
    int rssi = 0, news = 0, company = 0, headers = 0, missing = 0, fields = 0, on = 0, bad_range = 0;
    std::string types;
    for (const std::string &l : lines) {
        BtEvent e;
        if (!parse_btctl_line(clean_btctl_line(l), e)) continue;
        switch (e.kind) {
        case BtEvent::Kind::Rssi:
            ++rssi;
            if (e.value < -127 || e.value > 20) ++bad_range;
            break;
        case BtEvent::Kind::NewDevice: ++news; break;
        case BtEvent::Kind::Company: ++company; break;
        case BtEvent::Kind::InfoHeader:
            ++headers;
            types += e.address + "=" + e.text + " ";
            break;
        case BtEvent::Kind::InfoMissing: ++missing; break;
        case BtEvent::Kind::InfoField: ++fields; break;
        case BtEvent::Kind::Discovering:
            if (e.value) ++on;
            break;
        default: break;
        }
    }
    CHECK(raw_rssi > 100 && rssi == raw_rssi && bad_range == 0);
    CHECK(raw_new > 10 && news == raw_new);
    CHECK(raw_company > 50 && company == raw_company);
    CHECK_EQ(headers, 2);
    CHECK_EQ(types, std::string("0A:00:00:00:00:05=public 4A:00:00:00:00:04=random "));
    CHECK(missing == 1 && fields > 20 && on == 2);
    // the killed recording parses as well
    int killed_rssi = 0;
    for (const std::string &l : fake::raw_lines(fixture("ble", "scan-killed-3a-plus.txt"))) {
        BtEvent e;
        if (parse_btctl_line(clean_btctl_line(l), e) && e.kind == BtEvent::Kind::Rssi) ++killed_rssi;
    }
    CHECK(killed_rssi > 20);

    // single lines, old formats and hostile ones
    BtEvent e;
    CHECK(parse_btctl_line("[CHG] Device 4a:00:00:00:00:04 RSSI: -72", e) && e.kind == BtEvent::Kind::Rssi &&
          e.value == -72 && e.address == "4A:00:00:00:00:04");
    CHECK(parse_btctl_line("[CHG] Device 4A:00:00:00:00:04 TxPower: 0x0008 (8)", e) && e.kind == BtEvent::Kind::TxPower && e.value == 8);
    CHECK(parse_btctl_line("[CHG] Device 4A:00:00:00:00:04 ManufacturerData Key: 0x004c", e) &&
          e.kind == BtEvent::Kind::Company && e.value == 0x4c);
    CHECK(parse_btctl_line("[CHG] Device 4A:00:00:00:00:04 RSSI is nil", e) && e.kind == BtEvent::Kind::RssiGone);
    CHECK(parse_btctl_line("[NEW] Device 4A:00:00:00:00:04", e) && e.kind == BtEvent::Kind::NewDevice && e.text.empty());
    CHECK(parse_btctl_line("[DEL] Device 4A:00:00:00:00:04 Phone", e) && e.kind == BtEvent::Kind::DelDevice);
    CHECK(parse_btctl_line("[CHG] Controller B8:27:EB:00:00:01 Discovering: no", e) && e.kind == BtEvent::Kind::Discovering && e.value == 0);
    CHECK(parse_btctl_line("Discovery stopped", e) && e.kind == BtEvent::Kind::Discovering && e.value == 0);
    CHECK(parse_btctl_line("Failed to start discovery: org.bluez.Error.NotReady", e) && e.kind == BtEvent::Kind::Failed &&
          e.text == "org.bluez.Error.NotReady");
    CHECK(parse_btctl_line("No default controller available", e) && e.kind == BtEvent::Kind::NoController);
    CHECK(parse_btctl_line("Waiting to connect to bluetoothd...", e) && e.kind == BtEvent::Kind::WaitingDaemon);
    CHECK(parse_btctl_line("\tUUID: Battery Service           (0000180f-0000-1000-8000-00805f9b34fb)", e) &&
          e.kind == BtEvent::Kind::InfoField && e.key == "UUID");
    CHECK(parse_btctl_line("\tAdvertisingFlags:", e) && e.kind == BtEvent::Kind::InfoField && e.text.empty());
    CHECK(parse_btctl_line("[CHG] Device 4A:00:00:00:00:04 Name: a\x01" "b", e) && e.kind == BtEvent::Kind::Name && e.text == "a\\x01b");
    CHECK(parse_btctl_line("[CHG] Device 4A:00:00:00:00:04 Alias: " + std::string(300, 'x'), e) && e.text.size() == 64);
    const char *bad[] = {"",
                         "\t",
                         "[CHG]",
                         "[CHG] Device 4A:00:00:00:00:04",
                         "[CHG] Device ZZ:00:00:00:00:04 RSSI: -1",
                         "[CHG] Device 4A:00:00:00:00:04 RSSI: 0xffffffb8",
                         "[CHG] Device 4A:00:00:00:00:04 RSSI: (-500)",
                         "[CHG] Device 4A:00:00:00:00:04 RSSI: lots",
                         "[CHG] Device 4A:00:00:00:00:04 TxPower: (999)",
                         "[CHG] Device 4A:00:00:00:00:04 ManufacturerData.Key: 0x1ffff (131071)",
                         "[CHG] Device 4A-00-00-00-00-04 RSSI: -1",
                         "[XYZ] Device 4A:00:00:00:00:04 RSSI: -1",
                         "[CHG] Controller B8:27:EB:00:00:01 Powered: yes",
                         "Device 4A:00:00:00:00:04",
                         "Device 4A:00:00:00:00:04 (",
                         "  10 07 39 1f 40 26 24 37 18                       ..9.@&$7.",
                         "menu scan",
                         "SetDiscoveryFilter success"};
    for (const char *b : bad) CHECK(!parse_btctl_line(b, e));
    CHECK(bt_failure_words("org.bluez.Error.NotReady").find("powered off") != std::string::npos);
    CHECK(bt_failure_words("org.bluez.Error.Failed: Blocked through rfkill").find("rfkill") != std::string::npos);
    CHECK(bt_failure_words("org.bluez.Error.InProgress").find("already") != std::string::npos);
    CHECK(bt_failure_words("org.freedesktop.DBus.Error.AccessDenied").find("may not") != std::string::npos);
    CHECK(bt_failure_words("org.bluez.Error.Strange").find("org.bluez.Error.Strange") != std::string::npos);
}

void test_ble_adapter()
{
    BtAdapter a = parse_btctl_show(fixture("ble", "show-3a-plus.txt"));
    CHECK(a.present && a.address == "B8:27:EB:00:00:01" && a.name == "PI3A" && a.powered && a.power_state == "on" && !a.discovering);
    a = parse_btctl_show(fixture("ble", "show-deck.txt"));
    CHECK(a.present && a.name == "zero7" && a.powered);
    a = parse_btctl_show(fixture("ble", "show-powered-off.txt"));
    CHECK(a.present && !a.powered && a.power_state == "off");
    a = parse_btctl_show("No default controller available\n");
    CHECK(!a.present && a.no_controller);
    a = parse_btctl_show("Waiting to connect to bluetoothd...");
    CHECK(!a.present && a.waiting_daemon);
    a = parse_btctl_show("Controller 00:00:00:00:00:01 not available\n");
    CHECK(!a.present);

    BtSysState ok;
    ok.adapter = true;
    const CommandResult good = shown(fixture("ble", "show-3a-plus.txt"));
    CHECK_EQ(bt_unavailable_words(ok, good, parse_btctl_show(good.output)), std::string());
    BtSysState none;
    CHECK(bt_unavailable_words(none, good, BtAdapter()).find("no Bluetooth adapter") != std::string::npos);
    BtSysState hard = ok;
    hard.hard_blocked = true;
    CHECK(bt_unavailable_words(hard, good, BtAdapter()).find("hardware switch") != std::string::npos);
    BtSysState soft = ok;
    soft.soft_blocked = true;
    CHECK(bt_unavailable_words(soft, good, BtAdapter()).find("rfkill unblock bluetooth") != std::string::npos);
    CommandResult missing = shown("", 127);
    CHECK(bt_unavailable_words(ok, missing, BtAdapter()).find("bluez") != std::string::npos);
    CommandResult waiting = shown("Waiting to connect to bluetoothd...");
    waiting.timed_out = true;
    waiting.exit_code = -1;
    CHECK(bt_unavailable_words(ok, waiting, parse_btctl_show(waiting.output)).find("not running") != std::string::npos);
    const CommandResult nocontroller = shown("No default controller available\n", 1);
    CHECK(bt_unavailable_words(ok, nocontroller, parse_btctl_show(nocontroller.output)).find("sees no") != std::string::npos);
    const CommandResult off = shown(fixture("ble", "show-powered-off.txt"));
    CHECK(bt_unavailable_words(ok, off, parse_btctl_show(off.output)).find("powered off") != std::string::npos);
    BtAdapter blocked = parse_btctl_show(off.output);
    blocked.power_state = "off-blocked";
    CHECK(bt_unavailable_words(ok, off, blocked).find("rfkill") != std::string::npos);
    CHECK(bt_unavailable_words(ok, shown("garbage"), parse_btctl_show("garbage")).find("does not answer") != std::string::npos);

    // /sys, through a fixture folder
    std::string dir = make_sys(true);
    BtSysState s = read_bt_sys(*make_fixture_source(dir));
    CHECK(s.adapter && s.rfkill_found && !s.soft_blocked && !s.hard_blocked); /* the blocked Wi-Fi does not count */
    remove_dir(dir);
    dir = make_sys(true, "1");
    s = read_bt_sys(*make_fixture_source(dir));
    CHECK(s.adapter && s.soft_blocked && !s.hard_blocked);
    remove_dir(dir);
    dir = make_sys(false, "0", "1");
    s = read_bt_sys(*make_fixture_source(dir));
    CHECK(!s.adapter && s.hard_blocked);
    remove_dir(dir);
    s = read_bt_sys(*make_fixture_source("/nonexistent-nt-dir"));
    CHECK(!s.adapter && !s.rfkill_found);

    CHECK_EQ(std::string(company_name(0x004C)), std::string("Apple"));
    CHECK_EQ(std::string(company_name(0x02E5)), std::string("Espressif"));
    CHECK_EQ(std::string(company_name(0x4554)), std::string(""));
    struct D {
        double dbm;
        DistanceClass c;
    } dist[] = {{-40, DistanceClass::Near},    {-60, DistanceClass::Near}, {-60.5, DistanceClass::Medium},
                {-80, DistanceClass::Medium}, {-80.1, DistanceClass::Far}, {-100, DistanceClass::Far}};
    for (const D &d : dist) CHECK(distance_class(d.dbm) == d.c);
    CHECK_EQ(std::string(distance_words(DistanceClass::Medium)), std::string("medium"));
}

BtEvent ev(BtEvent::Kind k, const std::string &addr, int value = 0, const std::string &text = "", const std::string &key = "")
{
    BtEvent e;
    e.kind = k;
    e.address = addr;
    e.value = value;
    e.text = text;
    e.key = key;
    return e;
}

void test_ble_table()
{
    using K = BtEvent::Kind;
    const std::string A = "4A:00:00:00:00:04", B = "0A:00:00:00:00:05", C = "CA:00:00:00:00:07";
    BleTable t(20000, 0.3);
    t.apply(ev(K::NewDevice, A, 0, "4A-00-00-00-00-04"), 0); /* the alias bluez makes from the address: no name */
    CHECK(t.find(A) && t.find(A)->name.empty() && t.live_count() == 0 && t.seen_count() == 0);
    t.apply(ev(K::Rssi, A, -60), 1000);
    t.apply(ev(K::Rssi, A, -70), 2000);
    const BleDevice *a = t.find(A);
    CHECK(a && a->has_rssi && a->rssi_dbm == -70 && a->readings == 2 && a->first_seen_ms == 1000 && a->last_seen_ms == 2000);
    CHECK_NEAR(a->smooth_dbm, -63, 1e-9);
    t.apply(ev(K::Rssi, A, -80), 3000);
    CHECK_NEAR(t.find(A)->smooth_dbm, -68.1, 1e-9);
    CHECK(t.find(A)->distance == DistanceClass::Medium && t.find(A)->rssi_at_ms == 3000);
    // names: an alias fills an empty name, a real name wins, a later alias does not replace it
    t.apply(ev(K::Alias, A, 0, "Kitchen"), 3000);
    CHECK_EQ(t.find(A)->name, std::string("Kitchen"));
    t.apply(ev(K::Name, A, 0, "Speaker"), 3000);
    t.apply(ev(K::Alias, A, 0, "Other"), 3000);
    CHECK_EQ(t.find(A)->name, std::string("Speaker"));
    // the company: a known one by name, another as its number
    t.apply(ev(K::Company, A, 0x004C), 3100);
    CHECK(t.find(A)->company == 0x4C && t.find(A)->vendor == "Apple");
    t.apply(ev(K::Company, A, 0x4554), 3100);
    CHECK_EQ(t.find(A)->vendor, std::string("company 0x4554"));
    // the `info` answer: its fields go to its device until another event comes
    t.apply(ev(K::Rssi, B, -50), 4000);
    t.apply(ev(K::InfoHeader, B, 0, "random"), 4100);
    t.apply(ev(K::InfoField, "", 0, "Meshtastic_0a0a", "Name"), 4100);
    t.apply(ev(K::InfoField, "", 0, "0x0008 (8)", "TxPower"), 4100);
    t.apply(ev(K::InfoField, "", 0, "0x02e5 (741)", "ManufacturerData.Key"), 4100);
    t.apply(ev(K::InfoField, "", 0, "0xffffffa4 (-92)", "RSSI"), 4100); /* a cached reading: not a fresh one */
    const BleDevice *b = t.find(B);
    CHECK(b && b->address_type == "random" && b->name == "Meshtastic_0a0a" && b->has_tx && b->tx_dbm == 8 && b->vendor == "Espressif");
    CHECK(b->rssi_dbm == -50 && b->readings == 1);
    t.apply(ev(K::Rssi, B, -52), 4200);
    t.apply(ev(K::InfoField, "", 0, "Late", "Name"), 4200); /* out of the answer: ignored */
    CHECK_EQ(t.find(B)->name, std::string("Meshtastic_0a0a"));
    t.apply(ev(K::InfoHeader, C, 0, "strange"), 4300);
    CHECK(t.find(C) && t.find(C)->address_type.empty() && t.seen_count() == 2);
    // the live list: strongest first
    std::vector<BleDevice> live = t.live();
    CHECK(live.size() == 2 && live[0].address == B && live[1].address == A);
    CHECK(live[0].distance == DistanceClass::Near);
    // expiry: 20 s without a reading leaves the live list, the seen count stays
    t.expire(23000); /* A: last 3100 -> 19.9 s */
    CHECK(t.live_count() == 2);
    t.expire(23200); /* A: 20.1 s */
    CHECK(t.live_count() == 1 && t.live()[0].address == B && t.seen_count() == 2);
    t.apply(ev(K::Rssi, A, -90), 30000); /* back */
    CHECK(t.live_count() == 2 && t.seen_count() == 2);
    CHECK_NEAR(t.find(A)->smooth_dbm, 0.3 * -90 + 0.7 * -68.1, 1e-9); /* smoothed from where it was */
    CHECK(t.find(A)->distance == DistanceClass::Medium && t.find(A)->rssi_dbm == -90);
    t.apply(ev(K::DelDevice, B), 30000);
    CHECK(t.live_count() == 1);
    // a TX power or a company of a silent device does not keep it alive
    BleTable u(1000, 0.3);
    u.apply(ev(K::Rssi, A, -60), 0);
    u.expire(1500);
    u.apply(ev(K::TxPower, A, 4), 1600);
    CHECK(u.live_count() == 0 && u.find(A)->tx_dbm == 4);
    // the bound: 300 devices heard, 256 kept, all counted
    BleTable big;
    for (int i = 0; i < 300; ++i) {
        char addr[24];
        std::snprintf(addr, sizeof(addr), "CA:00:00:00:%02X:%02X", i / 256, i % 256);
        big.apply(ev(K::Rssi, addr, -40 - (i % 50)), i);
    }
    CHECK(big.size() == kBleMaxDevices && big.seen_count() == 300);
}

struct Scan {
    std::shared_ptr<fake::StreamRunner> stream = std::make_shared<fake::StreamRunner>();
    std::shared_ptr<fake::CommandRunner> cmd = std::make_shared<fake::CommandRunner>();
    std::string sys = make_sys(true);
    std::vector<BleSnapshot> seen;
    BleSnapshot last;
    ~Scan() { remove_dir(sys); }
    void go(BleConfig cfg, const CancelToken &cancel = CancelToken(),
            const std::function<void(const BleSnapshot &)> &also = nullptr)
    {
        last = run_ble_scan(cfg, *make_fixture_source(sys), *cmd, *stream, cancel, [&](const BleSnapshot &s) {
            seen.push_back(s);
            if (also) also(s);
        });
    }
};

void test_ble_scan()
{
    // the recording replayed: 5 s of scan, then scan off and quit on stdin
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        s.stream->scripts = {replay("scan-3a-plus.txt")};
        BleConfig cfg;
        cfg.seconds = 5;
        s.go(cfg);
        const BleSnapshot &r = s.last;
        CHECK(r.state == BleSnapshot::State::Done && r.error.empty());
        CHECK(r.adapter == "PI3A (B8:27:EB:00:00:01)" && !r.other_discovery && !r.warn.empty());
        CHECK(r.stop_note.find("after 5 s") != std::string::npos);
        CHECK_NEAR(r.elapsed_s, 5.0, 0.11);
        CHECK(r.live > 5 && r.seen >= r.live && static_cast<int>(r.devices.size()) == r.live);
        bool sorted = true, typed = false, apple = false, named = false;
        for (std::size_t i = 1; i < r.devices.size(); ++i)
            if (r.devices[i].smooth_dbm > r.devices[i - 1].smooth_dbm) sorted = false;
        for (const BleDevice &d : r.devices) {
            if (d.address == "4A:00:00:00:00:04" && d.address_type == "random") typed = true;
            if (d.vendor == "Apple") apple = true;
            if (d.address == "0A:00:00:00:00:05" && d.name == "Meshtastic_0a0a" && d.address_type == "public") named = true;
        }
        CHECK(sorted && typed && apple && named);
        // what bluetoothctl was told: duplicate data on, LE only, scan on; an info per new device; scan off and quit
        const StreamSpec &spec = s.stream->specs[0];
        CHECK(spec.argv.size() == 1 && spec.argv[0] == "bluetoothctl" && spec.input == StreamSpec::Input::Text);
        CHECK_EQ(spec.text, std::string("menu scan\nduplicate-data on\ntransport le\nback\nscan on\n"));
        CHECK_EQ(spec.farewell, std::string("scan off\nquit\n"));
        const std::string &log = s.stream->stdin_log[0];
        CHECK(log.find("info 0A:00:00:00:00:05\n") != std::string::npos);
        CHECK(ends_with_off(log) && s.stream->input_ended[0] && !s.stream->farewell[0]);
        CHECK(s.cmd->specs.size() == 1 && s.cmd->specs[0].argv.size() == 2 && s.cmd->specs[0].argv[1] == "show");
        // the live snapshots: checking, then scanning with a growing list, twice a second
        CHECK(s.seen.front().state == BleSnapshot::State::Checking);
        int scanning = 0;
        for (const BleSnapshot &x : s.seen)
            if (x.state == BleSnapshot::State::Scanning && x.live > 0) ++scanning;
        CHECK(scanning >= 8 && scanning <= 15);
    }
    // the user stops it: the farewell turns discovery off
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        s.stream->scripts = {replay("scan-3a-plus.txt")};
        CancelToken cancel;
        s.go(BleConfig(), cancel, [&](const BleSnapshot &x) {
            if (x.elapsed_s > 1.0) cancel.cancel();
        });
        CHECK(s.last.state == BleSnapshot::State::Cancelled && s.last.stop_note.find("discovery turned off") != std::string::npos);
        CHECK(s.stream->cancelled[0] && s.stream->farewell[0] && ends_with_off(s.stream->stdin_log[0]));
        CHECK(s.last.live > 0); /* what was found stays on screen */
    }
    // BlueZ refuses (powered off between the check and the scan)
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        fake::Script refuse;
        refuse.steps = {fake::Step{kSec, 0, 0, {"Waiting to connect to bluetoothd...\r\x1b[0;94m[bluetoothctl]> \x1b[0mscan on"}},
                        fake::Step{kSec + 50000, 0, 0, {"Failed to start discovery: org.bluez.Error.NotReady"}}};
        refuse.hang = true;
        s.stream->scripts = {refuse};
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("powered off") != std::string::npos);
        CHECK(s.stream->stopped[0] && s.stream->farewell[0] && ends_with_off(s.stream->stdin_log[0]));
    }
    // the adapter went away
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        fake::Script gone;
        gone.steps = {fake::Step{kSec, 0, 0, {"No default controller available"}}};
        gone.hang = true;
        s.stream->scripts = {gone};
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("sees no") != std::string::npos);
        CHECK(s.stream->farewell[0] && ends_with_off(s.stream->stdin_log[0]));
    }
    // the scan never starts (bluetoothd silent): given up after 8 s, discovery told off anyway
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        fake::Script silent;
        silent.steps = {fake::Step{kSec, 0, 0, {"Waiting to connect to bluetoothd..."}}};
        silent.hang = true;
        s.stream->scripts = {silent};
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("does not answer") != std::string::npos);
        CHECK(s.stream->stopped[0] && ends_with_off(s.stream->stdin_log[0]));
    }
    // the runner's own timeout: the farewell is written before it ends the group
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        fake::Script slow = replay("scan-killed-3a-plus.txt"); /* its 300 lines end 3.4 s into the scan */
        slow.hang_limit_us = 1 * kSec;                         /* the runner gives up before the 5 s are up */
        s.stream->scripts = {slow};
        BleConfig cfg;
        cfg.seconds = 5;
        s.go(cfg);
        CHECK(s.last.state == BleSnapshot::State::Done && s.last.stop_note.find("did not stop in time") != std::string::npos);
        CHECK(s.stream->farewell[0] && ends_with_off(s.stream->stdin_log[0]));
    }
    // no bluetoothctl for the scan itself
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        fake::Script none;
        none.started = false;
        s.stream->scripts = {none};
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("bluez") != std::string::npos);
    }
    // the adapter cannot scan: said before anything runs
    {
        Scan s;
        remove_dir(s.sys);
        s.sys = make_sys(false);
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("no Bluetooth adapter") != std::string::npos);
        CHECK(s.cmd->specs.empty() && s.stream->specs.empty());
    }
    {
        Scan s;
        remove_dir(s.sys);
        s.sys = make_sys(true, "1");
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("rfkill") != std::string::npos);
        CHECK(s.cmd->specs.empty() && s.stream->specs.empty());
    }
    {
        Scan s;
        s.cmd->results = {shown(fixture("ble", "show-powered-off.txt"))};
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("powered off") != std::string::npos);
        CHECK(s.stream->specs.empty());
    }
    {
        Scan s;
        s.cmd->results = {shown("", 127)};
        s.go(BleConfig());
        CHECK(s.last.state == BleSnapshot::State::Failed && s.last.error.find("bluez package") != std::string::npos);
    }
    // another program scanning already: noted, the scan runs
    {
        Scan s;
        std::string busy = fixture("ble", "show-3a-plus.txt");
        const std::size_t at = busy.find("Discovering: no");
        busy.replace(at, 15, "Discovering: yes");
        s.cmd->results = {shown(busy)};
        s.stream->scripts = {replay("scan-killed-3a-plus.txt")};
        BleConfig cfg;
        cfg.seconds = 5;
        s.go(cfg);
        CHECK(s.last.state == BleSnapshot::State::Done && s.last.other_discovery && s.last.seen > 0);
    }
    // the session destroyed in the middle of a scan: the farewell still goes
    {
        auto stream = std::make_shared<fake::StreamRunner>();
        auto cmd = std::make_shared<fake::CommandRunner>();
        cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        fake::Script slow = replay("scan-3a-plus.txt");
        slow.real_sleep_us = 2000;
        stream->scripts = {slow};
        const std::string sys = make_sys(true);
        {
            BleSession session(std::shared_ptr<const Source>(make_fixture_source(sys)), cmd, stream);
            CHECK(session.start(BleConfig()));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(session.running() && session.latest()->state == BleSnapshot::State::Scanning);
        }
        CHECK(stream->cancelled.size() == 1 && stream->cancelled[0] && stream->farewell[0]);
        CHECK(ends_with_off(stream->stdin_log[0]));
        remove_dir(sys);
    }
}

/* a stand-in for bluetoothctl: logs what it is told, prints an RSSI line every 50 ms while scanning */
const char *kFakeBtctl = "log=\"$1\"\n"
                         "echo 'Waiting to connect to bluetoothd...'\n"
                         "pid=''\n"
                         "while IFS= read -r cmd; do\n"
                         "  echo \"$cmd\" >> \"$log\"\n"
                         "  case \"$cmd\" in\n"
                         "  'scan on') echo 'Discovery started'\n"
                         "    echo '[CHG] Controller B8:27:EB:00:00:01 Discovering: yes'\n"
                         "    ( while :; do echo '[CHG] Device 4A:00:00:00:00:04 RSSI: 0xffffffb8 (-72)'; sleep 0.05; done ) &\n"
                         "    pid=$! ;;\n"
                         "  'scan off') [ -n \"$pid\" ] && kill $pid; pid=''; echo 'Discovery stopped' ;;\n"
                         "  quit) exit 0 ;;\n"
                         "  esac\n"
                         "done\n"
                         "[ -n \"$pid\" ] && kill $pid\n"
                         "echo EOF >> \"$log\"\n";

void test_ble_real_runner()
{
    const std::string dir = test_tmpdir("fakebtctl");
    write_file(dir + "/btctl.sh", kFakeBtctl);
    const std::string sys = make_sys(true);
    const auto session = [&](const std::string &log) {
        auto cmd = std::make_shared<fake::CommandRunner>();
        cmd->results = {shown(fixture("ble", "show-3a-plus.txt"))};
        return std::make_unique<BleSession>(std::shared_ptr<const Source>(make_fixture_source(sys)), cmd,
                                            std::make_shared<DirectStreamRunner>());
    };
    BleConfig cfg;
    cfg.seconds = 5;
    const std::string want = "menu scan\nduplicate-data on\ntransport le\nback\nscan on\nscan off\nquit\n";
    // the time is up: scan off, quit
    {
        cfg.program = {"/bin/sh", dir + "/btctl.sh", dir + "/log1"};
        auto s = session(dir + "/log1");
        CHECK(s->start(cfg));
        CHECK(s->wait(12000));
        const auto snap = s->latest();
        CHECK(snap->state == BleSnapshot::State::Done && snap->live == 1 && snap->devices[0].rssi_dbm == -72);
        CHECK(snap->devices[0].readings > 50);
        CHECK_EQ(slurp(dir + "/log1"), want);
    }
    // Stop: the farewell, quickly
    {
        cfg.program = {"/bin/sh", dir + "/btctl.sh", dir + "/log2"};
        auto s = session(dir + "/log2");
        CHECK(s->start(cfg));
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const std::int64_t t0 = now_ms();
        s->stop();
        CHECK(s->wait(3000) && now_ms() - t0 < 1500);
        CHECK(s->latest()->state == BleSnapshot::State::Cancelled);
        CHECK_EQ(slurp(dir + "/log2"), want);
    }
    // the session destroyed while scanning
    {
        cfg.program = {"/bin/sh", dir + "/btctl.sh", dir + "/log3"};
        {
            auto s = session(dir + "/log3");
            CHECK(s->start(cfg));
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        CHECK_EQ(slurp(dir + "/log3"), want);
    }
    remove_dir(sys);
    remove_dir(dir);
}

} // namespace

void test_ble()
{
    test_ble_lines();
    test_ble_adapter();
    test_ble_table();
    test_ble_scan();
    test_ble_real_runner();
}
