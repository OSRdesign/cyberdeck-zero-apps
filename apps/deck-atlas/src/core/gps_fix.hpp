/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: what the receiver says, as a position. FixTracker folds the NMEA sentences (nmea.hpp) into one Fix; the
 * status (no device, searching, 2D, 3D, stale) and the status chip's text come from the link's state, the tracker and
 * the clock. Pure: the clock is a parameter (CLOCK_MONOTONIC in ms, the engine's cp0_map_monotonic_ms()).
 */
#pragma once

#include "nmea.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace atlas {

constexpr std::int64_t kStaleMs = 5000;        /* no valid fix sentence for this long: the fix is stale */
constexpr double kMovingMps = 1.0;             /* the speed (and the heading wedge) show above this */

/* A position the way the map and position.state need it. */
struct Fix {
    int kind = 0;                 /* 0 none, 2 = 2D, 3 = 3D */
    double lat = 0, lon = 0;
    bool has_alt = false;
    double alt_m = 0;
    double speed_mps = -1;        /* < 0 unknown */
    double course_deg = -1;       /* < 0 unknown (empty at a standstill) */
    double hdop = -1;             /* < 0 unknown */
    int sats_used = -1;           /* < 0 unknown */
    int sats_view = -1;           /* < 0 unknown */
    std::string utc;              /* "2026-11-07T10:15:30Z" from the receiver's date and time, "" unknown */
    std::int64_t fix_ms = 0;      /* monotonic time the last valid fix sentence arrived (0: none yet) */
};

class FixTracker {
public:
    void on_msg(const NmeaMsg &m, std::int64_t now_ms);
    void reset();
    const Fix &fix() const { return fix_; }

private:
    int mode_now(std::int64_t now_ms) const;   /* the best GSA mode seen within the last 3 s, 0 unknown */
    int in_view_now(std::int64_t now_ms) const;
    void set_fix(const NmeaMsg &m, std::int64_t now_ms);

    Fix fix_;
    struct Seen {
        int value = 0;
        std::int64_t ms = 0;
    };
    std::map<std::string, Seen> gsa_, gsv_;   /* GSA by talker and constellation (a GN receiver sends one per constellation), GSV by talker */
    bool alt_known_ = false;
};

/* Horizontal accuracy in metres from the HDOP: HDOP x 5 m (a typical user range error), at least 3 m, at most 100 m;
 * 10 m when the HDOP is unknown. */
double accuracy_from_hdop(double hdop);

enum class GpsLinkState { Off, NoDevice, NoReceiver, NoAccess, Searching, Locked };

struct GpsStatus {
    enum class Kind { Off, NoDevice, NoReceiver, NoAccess, Searching, Fix2D, Fix3D };
    Kind kind = Kind::Off;
    bool stale = false;           /* a 2D / 3D fix whose last valid sentence is older than kStaleMs */
    std::int64_t age_ms = 0;      /* of the fix; 0 when there is none */
    Fix fix;                      /* valid for Fix2D / Fix3D */
    bool has_fix() const { return kind == Kind::Fix2D || kind == Kind::Fix3D; }
};

GpsStatus make_status(GpsLinkState link, const FixTracker &tracker, std::int64_t now_ms);

/* The status chip: "" (GPS off: no chip), "No GPS device", "No GPS receiver found" (Auto sees only serial ports that are no known receiver), "No access to GPS port", "Searching", "2D fix", "3D fix 9
 * sats", "Last fix 12 s ago" (stale); a moving fix adds ", 12 km/h" (", 8 mph" with miles). */
std::string chip_text(const GpsStatus &s, bool miles);

} // namespace atlas
