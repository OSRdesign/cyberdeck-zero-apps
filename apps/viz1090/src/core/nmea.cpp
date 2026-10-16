/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: NMEA sentences (see nmea.hpp). The rules are the 0.1 intro screen's, plus the checksum and a range
 * check of the position.
 */
#include "nmea.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace viz {

namespace {

std::vector<std::string> split(const std::string &line, char separator)
{
    std::vector<std::string> out;
    std::string current;
    for (const char c : line) {
        if (c == separator) {
            out.push_back(current);
            current.clear();
        } else if (c != '\r' && c != '\n') {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

double nmea_degrees(const std::string &value, const std::string &hemisphere)
{
    if (value.size() < 4) return 0;
    const double raw = std::atof(value.c_str());
    const double degrees = std::floor(raw / 100.0);
    double result = degrees + (raw - degrees * 100.0) / 60.0;
    if (hemisphere == "S" || hemisphere == "W") result = -result;
    return result;
}

bool nmea_checksum_ok(const std::string &line)
{
    const std::size_t star = line.find('*');
    if (star == std::string::npos) return true;
    if (star + 3 > line.size()) return false; /* fewer than two characters after '*' */
    const int hi = hex_digit(line[star + 1]), lo = hex_digit(line[star + 2]);
    if (hi < 0 || lo < 0) return false;
    unsigned sum = 0;
    for (std::size_t i = 1; i < star; ++i) sum ^= static_cast<unsigned char>(line[i]);
    return sum == static_cast<unsigned>(hi * 16 + lo);
}

bool nmea_feed_line(const std::string &raw, NmeaResult &result)
{
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.size() < 8 || line[0] != '$') return false;
    if (!nmea_checksum_ok(line)) return false;
    const auto f = split(line.substr(0, line.find('*')), ',');
    if (f[0].size() < 6) return false;
    const std::string kind = f[0].substr(3);
    if (kind != "GGA" && kind != "RMC" && kind != "GLL" && kind != "GSV" && kind != "GSA" && kind != "VTG") return false;
    result.seen = true;
    double lat = 0, lon = 0;
    if (kind == "GGA" && f.size() > 7 && std::atoi(f[6].c_str()) > 0 && !f[2].empty() && !f[4].empty()) {
        lat = nmea_degrees(f[2], f[3]);
        lon = nmea_degrees(f[4], f[5]);
        if (std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0) {
            result.fix = true;
            result.lat = lat;
            result.lon = lon;
            result.sats = std::atoi(f[7].c_str());
        }
    } else if (kind == "RMC" && f.size() > 6 && f[2] == "A" && !f[3].empty() && !f[5].empty()) {
        lat = nmea_degrees(f[3], f[4]);
        lon = nmea_degrees(f[5], f[6]);
        if (std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0) {
            result.fix = true;
            result.lat = lat;
            result.lon = lon;
        }
    }
    return true;
}

void nmea_feed_bytes(std::string &pending, const char *data, std::size_t size, NmeaResult &result)
{
    pending.append(data, size);
    std::size_t newline;
    while ((newline = pending.find('\n')) != std::string::npos) {
        nmea_feed_line(pending.substr(0, newline), result);
        pending.erase(0, newline + 1);
    }
    if (pending.size() > 256) pending.erase(0, pending.size() - 256);
}

} // namespace viz
