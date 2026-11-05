/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the injectable port layer (filesystem, locks, termios). The detector and the link use only this
 * interface, so tests run them on fakes (tests/fake_sys.hpp) and the app on posix_sys.hpp.
 *
 * LISTEN-ONLY BY CONSTRUCTION: the interface has no write call and no modem-line call. Nothing in the app can send a
 * byte to a port or toggle DTR/RTS (open() itself raises them: Linux's tty layer does that for the first opener, and
 * drops them again at the last close when HUPCL is set; restore() puts the original termios back, HUPCL included).
 * The CH340 (ch341 driver) wires DTR/RTS to its DTR#/RTS# pins; the radar board's 4-pin connector (5V, RX, TX, GND)
 * has no use for them. A microcontroller board with auto-reset could restart when opened: scanning is limited to
 * ttyUSB* and ttyACM*, and an unknown board is only opened once per scan.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mmw {

enum class OpenErr { None, Busy, Denied, Gone, Other };

struct PortMeta {
    std::string tty;        /* "ttyUSB0" */
    std::string path;       /* "/dev/ttyUSB0" */
    std::string by_id;      /* the /dev/serial/by-id link name, "" when none */
    std::uint16_t vid = 0, pid = 0;
    std::string product;    /* the USB product string */
    std::string driver;     /* ch341, cp210x, ftdi_sio, cdc_acm, ... */
};

/* The stable key the remembered port is kept under: the by-id name, else "usb-vvvv:pppp", else the tty name. */
std::string port_id(const PortMeta &m);
/* "ttyUSB0 (CH340)": the chip for the well known bridges, else the product string, else just the tty name. */
std::string port_title(const PortMeta &m);

class Sys {
public:
    virtual ~Sys() {}
    /* The candidate ports, by tty name order: /dev/ttyUSB* and /dev/ttyACM* (ttyUSB2 before ttyUSB10). */
    virtual std::vector<PortMeta> list_ports() = 0;
    /* True when another process (not this one) has the device open (/proc/<pid>/fd), as far as we can see. */
    virtual bool held_by_other(const std::string &path) = 0;
    /* Opens O_RDWR|O_NOCTTY|O_NONBLOCK, takes flock(LOCK_EX|LOCK_NB) and TIOCEXCL. A handle >= 0, or -1 with err:
     * Busy (EBUSY or the lock is held: "held by another app"), Denied (EACCES/EPERM), Gone (ENOENT/ENODEV/ENXIO). */
    virtual int open(const std::string &path, OpenErr &err) = 0;
    /* Raw 8N1 at baud, nothing else changed (the first call keeps the original settings for restore()). */
    virtual bool configure(int handle, int baud) = 0;
    virtual void flush_input(int handle) = 0;
    /* Non-blocking: >0 bytes read, 0 nothing yet, <0 the port is gone (EIO, ENODEV, ENXIO, EOF on a dead device). */
    virtual long read(int handle, std::uint8_t *buf, std::size_t n) = 0;
    /* Puts back the settings open() found; a failure is ignored (the port may be gone). */
    virtual void restore(int handle) = 0;
    /* Releases the lock and closes. */
    virtual void close(int handle) = 0;
};

} // namespace mmw
