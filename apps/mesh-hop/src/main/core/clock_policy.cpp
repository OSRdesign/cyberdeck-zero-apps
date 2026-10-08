/*
 * SPDX-License-Identifier: MIT
 */

#include "clock_policy.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>

namespace meshzero {

ClockAction decide_clock(DeckClock deck, bool gps_enabled)
{
    const bool online = deck == DeckClock::Synced;
    if (gps_enabled) return ClockAction::UseBoardClock;     // offline + GPS (decided) and online + GPS (default: no write)
    return online ? ClockAction::SetFromDeck : ClockAction::PromptUser;
}

const char *clock_source_name(ClockSource s)
{
    switch (s) {
    case ClockSource::DeckNtp: return "deck NTP";
    case ClockSource::SetByUser: return "set by you";
    case ClockSource::BoardGps: return "board GPS";
    case ClockSource::BoardKept: return "board clock, unchanged";
    case ClockSource::NotSet: return "not set";
    }
    return "";
}

std::string format_local_datetime(uint32_t unix_time)
{
    const time_t t = static_cast<time_t>(unix_time);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}

bool parse_local_datetime(const std::string &text, uint32_t &out)
{
    int y, mo, d, h, mi;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d%c", &y, &mo, &d, &h, &mi, &tail) != 5) return false;
    if (y < 2025 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    struct tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_isdst = -1;
    const time_t t = mktime(&tmv);
    if (t == static_cast<time_t>(-1) || tmv.tm_mday != d || tmv.tm_mon != mo - 1) return false;   // 31 February and the like
    out = static_cast<uint32_t>(t);
    return true;
}

bool gps_value_enabled(const std::string &value)
{
    std::string v;
    for (char c : value) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return v == "1" || v == "on" || v == "true" || v == "yes" || v == "enabled";
}

} // namespace meshzero
