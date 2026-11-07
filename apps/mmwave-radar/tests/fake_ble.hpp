/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: a scripted BleBackend (ble_sys.hpp), no radio. Devices, BlueZ's answers and the notifications
 * are set by the test; every call is counted and logged in order. The interface has no call that changes or fetches a
 * characteristic value or bonds: a test that compiles proves the code under test cannot ask for one.
 */
#pragma once

#include "ble_sys.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace mmw {

class FakeBle : public BleBackend {
public:
    /* the script */
    bool start_ok = true;
    std::string start_why = "libsystemd.so.0 is missing (test)";
    BleAvail avail = BleAvail::Ok;     /* what BlueZ says once Ready came */
    std::string why;
    bool auto_ready = true;            /* Ready on the first poll after start() */
    bool auto_connect = true;          /* Connect answers on the next poll (ok unless connect_error is set) */
    std::string connect_error;
    std::string disconnect_error;      /* BlueZ's answer to Disconnect for the diagnostic log ("No matching connection" ...) */
    std::function<void(const std::string &)> diag;
    bool subscribe_ok = true;
    std::string subscribe_error = "no 0xfff1 characteristic: not an HLK-LD radar module";
    struct Dev {
        BleDevice d;
        bool known = false;            /* BlueZ has it (cached or seen) */
        bool in_range = true;          /* a discovery window finds it */
        bool reported = false;
    };
    std::vector<Dev> devs;
    /* what happened */
    int starts = 0, stops = 0, scans_on = 0, scans_off = 0, connects = 0, disconnects = 0, subscribes = 0, unsubscribes = 0;
    std::vector<std::string> calls;
    /* state */
    bool started_ = false, ready_sent = false, scanning = false, subscribed = false;
    std::string connected, pending_connect, pending_sub;
    std::vector<BleEvent> queue;
    NotifySink *sink = nullptr;

    Dev &add(const std::string &address, const std::string &name, int rssi, bool known = false)
    {
        Dev d;
        d.d.address = address;
        d.d.name = name;
        d.d.rssi = rssi;
        d.known = known;
        devs.push_back(d);
        return devs.back();
    }
    Dev *dev(const std::string &a)
    {
        for (Dev &d : devs)
            if (d.d.address == a) return &d;
        return nullptr;
    }
    /* one notification of 0xfff1 (only while subscribed, as BlueZ) */
    void notify(const std::vector<std::uint8_t> &b)
    {
        if (subscribed && sink && !b.empty()) sink->on_notify(b.data(), b.size());
    }
    /* the radio link drops (out of range, the module reset) */
    void drop()
    {
        if (connected.empty()) return;
        if (Dev *d = dev(connected)) d->d.connected = d->d.services_resolved = false;
        queue.push_back(ev(BleEvent::Kind::ConnectionChanged, connected, false));
        if (subscribed) queue.push_back(ev(BleEvent::Kind::NotifyClosed, connected, false));
        connected.clear();
        subscribed = false;
    }
    /* the adapter is switched on / off from elsewhere (the launcher's Settings) */
    void power(bool on)
    {
        avail = on ? BleAvail::Ok : BleAvail::Off;
        if (!on) drop();
        queue.push_back(ev(BleEvent::Kind::AdapterChanged, "", on));
    }
    int count(const std::string &call) const { return static_cast<int>(std::count(calls.begin(), calls.end(), call)); }

    bool start(std::string &w) override
    {
        ++starts;
        calls.push_back("start");
        if (!start_ok) {
            w = start_why;
            return false;
        }
        started_ = true;
        ready_sent = false;
        return true;
    }
    void stop() override
    {
        ++stops;
        calls.push_back("stop");
        started_ = scanning = subscribed = false;
        ready_sent = false;
    }
    bool started() const override { return started_; }
    BleAvail availability() const override { return started_ && ready_sent ? avail : BleAvail::Unknown; }
    std::string unavailable_why() const override { return why; }
    void poll(std::vector<BleEvent> &out) override
    {
        if (!started_) return;
        if (!ready_sent && auto_ready) {
            ready_sent = true;
            out.push_back(ev(BleEvent::Kind::Ready, "", avail == BleAvail::Ok, why));
        }
        if (scanning && avail == BleAvail::Ok) {
            for (Dev &d : devs) {
                if (!d.in_range || d.reported) continue;
                d.reported = d.known = d.d.has_rssi = true;
                out.push_back(ev(BleEvent::Kind::DeviceUpdated, d.d.address, true));
            }
        }
        if (!pending_connect.empty() && auto_connect) {
            const std::string a = pending_connect;
            pending_connect.clear();
            Dev *d = dev(a);
            if (!connect_error.empty() || !d || !d->in_range) {
                out.push_back(ev(BleEvent::Kind::ConnectResult, a, false,
                                 connect_error.empty() ? "org.bluez.Error.Failed: le-connection-abort-by-local" : connect_error));
            } else {
                d->d.connected = d->d.services_resolved = true;
                connected = a;
                out.push_back(ev(BleEvent::Kind::ConnectResult, a, true));
                out.push_back(ev(BleEvent::Kind::ServicesResolved, a, true));
            }
        }
        if (!pending_sub.empty()) {
            const std::string a = pending_sub;
            pending_sub.clear();
            subscribed = subscribe_ok && a == connected;
            out.push_back(ev(BleEvent::Kind::SubscribeResult, a, subscribed, subscribed ? "" : subscribe_error));
        }
        for (BleEvent &e : queue) out.push_back(e);
        queue.clear();
    }
    void start_scan() override
    {
        ++scans_on;
        calls.push_back("scan on");
        scanning = true;
        for (Dev &d : devs) d.reported = false;
    }
    void stop_scan() override
    {
        ++scans_off;
        calls.push_back("scan off");
        scanning = false;
        for (Dev &d : devs) d.d.has_rssi = false;
    }
    bool find(const std::string &a, BleDevice &out) const override
    {
        for (const Dev &d : devs)
            if (d.d.address == a && d.known) {
                out = d.d;
                return true;
            }
        return false;
    }
    void connect(const std::string &a) override
    {
        ++connects;
        calls.push_back("connect " + a);
        pending_connect = a;
    }
    void disconnect(const std::string &a) override
    {
        ++disconnects;
        calls.push_back("disconnect " + a);
        if (diag) diag("ble-disconnect addr=" + a + " result=" + ble_disconnect_result(disconnect_error));
        if (pending_connect == a) pending_connect.clear();
        if (connected == a) {
            if (Dev *d = dev(a)) d->d.connected = d->d.services_resolved = false;
            connected.clear();
            subscribed = false;
            queue.push_back(ev(BleEvent::Kind::ConnectionChanged, a, false));
        }
    }
    void subscribe(const std::string &a) override
    {
        ++subscribes;
        calls.push_back("subscribe " + a);
        pending_sub = a;
    }
    void unsubscribe() override
    {
        ++unsubscribes;
        calls.push_back("unsubscribe");
        subscribed = false;
        pending_sub.clear();
    }
    void set_sink(NotifySink *s) override { sink = s; }
    void set_diag(std::function<void(const std::string &)> f) override { diag = std::move(f); }
    std::string describe() const override { return subscribed ? "notify=AcquireNotify mtu=512" : std::string(); }

private:
    static BleEvent ev(BleEvent::Kind k, const std::string &a, bool ok, const std::string &e = std::string())
    {
        BleEvent x;
        x.kind = k;
        x.address = a;
        x.ok = ok;
        x.error = e;
        return x;
    }
};

} // namespace mmw
