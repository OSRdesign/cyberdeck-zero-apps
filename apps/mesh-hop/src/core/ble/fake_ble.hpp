/*
 * SPDX-License-Identifier: MIT
 *
 * A scripted IBleBackend for the unit tests (header only: nothing of it is in the app). It records every call as a line
 * ("connect AA:..", "pair AA:..", "write 0a1b..", "passkey 123456" ...) and hands out the events a test pushes. With
 * auto_answer it plays a well behaved BlueZ + MeshCore board on its own: connect, pair (asks the passkey, accepts
 * `good_pin`), services, GATT with `mtu`, and every write answered ok.
 */

#pragma once

#include "ble_types.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace meshzero::ble {

class FakeBle : public IBleBackend {
public:
    /* knobs */
    bool start_ok = true;
    std::string start_why = "no system D-Bus";
    bool ready_ok = true;             // the Ready event carries ok
    bool powered = true;
    bool auto_answer = false;
    uint32_t good_pin = 123456;
    int mtu = 247;
    bool nus = true;                  // open_gatt finds the Nordic UART service

    /* what happened */
    std::vector<std::string> calls;
    std::vector<Bytes> writes;
    std::map<std::string, BleDevice> devs;

    void push(const BleEvent &e) { queued_.push_back(e); }
    void add_device(const std::string &addr, const std::string &name, bool paired = false, int rssi = -60)
    {
        BleDevice d;
        d.address = addr;
        d.path = device_path("/org/bluez/hci0", addr);
        d.name = name;
        d.nus = name.rfind(kNamePrefix, 0) == 0;
        d.paired = d.bonded = paired;
        d.has_rssi = true;
        d.rssi = rssi;
        devs[addr] = d;
    }
    /* A device appears in the scan (DeviceUpdated). */
    void appear(const std::string &addr, const std::string &name, bool paired = false, int rssi = -60)
    {
        add_device(addr, name, paired, rssi);
        BleEvent e;
        e.kind = BleEvent::Kind::DeviceUpdated;
        e.address = addr;
        e.dev = devs[addr];
        push(e);
    }
    void set_connected(const std::string &addr, bool on, bool resolved)
    {
        BleDevice &d = devs[addr];
        d.connected = on;
        d.services_resolved = on && resolved;
        BleEvent e;
        e.kind = BleEvent::Kind::ConnectionChanged;
        e.address = addr;
        e.ok = on;
        push(e);
        if (on && resolved) {
            e.kind = BleEvent::Kind::ServicesResolved;
            push(e);
        }
    }
    void notify(const Bytes &data)
    {
        BleEvent e;
        e.kind = BleEvent::Kind::Notify;
        e.data = data;
        push(e);
    }
    void result(BleEvent::Kind k, const std::string &addr, bool ok, const std::string &error = "")
    {
        BleEvent e;
        e.kind = k;
        e.address = addr;
        e.ok = ok;
        e.error = error;
        push(e);
    }
    bool called(const std::string &line) const
    {
        for (const std::string &c : calls)
            if (c == line) return true;
        return false;
    }
    size_t count(const std::string &prefix) const
    {
        size_t n = 0;
        for (const std::string &c : calls) n += c.rfind(prefix, 0) == 0;
        return n;
    }

    /* IBleBackend */
    bool start(std::string &why) override
    {
        calls.push_back("start");
        if (!start_ok) {
            why = start_why;
            return false;
        }
        started_ = true;
        BleEvent e;
        e.kind = BleEvent::Kind::Ready;
        e.ok = ready_ok;
        e.error = ready_ok ? "" : "no Bluetooth adapter";
        ready_ = ready_ok;
        push(e);
        return true;
    }
    void stop() override
    {
        calls.push_back("stop");
        started_ = ready_ = false;
    }
    bool started() const override { return started_; }
    bool ready() const override { return ready_; }
    bool adapter_powered() const override { return powered; }
    void poll(std::vector<BleEvent> &out) override
    {
        std::vector<BleEvent> now;
        now.swap(queued_);
        for (BleEvent &e : now) out.push_back(std::move(e));
    }
    void start_scan() override { calls.push_back("scan on"); }
    void stop_scan() override { calls.push_back("scan off"); }
    std::vector<BleDevice> devices() const override
    {
        std::vector<BleDevice> v;
        for (const auto &kv : devs) v.push_back(kv.second);
        return v;
    }
    bool find(const std::string &address, BleDevice &out) const override
    {
        auto it = devs.find(address);
        if (it == devs.end()) return false;
        out = it->second;
        return true;
    }
    void connect(const std::string &address) override
    {
        calls.push_back("connect " + address);
        if (!auto_answer) return;
        result(BleEvent::Kind::ConnectResult, address, true);
        set_connected(address, true, true);
    }
    void pair(const std::string &address) override
    {
        calls.push_back("pair " + address);
        pairing_ = address;
        if (!auto_answer) return;
        devs[address].connected = true;
        BleEvent e;
        e.kind = BleEvent::Kind::PasskeyRequest;
        e.address = address;
        push(e);
    }
    void answer_passkey(bool ok, uint32_t passkey) override
    {
        char line[48];
        std::snprintf(line, sizeof(line), ok ? "passkey %06u" : "passkey cancel", static_cast<unsigned>(passkey));
        calls.push_back(line);
        if (!auto_answer || pairing_.empty()) return;
        const std::string a = pairing_;
        pairing_.clear();
        if (ok && passkey == good_pin) {
            devs[a].paired = devs[a].bonded = true;
            devs[a].services_resolved = true;
            result(BleEvent::Kind::PairResult, a, true);
        } else {
            devs[a].connected = false;
            result(BleEvent::Kind::PairResult, a, false,
                   ok ? "org.bluez.Error.AuthenticationFailed: Authentication Failed" : "org.bluez.Error.AuthenticationCanceled");
        }
    }
    void cancel_pairing(const std::string &address) override { calls.push_back("cancel " + address); }
    void disconnect(const std::string &address) override
    {
        calls.push_back("disconnect " + address);
        auto it = devs.find(address);
        if (it != devs.end()) it->second.connected = it->second.services_resolved = false;
    }
    void remove_device(const std::string &address) override
    {
        calls.push_back("remove " + address);
        if (devs.erase(address)) result(BleEvent::Kind::DeviceRemoved, address, true);
    }
    void open_gatt(const std::string &address) override
    {
        calls.push_back("gatt " + address);
        if (!auto_answer) return;
        BleEvent e;
        e.kind = BleEvent::Kind::GattResult;
        e.address = address;
        e.ok = nus;
        e.not_companion = !nus;
        e.mtu = mtu;
        if (!nus) e.error = "no Nordic UART service";
        push(e);
    }
    void close_gatt() override { calls.push_back("gatt close"); }
    void write(const Bytes &frame) override
    {
        calls.push_back("write " + to_hex(frame));
        writes.push_back(frame);
        if (auto_answer) result(BleEvent::Kind::WriteResult, "", true);
    }

private:
    bool started_ = false;
    bool ready_ = false;
    std::string pairing_;
    std::vector<BleEvent> queued_;
};

} // namespace meshzero::ble
