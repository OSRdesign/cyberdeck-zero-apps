/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the injectable serial-port layer of the GPS reader (the pattern of mmWave Radar's port_sys.hpp, written
 * again for this app). The link (gps_link.hpp) uses only this interface, so the tests run it on a fake port
 * (tests/fake_gps_port.hpp) and the app on the real one (gps_posix.hpp).
 *
 * LISTEN-ONLY BY CONSTRUCTION: the interface has no call that sends a byte and none that touches the modem lines.
 * Nothing in the app can send a UBX or NMEA command to the receiver. (Opening a tty raises DTR and RTS in the
 * kernel; that is outside the app's hands. The real layer opens the device read-only, so even a slip could not send.)
 * tests/test_package.cpp greps the GPS sources for any send call.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atlas {

enum class PortErr { None, Busy, Denied, Gone, Other };

struct PortInfo {
    std::string path;      /* "/dev/ttyACM0" */
    std::string tty;       /* "ttyACM0" */
    std::string by_id;     /* the /dev/serial/by-id link name, "" when none */
    std::string product;   /* the USB product string, "" when unknown */
    std::uint16_t vid = 0, pid = 0;
};

class GpsPortSys {
public:
    virtual ~GpsPortSys() {}
    /* The candidate ports, in tty name order (ttyACM2 before ttyACM10): /dev/ttyACM* and /dev/ttyUSB*. */
    virtual std::vector<PortInfo> list_ports() = 0;
    /* Opens read-only, non-blocking, no controlling terminal, takes flock(LOCK_EX | LOCK_NB) and TIOCEXCL. A handle
     * >= 0, or -1 with err: Busy (EBUSY or the lock is held: another app has it), Denied (EACCES / EPERM: the user
     * is not in the dialout group), Gone (ENOENT / ENODEV / ENXIO), Other (not a tty, ...). */
    virtual int open(const std::string &path, PortErr &err) = 0;
    /* Raw 8N1 at baud, nothing else changed (the first call keeps the original settings for close()). */
    virtual bool configure(int handle, int baud) = 0;
    /* Drops what the kernel has buffered but not delivered yet (after a change of speed). */
    virtual void discard_input(int handle) = 0;
    /* Non-blocking: > 0 bytes read, 0 nothing yet, < 0 the port is gone (EIO, ENODEV, ENXIO, a hung-up tty). */
    virtual long read(int handle, std::uint8_t *buf, std::size_t n) = 0;
    /* Puts back the settings open() found, releases the lock and closes. */
    virtual void close(int handle) = 0;
};

} // namespace atlas
