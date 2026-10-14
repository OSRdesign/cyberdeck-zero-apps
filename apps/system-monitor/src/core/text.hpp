/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: string helpers and the number formats the pages show (no LVGL; tests/test_parsers.cpp).
 *
 * Sizes are binary (1 MB = 1024 x 1024 bytes, as `free -m` and `df -h` count), written kB / MB / GB; one decimal under
 * 10, none from 10 up ("9.8 MB", "212 MB", "1.2 GB", "29 GB").
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sm {

/* ---------------------------------------------------------------- no-copy scanning (the sampler's parsers) */

/* The lines of a text, "\r" dropped, without copies. */
class Lines {
public:
    explicit Lines(std::string_view text) : rest_(text) {}
    bool next(std::string_view &line);

private:
    std::string_view rest_;
};

/* The blank-separated fields of a line, without copies. */
class Fields {
public:
    explicit Fields(std::string_view line) : rest_(line) {}
    bool next(std::string_view &field);
    bool skip(int n); /* n fields; false when the line ends first */

private:
    std::string_view rest_;
};

std::string_view trim_view(std::string_view s);
bool to_u64(std::string_view s, std::uint64_t &out);
bool to_i64(std::string_view s, std::int64_t &out);
bool to_double(std::string_view s, double &out);

/* ---------------------------------------------------------------- strings */

std::vector<std::string> split_lines(const std::string &text); /* "\r" removed, the last empty line dropped */
std::vector<std::string> split_ws(const std::string &line);    /* on blanks and tabs, empty fields dropped */
std::string trim(const std::string &s);
bool starts_with(const std::string &s, const std::string &prefix);
bool parse_u64(const std::string &s, std::uint64_t &out);      /* decimal digits only (a trailing blank is fine) */
bool parse_i64(const std::string &s, std::int64_t &out);       /* optional sign */
bool parse_double(const std::string &s, double &out);          /* "60." and "-52." too (/proc/net/wireless) */
/* "0x50005", "50005" (hex without prefix) */
bool parse_hex(const std::string &s, std::uint32_t &out);

/* ---------------------------------------------------------------- numbers */

std::string fmt_bytes(std::uint64_t bytes);           /* "512 B", "9.8 kB", "212 MB", "1.2 GB" */
std::string fmt_kb(std::uint64_t kb);                 /* a /proc/meminfo value (kB) */
/* "used / total unit" with one unit, the total's: "212 / 416 MB", "5.2 / 29 GB", "0 / 100 MB" */
std::string fmt_used_of(std::uint64_t used_bytes, std::uint64_t total_bytes);
std::string fmt_rate(double bytes_per_s);             /* "0 B/s", "512 B/s", "12.3 kB/s", "1.2 MB/s" */
std::string fmt_pct(double pct);                      /* "23%" (rounded, 0..100 kept, more is shown) */
std::string fmt_temp(double celsius);                 /* "54.2 °C" (UTF-8 degree sign) */
std::string fmt_temp_short(double celsius);           /* "54 °C" */
std::string fmt_mhz(int mhz);                         /* "1000 MHz" */
std::string fmt_volts(double v);                      /* "5.08 V", "0.86 V" */
/* time since boot, coarse: "45 s", "12 min", "2 h 05 min", "3 d 4 h" */
std::string fmt_uptime(double seconds);
/* a moment after boot, exact to the second: "0:41:12", "1 d 02:41:12" */
std::string fmt_since_boot(double seconds);
/* the chart's time window: 120 samples x period: "2 min", "4 min", "10 min", "90 s" */
std::string fmt_window(int samples, int period_s);

} // namespace sm
