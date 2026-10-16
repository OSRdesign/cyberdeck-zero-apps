/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: a place (the position viz1090 centres its map on) and the rules of the values the user types.
 * Pure C++, no LVGL: unit-tested by tests/test_core.cpp.
 *
 *   Place          an airport code (may be empty) and a latitude / longitude in degrees
 *   parse_degrees  "48.8566", " -2.5 " -> a number within +-limit (90 for a latitude, 180 for a longitude)
 *   texts          the config file keeps 5 decimals ("48.85660", as the 0.1 intro did); the screen shows 4
 *   recent         the last places used (most recent first, at most kRecentMax, no duplicates)
 *   input filters  what a field keeps of what was typed: an airport code (letters, upper case, at most 4), a number of
 *                  degrees (digits, one leading '-', one '.')
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace viz {

constexpr std::size_t kRecentMax = 5;
constexpr std::size_t kCodeMax = 4;     /* IATA (3 letters) or ICAO (4) */
constexpr std::size_t kDegreesMax = 12; /* "-179.123456" and a little more */
constexpr double kLatLimit = 90.0;
constexpr double kLonLimit = 180.0;

struct Place {
    std::string code; /* "" = a position without an airport */
    double lat = 0;
    double lon = 0;
};

/* text -> degrees within +-limit; leading / trailing spaces allowed, nothing else after the number */
bool parse_degrees(const std::string &text, double limit, double &out);

/* "48.85660": what the config file keeps (5 decimals, as the first intro screen wrote it) */
std::string degrees_text(double value);
/* "48.8566": what the screen shows */
std::string short_degrees(double value);
/* "49.0097, 2.5478" (decimals 4), "49.01, 2.55" (decimals 2: the short form of a narrow row) */
std::string coords_text(double lat, double lon, int decimals = 4);
/* "CDG  49.0097, 2.5478" (two spaces after the code), or "49.0097, 2.5478" without a code */
std::string place_text(const Place &place);

/* the same place: the same code (both set), else coordinates within 0.0001 degree */
bool same_place(const Place &a, const Place &b);
/* `place` first, an older copy of it removed, at most kRecentMax */
std::vector<Place> push_recent(std::vector<Place> list, const Place &place);

/* an airport code as typed: letters only, upper case, at most kCodeMax */
std::string filter_code(const std::string &typed);
/* a number of degrees as typed: digits, '-' only first, one '.', at most kDegreesMax characters */
std::string filter_degrees(const std::string &typed);

/* "" when text is a latitude / longitude, else the reason (one line) */
std::string latitude_error(const std::string &text);
std::string longitude_error(const std::string &text);

} // namespace viz
