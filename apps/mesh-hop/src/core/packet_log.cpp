/*
 * SPDX-License-Identifier: MIT
 */

#include "packet_log.hpp"

namespace meshzero {

void PacketLog::add(const LogPacket &pkt, uint32_t time)
{
    if (!capturing_) return;
    ring_.push_back({time, pkt});
    while (ring_.size() > capacity_) {
        ring_.pop_front();
        ++dropped_;
    }
    ++revision_;
}

} // namespace meshzero
