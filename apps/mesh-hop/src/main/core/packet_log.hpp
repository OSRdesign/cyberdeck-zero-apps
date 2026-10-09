/*
 * SPDX-License-Identifier: MIT
 *
 * The packet log (D8): the radio packets the board reports (PUSH_LOG_RX_DATA, decoded by logdata.hpp) kept in a bounded ring while the
 * user has capture switched on. Capture is off at every start of the app (the packets of other people's traffic are only kept when asked
 * for, and only in memory: nothing is written to disk). No LVGL, no I/O.
 */

#pragma once

#include "logdata.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace meshzero {

struct LoggedPacket {
    uint32_t time = 0;                     // app clock (UNIX seconds) when it was heard
    LogPacket pkt;
};

class PacketLog {
public:
    static constexpr size_t kCapacity = 500;

    explicit PacketLog(size_t capacity = kCapacity) : capacity_(capacity ? capacity : 1) {}
    bool capturing() const { return capturing_; }
    void start() { capturing_ = true; ++revision_; }
    void stop() { capturing_ = false; ++revision_; }
    void clear() { ring_.clear(); dropped_ = 0; ++revision_; }
    /* Keeps the packet while capturing (the oldest one goes when the ring is full); ignored otherwise. */
    void add(const LogPacket &pkt, uint32_t time);

    size_t size() const { return ring_.size(); }
    size_t capacity() const { return capacity_; }
    /* Oldest first. */
    const LoggedPacket &at(size_t i) const { return ring_[i]; }
    /* Packets that fell out of the full ring since the last clear. */
    uint64_t dropped() const { return dropped_; }
    /* Grows with every change (the UI rebuilds its rows when it moves). */
    uint32_t revision() const { return revision_; }

private:
    size_t capacity_;
    bool capturing_ = false;
    std::deque<LoggedPacket> ring_;
    uint64_t dropped_ = 0;
    uint32_t revision_ = 1;
};

} // namespace meshzero
