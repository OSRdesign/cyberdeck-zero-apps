/*
 * SPDX-License-Identifier: MIT
 */
#include "dates.hpp"

#include <cstdio>
#include <cstring>

namespace mdnotes {

namespace {

std::string fmt(std::time_t t, const char *f)
{
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return std::string();
    char buf[64];
    const std::size_t n = std::strftime(buf, sizeof buf, f, &tmv);
    return std::string(buf, n);
}

/* Days since the epoch of the local calendar day of t. */
long long local_day(std::time_t t)
{
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return 0;
    /* days from civil (Howard Hinnant's algorithm), from the local Y-M-D */
    long long y = tmv.tm_year + 1900;
    const long long m = tmv.tm_mon + 1, d = tmv.tm_mday;
    if (m <= 2) --y;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

} // namespace

std::string date_text(std::time_t t) { return fmt(t, "%Y-%m-%d"); }
std::string stamp_text(std::time_t t) { return fmt(t, "%Y-%m-%d %H:%M"); }
std::string conflict_stamp(std::time_t t) { return fmt(t, "%Y-%m-%d %H%M"); }
std::string daily_note_file(std::time_t now) { return date_text(now) + ".md"; }

int build_year(const char *date)
{
    if (!date || std::strlen(date) < 10) return 0;
    for (int i = 0; i < 4; ++i)
        if (date[i] < '0' || date[i] > '9') return 0;
    if (date[4] != '-' || date[7] != '-') return 0;
    return (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
}

bool clock_looks_unsynced(std::time_t now, int year)
{
    if (year <= 0) return false;
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) return true;
    return tmv.tm_year + 1900 < year;
}

std::string age_text(std::int64_t then_s, std::int64_t now_s)
{
    const std::int64_t d = now_s - then_s;
    if (d < -60) return date_text(static_cast<std::time_t>(then_s));
    if (d < 60) return "just now";
    if (d < 3600) return std::to_string(d / 60) + " min ago";
    if (d < 12 * 3600) return std::to_string(d / 3600) + " h ago";
    const long long days = local_day(static_cast<std::time_t>(now_s)) - local_day(static_cast<std::time_t>(then_s));
    if (days <= 0) return std::to_string(d / 3600) + " h ago";
    if (days == 1) return "yesterday";
    if (days < 7) return std::to_string(days) + " days ago";
    return date_text(static_cast<std::time_t>(then_s));
}

} // namespace mdnotes
