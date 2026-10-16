/*
 * SPDX-License-Identifier: MIT
 *
 * The message archive (M7): search the history kept on the deck (up to 200 messages per conversation, 1500 in all) by text, by
 * conversation, by date range and by direction. A hit names the conversation and the message so the app can open it. No LVGL, no I/O.
 */

#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace meshzero {

enum class SearchDir { Any, Received, Sent };
enum class DatePreset { Any, Day, Week, Month, Quarter, Custom };
const char *search_dir_name(SearchDir d);           // "All", "Received", "Sent"
const char *date_preset_name(DatePreset p);         // "Any date", "Last 24 h", "Last 7 days", "Last 30 days", "Last 90 days", "Custom range"

struct SearchQuery {
    std::string text;               // matched in the message text and in the sender's name; empty = every message
    std::string conv;               // a conversation key ("c:0", "d:<hex>"); empty = all
    uint32_t from = 0, to = 0;      // app clock, both inclusive; 0 = open ended. A message with an unknown time never passes a range.
    SearchDir dir = SearchDir::Any;
    bool empty() const { return text.empty() && conv.empty() && from == 0 && to == 0 && dir == SearchDir::Any; }
};

struct SearchHit {
    uint32_t seq = 0;
    std::string conv;
    std::string title;              // conversation title
    uint32_t ts = 0;
    bool outgoing = false;
    std::string sender;
    std::string text;
};

/* Newest first, at most `limit` hits; *total (if given) is the number of messages that matched. */
std::vector<SearchHit> search_messages(const Model &model, const SearchQuery &q, size_t limit = 300, size_t *total = nullptr);

/* Case-insensitive for ASCII; other bytes (accents) must match exactly. An empty needle matches. */
bool text_contains_nocase(const std::string &hay, const std::string &needle);

/* The range a preset stands for at `now`: from = now - span. Any and Custom give 0, 0. now before 2025 (clock unknown): 0, 0. */
void date_preset_range(DatePreset p, uint32_t now, uint32_t &from, uint32_t &to);
/* A typed range: "2026-09-01" (that day) or "2026-09-01 2026-09-30" (both days included, local time). "" when valid, else the reason. */
std::string parse_date_range(const std::string &text, uint32_t &from, uint32_t &to);
/* "2026-09-01 to 2026-09-30" for the filter button. */
std::string fmt_date_range(uint32_t from, uint32_t to);

} // namespace meshzero
