/*
 * SPDX-License-Identifier: MIT
 */

#include "position.hpp"

#include <cmath>
#include <cstdio>

namespace meshzero {

std::string parse_coordinate(const std::string &text, bool latitude, double &out)
{
    const char *what = latitude ? "Latitude" : "Longitude";
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
    if (a == b) return std::string("Type the ") + (latitude ? "latitude" : "longitude");
    const std::string bad = std::string(what) + ": use decimal degrees, for example " + (latitude ? "48.8566" : "2.3522");
    if (b - a > 24) return bad;
    // parsed by hand: strtod would also take "inf", "nan", hex and exponents, and depends on the locale
    size_t i = a;
    bool neg = false;
    if (text[i] == '+' || text[i] == '-') neg = text[i++] == '-';
    double v = 0, scale = 1;
    int digits = 0;
    bool point = false;
    for (; i < b; ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            if (point) { scale /= 10; v += (c - '0') * scale; }
            else v = v * 10 + (c - '0');
            ++digits;
        } else if ((c == '.' || c == ',') && !point) {
            point = true;
        } else {
            return bad;
        }
    }
    if (digits == 0) return bad;
    if (neg) v = -v;
    const double lim = latitude ? 90.0 : 180.0;
    if (v < -lim || v > lim) return std::string(what) + (latitude ? " must be between -90 and 90" : " must be between -180 and 180");
    out = v == 0 ? 0.0 : v;                     // no "-0"
    return "";
}

std::string parse_position(const std::string &lat_text, const std::string &lon_text, double &lat, double &lon)
{
    double a = 0, o = 0;
    std::string bad = parse_coordinate(lat_text, true, a);
    if (bad.empty()) bad = parse_coordinate(lon_text, false, o);
    if (!bad.empty()) return bad;
    lat = a;
    lon = o;
    return "";
}

std::string check_position(double lat, double lon)
{
    if (!std::isfinite(lat) || lat < -90.0 || lat > 90.0) return "Latitude must be between -90 and 90";
    if (!std::isfinite(lon) || lon < -180.0 || lon > 180.0) return "Longitude must be between -180 and 180";
    return "";
}

std::string fmt_coordinate(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6f", std::round(v * 1e6) / 1e6);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

bool position_box_has_gps(const DeviceInfo *dev, const BoardCaps &caps)
{
    return feature_state(Feature::BoardGps, dev, caps).available;
}

} // namespace meshzero
