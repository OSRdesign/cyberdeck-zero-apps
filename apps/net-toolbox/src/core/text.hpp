/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: string helpers and the number formats of the results (no LVGL; tests/test_parsers.cpp).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

std::vector<std::string> split_lines(const std::string &text); /* "\r" removed, the last empty line dropped */
std::vector<std::string> split_ws(const std::string &line);    /* on blanks and tabs, empty fields dropped */
std::string trim(const std::string &s);
std::string lower(const std::string &s);                        /* ASCII only */
bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);
bool parse_u64(const std::string &s, std::uint64_t &out);       /* decimal digits only (blanks around are fine) */
bool parse_i64(const std::string &s, std::int64_t &out);        /* optional sign */
bool parse_int(const std::string &s, int &out);                 /* parse_i64 within int */
bool parse_double(const std::string &s, double &out);           /* "60." and "-52." too (/proc/net/wireless) */
bool parse_hex(const std::string &s, std::uint32_t &out);       /* "0x1003", "0144A8C0" */

/* A duration in milliseconds as the results show it: "0.4 ms", "3.2 ms", "14 ms", "1203 ms" (one decimal under 10). */
std::string fmt_ms(double ms);
/* "0%", "2.5%", "33%", "100%" (one decimal under 10, except 0) */
std::string fmt_pct(double pct);
/* An interface speed from /sys/class/net/<if>/speed: "100 Mbit/s", "1 Gbit/s", "2.5 Gbit/s"; "" when unknown (< 0). */
std::string fmt_speed(int mbps);
/* A Wi-Fi bit rate: "72.2 Mbit/s". */
std::string fmt_mbit(double mbit);

/* Printable text for a log line or a label: control bytes as "\xNN" (UTF-8 kept). */
std::string printable(const std::string &s);

} // namespace nt
