/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: a scripted BleBackend (ble_sys.hpp) for the headless scenes, the screenshots and a demonstration
 * without a radio ($MMWAVE_DEMO=ble...). No D-Bus, no adapter: devices, answers and notifications are made up here,
 * against the clock given. The connected radar sends the generated LD2451 stream of the ld2451-two scenario
 * (demo_sys.hpp), one frame per notification as the real module does.
 *
 * Scenarios:
 *   ble            the adapter is on; a discovery window finds HLK-LD2451_49F0 (-58 dBm) and HLK-LD2451_7C1E (-77 dBm)
 *                  within 1.5 s, and three other devices (a Meshtastic node, a MeshCore node, one without a name) that
 *                  are not listed; Connect takes 0.8 s, the services 0.4 s more, the subscription 0.3 s
 *   ble-off        the adapter is powered off
 *   ble-noadapter  BlueZ runs, there is no adapter
 *   ble-nobluez    bluetoothd is not on the bus
 * An unknown name is "ble".
 */
#pragma once

#include "ble_sys.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mmw {

class DemoBle : public BleBackend {
public:
    using Clock = std::function<std::int64_t()>;
    DemoBle(const std::string &scenario, Clock clock);

    bool start(std::string &why) override;
    void stop() override;
    bool started() const override { return started_; }
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
    void set_sink(NotifySink *sink) override { sink_ = sink; }
    std::string describe() const override;

private:
    struct Dev {
        BleDevice d;
        std::int64_t seen_after_ms = 0; /* from the start of a discovery window */
        bool known = false;
    };
    Dev *dev(const std::string &address);
    std::int64_t now() const { return clock_ ? clock_() : 0; }

    std::string scenario_;
    Clock clock_;
    bool started_ = false;
    std::int64_t ready_at_ = -1;
    bool ready_sent_ = false;
    std::vector<Dev> devs_;
    bool scanning_ = false;
    std::int64_t scan_t0_ = 0;
    std::string connecting_, connected_;
    std::int64_t connect_at_ = -1, resolve_at_ = -1;
    std::string subscribing_;
    std::int64_t subscribe_at_ = -1;
    bool subscribed_ = false;
    std::int64_t stream_t0_ = 0;
    std::int64_t next_slot_ = 0;
    NotifySink *sink_ = nullptr;
    std::vector<BleEvent> pending_;
};

} // namespace mmw
