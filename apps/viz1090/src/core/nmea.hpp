/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the NMEA 0183 sentences of a USB GPS receiver, as far as the intro needs them: is this port talking
 * NMEA (any GGA, RMC, GLL, GSV, GSA or VTG sentence of any talker), and is there a fix (GGA with a fix quality above 0,
 * or RMC with status A) at which latitude / longitude. A sentence with a checksum (*hh) that does not match is ignored
 * (a port read at the wrong speed gives garbage). Pure C++, no LVGL, no I/O: unit-tested on recorded sentences.
 */
#pragma once

#include <cstddef>
#include <string>

namespace viz {

struct NmeaResult {
    bool seen = false; /* at least one NMEA sentence */
    bool fix = false;  /* a position */
    double lat = 0, lon = 0;
    int sats = 0;      /* from GGA (0 when the fix came from RMC) */
};

/* "4851.396" + "N" -> 48.8566; S and W are negative. 0 for a value too short to be one. */
double nmea_degrees(const std::string &value, const std::string &hemisphere);

/* true when the line has no checksum, or a checksum that matches */
bool nmea_checksum_ok(const std::string &line);

/* One line (without its end of line; a trailing CR is ignored): updates `result`. Returns true when the line was an
 * NMEA sentence the intro knows. A later fix replaces an earlier one. */
bool nmea_feed_line(const std::string &line, NmeaResult &result);

/* Bytes as they come from the port: whole lines are fed, the rest of a line waits in `pending` (capped at 256 bytes:
 * no NMEA line is longer than 82). */
void nmea_feed_bytes(std::string &pending, const char *data, std::size_t size, NmeaResult &result);

} // namespace viz
