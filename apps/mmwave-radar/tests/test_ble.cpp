/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the Bluetooth LE transport, all on a scripted backend (fake_ble.hpp), no radio. The owner's real
 * HLK-LD2451 notifications (tests/data/ble-ld2451-notify-75s.csv) through BleSys, the detector, the link and the
 * tracker, against the UART stream recorded at the same time (ld2451-combo-uart-75s.bin), the malformed frame included;
 * one frame per notification and frames split over notifications; the name filter, the discovery windows, the
 * remembered radar, the connect timeout, the retry waits, a lost link and the reconnection, the clean disconnection,
 * the adapter off / absent / BlueZ missing messages, the Connection setting and the remembered file, the Auto rule, the
 * diagnostic line and the scripted demo backend.
 */
#include "check.hpp"

#include "ble_sys.hpp"
#include "demo_ble.hpp"
#include "detect.hpp"
#include "diag.hpp"
#include "fake_ble.hpp"
#include "fake_sys.hpp"
#include "ld2451.hpp"
#include "link.hpp"
#include "mux_sys.hpp"
#include "settings.hpp"

#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mmw;

namespace {

using Bytes = std::vector<std::uint8_t>;

const char *const kAddr = "ED:2F:90:B4:49:F0";
const char *const kName = "HLK-LD2451_49F0";

Bytes hex_bytes(const std::string &h)
{
    Bytes b;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
    return b;
}

struct Note {
    Ms t;
    Bytes data;
};

/* the fixture: "t_s,hex" per notification, t in seconds from the subscription request */
std::vector<Note> notes()
{
    std::vector<Note> v;
    std::istringstream in(test_read_file(test_data_dir() + "ble-ld2451-notify-75s.csv"));
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t c = line.find(',');
        if (c == std::string::npos) continue;
        v.push_back({static_cast<Ms>(std::atof(line.c_str()) * 1000.0 + 0.5), hex_bytes(line.substr(c + 1))});
    }
    return v;
}

Bytes uart_bytes()
{
    const std::string s = test_read_file(test_data_dir() + "ld2451-combo-uart-75s.bin");
    return Bytes(s.begin(), s.end());
}

std::vector<Frame> decode(const Bytes &d, DecoderStats *st = nullptr)
{
    Ld2451Decoder dec;
    std::vector<Frame> f;
    dec.feed(d.data(), d.size(), &f);
    if (st) *st = dec.stats();
    return f;
}

bool same_frames(const std::vector<Frame> &a, const std::vector<Frame> &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int k = 0; k < 30; ++k)
            if (a[i].raw[k] != b[i].raw[k]) return false;
        for (int s = 0; s < 3; ++s) {
            const Target &x = a[i].target[s], &y = b[i].target[s];
            if (x.present != y.present || x.x_mm != y.x_mm || x.y_mm != y.y_mm || x.speed_cm_s != y.speed_cm_s ||
                x.distance_mm != y.distance_mm || x.angle_deg != y.angle_deg || x.snr != y.snr)
                return false;
        }
    }
    return true;
}

/* A Sys in front of another that records what the readers do: the bytes read per handle, configure / flush calls. */
class TapSys : public Sys {
public:
    explicit TapSys(Sys &s) : s_(s) {}
    std::map<int, Bytes> read_bytes;
    int configures = 0, flushes = 0;
    Bytes all_ble() const
    {
        Bytes b;
        for (const auto &kv : read_bytes)
            if (kv.first >= kBleHandleBase) b.insert(b.end(), kv.second.begin(), kv.second.end());
        return b;
    }
    std::vector<PortMeta> list_ports() override { return s_.list_ports(); }
    bool held_by_other(const std::string &p) override { return s_.held_by_other(p); }
    int open(const std::string &p, OpenErr &e) override { return s_.open(p, e); }
    bool configure(int h, int b) override
    {
        if (h >= kBleHandleBase) ++configures;
        return s_.configure(h, b);
    }
    void flush_input(int h) override
    {
        if (h >= kBleHandleBase) ++flushes;
        s_.flush_input(h);
    }
    long read(int h, std::uint8_t *buf, std::size_t n) override
    {
        const long r = s_.read(h, buf, n);
        if (r > 0) read_bytes[h].insert(read_bytes[h].end(), buf, buf + r);
        return r;
    }
    void restore(int h) override { s_.restore(h); }
    void close(int h) override { s_.close(h); }
    std::string describe(int h) override { return s_.describe(h); }
    std::uint64_t ports_generation() override { return s_.ports_generation(); }

private:
    Sys &s_;
};

/* The app's arrangement: serial ports (fake), BleSys over a FakeBle, MuxSys, the Link; one tick = 30 ms. */
struct Rig {
    FakeSys serial;
    FakeBle *fake = new FakeBle;
    BleSys ble;
    MuxSys mux;
    TapSys tap;
    Link link;
    Ms now = 0;
    bool active = true;
    explicit Rig(Connection c, const BleTimings &t = BleTimings())
        : ble(std::unique_ptr<BleBackend>(fake), t), mux(serial, &ble), tap(mux), link(tap)
    {
        mux.set_mode(c);
    }
    void start(const Remembered &r = Remembered())
    {
        if (!r.ble_empty()) ble.set_remembered(r.ble_address, r.ble_name);
        link.start(now, r);
    }
    void tick()
    {
        now += 30;
        serial.now_ms = now;
        link.poll(now);
        mux.update(link, active, now);
    }
    void run(Ms ms)
    {
        const Ms end = now + ms;
        while (now < end) tick();
    }
    /* runs until cond() or the time is out; true when cond() came */
    template <typename F> bool until(F cond, Ms max_ms)
    {
        const Ms end = now + max_ms;
        while (now < end) {
            tick();
            if (cond()) return true;
        }
        return false;
    }
};

Remembered remembered_ble()
{
    Remembered r;
    r.ble_address = kAddr;
    r.ble_name = kName;
    return r;
}

/* ------------------------------------------------------------------------------------------ names, settings, files */

void test_names_and_settings()
{
    CHECK(hlk_candidate("HLK-LD2451_49F0"));
    CHECK(hlk_candidate("hlk-ld2450_c3a1"));
    CHECK(hlk_candidate("HLK-LD"));
    CHECK(!hlk_candidate("HLK-2410"));
    CHECK(!hlk_candidate("Meshtastic_1eec"));
    CHECK(!hlk_candidate(""));
    CHECK(!hlk_candidate(" HLK-LD2451"));
    CHECK_EQ(address_tail(kAddr), "49:F0");
    CHECK_EQ(address_tail("ed:2f:90:b4:49:f0"), "49:F0");
    CHECK_EQ(address_tail("nonsense"), "");
    CHECK(valid_ble_address(kAddr));
    CHECK(!valid_ble_address("ED:2F:90:B4:49"));
    CHECK(!valid_ble_address("ED-2F-90-B4-49-F0"));
    CHECK(!valid_ble_address("GG:2F:90:B4:49:F0"));

    /* the Connection setting: default USB, written only when not USB, an older file reads as USB */
    Settings d;
    CHECK(d.connection == Connection::Usb);
    CHECK_EQ(serialize_settings(d), "units=metres\norientation=normal\nrange=auto\ntrails=on\nhide_held=off\n");
    CHECK(parse_settings("units=metres\norientation=mirror\nrange=4\ntrails=off\nhide_held=on\n").connection == Connection::Usb);
    CHECK(parse_settings("units=mm\nsensor=ld2451\n").connection == Connection::Usb);
    for (Connection c : {Connection::Usb, Connection::Bluetooth, Connection::Auto}) {
        Settings s;
        s.connection = c;
        s.sensor = "ld2451";
        const Settings r = parse_settings(serialize_settings(s));
        CHECK(r.connection == c);
        CHECK_EQ(r.sensor, "ld2451");
        CHECK_EQ(serialize_settings(r), serialize_settings(s));
    }
    Settings b;
    b.connection = Connection::Bluetooth;
    CHECK(serialize_settings(b).find("connection=bluetooth\n") != std::string::npos);
    b.connection = Connection::Auto;
    CHECK(serialize_settings(b).find("connection=auto\n") != std::string::npos);
    CHECK(parse_settings("connection=bogus\n").connection == Connection::Usb);
    CHECK(parse_settings("connection=bluetooth\r\n").connection == Connection::Bluetooth);
    CHECK_EQ(connection_label(Connection::Usb), "USB");
    CHECK_EQ(connection_label(Connection::Bluetooth), "Bluetooth");
    CHECK_EQ(connection_label(Connection::Auto), "Auto");
    CHECK(next_connection(Connection::Auto) == Connection::Usb);
    CHECK(next_connection(Connection::Usb) == Connection::Bluetooth);
    CHECK(next_connection(Connection::Bluetooth) == Connection::Auto);

    /* remembered.conf: the serial part as before, the Bluetooth radar beside it */
    Remembered r;
    r.port_id = "usb-1a86:7523";
    r.baud = 115200;
    r.ble_address = kAddr;
    r.ble_name = kName;
    CHECK_EQ(r.serialize(), std::string("port=usb-1a86:7523\nbaud=115200\nble=") + kAddr + "\nble_name=" + kName + "\n");
    const Remembered p = Remembered::parse(r.serialize());
    CHECK_EQ(p.port_id, r.port_id);
    CHECK_EQ(p.baud, 115200);
    CHECK_EQ(p.ble_address, kAddr);
    CHECK_EQ(p.ble_name, kName);
    const Remembered only = Remembered::parse(std::string("ble=") + kAddr + "\nble_name=" + kName + "\n");
    CHECK(only.empty());
    CHECK(!only.ble_empty());
    CHECK_EQ(only.serialize(), std::string("ble=") + kAddr + "\nble_name=" + kName + "\n");
    const Remembered old = Remembered::parse("port=usb-1a86:7523\nbaud=9600\n"); /* an older file */
    CHECK(!old.empty());
    CHECK(old.ble_empty());
    CHECK_EQ(old.serialize(), "port=usb-1a86:7523\nbaud=9600\n");
    CHECK(Remembered::parse("ble=nonsense\nble_name=x\n").ble_empty());
    const Remembered half = Remembered::parse(std::string("port=x\nbaud=5\nble=") + kAddr + "\n");
    CHECK(half.empty());
    CHECK_EQ(half.ble_address, kAddr);
    CHECK(Remembered().serialize().empty());
}

void test_backoff()
{
    CHECK_EQ(ble_backoff_ms(1), 2000);
    CHECK_EQ(ble_backoff_ms(2), 4000);
    CHECK_EQ(ble_backoff_ms(3), 8000);
    CHECK_EQ(ble_backoff_ms(4), 15000);
    CHECK_EQ(ble_backoff_ms(5), 30000);
    CHECK_EQ(ble_backoff_ms(6), 30000);
    CHECK_EQ(ble_backoff_ms(40), 30000);
    CHECK_EQ(ble_backoff_ms(0), 2000);
}

/* ------------------------------------------------------------------------------------------ discovery */

void test_discovery()
{
    Rig r(Connection::Bluetooth);
    r.fake->add("34:B7:DA:62:1E:ED", "Meshtastic_1eec", -50);
    r.fake->add(kAddr, kName, -58);
    r.fake->add("79:1E:3F:23:EC:7D", "", -40);
    r.fake->add("C4:3A:1C:7E:A1:C3", "hlk-ld2450_a1c3", -71);
    r.fake->add("EC:EB:45:39:28:2D", "MeshCore-OSR T1000", -45);
    r.start();
    r.run(90);
    CHECK(r.ble.enabled());
    CHECK_EQ(r.fake->starts, 1);
    CHECK_EQ(r.fake->scans_on, 1); /* nothing remembered: a window for the list at once */
    CHECK(r.ble.state() == BleState::Scanning);
    CHECK_EQ(r.ble.title(), "Searching for radars over Bluetooth...");
    r.run(300);
    std::vector<BleCandidate> c = r.ble.candidates();
    CHECK_EQ(c.size(), 2u); /* HLK-LD names only, any case */
    if (c.size() == 2) {
        CHECK_EQ(c[0].address, kAddr); /* the strongest first */
        CHECK_EQ(c[0].rssi, -58);
        CHECK_EQ(c[1].name, "hlk-ld2450_a1c3");
    }
    CHECK_EQ(r.fake->connects, 0); /* nothing is connected without the user's choice */
    /* the window lasts 10 s, then 20 s without discovery */
    const Ms t0 = r.now;
    CHECK(r.until([&] { return !r.fake->scanning; }, 11000));
    CHECK(r.now - t0 >= 9500 && r.now - t0 <= 10100);
    CHECK_EQ(r.fake->scans_off, 1);
    CHECK(r.ble.state() == BleState::Idle);
    CHECK_EQ(r.ble.title(), "Choose a radar");
    CHECK_EQ(r.ble.candidates().size(), 2u); /* the list stays between windows, with the last RSSI */
    CHECK(r.ble.candidates()[0].has_rssi);
    r.run(19000);
    CHECK_EQ(r.fake->scans_on, 1);
    r.run(1200);
    CHECK_EQ(r.fake->scans_on, 2);
    /* hidden page: the window stops, none starts; shown again: one at once */
    r.active = false;
    r.run(60);
    CHECK(!r.fake->scanning);
    CHECK_EQ(r.fake->scans_off, 2);
    r.run(40000);
    CHECK_EQ(r.fake->scans_on, 2);
    r.active = true;
    r.run(60);
    CHECK_EQ(r.fake->scans_on, 3);
    CHECK_EQ(r.fake->count("connect " + std::string(kAddr)), 0);
    /* the user chooses: discovery stops before the connection, the choice is reported once */
    r.ble.choose(kAddr, r.now);
    CHECK(!r.fake->scanning);
    CHECK_EQ(r.fake->connects, 1);
    std::size_t off = 0, con = 0;
    for (std::size_t i = 0; i < r.fake->calls.size(); ++i) {
        if (r.fake->calls[i] == "scan off") off = i;
        if (r.fake->calls[i] == "connect " + std::string(kAddr)) con = i;
    }
    CHECK(off < con);
    std::string a, n;
    CHECK(r.ble.take_chosen(a, n));
    CHECK_EQ(a, kAddr);
    CHECK_EQ(n, kName);
    CHECK(!r.ble.take_chosen(a, n));
    CHECK(r.until([&] { return r.ble.state() == BleState::Connected; }, 1000));
    CHECK_EQ(r.ble.title(), std::string("Connected to ") + kName);
    const std::vector<PortMeta> ports = r.mux.list_ports();
    CHECK_EQ(ports.size(), 1u);
    if (!ports.empty()) {
        CHECK(ports[0].transport == Transport::Ble);
        CHECK_EQ(ports[0].path, std::string("ble:") + kAddr);
        CHECK_EQ(port_title(ports[0]), std::string(kName) + " (Bluetooth)");
        CHECK_EQ(port_id(ports[0]), std::string("ble:") + kAddr);
    }
    CHECK_EQ(r.fake->subscribes, 1);
}

void test_remembered_flow()
{
    /* known to BlueZ: connected without any discovery */
    {
        Rig r(Connection::Bluetooth);
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        CHECK(r.until([&] { return r.ble.state() == BleState::Connected; }, 2000));
        CHECK_EQ(r.fake->scans_on, 0);
        CHECK_EQ(r.fake->connects, 1);
        CHECK_EQ(r.fake->count(std::string("subscribe ") + kAddr), 1);
    }
    /* not known: a window looks for it, it is connected as soon as it is seen */
    {
        Rig r(Connection::Bluetooth);
        r.fake->add("C4:3A:1C:7E:A1:C3", "HLK-LD2450_A1C3", -50);
        r.fake->add(kAddr, kName, -60, false);
        r.start(remembered_ble());
        r.run(30);
        CHECK_EQ(r.fake->scans_on, 1);
        CHECK_EQ(r.ble.title(), std::string("Looking for ") + kName);
        CHECK(r.until([&] { return r.ble.state() == BleState::Connected; }, 2000));
        CHECK_EQ(r.fake->scans_off, 1);
        CHECK_EQ(r.fake->connects, 1);
        CHECK_EQ(r.fake->count(std::string("connect ") + kAddr), 1);
    }
    /* not in range: the window ends, the next one comes 20 s later, nothing else is tried */
    {
        Rig r(Connection::Bluetooth);
        r.fake->add(kAddr, kName, -60, false).in_range = false;
        r.start(remembered_ble());
        r.run(10500);
        CHECK(r.ble.state() == BleState::Idle);
        CHECK_EQ(r.fake->scans_on, 1);
        CHECK_EQ(r.fake->scans_off, 1);
        CHECK_EQ(r.fake->connects, 0);
        r.run(20000);
        CHECK_EQ(r.fake->scans_on, 2);
    }
}

/* ------------------------------------------------------------------------------------------ connection lifecycle */

void test_connect_timeout_and_backoff()
{
    Rig r(Connection::Bluetooth);
    r.fake->add(kAddr, kName, -60, true);
    r.fake->auto_connect = false; /* BlueZ never answers */
    r.start(remembered_ble());
    std::vector<Ms> connect_times, disconnect_times;
    int seen_c = 0, seen_d = 0;
    for (int i = 0; i < 6000 && connect_times.size() < 7; ++i) {
        r.tick();
        if (r.fake->connects > seen_c) connect_times.push_back(r.now), seen_c = r.fake->connects;
        if (r.fake->disconnects > seen_d) disconnect_times.push_back(r.now), seen_d = r.fake->disconnects;
    }
    CHECK_EQ(connect_times.size(), 7u);
    CHECK(disconnect_times.size() >= 6u);
    /* each try: 10 s, then the cancelling disconnection, then the wait: 2, 4, 8, 15, 30, 30 s */
    const Ms waits[] = {2000, 4000, 8000, 15000, 30000, 30000};
    for (std::size_t i = 0; i + 1 < connect_times.size() && i < 6; ++i) {
        const Ms timeout = disconnect_times[i] - connect_times[i];
        const Ms wait = connect_times[i + 1] - disconnect_times[i];
        CHECK(timeout >= 9990 && timeout <= 10060);
        CHECK(wait >= waits[i] - 30 && wait <= waits[i] + 60);
    }
    CHECK(r.ble.attempt() >= 6);
    /* a hidden page: no retry; shown: the wait goes on */
    Rig h(Connection::Bluetooth);
    h.fake->add(kAddr, kName, -60, true);
    h.fake->connect_error = "org.bluez.Error.Failed: Software caused connection abort";
    h.start(remembered_ble());
    h.run(200);
    CHECK(h.ble.state() == BleState::Waiting);
    CHECK_EQ(h.ble.title(), std::string("Could not connect to ") + kName);
    CHECK(h.ble.detail(h.now).find("Software caused connection abort") != std::string::npos);
    h.active = false;
    h.run(60000);
    CHECK_EQ(h.fake->connects, 1);
    CHECK_EQ(h.ble.detail(h.now), "Paused while the Radar page is hidden.");
    h.active = true;
    h.run(90);
    CHECK_EQ(h.fake->connects, 2);
    /* Rescan while waiting: the next try now */
    h.run(200);
    const int before = h.fake->connects;
    h.ble.rescan(h.now);
    h.run(60);
    CHECK_EQ(h.fake->connects, before + 1);
}

void test_subscribe_failure()
{
    Rig r(Connection::Bluetooth);
    r.fake->add(kAddr, kName, -60, true);
    r.fake->subscribe_ok = false;
    r.start(remembered_ble());
    r.run(300);
    CHECK(r.ble.state() == BleState::Waiting);
    CHECK(r.ble.last_error().find("0xfff1") != std::string::npos);
    CHECK_EQ(r.fake->count(std::string("disconnect ") + kAddr), 1); /* a half-open link is not kept */
    CHECK(r.mux.list_ports().empty());
}

/* LD2451 frames for the scripted radar */
Bytes ld_frame(int angle, int dist, int dir, int speed)
{
    Bytes b = {0xF4, 0xF3, 0xF2, 0xF1, 7, 0, 1, static_cast<std::uint8_t>(dir), static_cast<std::uint8_t>(angle + 0x80),
               static_cast<std::uint8_t>(dist), static_cast<std::uint8_t>(dir), static_cast<std::uint8_t>(speed), 200,
               0xF8, 0xF7, 0xF6, 0xF5};
    return b;
}

/* runs the rig, the radar sending a target frame every 150 ms while `sending` */
void run_radar(Rig &r, Ms ms, bool sending = true)
{
    const Ms end = r.now + ms;
    Ms next = r.now;
    int k = 0;
    while (r.now < end) {
        if (sending && r.now >= next) {
            r.fake->notify(ld_frame(5 + (k % 7), 3 + (k / 10) % 4, 1, 4));
            ++k;
            next += 150;
        }
        r.tick();
    }
}

void test_loss_and_reconnect()
{
    Rig r(Connection::Bluetooth);
    r.fake->add(kAddr, kName, -60, true);
    r.start(remembered_ble());
    run_radar(r, 3000);
    CHECK(r.link.state() == LinkState::Found);
    CHECK(r.link.port().transport == Transport::Ble);
    CHECK_EQ(r.link.baud(), 0);
    CHECK(r.link.driver() == find_driver("ld2451"));
    CHECK_EQ(r.tap.configures, 0); /* no speed is ever set on a Bluetooth radar */
    CHECK_EQ(r.tap.flushes, 0);
    CHECK(r.link.tracker().slot(0).active);
    CHECK(!r.link.take_remembered_changed()); /* a Bluetooth radar is not remembered as the serial port */
    CHECK(r.link.remembered().empty());
    /* the radio link drops: the reader sees the port gone, the link is Lost, the retry comes 2 s later (data flowed) */
    r.fake->drop();
    run_radar(r, 90, false);
    CHECK(r.link.state() == LinkState::Lost);
    CHECK(r.ble.state() == BleState::Waiting);
    CHECK_EQ(r.ble.title(), "Signal lost, reconnecting");
    const int c0 = r.fake->connects;
    run_radar(r, 1800, false);
    CHECK_EQ(r.fake->connects, c0);
    run_radar(r, 400, false);
    CHECK_EQ(r.fake->connects, c0 + 1);
    run_radar(r, 2500);
    CHECK(r.link.state() == LinkState::Found);
    CHECK(r.ble.state() == BleState::Connected);
    /* the radar goes quiet (link up, no frame): the link's Lost after the LD2451's 5 s, then the disconnection */
    const int d0 = r.fake->disconnects;
    run_radar(r, 4800, false);
    CHECK(r.link.state() == LinkState::Found);
    CHECK_EQ(r.fake->disconnects, d0);
    run_radar(r, 400, false);
    CHECK(r.link.state() != LinkState::Found);
    CHECK_EQ(r.fake->disconnects, d0 + 1);
    CHECK_EQ(r.fake->count(std::string("disconnect ") + kAddr), d0 + 1);
    CHECK(r.ble.state() == BleState::Waiting);
    CHECK(r.mux.list_ports().empty());
}

void test_clean_disconnect()
{
    /* set_enabled(false) (Connection back to USB): unsubscribe, disconnect, BlueZ closed */
    {
        Rig r(Connection::Bluetooth);
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        run_radar(r, 2500);
        CHECK(r.link.state() == LinkState::Found);
        r.mux.set_mode(Connection::Usb);
        run_radar(r, 90, false);
        CHECK(!r.ble.enabled());
        CHECK_EQ(r.fake->unsubscribes, 1);
        CHECK_EQ(r.fake->count(std::string("disconnect ") + kAddr), 1);
        CHECK_EQ(r.fake->stops, 1);
        CHECK(r.fake->connected.empty());
        CHECK(r.link.state() != LinkState::Found);
    }
    /* the app ends while connected: the link lets go, the destructor disconnects and closes */
    {
        FakeBle *fake = new FakeBle;
        fake->add(kAddr, kName, -60, true);
        {
            FakeSys serial;
            BleSys ble{std::unique_ptr<BleBackend>(fake)};
            MuxSys mux(serial, &ble);
            mux.set_mode(Connection::Bluetooth);
            Link link(mux);
            ble.set_remembered(kAddr, kName);
            Ms now = 0;
            link.start(now, remembered_ble());
            for (int i = 0; i < 100; ++i) {
                now += 30;
                if (i % 5 == 0) fake->notify(ld_frame(3, 4, 0, 3));
                link.poll(now);
                mux.update(link, true, now);
            }
            CHECK(link.state() == LinkState::Found);
            link.close();
            CHECK_EQ(fake->count(std::string("disconnect ") + kAddr), 1);
            ble.shutdown();
            CHECK_EQ(fake->stops, 1);
            CHECK(fake->connected.empty());
            CHECK(!fake->subscribed);
        }
    }
    {
        /* the destructor's order: unsubscribe, disconnect, then stop (seen through a backend that outlives BleSys) */
        struct Keep : FakeBle {
            std::vector<std::string> *out = nullptr;
            ~Keep() override
            {
                if (out) *out = calls;
            }
        };
        std::vector<std::string> log;
        Keep *k = new Keep;
        k->out = &log;
        k->add(kAddr, kName, -60, true);
        {
            BleSys ble{std::unique_ptr<BleBackend>(k)};
            ble.set_remembered(kAddr, kName);
            ble.set_active(true, 0);
            ble.set_enabled(true, 0);
            for (Ms t = 30; t < 600; t += 30) ble.poll(t);
            CHECK(ble.state() == BleState::Connected);
        }
        const std::string tail = log.size() >= 3 ? log[log.size() - 3] + "|" + log[log.size() - 2] + "|" + log.back() : "";
        CHECK_EQ(tail, std::string("unsubscribe|disconnect ") + kAddr + "|stop");
    }
}

void test_unavailable()
{
    /* BlueZ missing (no libsystemd / bus): a message, nothing else */
    {
        Rig r(Connection::Bluetooth);
        r.fake->start_ok = false;
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        r.run(3000);
        CHECK(r.ble.state() == BleState::Unavailable);
        CHECK(r.ble.availability() == BleAvail::NoBluez);
        CHECK_EQ(r.ble.title(), "BlueZ not available");
        CHECK_EQ(r.ble.last_error(), "libsystemd.so.0 is missing (test)");
        CHECK_EQ(r.fake->scans_on + r.fake->connects, 0);
        CHECK(r.mux.list_ports().empty());
    }
    /* bluetoothd not running, no adapter, adapter off */
    struct Case {
        BleAvail avail;
        const char *title;
    };
    for (const Case &c : {Case{BleAvail::NoBluez, "BlueZ not available"}, Case{BleAvail::NoAdapter, "No Bluetooth adapter"},
                          Case{BleAvail::Off, "Bluetooth is off"}}) {
        Rig r(Connection::Bluetooth);
        r.fake->avail = c.avail;
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        r.run(3000);
        CHECK(r.ble.state() == BleState::Unavailable);
        CHECK_EQ(r.ble.title(), c.title);
        CHECK_EQ(r.fake->scans_on + r.fake->connects + r.fake->subscribes, 0);
    }
    /* the adapter switched on later (by the user, elsewhere): the remembered radar is connected */
    {
        Rig r(Connection::Bluetooth);
        r.fake->avail = BleAvail::Off;
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        r.run(500);
        CHECK_EQ(r.ble.title(), "Bluetooth is off");
        r.fake->power(true);
        CHECK(r.until([&] { return r.ble.state() == BleState::Connected; }, 2000));
        /* and switched off while connected: lost, then the message */
        run_radar(r, 2500);
        CHECK(r.link.state() == LinkState::Found);
        r.fake->power(false);
        run_radar(r, 200, false);
        CHECK(r.ble.state() == BleState::Unavailable);
        CHECK_EQ(r.ble.title(), "Bluetooth is off");
        CHECK(r.link.state() != LinkState::Found);
    }
    /* BlueZ never answers */
    {
        Rig r(Connection::Bluetooth);
        r.fake->auto_ready = false;
        r.start(remembered_ble());
        r.run(6000);
        CHECK(r.ble.state() == BleState::Unavailable);
        CHECK_EQ(r.ble.title(), "BlueZ not available");
    }
    /* USB mode: BlueZ is never started at all */
    {
        Rig r(Connection::Usb);
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        r.run(5000);
        CHECK_EQ(r.fake->starts, 0);
        CHECK(r.fake->calls.empty());
    }
}

/* ------------------------------------------------------------------------------------------ Auto */

std::string ld2451_stream()
{
    std::string s;
    for (int k = 0; k < 40; ++k) {
        const Bytes f = ld_frame(-4 + k % 9, 2 + k % 5, k % 2, 3);
        s.append(f.begin(), f.end());
    }
    return s;
}

void test_auto()
{
    /* a USB radar: Bluetooth is never started */
    {
        Rig r(Connection::Auto);
        FakePort &p = r.serial.add("ttyUSB0");
        p.stream[115200] = ld2451_stream();
        p.rate_bps = 120;
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        r.run(20000);
        CHECK(r.link.state() == LinkState::Found);
        CHECK(r.link.port().transport == Transport::Serial);
        CHECK_EQ(r.fake->starts, 0);
        CHECK(!r.mux.fallback());
    }
    /* no USB radar: after the scan, the remembered Bluetooth radar */
    {
        Rig r(Connection::Auto);
        r.serial.add("ttyACM0", 0x2e8a, 0x000a); /* a silent board */
        r.serial.ports[0].wrong_speed.clear();
        r.fake->add(kAddr, kName, -60, true);
        r.start(remembered_ble());
        r.run(300);
        CHECK(r.link.scanning());
        CHECK_EQ(r.fake->starts, 0); /* not while the USB scan runs */
        CHECK(r.until([&] { return r.fake->starts > 0; }, 20000));
        CHECK(!r.link.scanning());
        run_radar(r, 4000);
        CHECK(r.link.state() == LinkState::Found);
        CHECK(r.link.port().transport == Transport::Ble);
        CHECK(r.mux.fallback());
        /* the Bluetooth radar is lost; meanwhile a USB radar was plugged in: the next scan finds it, Bluetooth stops */
        FakePort &p = r.serial.add("ttyUSB0");
        p.stream[115200] = ld2451_stream();
        p.rate_bps = 120;
        r.fake->drop();
        run_radar(r, 20000, false);
        CHECK(r.link.state() == LinkState::Found);
        CHECK(r.link.port().transport == Transport::Serial);
        CHECK(!r.mux.fallback());
        CHECK(!r.ble.enabled());
        CHECK(r.fake->stops >= 1);
        CHECK(r.fake->connected.empty());
    }
    /* no remembered Bluetooth radar: Auto stays on USB */
    {
        Rig r(Connection::Auto);
        r.fake->add(kAddr, kName, -60, true);
        r.start();
        r.run(15000);
        CHECK(r.mux.fallback());
        CHECK_EQ(r.fake->starts, 0);
        CHECK(r.link.state() == LinkState::Searching);
    }
}

/* ------------------------------------------------------------------------------------------ the real notifications */

void test_fixture_byte_path()
{
    const std::vector<Note> ns = notes();
    CHECK_EQ(ns.size(), 183u);
    Bytes stream;
    std::size_t lens[3] = {0, 0, 0};
    for (const Note &n : ns) {
        stream.insert(stream.end(), n.data.begin(), n.data.end());
        lens[n.data.size() == 10 ? 0 : n.data.size() == 17 ? 1 : 2] += 1;
    }
    CHECK_EQ(lens[0], 14u);  /* empty frames */
    CHECK_EQ(lens[1], 168u); /* one target */
    CHECK_EQ(lens[2], 1u);   /* the malformed one */
    const Bytes uart = uart_bytes();
    CHECK_EQ(uart.size(), 3008u);
    CHECK(stream == uart); /* the notifications mirror the UART stream byte for byte */
    bool malformed_seen = false;
    for (const Note &n : ns)
        if (n.data == hex_bytes("f4f3f2f107000100f8f7f6f5")) malformed_seen = true;
    CHECK(malformed_seen);

    /* through BleSys: one frame per notification, read back as bytes */
    for (int split = 0; split < 2; ++split) {
        FakeBle *fake = new FakeBle;
        fake->add(kAddr, kName, -60, true);
        BleSys ble{std::unique_ptr<BleBackend>(fake)};
        ble.set_remembered(kAddr, kName);
        ble.set_active(true, 0);
        ble.set_enabled(true, 0);
        Ms t = 0;
        for (; t < 600 && ble.state() != BleState::Connected; t += 30) ble.poll(t);
        CHECK(ble.state() == BleState::Connected);
        OpenErr err = OpenErr::None;
        const int h = ble.open(std::string("ble:") + kAddr, err);
        CHECK(h >= kBleHandleBase);
        Bytes got;
        std::uint8_t buf[64];
        std::size_t piece = 1;
        for (const Note &n : ns) {
            if (split == 0) {
                fake->notify(n.data);
            } else { /* the same bytes in notifications of 1 to 7 bytes (a module with a small MTU) */
                for (std::size_t i = 0; i < n.data.size();) {
                    const std::size_t k = std::min(piece, n.data.size() - i);
                    fake->notify(Bytes(n.data.begin() + static_cast<long>(i), n.data.begin() + static_cast<long>(i + k)));
                    i += k;
                    piece = piece % 7 + 1;
                }
            }
            ble.poll(t += 30);
            long r;
            while ((r = ble.read(h, buf, sizeof buf)) > 0) got.insert(got.end(), buf, buf + r);
        }
        CHECK(got == uart);
        DecoderStats sb, su;
        const std::vector<Frame> fb = decode(got, &sb), fu = decode(uart, &su);
        CHECK_EQ(fb.size(), 182u);
        CHECK(same_frames(fb, fu));
        CHECK_EQ(sb.frames_bad, 1u); /* the malformed frame (len 7, a 2-byte body) */
        CHECK_EQ(su.frames_bad, 1u);
        std::size_t empty = 0, target = 0;
        for (const Frame &f : fb) (f.present_count() ? target : empty) += 1;
        CHECK_EQ(empty, 14u);
        CHECK_EQ(target, 168u);
        CHECK_EQ(ble.dropped_bytes(), 0u);
        CHECK(ble.notifications() >= ns.size());
        ble.close(h);
    }
}

/* The decoder resyncs on the malformed frame however it arrives: alone, glued to the next frame, cut in two. */
void test_malformed_frame()
{
    const Bytes bad = hex_bytes("f4f3f2f107000100f8f7f6f5");
    const Bytes next = hex_bytes("f4f3f2f1070001016605010359f8f7f6f5");
    const Bytes empty = hex_bytes("f4f3f2f10000f8f7f6f5");
    Bytes all = bad;
    all.insert(all.end(), next.begin(), next.end());
    all.insert(all.end(), empty.begin(), empty.end());
    for (std::size_t chunk : {1u, 2u, 5u, 12u, 13u, 64u}) {
        Ld2451Decoder dec;
        std::vector<Frame> out;
        for (std::size_t i = 0; i < all.size(); i += chunk) dec.feed(all.data() + i, std::min(chunk, all.size() - i), &out);
        CHECK_EQ(out.size(), 2u);
        if (out.size() == 2) {
            CHECK(out[0].target[0].present);
            CHECK_EQ(out[0].target[0].distance_mm, 5000);
            CHECK(!out[1].target[0].present);
        }
        CHECK_EQ(dec.stats().frames_bad, 1u);
    }
    /* the malformed notification alone: nothing is decoded until more bytes come, and nothing wrong either */
    Ld2451Decoder d1;
    std::vector<Frame> o1;
    d1.feed(bad.data(), bad.size(), &o1);
    CHECK_EQ(o1.size(), 0u);
    d1.feed(empty.data(), empty.size(), &o1);
    CHECK_EQ(o1.size(), 1u);
    CHECK_EQ(d1.stats().frames_bad, 1u);
}

/* The whole path with the recording's own timing: BleSys -> the detector -> the link -> the tracker. */
void run_fixture_link(bool split, Rig &r)
{
    const std::vector<Note> ns = notes();
    r.fake->add(kAddr, kName, -60, true);
    r.start(remembered_ble());
    CHECK(r.until([&] { return r.ble.state() == BleState::Connected; }, 2000));
    const Ms t0 = r.now - 545; /* the subscription came 0.545 s after it was asked for in the recording */
    std::size_t i = 0, piece = 3;
    while (i < ns.size()) {
        r.tick();
        while (i < ns.size() && r.now >= t0 + ns[i].t) {
            const Bytes &d = ns[i].data;
            if (!split) {
                r.fake->notify(d);
            } else {
                for (std::size_t j = 0; j < d.size();) {
                    const std::size_t k = std::min(piece, d.size() - j);
                    r.fake->notify(Bytes(d.begin() + static_cast<long>(j), d.begin() + static_cast<long>(j + k)));
                    j += k;
                    piece = piece % 9 + 2;
                }
            }
            ++i;
        }
    }
    r.run(300);
}

void test_fixture_link()
{
    for (int split = 0; split < 2; ++split) {
        Rig r(Connection::Bluetooth);
        run_fixture_link(split != 0, r);
        CHECK(r.link.state() == LinkState::Found);
        CHECK(r.link.driver() == find_driver("ld2451"));
        CHECK(r.link.port().transport == Transport::Ble);
        CHECK_EQ(r.link.port().address, kAddr);
        CHECK_EQ(r.link.port_title(), std::string(kName) + " (Bluetooth)");
        CHECK_EQ(r.link.baud(), 0);
        CHECK_EQ(r.tap.configures, 0);
        CHECK_EQ(r.tap.flushes, 0);
        CHECK_EQ(r.fake->connects, 1); /* never lost on the way: the longest gap is 3.2 s, the LD2451 quiet time 5 s */
        CHECK_EQ(r.fake->disconnects, 0);
        /* everything the readers (the detector, then the link) got: the UART stream of the same 75 s */
        const Bytes got = r.tap.all_ble();
        const Bytes uart = uart_bytes();
        CHECK(got == uart);
        DecoderStats st;
        CHECK(same_frames(decode(got, &st), decode(uart)));
        CHECK_EQ(st.frames_ok, 182u);
        /* the link decoded all but the frames the detector used to recognise the radar (3, one per notification) */
        const LinkStats s = r.link.stats();
        CHECK(s.frames >= 178u && s.frames <= 179u);
        CHECK_EQ(s.bad_frames, 1u); /* the malformed frame came at 66.9 s, well after detection */
        CHECK(r.link.tracker().frames() >= 178u);
        CHECK(r.link.reports().size() == 1u && r.link.reports()[0].verdict == Verdict::Radar);
        if (!r.link.reports().empty()) {
            CHECK_EQ(r.link.reports()[0].baud, 0);
            CHECK_EQ(r.link.reports()[0].text, "mmWave radar (LD2451)");
        }
        /* the last target came at 72.6 s, an empty frame at 74.9 s: the LD2451 keeps a slot for 3 s, so it is still held */
        CHECK(r.link.tracker().slot(0).active);
        r.link.close();
        CHECK_EQ(r.fake->count(std::string("disconnect ") + kAddr), 1);
    }
}

/* A detector verdict over Bluetooth (the Sensor type fixed to the LD2450 family): not a radar of that type, the
 * device is disconnected and tried again later (the status reads "Listening to ..."). */
void test_detector_verdict()
{
    Rig r(Connection::Bluetooth);
    r.link.set_sensor_type("ld2450", 0);
    r.fake->add(kAddr, kName, -60, true);
    r.start(remembered_ble());
    std::string status;
    const Ms end = r.now + 12000;
    Ms next = r.now;
    while (r.now < end) {
        if (r.now >= next) {
            r.fake->notify(ld_frame(4, 3, 1, 4));
            next += 150;
        }
        r.tick();
        if (status.empty() && !r.link.status().empty()) status = r.link.status();
    }
    CHECK_EQ(status, std::string("Listening to ") + kName + " (Bluetooth)");
    CHECK(r.link.state() != LinkState::Found);
    bool reported = false;
    for (const PortReport &p : r.link.reports())
        if (p.meta.transport == Transport::Ble) {
            reported = true;
            CHECK(p.verdict == Verdict::NotRadar);
            CHECK_EQ(p.baud, 0);
            CHECK(p.text.find("not a supported radar") == 0);
        }
    CHECK(reported);
    CHECK(r.fake->count(std::string("disconnect ") + kAddr) >= 1);
    CHECK_EQ(r.tap.configures, 0);
}

/* ------------------------------------------------------------------------------------------ the diagnostic line */

void test_diag()
{
    Rig r(Connection::Bluetooth);
    r.fake->add(kAddr, kName, -60, true);
    r.start(remembered_ble());
    run_radar(r, 3000);
    CHECK(r.link.state() == LinkState::Found);
    const std::string line = diag_line(r.link, r.now);
    CHECK(line.find(std::string("state=Found transport=ble addr=") + kAddr + " name=" + kName + " driver=ld2451") !=
          std::string::npos);
    CHECK(line.find("baud=") == std::string::npos);
    CHECK(line.find(" port=") == std::string::npos);
    unsigned sent = 0;
    const std::string extra = diag_extra(r.link, sent);
    CHECK(extra.find(std::string("ble transport=ble addr=") + kAddr + " name=" + kName + " notify=AcquireNotify mtu=512") == 0);
    /* a serial radar keeps its line */
    FakeSys sys;
    FakePort &p = sys.add("ttyUSB0");
    p.stream[115200] = ld2451_stream();
    p.rate_bps = 120;
    Link link(sys);
    link.start(0, Remembered());
    for (int i = 0; i < 700 && link.state() != LinkState::Found; ++i) {
        sys.now_ms += 30;
        link.poll(sys.now_ms);
    }
    CHECK(link.state() == LinkState::Found);
    const std::string sl = diag_line(link, sys.now_ms);
    CHECK(sl.find("state=Found port=ttyUSB0 baud=115200 driver=ld2451") != std::string::npos);
    CHECK(sl.find("transport=") == std::string::npos);
}

/* ------------------------------------------------------------------------------------------ the scripted demo */

void test_demo_backend()
{
    Ms clock = 0;
    {
        FakeSys serial;
        BleSys ble(std::unique_ptr<BleBackend>(new DemoBle("ble", [&] { return clock; })));
        MuxSys mux(serial, &ble);
        mux.set_mode(Connection::Bluetooth);
        Link link(mux);
        link.start(clock, Remembered());
        auto run = [&](Ms ms) {
            const Ms end = clock + ms;
            while (clock < end) {
                clock += 30;
                link.poll(clock);
                mux.update(link, true, clock);
            }
        };
        run(2500);
        const std::vector<BleCandidate> c = ble.candidates();
        CHECK_EQ(c.size(), 2u);
        if (c.size() == 2) {
            CHECK_EQ(c[0].name, "HLK-LD2451_49F0");
            CHECK_EQ(c[1].name, "HLK-LD2451_7C1E");
        }
        ble.choose(c.empty() ? kAddr : c[0].address, clock);
        run(6000);
        CHECK(link.state() == LinkState::Found);
        CHECK(link.port().transport == Transport::Ble);
        CHECK(link.tracker().slot(0).active || link.tracker().slot(1).active);
        std::string a, n;
        CHECK(ble.take_chosen(a, n));
        CHECK_EQ(n, "HLK-LD2451_49F0");
    }
    for (const char *s : {"ble-off", "ble-noadapter", "ble-nobluez"}) {
        BleSys ble(std::unique_ptr<BleBackend>(new DemoBle(s, [&] { return clock; })));
        ble.set_active(true, clock);
        ble.set_enabled(true, clock);
        for (int i = 0; i < 20; ++i) ble.poll(clock += 30);
        CHECK(ble.state() == BleState::Unavailable);
        const std::string t = ble.title();
        CHECK_EQ(t, std::string(s) == "ble-off" ? "Bluetooth is off" : std::string(s) == "ble-noadapter" ? "No Bluetooth adapter"
                                                                                                         : "BlueZ not available");
    }
}

} // namespace

void test_ble()
{
    test_names_and_settings();
    test_backoff();
    test_discovery();
    test_remembered_flow();
    test_connect_timeout_and_backoff();
    test_subscribe_failure();
    test_loss_and_reconnect();
    test_clean_disconnect();
    test_unavailable();
    test_auto();
    test_fixture_byte_path();
    test_malformed_frame();
    test_fixture_link();
    test_detector_verdict();
    test_diag();
    test_demo_backend();
}
