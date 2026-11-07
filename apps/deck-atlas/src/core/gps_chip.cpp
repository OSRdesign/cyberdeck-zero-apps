/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS chip's state (see gps_chip.hpp).
 */
#include "gps_chip.hpp"

namespace atlas {

bool fix_geometry_good(const Fix &f)
{
    return f.kind == 3 && f.hdop >= 0 && f.hdop <= kGoodHdopMax && f.sats_used >= kGoodSatsMin;
}

ChipState chip_state(const GpsStatus &s)
{
    ChipState c;
    switch (s.kind) {
    case GpsStatus::Kind::Off: return c;
    case GpsStatus::Kind::NoDevice:
    case GpsStatus::Kind::NoReceiver:
    case GpsStatus::Kind::NoAccess:
        c.level = ChipLevel::NoReceiver;
        c.crossed = true;
        return c;
    case GpsStatus::Kind::Searching:
        if (!s.receiver_alive) {
            c.level = ChipLevel::NoReceiver; /* a port that is open but says nothing valid is no receiver (yet) */
            c.crossed = true;
        } else {
            c.level = ChipLevel::Searching;
            c.bars = 1;
        }
        return c;
    default: break;
    }
    if (fix_geometry_good(s.fix)) {
        c.level = ChipLevel::Good;
        c.bars = 3;
    } else {
        c.level = ChipLevel::Weak;
        c.bars = 2;
    }
    c.stale = s.stale;
    return c;
}

std::string chip_description(const ChipState &c)
{
    std::string t;
    switch (c.level) {
    case ChipLevel::Hidden: return "GPS off";
    case ChipLevel::NoReceiver: return "No GPS receiver";
    case ChipLevel::Searching: return "Receiver connected, no fix yet";
    case ChipLevel::Weak: t = "Fix with weak geometry"; break;
    case ChipLevel::Good: t = "Good 3D fix"; break;
    }
    if (c.stale) t += ", signal lost";
    return t;
}

} // namespace atlas
