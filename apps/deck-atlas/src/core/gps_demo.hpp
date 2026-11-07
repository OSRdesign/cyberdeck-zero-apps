/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: a GPS port that is not one (demonstration mode only: the scenes and screenshots, never on a board).
 * It serves a text file of NMEA sentences as if a receiver sent them, one line at a time:
 *   $GPRMC,...*5A        a sentence (sent as it stands; its line end is added)
 *   #wait 1500           pauses the feed for 1500 ms (relative to the one before)
 *   # anything else      a comment
 * Without a file the list of ports is empty ("No GPS device"). The file is made by tests/tools/gen_nmea.py from an
 * invented coordinate (never a recording). Every speed is accepted.
 */
#pragma once

#include "gps_port.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace atlas {

class DemoGpsPorts : public GpsPortSys {
public:
    /* feed_path "" or unreadable: no device. */
    explicit DemoGpsPorts(const std::string &feed_path);

    std::vector<PortInfo> list_ports() override;
    int open(const std::string &path, PortErr &err) override;
    bool configure(int, int) override { return true; }
    void discard_input(int) override {}
    long read(int handle, std::uint8_t *buf, std::size_t n) override;
    void close(int handle) override;

private:
    std::vector<std::pair<std::int64_t, std::string>> feed_;   /* (due ms after the open, bytes) */
    bool opened_ = false;
    std::chrono::steady_clock::time_point t0_;
    std::size_t next_ = 0;
    std::string pending_;
    std::size_t pending_off_ = 0;
};

} // namespace atlas
