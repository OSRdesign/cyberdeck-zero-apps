/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: the date and time texts (local time, the TZ of the process). The boards have no RTC: before
 * NTP the clock can be years off, hence clock_looks_unsynced().
 */
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace mdnotes {

std::string date_text(std::time_t t);         /* "2026-10-24" */
std::string stamp_text(std::time_t t);        /* "2026-10-24 14:03": what Ctrl+T / Insert date and time inserts */
std::string conflict_stamp(std::time_t t);    /* "2026-10-24 1530": the name of a Keep both copy */
/* Today's note at the vault root, as Obsidian names it by default: "2026-10-24.md". */
std::string daily_note_file(std::time_t now);

/* The year of a "YYYY-MM-DD" build date (CDK_BUILD_DATE), 0 when it is not one. */
int build_year(const char *date);
/* The clock is older than the build: it was not set yet (no RTC, no NTP). False when build_year is 0. */
bool clock_looks_unsynced(std::time_t now, int build_year);

/* How long ago `then` was, for a list row: "just now", "5 min ago", "2 h ago", "yesterday", "3 days ago", then the date
 * ("2026-09-01"). Hours under 12 win over "yesterday"; a time in the future (a clock set back) gives the date. */
std::string age_text(std::int64_t then_s, std::int64_t now_s);

} // namespace mdnotes
