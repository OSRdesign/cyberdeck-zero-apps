/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: places and typed values (see place.hpp).
 */
#include "place.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace viz {

namespace {

std::string trim(const std::string &text)
{
    std::size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n')) --b;
    return text.substr(a, b - a);
}

} // namespace

bool parse_degrees(const std::string &text, double limit, double &out)
{
    const std::string t = trim(text);
    /* a plain decimal number: an optional sign, digits, at most one '.', at least one digit (strtod alone would take
     * "nan", "inf", "1e2" and hex) */
    std::size_t i = 0, digits = 0, dots = 0;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
    for (; i < t.size(); ++i) {
        if (t[i] >= '0' && t[i] <= '9') ++digits;
        else if (t[i] == '.' && ++dots == 1) continue;
        else return false;
    }
    if (digits == 0) return false;
    char *end = nullptr;
    const double value = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(value) || std::fabs(value) > limit) return false;
    out = value;
    return true;
}

std::string degrees_text(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.5f", value);
    return buffer;
}

std::string short_degrees(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    std::string s = buffer;
    if (s == "-0.0000") s = "0.0000";
    return s;
}

std::string coords_text(double lat, double lon, int decimals)
{
    if (decimals == 4) return short_degrees(lat) + ", " + short_degrees(lon);
    const int d = decimals < 0 ? 0 : decimals > 6 ? 6 : decimals;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f, %.*f", d, lat, d, lon);
    return buffer;
}

std::string place_text(const Place &place)
{
    const std::string coords = coords_text(place.lat, place.lon);
    return place.code.empty() ? coords : place.code + "  " + coords;
}

bool same_place(const Place &a, const Place &b)
{
    if (!a.code.empty() && !b.code.empty()) return a.code == b.code;
    return std::fabs(a.lat - b.lat) < 0.0001 && std::fabs(a.lon - b.lon) < 0.0001 && a.code == b.code;
}

std::vector<Place> push_recent(std::vector<Place> list, const Place &place)
{
    std::vector<Place> out;
    out.push_back(place);
    for (const Place &p : list) {
        if (out.size() >= kRecentMax) break;
        if (!same_place(p, place)) out.push_back(p);
    }
    return out;
}

std::string filter_code(const std::string &typed)
{
    std::string out;
    for (const char c : typed) {
        if (out.size() >= kCodeMax) break;
        if (c >= 'a' && c <= 'z') out += static_cast<char>(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') out += c;
    }
    return out;
}

std::string filter_degrees(const std::string &typed)
{
    std::string out;
    bool dot = false;
    for (const char c : typed) {
        if (out.size() >= kDegreesMax) break;
        if (c >= '0' && c <= '9') out += c;
        else if (c == '-' && out.empty()) out += c;
        else if ((c == '.' || c == ',') && !dot) { /* a decimal comma is taken as the point */
            dot = true;
            out += '.';
        }
    }
    return out;
}

std::string latitude_error(const std::string &text)
{
    double v = 0;
    if (trim(text).empty()) return "Type a latitude from -90 to 90";
    return parse_degrees(text, kLatLimit, v) ? std::string() : "Not a latitude: from -90 to 90";
}

std::string longitude_error(const std::string &text)
{
    double v = 0;
    if (trim(text).empty()) return "Type a longitude from -180 to 180";
    return parse_degrees(text, kLonLimit, v) ? std::string() : "Not a longitude: from -180 to 180";
}

} // namespace viz
