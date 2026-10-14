/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: string helpers and number formats (see text.hpp).
 */
#include "text.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sm {

bool Lines::next(std::string_view &line)
{
    if (rest_.empty()) return false;
    const std::size_t nl = rest_.find('\n');
    line = rest_.substr(0, nl);
    rest_ = nl == std::string_view::npos ? std::string_view() : rest_.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return true;
}

bool Fields::next(std::string_view &field)
{
    std::size_t a = 0;
    while (a < rest_.size() && (rest_[a] == ' ' || rest_[a] == '\t')) ++a;
    if (a == rest_.size()) {
        rest_ = std::string_view();
        return false;
    }
    std::size_t b = a;
    while (b < rest_.size() && rest_[b] != ' ' && rest_[b] != '\t') ++b;
    field = rest_.substr(a, b - a);
    rest_ = rest_.substr(b);
    return true;
}

bool Fields::skip(int n)
{
    std::string_view f;
    for (int i = 0; i < n; ++i)
        if (!next(f)) return false;
    return true;
}

std::string_view trim_view(std::string_view s)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0'; };
    while (!s.empty() && blank(s.front())) s.remove_prefix(1);
    while (!s.empty() && blank(s.back())) s.remove_suffix(1);
    return s;
}

bool to_u64(std::string_view s, std::uint64_t &out)
{
    s = trim_view(s);
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t next = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (next / 10 != v) return false; /* overflow */
        v = next;
    }
    out = v;
    return true;
}

bool to_i64(std::string_view s, std::int64_t &out)
{
    s = trim_view(s);
    if (s.empty()) return false;
    const bool neg = s[0] == '-';
    if (neg || s[0] == '+') s.remove_prefix(1);
    std::uint64_t v = 0;
    if (!to_u64(s, v) || v > static_cast<std::uint64_t>(INT64_MAX)) return false;
    out = neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
    return true;
}

bool to_double(std::string_view s, double &out)
{
    s = trim_view(s);
    if (s.empty() || s.size() >= 64) return false;
    char buf[64]; /* strtod needs a terminated copy: on the stack, no heap */
    s.copy(buf, s.size());
    buf[s.size()] = '\0';
    char *end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == buf || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::vector<std::string> split_ws(const std::string &line)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r' || s[a] == '\0')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == '\0'))
        --b;
    return s.substr(a, b - a);
}

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool parse_u64(const std::string &s, std::uint64_t &out) { return to_u64(s, out); }
bool parse_i64(const std::string &s, std::int64_t &out) { return to_i64(s, out); }
bool parse_double(const std::string &s, double &out) { return to_double(s, out); }

bool parse_hex(const std::string &s, std::uint32_t &out)
{
    std::string t = trim(s);
    if (starts_with(t, "0x") || starts_with(t, "0X")) t = t.substr(2);
    if (t.empty() || t.size() > 8) return false;
    std::uint32_t v = 0;
    for (const char c : t) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    out = v;
    return true;
}

namespace {

const char *const kUnits[] = {"B", "kB", "MB", "GB", "TB"};

/* index of the unit for a value, and the value in it */
int unit_of(double bytes, double &scaled)
{
    int u = 0;
    scaled = bytes;
    while (scaled >= 1024.0 && u < 4) {
        scaled /= 1024.0;
        ++u;
    }
    return u;
}

std::string number_in(double scaled, int unit)
{
    char buf[32];
    if (unit == 0 || scaled == 0.0) std::snprintf(buf, sizeof(buf), "%.0f", scaled);
    else if (scaled < 9.95) std::snprintf(buf, sizeof(buf), "%.1f", scaled);
    else std::snprintf(buf, sizeof(buf), "%.0f", scaled);
    return buf;
}

} // namespace

std::string fmt_bytes(std::uint64_t bytes)
{
    double scaled = 0;
    const int u = unit_of(static_cast<double>(bytes), scaled);
    return number_in(scaled, u) + " " + kUnits[u];
}

std::string fmt_kb(std::uint64_t kb) { return fmt_bytes(kb * 1024ull); }

std::string fmt_used_of(std::uint64_t used_bytes, std::uint64_t total_bytes)
{
    double total = 0;
    const int u = unit_of(static_cast<double>(total_bytes), total);
    double used = static_cast<double>(used_bytes);
    for (int i = 0; i < u; ++i) used /= 1024.0;
    return number_in(used, u) + " / " + number_in(total, u) + " " + kUnits[u];
}

std::string fmt_rate(double bytes_per_s)
{
    if (!(bytes_per_s > 0)) return "0 B/s";
    double scaled = 0;
    const int u = unit_of(bytes_per_s, scaled);
    return number_in(scaled, u) + " " + kUnits[u] + "/s";
}

std::string fmt_pct(double pct)
{
    if (!(pct > 0)) pct = 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", pct);
    return buf;
}

std::string fmt_temp(double celsius)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C", celsius);
    return buf;
}

std::string fmt_temp_short(double celsius)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.0f \xC2\xB0" "C", celsius);
    return buf;
}

std::string fmt_mhz(int mhz) { return std::to_string(mhz) + " MHz"; }

std::string fmt_volts(double v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f V", v);
    return buf;
}

std::string fmt_uptime(double seconds)
{
    if (!(seconds > 0)) seconds = 0;
    const std::uint64_t s = static_cast<std::uint64_t>(seconds);
    const std::uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    char buf[48];
    if (d > 0) std::snprintf(buf, sizeof(buf), "%llu d %llu h", static_cast<unsigned long long>(d),
                             static_cast<unsigned long long>(h));
    else if (h > 0) std::snprintf(buf, sizeof(buf), "%llu h %02llu min", static_cast<unsigned long long>(h),
                                  static_cast<unsigned long long>(m));
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%llu min", static_cast<unsigned long long>(m));
    else std::snprintf(buf, sizeof(buf), "%llu s", static_cast<unsigned long long>(s));
    return buf;
}

std::string fmt_since_boot(double seconds)
{
    if (!(seconds > 0)) seconds = 0;
    const std::uint64_t s = static_cast<std::uint64_t>(seconds);
    const std::uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, sec = s % 60;
    char buf[48];
    if (d > 0) std::snprintf(buf, sizeof(buf), "%llu d %02llu:%02llu:%02llu", static_cast<unsigned long long>(d),
                             static_cast<unsigned long long>(h), static_cast<unsigned long long>(m),
                             static_cast<unsigned long long>(sec));
    else std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu", static_cast<unsigned long long>(h),
                       static_cast<unsigned long long>(m), static_cast<unsigned long long>(sec));
    return buf;
}

std::string fmt_window(int samples, int period_s)
{
    const int s = samples * (period_s > 0 ? period_s : 1);
    if (s >= 120 && s % 60 == 0) return std::to_string(s / 60) + " min";
    return std::to_string(s) + " s";
}

} // namespace sm
