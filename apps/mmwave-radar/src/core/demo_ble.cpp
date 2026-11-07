/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the scripted Bluetooth backend (see demo_ble.hpp).
 */
#include "demo_ble.hpp"

#include "demo_sys.hpp"

#include <algorithm>

namespace mmw {

namespace {

BleEvent ev(BleEvent::Kind k, const std::string &address, bool ok, const std::string &error = std::string())
{
    BleEvent e;
    e.kind = k;
    e.address = address;
    e.ok = ok;
    e.error = error;
    return e;
}

} // namespace

DemoBle::DemoBle(const std::string &scenario, Clock clock) : scenario_(scenario), clock_(std::move(clock))
{
    auto add = [&](const char *address, const char *name, int rssi, std::int64_t after) {
        Dev d;
        d.d.address = address;
        d.d.name = name;
        d.d.rssi = rssi;
        d.seen_after_ms = after;
        devs_.push_back(d);
    };
    add("34:B7:DA:62:1E:ED", "Meshtastic_1eec", -64, 200);
    add("ED:2F:90:B4:49:F0", "HLK-LD2451_49F0", -58, 700);
    add("EC:EB:45:39:28:2D", "MeshCore-OSR T1000", -80, 900);
    add("79:1E:3F:23:EC:7D", "", -70, 1000);
    add("D4:11:6B:C0:7C:1E", "HLK-LD2451_7C1E", -77, 1500);
}

bool DemoBle::start(std::string &why)
{
    (void)why;
    started_ = true;
    ready_at_ = now() + 150;
    ready_sent_ = false;
    return true;
}

void DemoBle::stop()
{
    started_ = false;
    scanning_ = false;
    subscribed_ = false;
    connected_.clear();
    connecting_.clear();
    subscribing_.clear();
    ready_sent_ = false;
}

BleAvail DemoBle::availability() const
{
    if (!started_ || !ready_sent_) return BleAvail::Unknown;
    if (scenario_ == "ble-off") return BleAvail::Off;
    if (scenario_ == "ble-noadapter") return BleAvail::NoAdapter;
    if (scenario_ == "ble-nobluez") return BleAvail::NoBluez;
    return BleAvail::Ok;
}

std::string DemoBle::unavailable_why() const
{
    if (scenario_ == "ble-off") return "the adapter is powered off (demo)";
    if (scenario_ == "ble-noadapter") return "no Bluetooth adapter (demo)";
    if (scenario_ == "ble-nobluez") return "bluetoothd is not running (demo)";
    return std::string();
}

DemoBle::Dev *DemoBle::dev(const std::string &address)
{
    for (Dev &d : devs_)
        if (d.d.address == address) return &d;
    return nullptr;
}

bool DemoBle::find(const std::string &address, BleDevice &out) const
{
    for (const Dev &d : devs_)
        if (d.d.address == address && d.known) {
            out = d.d;
            return true;
        }
    return false;
}

void DemoBle::start_scan()
{
    if (!started_ || availability() != BleAvail::Ok) {
        pending_.push_back(ev(BleEvent::Kind::ScanStopped, "", false, "no adapter (demo)"));
        return;
    }
    scanning_ = true;
    scan_t0_ = now();
}

void DemoBle::stop_scan()
{
    scanning_ = false;
    for (Dev &d : devs_) d.d.has_rssi = false;
}

void DemoBle::connect(const std::string &address)
{
    connecting_ = address;
    connect_at_ = now() + 800;
}

void DemoBle::disconnect(const std::string &address)
{
    if (address == connecting_) connecting_.clear();
    if (address == connected_) {
        connected_.clear();
        if (Dev *d = dev(address)) {
            d->d.connected = false;
            d->d.services_resolved = false;
        }
        pending_.push_back(ev(BleEvent::Kind::ConnectionChanged, address, false));
    }
    subscribed_ = false;
}

void DemoBle::subscribe(const std::string &address)
{
    subscribing_ = address;
    subscribe_at_ = now() + 300;
}

void DemoBle::unsubscribe()
{
    subscribing_.clear();
    subscribed_ = false;
}

std::string DemoBle::describe() const { return subscribed_ ? "notify=AcquireNotify mtu=512 (demo)" : std::string(); }

void DemoBle::poll(std::vector<BleEvent> &out)
{
    if (!started_) return;
    const std::int64_t t = now();
    if (!ready_sent_ && t >= ready_at_) {
        ready_sent_ = true;
        out.push_back(ev(BleEvent::Kind::Ready, "", availability() == BleAvail::Ok, unavailable_why()));
    }
    if (scanning_) {
        for (Dev &d : devs_) {
            if (t - scan_t0_ < d.seen_after_ms || d.d.has_rssi) continue;
            d.known = true;
            d.d.has_rssi = true;
            out.push_back(ev(BleEvent::Kind::DeviceUpdated, d.d.address, true));
        }
    }
    if (!connecting_.empty() && t >= connect_at_) {
        const std::string a = connecting_;
        connecting_.clear();
        if (Dev *d = dev(a)) {
            d->d.connected = true;
            connected_ = a;
            resolve_at_ = t + 400;
            out.push_back(ev(BleEvent::Kind::ConnectionChanged, a, true));
            out.push_back(ev(BleEvent::Kind::ConnectResult, a, true));
        } else {
            out.push_back(ev(BleEvent::Kind::ConnectResult, a, false, "org.bluez.Error.Failed: unknown device (demo)"));
        }
    }
    if (!connected_.empty() && resolve_at_ >= 0 && t >= resolve_at_) {
        resolve_at_ = -1;
        if (Dev *d = dev(connected_)) {
            d->d.services_resolved = true;
            out.push_back(ev(BleEvent::Kind::ServicesResolved, connected_, true));
        }
    }
    if (!subscribing_.empty() && t >= subscribe_at_) {
        const std::string a = subscribing_;
        subscribing_.clear();
        subscribed_ = a == connected_;
        stream_t0_ = t;
        next_slot_ = 0;
        out.push_back(ev(BleEvent::Kind::SubscribeResult, a, subscribed_, subscribed_ ? "" : "not connected (demo)"));
    }
    for (BleEvent &e : pending_) out.push_back(std::move(e));
    pending_.clear();
    if (subscribed_ && sink_) { /* the frames due by now, one notification each (at most a second's worth per poll) */
        const std::int64_t due = (t - stream_t0_) / kLd2451DemoSlotMs;
        if (due - next_slot_ > 8) next_slot_ = due - 8;
        for (; next_slot_ <= due; ++next_slot_) {
            const std::vector<std::uint8_t> f = ld2451_demo_frame(2, static_cast<int>(next_slot_ % 100));
            if (!f.empty()) sink_->on_notify(f.data(), f.size());
        }
    }
}

} // namespace mmw
