/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: recognises what ELSE talks on a serial port, from the bytes it sends alone (the detector never writes).
 * Used to say "not a supported radar: GPS receiver (NMEA)" and to leave such a device alone.
 *
 *   NMEA     $...*hh sentences with a valid checksum ("$G.." = GPS receiver)
 *   UBX      B5 62 class id len(2) payload ck_a ck_b with a valid Fletcher checksum (u-blox GPS)
 *   LD2410   F4 F3 F2 F1 len(2) data F8 F7 F6 F5 (Hi-Link presence radar, not supported yet: the stub driver, ld2410.hpp;
 *            looked for only while set_stubs(true), the default: a fixed Sensor type leaves the stubs out)
 *
 * The tracking frame itself is the driver decoder's job (driver.hpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mmw {

enum class Sniffed { None, Nmea, Gps, Ubx, Ld2410 };

class Sniffer {
public:
    void feed(const std::uint8_t *data, std::size_t n);
    /* The first kind recognised (stays once found). */
    Sniffed kind() const { return kind_; }
    /* "GPS receiver (NMEA)", "GPS receiver (u-blox UBX)", "LD2410 presence radar (not supported yet)"; "" for None. */
    std::string describe() const;
    /* A description of what was heard when nothing was recognised: "no data", "only zeros", "text output (a console or
     * a log)", "unknown data". */
    std::string describe_unknown() const;
    std::size_t bytes() const { return total_; }
    /* Whether the stub drivers (known but unsupported models) are recognised; kept by reset(). Default true. */
    void set_stubs(bool on) { stubs_ = on; }
    void reset();

private:
    void scan();
    std::string win_;        /* the last bytes, capped */
    std::size_t total_ = 0;
    std::size_t zeros_ = 0, printable_ = 0;
    Sniffed kind_ = Sniffed::None;
    bool stubs_ = true;
};

} // namespace mmw
