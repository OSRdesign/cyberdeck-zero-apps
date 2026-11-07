/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the texts the pages show (see format.hpp).
 */
#include "format.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <utility>

namespace atlas {

std::string bytes_label(std::uint64_t bytes)
{
    char buf[32];
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
    const double b = static_cast<double>(bytes);
    if (bytes < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else if (b < mb) std::snprintf(buf, sizeof buf, "%.0f KB", std::floor(b / kb + 0.5));
    else if (b < 10 * mb) std::snprintf(buf, sizeof buf, "%.1f MB", b / mb);
    else if (b < gb) std::snprintf(buf, sizeof buf, "%.0f MB", std::floor(b / mb + 0.5));
    else std::snprintf(buf, sizeof buf, "%.1f GB", b / gb);
    std::string s = buf;
    /* "1024 KB" -> "1.0 MB", "1024 MB" -> "1.0 GB" after rounding */
    if (s == "1024 KB") s = "1.0 MB";
    if (s == "1024 MB") s = "1.0 GB";
    return s;
}

std::string count_label(std::uint64_t n)
{
    std::string digits = std::to_string(n), out;
    const int len = static_cast<int>(digits.size());
    for (int i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0) out += ',';
        out += digits[static_cast<std::size_t>(i)];
    }
    return out;
}

std::string zoom_label(int zmin, int zmax)
{
    if (zmin < 0 || zmax < 0) return std::string();
    if (zmin > zmax) std::swap(zmin, zmax);
    if (zmin == zmax) return "z" + std::to_string(zmin);
    return "z" + std::to_string(zmin) + "-" + std::to_string(zmax);
}

const char *units_label(Units u) { return u == Units::Mi ? "mi" : "km"; }

namespace {
const char *scale_text(float s)
{
    const float v = snap_scale(s);
    if (v == 1.0f) return "1x";
    if (v == 1.5f) return "1.5x";
    if (v == 2.0f) return "2x";
    return "";
}
} // namespace

std::string scale_label(float setting, float in_use)
{
    if (snap_scale(setting) == 0.0f) {
        const char *u = scale_text(in_use);
        return *u ? std::string("Auto (") + u + ")" : std::string("Auto");
    }
    return scale_text(setting);
}

std::string cache_label(int mb, std::uint64_t cap_bytes)
{
    if (mb == 0) return cap_bytes ? "Auto (" + bytes_label(cap_bytes) + ")" : std::string("Auto");
    if (mb >= 1024 && mb % 1024 == 0) return std::to_string(mb / 1024) + " GB";
    return std::to_string(mb) + " MB";
}

const char *on_off(bool on) { return on ? "On" : "Off"; }

const char *gps_label(bool on) { return on ? "Auto" : "Off"; }
std::string gps_port_label(const std::string &port, bool present)
{
    if (port.empty()) return "Auto";
    return present ? port : port + " (absent)";
}
std::string gps_speed_label(int baud) { return baud == 0 ? std::string("Auto") : std::to_string(baud); }

std::string latlon_label(double lat, double lon)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f %c, %.3f %c", std::fabs(lat), lat < 0 ? 'S' : 'N', std::fabs(lon),
                  lon < 0 ? 'W' : 'E');
    return buf;
}

namespace {
std::string span(double a, double b, char neg, char pos)
{
    char buf[48];
    if ((a < 0) == (b < 0)) std::snprintf(buf, sizeof buf, "%.2f-%.2f %c", std::fabs(a), std::fabs(b), a < 0 ? neg : pos);
    else std::snprintf(buf, sizeof buf, "%.2f %c-%.2f %c", std::fabs(a), neg, std::fabs(b), pos);
    return buf;
}
} // namespace

std::string bounds_label(double west, double south, double east, double north)
{
    /* latitudes from south to north; on the south side both are S and listed from the smaller number */
    std::string lat;
    if (south < 0 && north < 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.2f-%.2f S", -north, -south);
        lat = buf;
    } else {
        lat = span(south, north, 'S', 'N');
    }
    std::string lon;
    if (west < 0 && east < 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.2f-%.2f W", -east, -west);
        lon = buf;
    } else {
        lon = span(west, east, 'W', 'E');
    }
    return lat + ", " + lon;
}

int utf8_chars(const std::string &s)
{
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

namespace {
/* byte offset of the character index i (s.size() past the end) */
std::size_t char_offset(const std::string &s, int i)
{
    int n = 0;
    for (std::size_t b = 0; b < s.size(); ++b) {
        if ((static_cast<unsigned char>(s[b]) & 0xC0) != 0x80) {
            if (n == i) return b;
            ++n;
        }
    }
    return s.size();
}
} // namespace

std::string ellipsize_middle(const std::string &s, int max_chars)
{
    const int n = utf8_chars(s);
    if (n <= max_chars) return s;
    if (max_chars <= 3) return std::string("...").substr(0, static_cast<std::size_t>(max_chars < 0 ? 0 : max_chars));
    const int keep = max_chars - 3;
    const int head = keep / 2, tail = keep - head;
    return s.substr(0, char_offset(s, head)) + "..." + s.substr(char_offset(s, n - tail));
}

std::string tile_rate_label(double per_s)
{
    char buf[40];
    if (!(per_s > 0) || !std::isfinite(per_s)) return std::string();
    if (per_s >= 10) std::snprintf(buf, sizeof buf, "%s tiles/s", count_label(static_cast<std::uint64_t>(per_s + 0.5)).c_str());
    else std::snprintf(buf, sizeof buf, "%.1f tiles/s", per_s);
    return buf;
}

std::string byte_rate_label(double per_s)
{
    if (!(per_s > 0) || !std::isfinite(per_s)) return std::string();
    return bytes_label(static_cast<std::uint64_t>(per_s)) + "/s";
}

std::string eta_label(double seconds)
{
    if (!(seconds >= 0) || !std::isfinite(seconds)) return std::string();
    char buf[40];
    const long s = static_cast<long>(seconds + 0.5);
    if (s < 60) std::snprintf(buf, sizeof buf, "about %ld s", s < 1 ? 1L : s);
    else if (s < 3600) std::snprintf(buf, sizeof buf, "about %ld min", (s + 30) / 60);
    else std::snprintf(buf, sizeof buf, "about %ld h %ld min", s / 3600, (s % 3600) / 60);
    return buf;
}

std::string date_label(const std::string &iso)
{
    if (iso.size() >= 10 && iso[4] == '-' && iso[7] == '-') return iso.substr(0, 10);
    return iso;
}

std::string iso_utc(std::int64_t epoch)
{
    if (epoch <= 0) return std::string();
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0) return std::string();
#else
    if (!gmtime_r(&t, &tm)) return std::string();
#endif
    char buf[96]; /* any int field fits: no truncation warning, the text is 20 characters for a real date */
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace atlas
