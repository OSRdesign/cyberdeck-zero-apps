/*
 * SPDX-License-Identifier: MIT
 */

#include "node_demo.hpp"

#include "ble/fake_ble.hpp"

#include <chrono>
#include <cstdlib>

namespace meshzero {

namespace {

uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/* FakeBle that plays a scan: each device is heard some time after StartDiscovery, at its RSSI; the RSSI goes when the scan
 * stops (as BlueZ invalidates it). connect_fails: Connect answers an error after 0.8 s (a node out of reach). */
class DemoBle : public ble::FakeBle {
public:
    struct Dev {
        std::string address, name;
        int rssi = -60;
        bool paired = false;
        uint64_t after_ms = 300;
        bool announced = false;
    };
    std::vector<Dev> world;
    bool connect_fails = false;

    void start_scan() override
    {
        ble::FakeBle::start_scan();
        scanning_ = true;
        scan_t0_ = now_ms();
        for (Dev &d : world) d.announced = false;
    }
    void stop_scan() override
    {
        ble::FakeBle::stop_scan();
        scanning_ = false;
        for (auto &kv : devs) {
            if (!kv.second.has_rssi) continue;
            kv.second.has_rssi = false;
            ble::BleEvent e;
            e.kind = ble::BleEvent::Kind::DeviceUpdated;
            e.address = kv.first;
            e.dev = kv.second;
            push(e);
        }
    }
    void connect(const std::string &address) override
    {
        if (!connect_fails) {
            ble::FakeBle::connect(address);
            return;
        }
        calls.push_back("connect " + address);
        fail_at_ = now_ms() + 800;
        fail_addr_ = address;
    }
    void poll(std::vector<ble::BleEvent> &out) override
    {
        const uint64_t now = now_ms();
        if (scanning_)
            for (Dev &d : world) {
                if (d.announced || now < scan_t0_ + d.after_ms) continue;
                d.announced = true;
                ble::BleDevice &b = devs[d.address];
                b.address = d.address;
                b.path = ble::device_path("/org/bluez/hci0", d.address);
                b.name = d.name;
                b.address_type = "random";
                b.nus = true;
                b.has_rssi = true;
                b.rssi = d.rssi;
                b.paired = b.bonded = b.paired || d.paired;
                ble::BleEvent e;
                e.kind = ble::BleEvent::Kind::DeviceUpdated;
                e.address = d.address;
                e.dev = b;
                push(e);
            }
        if (fail_at_ && now >= fail_at_) {
            fail_at_ = 0;
            result(ble::BleEvent::Kind::ConnectResult, fail_addr_, false, "org.bluez.Error.Failed: le-connection-abort-by-local");
        }
        ble::FakeBle::poll(out);
    }
    /* A paired node is known to BlueZ before any scan. */
    void know(const Dev &d)
    {
        add_device(d.address, d.name, true, d.rssi);
        devs[d.address].has_rssi = false;
    }

private:
    bool scanning_ = false;
    uint64_t scan_t0_ = 0;
    uint64_t fail_at_ = 0;
    std::string fail_addr_;
};

SerialCandidate usb(const char *path, uint16_t vid, uint16_t pid, const char *vendor, const char *product)
{
    SerialCandidate c;
    c.path = path;
    c.vid = vid;
    c.pid = pid;
    c.vendor = vendor;
    c.product = product;
    return c;
}

} // namespace

std::unique_ptr<ble::IBleBackend> NodeDemo::make_backend() const
{
    auto be = std::make_unique<DemoBle>();
    if (name == "nobt") {
        be->start_ok = false;
        be->start_why = "no system D-Bus (demo)";
    } else if (name == "off") {
        be->powered = false;
    } else if (name == "ble") {
        be->world = {{"D7:C5:07:5B:55:EB", "MeshCore-OSR wio", -45, false, 400},
                     {"EC:EB:45:39:28:2D", "MeshCore-OSR T1000", -54, true, 700},
                     {"C2:7F:2A:ED:C4:D5", "MeshCore-Roof repeater", -68, false, 1100},
                     {"5A:5F:19:7A:0B:FF", "MeshCore-Garden", -84, false, 1600},
                     {"47:6B:5D:6D:5E:08", "MeshCore-Far away", -96, false, 2100}};
        be->know(be->world[1]);
    } else if (name == "pin") {
        be->auto_answer = true;
        be->world = {{"EC:EB:45:39:28:2D", "MeshCore-OSR T1000", -52, false, 300}};
    } else if (name == "fail") {
        be->connect_fails = true;
        DemoBle::Dev d{"D7:C5:07:5B:55:EB", "MeshCore-OSR wio", -70, true, 300};
        be->world = {d};
        be->know(d);
    }
    return be;
}

std::unique_ptr<NodeDemo> node_demo(const std::string &n)
{
    static const char *const known[] = {"empty", "usb", "ble", "first", "pin", "fail", "ip", "off", "nobt"};
    bool ok = false;
    for (const char *k : known) ok = ok || n == k;
    if (!ok) return nullptr;
    auto d = std::make_unique<NodeDemo>();
    d->name = n;
    if (n == "usb") {
        d->usb = {usb("/dev/ttyACM0", 0x2886, 0x8044, "Seeed Studio", "XIAO nRF52840"), usb("/dev/ttyUSB0", 0x1A86, 0x55D4, "QinHeng", "USB Single Serial")};
        d->scan = NodeBook::Scan::No;
        d->last = node_key(NodeKind::Usb, "/dev/ttyACM0");
    } else if (n == "first") {
        d->usb = {usb("/dev/ttyACM0", 0x2886, 0x8044, "Seeed Studio", "XIAO nRF52840")};
        d->scan = NodeBook::Scan::Ask;
    } else if (n == "fail") {
        d->usb = {usb("/dev/ttyACM9", 0x2886, 0x8044, "Seeed Studio", "XIAO nRF52840")};
    } else if (n == "ip") {
        IpNode a;
        a.host = "192.0.2.20";
        a.port = 5000;
        a.name = "Kitchen node";
        IpNode b;
        b.host = "127.0.0.1";
        b.port = 9;
        d->ip = {a, b};
        d->scan = NodeBook::Scan::No;
        d->last = a.key();
    }
    return d;
}

std::unique_ptr<NodeDemo> node_demo_from_env()
{
    const char *e = std::getenv("MESHHOP_DEMO");
    if (!e || !*e) return nullptr;
    return node_demo(e);
}

} // namespace meshzero
