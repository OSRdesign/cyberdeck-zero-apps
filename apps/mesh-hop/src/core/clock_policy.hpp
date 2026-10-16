/*
 * SPDX-License-Identifier: MIT
 *
 * Board clock policy (the user's decision of 2026-10-06). The deck has no RTC: its clock is trustworthy only when it is
 * network synchronised ("online"). A board with a GPS receiver may know the time without any network.
 *
 *   deck clock   board GPS      action
 *   online       none / off     SetFromDeck   write the deck time to the board (SET_TIME)
 *   offline      none / off     PromptUser    ask for the start date and time (typed), then write it
 *   offline      enabled        UseBoardClock read the board clock (GET_TIME), write nothing
 *   online       enabled        UseBoardClock default (not decided by the user): write nothing, use the GPS clock
 *
 * Everything is a pure function so every cell is unit tested.
 */

#pragma once

#include "util.hpp"

namespace meshzero {

enum class DeckClock { Unknown, Synced, Unsynced };     // Unknown: the check has not answered (yet)
enum class ClockAction { SetFromDeck, PromptUser, UseBoardClock };
enum class ClockSource { NotSet, DeckNtp, SetByUser, BoardGps, BoardKept };

/* The one place that maps (deck state, GPS) to an action: change the table here. Unknown counts as offline. */
ClockAction decide_clock(DeckClock deck, bool gps_enabled);
const char *clock_source_name(ClockSource s);

/* "YYYY-MM-DD HH:MM" in the deck's local time zone, and back. parse accepts 2025-01-01 .. 2099-12-31. */
std::string format_local_datetime(uint32_t unix_time);
bool parse_local_datetime(const std::string &text, uint32_t &unix_time);

/* A board clock before 2025-01-01 is "not set" (a board without time starts near 1970 or at its firmware build date). */
constexpr uint32_t kMinPlausibleTime = 1735689600;

/* Whether a value of the custom var "gps" means enabled ("1", "on", "true", "yes", "enabled"). */
bool gps_value_enabled(const std::string &value);

} // namespace meshzero
