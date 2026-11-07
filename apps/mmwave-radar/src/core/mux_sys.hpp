/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the port layer the link reads, by the Connection setting (settings.hpp): the USB serial ports (a
 * PosixSys, or the scripted DemoSys), the Bluetooth radar (BleSys), or both. Read only, as every Sys.
 *
 *   USB        everything goes to the serial layer, exactly as before the setting existed; Bluetooth is never started.
 *   Bluetooth  only the Bluetooth radar is listed; BleSys browses (discovery windows for the list).
 *   Auto       the USB ports first. Only when a scan of the link ended without a radar (and while the link streams
 *              from the Bluetooth radar) BleSys is enabled, for the remembered radar only (no browsing; without a
 *              remembered radar Auto is USB). A USB radar found by a later scan turns Bluetooth off again.
 * The Bluetooth radar's handles are kBleHandleBase and up (ble_sys.hpp), the serial ones below.
 */
#pragma once

#include "ble_sys.hpp"
#include "link.hpp"
#include "settings.hpp"

namespace mmw {

class MuxSys : public Sys {
public:
    /* ble may be null (no Bluetooth at all: the modes then read the serial ports, Bluetooth lists nothing). */
    MuxSys(Sys &serial, BleSys *ble) : serial_(serial), ble_(ble) {}

    void set_mode(Connection c) { mode_ = c; }
    Connection mode() const { return mode_; }
    /* Once per UI tick, after link.poll(): applies the mode (and Auto's rule) to BleSys and polls it. `active`: the
     * Radar page is visible. */
    void update(const Link &link, bool active, Ms now);
    /* Auto: the fallback to the Bluetooth radar is on. */
    bool fallback() const { return fallback_; }
    BleSys *ble() { return ble_; }

    std::vector<PortMeta> list_ports() override;
    bool held_by_other(const std::string &path) override;
    int open(const std::string &path, OpenErr &err) override;
    bool configure(int handle, int baud) override;
    void flush_input(int handle) override;
    long read(int handle, std::uint8_t *buf, std::size_t n) override;
    void restore(int handle) override;
    void close(int handle) override;
    std::string describe(int handle) override;
    std::uint64_t ports_generation() override;

private:
    static bool is_ble(int handle) { return handle >= kBleHandleBase; }
    static bool is_ble_path(const std::string &path) { return path.compare(0, 4, "ble:") == 0; }
    bool serial_on() const { return mode_ != Connection::Bluetooth; }

    Sys &serial_;
    BleSys *ble_;
    Connection mode_ = Connection::Usb;
    bool fallback_ = false;
};

} // namespace mmw
