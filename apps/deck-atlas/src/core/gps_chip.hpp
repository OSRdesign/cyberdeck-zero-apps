/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: what the GPS chip on the map shows, decided from the status (gps_fix.hpp), no drawing. The chip is a
 * satellite dish with three reception bars; the number of lit bars carries the level, the colour backs it up:
 *
 *   Hidden       GPS off: nothing is drawn.
 *   NoReceiver   no device, no access, only unknown serial ports, or a port that sends no valid sentence: the dish
 *                crossed out, three outlined bars, grey (0 lit).
 *   Searching    the receiver is connected (valid checksummed sentences within the last 3 s, not merely an open port)
 *                and has no fix: 1 bar, red.
 *   Weak         a 2D fix, or a 3D fix with weak geometry (HDOP above 5, fewer than 5 satellites used, or either number
 *                unknown): 2 bars, orange.
 *   Good         a 3D fix with HDOP <= 5 and at least 5 satellites used: 3 bars, green.
 *   stale        a fix whose last valid fix sentence is older than 5 s: the bars of the level it had, dimmed to 40 %,
 *                with a small "!" mark.
 *
 * Pure: no LVGL.
 */
#pragma once

#include "gps_fix.hpp"

#include <string>

namespace atlas {

constexpr double kGoodHdopMax = 5.0;   /* HDOP above this: weak geometry */
constexpr int kGoodSatsMin = 5;        /* fewer satellites used than this: weak geometry */

enum class ChipLevel { Hidden, NoReceiver, Searching, Weak, Good };

struct ChipState {
    ChipLevel level = ChipLevel::Hidden;
    int bars = 0;               /* lit bars: 0 (NoReceiver, Hidden), 1, 2 or 3 */
    bool crossed = false;       /* the dish is crossed out (NoReceiver) */
    bool stale = false;         /* dimmed to 40 % with a "!" mark (a fix older than kStaleMs) */
    bool operator==(const ChipState &o) const {
        return level == o.level && bars == o.bars && crossed == o.crossed && stale == o.stale;
    }
    bool operator!=(const ChipState &o) const { return !(*this == o); }
};

/* A 3D fix is good when HDOP <= kGoodHdopMax and at least kGoodSatsMin satellites are used; either unknown: not good. */
bool fix_geometry_good(const Fix &f);

/* The chip for a status. now_ms is the clock of the status's own times (receiver_alive was set by GpsLink::status). */
ChipState chip_state(const GpsStatus &s);

/* A plain-words description of a chip state (the status page's headline, a log line): "GPS off", "No GPS receiver",
 * "Receiver connected, no fix yet", "Fix with weak geometry", "Good 3D fix", with ", signal lost" when stale. */
std::string chip_description(const ChipState &c);

} // namespace atlas
