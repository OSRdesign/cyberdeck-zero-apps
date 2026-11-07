/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the sky the receiver reports. SkyTracker folds the GSV and GSA sentences (nmea.hpp) into a satellite table
 * (constellation, PRN, elevation, azimuth, SNR, used in the fix, last seen) and the per-talker GSA reports (mode, the PRNs
 * used, the DOPs). The GPS status page reads the result through GpsStatus (gps_fix.hpp: sats, gsa, dop); nothing here
 * draws or does I/O, the clock is a parameter (CLOCK_MONOTONIC in ms).
 *
 * Rules.
 *  - GSV: a group of messages (total, number, up to four satellites each) per talker (and signal id). A group is complete
 *    when its message number equals its total; the satellites of a complete group are entered into the table at that
 *    moment, a group that lost a message (numbers not in sequence) or restarted is dropped whole, so a half-heard group
 *    never shows. A satellite not reported for kSatKeepMs (10 s) is dropped. SNR -1 (empty field) = in view, not tracked.
 *  - Constellation: GP GPS, GL GLONASS, GA Galileo, GB or BD BeiDou, GQ QZSS; under GPS (talker GP, or a GN GSA with system
 *    id 1) PRN 33-64 and 120-158 are SBAS satellites. A GN talker (GSA only) without a system id is told by PRN range
 *    (1-32 GPS, 33-64 and 120-158 SBAS, 65-96 GLONASS, 193-202 QZSS, 301-336 Galileo, 401-437 BeiDou).
 *  - GSA: every talker sends its own report (a GN receiver one per constellation, the system id field of NMEA 4.1 says
 *    which); a report counts for kGsaFreshMs (3 s). A satellite is "used" when a fresh report with mode 2 or 3 lists its
 *    PRN for its constellation. The DOPs are those of the newest fresh report that has them.
 *  - Several signals (NMEA 4.1 signal id) of one satellite: the strongest SNR heard within 2 s is kept.
 */
#pragma once

#include "nmea.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace atlas {

enum class Constellation { Unknown, Gps, Glonass, Galileo, Beidou, Qzss, Sbas };

/* G R E C J S, '?' for Unknown. */
char constellation_letter(Constellation c);
/* "GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", "SBAS", "Unknown". */
const char *constellation_name(Constellation c);
/* The constellation of a satellite: talker ("GP", "GL", ...), PRN and the GSA system id (0 absent). */
Constellation constellation_of(const char talker[3], int prn, int system_id = 0);

constexpr std::int64_t kSatKeepMs = 10000;   /* a satellite not reported for this long leaves the table */
constexpr std::int64_t kGsaFreshMs = 3000;   /* a GSA report counts for this long */

struct SkySat {
    Constellation sys = Constellation::Unknown;
    int prn = 0;
    int elev = -1;               /* degrees, -1 unknown */
    int azim = -1;               /* degrees, -1 unknown */
    int snr = -1;                /* dB-Hz, -1 = in view but not tracked (empty field) */
    bool used = false;           /* in the position solution */
    std::int64_t seen_ms = 0;    /* monotonic time of the group that last reported it */
    int signal = 0;              /* NMEA 4.1 signal id of the value held, 0 = none / single signal */
};

/* One GSA sentence as the receiver sent it (the newest per talker and system id). */
struct GsaReport {
    char talker[3] = {0, 0, 0};
    int system_id = 0;           /* 0 absent */
    Constellation sys = Constellation::Unknown; /* of the system id, else of the talker; Unknown for GN without an id */
    int mode = 0;                /* 1 no fix, 2 2D, 3 3D */
    bool manual = false;
    std::vector<int> used;       /* PRNs */
    double pdop = -1, hdop = -1, vdop = -1;   /* -1 unknown */
    std::int64_t ms = 0;
};

struct Dop {
    double pdop = -1, hdop = -1, vdop = -1;   /* -1 unknown */
};

class SkyTracker {
public:
    void on_msg(const NmeaMsg &m, std::int64_t now_ms);
    void reset();

    /* The table at now_ms: satellites reported within kSatKeepMs, the used flags from the fresh GSA reports; ordered by
     * constellation (GPS, GLONASS, Galileo, BeiDou, QZSS, SBAS, Unknown) then PRN. */
    std::vector<SkySat> satellites(std::int64_t now_ms) const;
    /* The fresh GSA reports (kGsaFreshMs), ordered by talker then system id. */
    std::vector<GsaReport> reports(std::int64_t now_ms) const;
    /* The DOPs of the newest fresh report that has them, else all -1. */
    Dop dop(std::int64_t now_ms) const;

private:
    struct Group {
        int total = 0, next = 1, signal = 0;
        std::vector<GsvSat> sats;
    };
    void commit(const std::string &talker, const Group &g, std::int64_t now_ms);

    std::map<std::pair<int, int>, SkySat> table_;      /* (constellation, PRN) */
    std::map<std::string, Group> groups_;              /* GSV groups being heard, by talker and signal id */
    std::map<std::string, GsaReport> gsa_;             /* by talker and system id */
};

} // namespace atlas
