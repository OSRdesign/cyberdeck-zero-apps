/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: date parsing for feeds (RFC 822 / 2822 with numeric or named zones, RFC 3339 / ISO 8601, and the odd
 * variants real feeds produce). Pure and total.
 */
#pragma once

#include <cstdint>
#include <string>

namespace rssreader {

/* Seconds since 1970-01-01 UTC for the date in `text`; false when it is not a date in 1970..2100.
 * A date without a zone is taken as UTC; a date without a time is midnight UTC. A bare 10-digit number is an epoch in
 * seconds, a 13-digit one in milliseconds. */
bool parse_date(const std::string &text, std::int64_t &epoch);

/* "2026-10-18 15:10" (UTC) for logs and tests. */
std::string format_utc(std::int64_t epoch);

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
std::int64_t days_from_civil(int y, int m, int d);

} // namespace rssreader
