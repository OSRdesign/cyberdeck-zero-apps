/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the display texts of the disk guard (see disk_guard.hpp).
 */
#include "disk_guard.hpp"

#include <cstdio>

namespace vhs {

std::string bytes_text(std::uint64_t b)
{
    char buf[32];
    if (b >= 1000ull * 1000 * 1000) std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(b) / 1e9);
    else if (b >= 1000ull * 1000) std::snprintf(buf, sizeof buf, "%llu MB", static_cast<unsigned long long>((b + 500000) / 1000000));
    else std::snprintf(buf, sizeof buf, "%llu KB", static_cast<unsigned long long>((b + 500) / 1000));
    return buf;
}

std::string time_left_text(std::int64_t s)
{
    if (s < 0) return "";
    char buf[48];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld h %lld min left", static_cast<long long>(s / 3600), static_cast<long long>((s % 3600) / 60));
    else if (s >= 60) std::snprintf(buf, sizeof buf, "%lld min left", static_cast<long long>(s / 60));
    else std::snprintf(buf, sizeof buf, "%lld s left", static_cast<long long>(s));
    return buf;
}

std::string clock_text(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    const long long s = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    return buf;
}

} // namespace vhs
