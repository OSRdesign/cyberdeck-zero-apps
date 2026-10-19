/*
 * SPDX-License-Identifier: MIT
 *
 * The real IBleBackend: BlueZ over the system D-Bus with sd-bus (libsystemd0).
 *
 * libsystemd.so.0 is opened at run time (dlopen), so the app links no new library and needs no change in the shared
 * build (apps/_common/scons); without the library, a system bus or bluetoothd, start() / Ready say why and the app keeps
 * working over USB. One thread: poll() runs sd_bus_process() without waiting and reads the notify socket.
 *
 * BlueZ API used (doc/org.bluez.*.rst of BlueZ 5.82, the deck's version):
 *   ObjectManager.GetManagedObjects, InterfacesAdded / InterfacesRemoved, Properties.PropertiesChanged  (state)
 *   Adapter1.SetDiscoveryFilter({Transport: "le"}), StartDiscovery, StopDiscovery                        (scan)
 *   Device1.Connect, Pair, CancelPairing, Disconnect                                                       (link)
 *   AgentManager1.RegisterAgent(path, "KeyboardOnly") -- NOT RequestDefaultAgent: the launcher stays the default agent;
 *     BlueZ asks the agent of the process that called Pair() (src/device.c pair_device -> agent_get(sender)).
 *   Agent1.RequestPasskey / RequestPinCode answered later with the PIN typed by the user; Cancel; the rest refused.
 *   GattCharacteristic1.AcquireNotify (a socket: one read = one notification; HUP when the link drops), else
 *     StartNotify + PropertiesChanged "Value"; WriteValue(value, {type: "request"}).
 */

#pragma once

#include "ble_types.hpp"

#include <functional>
#include <memory>

namespace meshzero::ble {

class BluezBackend : public IBleBackend {
public:
    explicit BluezBackend(std::string agent_path = "/org/meshhop/agent");
    ~BluezBackend() override;
    BluezBackend(const BluezBackend &) = delete;
    BluezBackend &operator=(const BluezBackend &) = delete;

    bool start(std::string &why) override;
    void stop() override;
    bool started() const override;
    bool ready() const override;
    bool adapter_powered() const override;
    void poll(std::vector<BleEvent> &out) override;
    void start_scan() override;
    void stop_scan() override;
    std::vector<BleDevice> devices() const override;
    bool find(const std::string &address, BleDevice &out) const override;
    void connect(const std::string &address) override;
    void pair(const std::string &address) override;
    void answer_passkey(bool ok, uint32_t passkey) override;
    void cancel_pairing(const std::string &address) override;
    void disconnect(const std::string &address) override;
    void open_gatt(const std::string &address) override;
    void close_gatt() override;
    void write(const Bytes &frame) override;

    /* For the probe and the log. */
    std::string adapter_path() const;
    bool agent_registered() const;
    bool notify_acquired() const;     // AcquireNotify (true) or StartNotify (false)
    void set_log(std::function<void(const std::string &)> log);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace meshzero::ble
