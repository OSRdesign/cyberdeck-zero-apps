/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the port layer by the Connection setting (see mux_sys.hpp).
 */
#include "mux_sys.hpp"

namespace mmw {

void MuxSys::update(const Link &link, bool active, Ms now)
{
    if (!ble_) return;
    bool want = false;
    switch (mode_) {
    case Connection::Usb: fallback_ = false; break;
    case Connection::Bluetooth:
        fallback_ = false;
        want = true;
        break;
    case Connection::Auto:
        if (link.streaming()) fallback_ = link.port().transport == Transport::Ble; /* a USB radar ends the fallback */
        else if (!link.scanning()) fallback_ = true;                               /* the last scan found no radar */
        /* while a scan runs: unchanged */
        want = fallback_ && !link.remembered().ble_empty();
        break;
    }
    ble_->set_browse(mode_ == Connection::Bluetooth);
    ble_->set_active(active, now);
    ble_->set_enabled(want, now);
    ble_->poll(now);
}

std::vector<PortMeta> MuxSys::list_ports()
{
    std::vector<PortMeta> v;
    if (serial_on()) v = serial_.list_ports();
    if (ble_ && mode_ != Connection::Usb && ble_->enabled())
        for (const PortMeta &m : ble_->list_ports()) v.push_back(m);
    return v;
}

bool MuxSys::held_by_other(const std::string &path)
{
    if (is_ble_path(path)) return ble_ ? ble_->held_by_other(path) : false;
    return serial_.held_by_other(path);
}

int MuxSys::open(const std::string &path, OpenErr &err)
{
    if (is_ble_path(path)) {
        if (!ble_) {
            err = OpenErr::Gone;
            return -1;
        }
        return ble_->open(path, err);
    }
    return serial_.open(path, err);
}

bool MuxSys::configure(int handle, int baud)
{
    if (is_ble(handle)) return ble_ && ble_->configure(handle, baud);
    return serial_.configure(handle, baud);
}

void MuxSys::flush_input(int handle)
{
    if (is_ble(handle)) {
        if (ble_) ble_->flush_input(handle);
        return;
    }
    serial_.flush_input(handle);
}

long MuxSys::read(int handle, std::uint8_t *buf, std::size_t n)
{
    if (is_ble(handle)) return ble_ ? ble_->read(handle, buf, n) : -1;
    return serial_.read(handle, buf, n);
}

void MuxSys::restore(int handle)
{
    if (is_ble(handle)) {
        if (ble_) ble_->restore(handle);
        return;
    }
    serial_.restore(handle);
}

void MuxSys::close(int handle)
{
    if (is_ble(handle)) {
        if (ble_) ble_->close(handle);
        return;
    }
    serial_.close(handle);
}

std::string MuxSys::describe(int handle)
{
    if (is_ble(handle)) return ble_ ? ble_->describe(handle) : std::string();
    return serial_.describe(handle);
}

std::uint64_t MuxSys::ports_generation()
{
    /* the serial layer's own (0 for the real ports) plus the Bluetooth radar's comings and goings */
    return serial_.ports_generation() + (ble_ && mode_ != Connection::Usb ? ble_->ports_generation() : 0);
}

} // namespace mmw
