/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the real serial-port layer of the GPS reader (Linux): sysfs for the USB identity, flock + TIOCEXCL for
 * the lock, termios for the speed. The folders are injectable (tests point them at a fake tree). The device is opened
 * read-only: this layer cannot send a byte (see gps_port.hpp).
 */
#pragma once

#include "gps_port.hpp"

#include <map>
#include <string>
#include <vector>

namespace atlas {

struct GpsRoots {
    std::string sys = "/sys";
    std::string dev = "/dev";
};

class PosixGpsPorts : public GpsPortSys {
public:
    explicit PosixGpsPorts(const GpsRoots &roots = GpsRoots()) : roots_(roots) {}
    ~PosixGpsPorts() override;

    std::vector<PortInfo> list_ports() override;
    int open(const std::string &path, PortErr &err) override;
    bool configure(int handle, int baud) override;
    void discard_input(int handle) override;
    long read(int handle, std::uint8_t *buf, std::size_t n) override;
    void close(int handle) override;

    /* Test hook: also list this device node (a pseudo-terminal slave) as a port. */
    void add_extra_port(const std::string &path) { extra_.push_back(path); }

private:
    struct Saved;
    GpsRoots roots_;
    std::map<int, std::string> saved_;   /* handle -> the original termios as bytes */
    std::vector<std::string> extra_;
};

/* The speeds the reader offers (the Settings row and the Auto sniff): 4800, 9600, 38400, 115200. */
bool gps_baud_supported(int baud);

} // namespace atlas
