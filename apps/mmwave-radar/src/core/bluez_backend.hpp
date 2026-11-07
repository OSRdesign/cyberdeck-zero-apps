/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the real BleBackend (ble_sys.hpp): BlueZ over the system D-Bus with sd-bus (libsystemd0), the approach
 * of Mesh Hop's bluez_backend (apps/mesh-hop/src/core/ble), cut down to what a listen-only reader needs.
 *
 * libsystemd.so.0 is opened at run time (dlopen), so the app links no new library and the shared build is unchanged;
 * without the library, a system bus or bluetoothd, start() / availability() say so and the USB ports work as before.
 * One thread: poll() runs sd_bus_process() without waiting and reads the notification socket; every D-Bus call is
 * asynchronous (sd_bus_call_async with a timeout), so the UI thread never waits for BlueZ. Only stop() flushes the
 * queued calls (the disconnection) before closing the bus.
 *
 * BlueZ API used (doc/org.bluez.*.rst, BlueZ 5.x):
 *   ObjectManager.GetManagedObjects, InterfacesAdded / InterfacesRemoved, Properties.PropertiesChanged,
 *   NameOwnerChanged of org.bluez                                                      (state, read from signals)
 *   Adapter1.SetDiscoveryFilter({Transport: "le"}) (this client's filter, gone with its discovery), StartDiscovery,
 *   StopDiscovery                                                                      (discovery windows)
 *   Device1.Connect, Device1.Disconnect                                                (the link)
 *   GattCharacteristic1.AcquireNotify of 0xfff1 (a socket: one read = one notification, HUP when the link drops), else
 *   StartNotify + PropertiesChanged "Value"; StopNotify                                (the subscription)
 * Nothing else: no agent, no call that bonds, no property is set, no characteristic value is sent or fetched.
 */
#pragma once

#include "ble_sys.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mmw {

class BluezBackend : public BleBackend {
public:
    BluezBackend();
    ~BluezBackend() override;
    BluezBackend(const BluezBackend &) = delete;
    BluezBackend &operator=(const BluezBackend &) = delete;

    bool start(std::string &why) override;
    void stop() override;
    bool started() const override;
    BleAvail availability() const override;
    std::string unavailable_why() const override;
    void poll(std::vector<BleEvent> &out) override;
    void start_scan() override;
    void stop_scan() override;
    bool find(const std::string &address, BleDevice &out) const override;
    void connect(const std::string &address) override;
    void disconnect(const std::string &address) override;
    void subscribe(const std::string &address) override;
    void unsubscribe() override;
    void set_sink(NotifySink *sink) override;
    std::string describe() const override;
    void set_diag(std::function<void(const std::string &)> sink) override;

    /* Lines for the app's log ("bluez: ..."). */
    void set_log(std::function<void(const std::string &)> log);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace mmw
