/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: port identity and title (see port_sys.hpp).
 */
#include "port_sys.hpp"

#include <cstdio>

namespace mmw {

std::string port_id(const PortMeta &m)
{
    if (m.transport == Transport::Ble) return "ble:" + m.address;
    if (!m.by_id.empty()) return m.by_id;
    if (m.vid || m.pid) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "usb-%04x:%04x", m.vid, m.pid);
        return buf;
    }
    return m.tty;
}

std::string port_title(const PortMeta &m)
{
    if (m.transport == Transport::Ble) return (m.tty.empty() ? m.address : m.tty) + " (Bluetooth)";
    const char *chip = nullptr;
    if (m.vid == 0x1a86 && m.pid == 0x7523) chip = "CH340";
    else if (m.vid == 0x1a86 && (m.pid == 0x55d4 || m.pid == 0x55d3)) chip = "CH9102";
    else if (m.vid == 0x10c4 && m.pid == 0xea60) chip = "CP2102";
    else if (m.vid == 0x0403 && (m.pid == 0x6001 || m.pid == 0x6015)) chip = "FTDI";
    else if (m.vid == 0x067b && m.pid == 0x2303) chip = "PL2303";
    if (chip) return m.tty + " (" + chip + ")";
    if (!m.product.empty()) return m.tty + " (" + m.product + ")";
    return m.tty;
}

bool valid_ble_address(const std::string &a)
{
    if (a.size() != 17) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char c = a[i];
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace mmw
