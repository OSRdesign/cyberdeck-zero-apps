/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: string helpers (see text.hpp).
 */
#include "text.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace nt {

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::size_t end = nl;
        if (end > start && text[end - 1] == '\r') --end;
        out.push_back(text.substr(start, end - start));
        start = nl + 1;
    }
    return out;
}

std::vector<std::string> split_ws(const std::string &line)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) ++i;
        const std::size_t start = i;
        while (i < line.size() && !(line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool parse_u64(const std::string &text, std::uint64_t &out)
{
    const std::string s = trim(text);
    if (s.empty() || s.size() > 20) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool parse_i64(const std::string &text, std::int64_t &out)
{
    std::string s = trim(text);
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.erase(0, 1);
    }
    std::uint64_t v = 0;
    if (!parse_u64(s, v)) return false;
    if (!neg && v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    if (neg && v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u) return false;
    out = neg ? static_cast<std::int64_t>(0 - v) : static_cast<std::int64_t>(v);
    return true;
}

bool parse_int(const std::string &s, int &out)
{
    std::int64_t v = 0;
    if (!parse_i64(s, v) || v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_double(const std::string &text, double &out)
{
    const std::string s = trim(text);
    if (s.empty() || s.size() > 64) return false;
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parse_hex(const std::string &text, std::uint32_t &out)
{
    std::string s = trim(text);
    if (starts_with(s, "0x") || starts_with(s, "0X")) s.erase(0, 2);
    if (s.empty() || s.size() > 8) return false;
    std::uint32_t v = 0;
    for (const char c : s) {
        std::uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

std::string fmt_ms(double ms)
{
    if (!(ms >= 0)) ms = 0;
    char buf[32];
    if (ms < 9.95) std::snprintf(buf, sizeof(buf), "%.1f ms", ms);
    else std::snprintf(buf, sizeof(buf), "%.0f ms", ms);
    return buf;
}

std::string fmt_pct(double pct)
{
    if (!(pct >= 0)) pct = 0;
    char buf[32];
    if (pct == 0) return "0%";
    if (pct < 9.95) std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
    else std::snprintf(buf, sizeof(buf), "%.0f%%", pct);
    return buf;
}

std::string fmt_speed(int mbps)
{
    if (mbps <= 0) return std::string();
    char buf[32];
    if (mbps >= 1000 && mbps % 1000 == 0) std::snprintf(buf, sizeof(buf), "%d Gbit/s", mbps / 1000);
    else if (mbps >= 1000) std::snprintf(buf, sizeof(buf), "%.1f Gbit/s", mbps / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%d Mbit/s", mbps);
    return buf;
}

std::string fmt_mbit(double mbit)
{
    char buf[32];
    if (mbit == std::floor(mbit)) std::snprintf(buf, sizeof(buf), "%.0f Mbit/s", mbit);
    else std::snprintf(buf, sizeof(buf), "%.1f Mbit/s", mbit);
    return buf;
}

std::string printable(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out;
}

} // namespace nt
