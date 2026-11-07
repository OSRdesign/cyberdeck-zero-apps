/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: NMEA 0183, read only. A line splitter for the bytes of a serial port, the checksum, and the sentences
 * the map needs: RMC (position, speed, course, date), GGA (position, quality, satellites used, HDOP, altitude), GSA
 * (mode, the PRNs used per constellation, PDOP / HDOP / VDOP) and GSV (the satellites in view: up to four per sentence
 * with PRN, elevation, azimuth and SNR, and where the sentence stands in its group of messages; gps_sky.hpp folds the
 * groups into a satellite table). Any talker ($GP, $GN, $GL, $GA, $GB, $BD, $GQ ...) counts. Strict:
 * a sentence without a valid "*HH" checksum is rejected; empty fields are fine. Pure: no I/O, no clock.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atlas {

enum class MsgKind { None, Rmc, Gga, Gsa, Gsv, Other };

/* One satellite block of a GSV sentence. -1: the field was empty (SNR empty = the satellite is not tracked). */
struct GsvSat {
    int prn = 0;
    int elev = -1;     /* degrees above the horizon, 0..90 */
    int azim = -1;     /* degrees from true north, 0..359 */
    int snr = -1;      /* dB-Hz, 0..99 */
};

struct NmeaMsg {
    MsgKind kind = MsgKind::None;   /* None: not a valid sentence (bad checksum, malformed) */
    char talker[3] = {0, 0, 0};     /* "GP", "GN", ...; "" for a proprietary sentence ($PUBX) */
    std::string type;               /* "RMC", "GGA", ...; for Other the whole address ("PUBX") */

    /* RMC and GGA: a fix. RMC: status A (and a mode other than N); GGA: quality > 0 */
    bool valid = false;
    bool has_pos = false;
    double lat = 0, lon = 0;        /* degrees, south and west negative */
    bool has_time = false;
    int hour = 0, minute = 0;
    double second = 0;
    bool has_date = false;          /* RMC only */
    int day = 0, month = 0, year = 0; /* year with the century (2000 + yy) */
    bool has_speed = false;
    double speed_mps = 0;           /* RMC (knots converted) */
    bool has_course = false;
    double course_deg = 0;          /* RMC; empty at a standstill */

    /* GGA */
    int quality = 0;
    bool has_sats = false;
    int sats = 0;                   /* satellites used */
    bool has_hdop = false;
    double hdop = 0;                /* GGA and GSA */
    bool has_alt = false;
    double alt_m = 0;

    /* GSA */
    int mode = 0;                   /* 1 no fix, 2 2D, 3 3D; 0 unknown */
    bool gsa_manual = false;        /* the receiver was told to use 2D / 3D only (M); false: automatic (A) or unknown */
    int system_id = 0;              /* NMEA 4.1: which constellation this GSA is for (1 GPS, 2 GLONASS, 3 Galileo, 4 BeiDou,
                                       5 QZSS); 0 absent */
    std::vector<int> used_prns;     /* the satellites in the solution (up to 12 fields, the empty ones skipped) */
    bool has_pdop = false;
    double pdop = 0;
    bool has_vdop = false;
    double vdop = 0;                /* HDOP is has_hdop / hdop above (GGA and GSA) */

    /* GSV */
    bool has_in_view = false;
    int in_view = 0;                /* satellites in view (all the messages of the group together) */
    int gsv_total = 0;              /* messages in the group, 1..9; 0: the GSV header is unusable */
    int gsv_num = 0;                /* this message's number, 1..gsv_total */
    int gsv_signal = 0;             /* NMEA 4.1 signal id (the field after the last satellite block); 0 absent */
    std::vector<GsvSat> gsv_sats;   /* the satellite blocks of this message, up to four, empty-PRN blocks skipped */
};

/* The XOR of the characters between '$' and '*'. */
std::uint8_t nmea_checksum(const std::string &body);
/* "$" + body + "*HH" (upper-case hex): what the tests and the demonstration feed use. */
std::string nmea_frame(const std::string &body);
/* A whole line without its line end, "$....*HH": the checksum is present and right. */
bool nmea_checksum_ok(const std::string &line);
/* Parses one line (no CR/LF); kind None when the checksum is missing or wrong or the line is malformed. */
NmeaMsg parse_nmea(const std::string &line);

/* Splits a byte stream into lines. Garbage before a '$' is dropped, a '$' inside a line restarts it, a line longer
 * than kMaxLine bytes is dropped. Each completed line (CR, LF or CRLF ends it) is appended to `out` as text without
 * the line end. */
class NmeaSplitter {
public:
    static constexpr std::size_t kMaxLine = 120;
    void feed(const std::uint8_t *data, std::size_t n, std::vector<std::string> &out);
    void reset();

private:
    std::string line_;
    bool in_line_ = false;
};

} // namespace atlas
