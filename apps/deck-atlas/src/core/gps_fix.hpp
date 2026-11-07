/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: what the receiver says, as a position. FixTracker folds the NMEA sentences (nmea.hpp) into one Fix and the
 * sky (gps_sky.hpp); the status (no device, searching, 2D, 3D, stale) comes from the link's state, the tracker and the
 * clock. GpsStatus is what the GPS status page and the chip (gps_chip.hpp) read: the fix, the satellite table, the GSA
 * reports and DOPs, and the facts of the receiver link (port, speed, device, sentence rate, bad checksums). Pure: the
 * clock is a parameter (CLOCK_MONOTONIC in ms, the engine's cp0_map_monotonic_ms()).
 */
#pragma once

#include "gps_sky.hpp"
#include "nmea.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
    const SkyTracker &sky() const { return sky_; }

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
    SkyTracker sky_;
};

/* Horizontal accuracy in metres from the HDOP: HDOP x 5 m (a typical user range error), at least 3 m, at most 100 m;
 * 10 m when the HDOP is unknown. */
double accuracy_from_hdop(double hdop);

enum class GpsLinkState { Off, NoDevice, NoReceiver, NoAccess, Searching, Locked };

constexpr std::int64_t kReceiverAliveMs = 3000;  /* a receiver counts as connected while valid sentences arrive this often */

/* How the port in use was chosen. */
enum class PortChoice { None, AutoKnown, ByUser };

/* The receiver link as the status page shows it. All empty / 0 while no port is open. */
struct GpsLinkFacts {
    std::string port;                   /* "/dev/ttyACM0"; "" = no port open */
    std::string device;                 /* the USB product string, "" when unknown */
    std::uint16_t vid = 0, pid = 0;     /* the USB id, 0 for a UART / unknown */
    int baud = 0;                       /* the speed in use; 0 = none */
    PortChoice choice = PortChoice::None; /* AutoKnown: Port = Auto found a known receiver; ByUser: the Port setting */
    bool baud_auto = false;             /* the speed was found by listening (Speed = Auto), not set */
    double sentences_per_s = 0;         /* valid sentences per second over the last 3 s (shorter just after the port opened) */
    std::int64_t last_valid_ms = 0;     /* monotonic time of the last valid (checksummed) sentence on this port; 0 = none yet */
    int bad_checksums = 0;              /* sentences that failed the checksum since the port (or the speed) was set */
};

/* Valid sentences arrived within kReceiverAliveMs: judged by the sentences, not by the port being open. */
bool receiver_alive(const GpsLinkFacts &f, std::int64_t now_ms);

struct GpsStatus {
    enum class Kind { Off, NoDevice, NoReceiver, NoAccess, Searching, Fix2D, Fix3D };
    Kind kind = Kind::Off;
    bool stale = false;           /* a 2D / 3D fix whose last valid sentence is older than kStaleMs */
    std::int64_t age_ms = 0;      /* of the fix; 0 when there is none */
    Fix fix;                      /* valid for Fix2D / Fix3D */
    bool has_fix() const { return kind == Kind::Fix2D || kind == Kind::Fix3D; }

    /* the sky and the link (filled while the link is Locked / has a port) */
    bool receiver_alive = false;         /* receiver_alive(link, now) with a port open */
    std::vector<SkySat> sats;            /* the satellite table, ordered by constellation then PRN */
    std::vector<GsaReport> gsa;          /* the fresh GSA reports, per talker and system id */
    Dop dop;                             /* PDOP / HDOP / VDOP of the newest report that has them, -1 unknown */
    GpsLinkFacts link;
};

/* The status of the tracker's fix and sky at now_ms (the link facts and receiver_alive are the link's to add: GpsLink::status). */
GpsStatus make_status(GpsLinkState link, const FixTracker &tracker, std::int64_t now_ms);

/* The status in words (logs, and the status page's headline; the map's chip is an icon, gps_chip.hpp): "" (GPS off), "No GPS device", "No GPS receiver found" (Auto sees only serial ports that are no known receiver), "No access to GPS port", "Searching", "2D fix", "3D fix 9
 * sats", "Last fix 12 s ago" (stale); a moving fix adds ", 12 km/h" (", 8 mph" with miles). */
std::string chip_text(const GpsStatus &s, bool miles);

} // namespace atlas
