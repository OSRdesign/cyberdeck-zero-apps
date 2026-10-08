/*
 * SPDX-License-Identifier: MIT
 */

#include "archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace meshzero {

const char *search_dir_name(SearchDir d)
{
    switch (d) {
    case SearchDir::Any: return "All";
    case SearchDir::Received: return "Received";
    case SearchDir::Sent: return "Sent";
    }
    return "";
}

const char *date_preset_name(DatePreset p)
{
    switch (p) {
    case DatePreset::Any: return "Any date";
    case DatePreset::Day: return "Last 24 h";
    case DatePreset::Week: return "Last 7 days";
    case DatePreset::Month: return "Last 30 days";
    case DatePreset::Quarter: return "Last 90 days";
    case DatePreset::Custom: return "Custom range";
    }
    return "";
}

bool text_contains_nocase(const std::string &hay, const std::string &needle)
{
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto low = [](unsigned char c) { return static_cast<unsigned char>(c < 0x80 ? std::tolower(c) : c); };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && low(static_cast<unsigned char>(hay[i + k])) == low(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

std::vector<SearchHit> search_messages(const Model &model, const SearchQuery &q, size_t limit, size_t *total)
{
    std::vector<SearchHit> hits;
    size_t matched = 0;
    const auto &all = model.all_messages();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {                  // the store keeps the messages in order: newest from the back
        const Message &m = *it;
        if (!q.conv.empty() && m.conv != q.conv) continue;
        if (q.dir == SearchDir::Received && m.dir != Dir::In) continue;
        if (q.dir == SearchDir::Sent && m.dir != Dir::Out) continue;
        if (q.from != 0 || q.to != 0) {
            if (m.ts < kMinPlausibleTime) continue;
            if (q.from != 0 && m.ts < q.from) continue;
            if (q.to != 0 && m.ts > q.to) continue;
        }
        if (!q.text.empty() && !text_contains_nocase(m.text, q.text) && !text_contains_nocase(m.sender, q.text)) continue;
        ++matched;
        if (hits.size() < limit) {
            SearchHit h;
            h.seq = m.seq;
            h.conv = m.conv;
            h.title = model.conv_title(m.conv);
            h.ts = m.ts;
            h.outgoing = m.dir == Dir::Out;
            h.sender = m.sender;
            h.text = m.text;
            hits.push_back(std::move(h));
        }
    }
    if (total) *total = matched;
    return hits;
}

void date_preset_range(DatePreset p, uint32_t now, uint32_t &from, uint32_t &to)
{
    from = to = 0;
    if (now < kMinPlausibleTime) return;
    uint32_t span = 0;
    switch (p) {
    case DatePreset::Day: span = 86400; break;
    case DatePreset::Week: span = 7 * 86400; break;
    case DatePreset::Month: span = 30 * 86400; break;
    case DatePreset::Quarter: span = 90 * 86400; break;
    default: return;
    }
    from = now > span ? now - span : 0;
}

namespace {

bool parse_day(const std::string &s, bool end_of_day, uint32_t &out)
{
    uint32_t t = 0;
    if (!parse_local_datetime(s + (end_of_day ? " 23:59" : " 00:00"), t)) return false;
    out = end_of_day ? t + 59 : t;
    return true;
}

} // namespace

std::string parse_date_range(const std::string &text, uint32_t &from, uint32_t &to)
{
    // split on spaces
    std::vector<std::string> parts;
    std::string cur;
    for (char c : text) {
        if (c == ' ') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty() || parts.size() > 2) return "Type a day (2026-09-01) or two days (2026-09-01 2026-09-30)";
    uint32_t a = 0, b = 0;
    if (!parse_day(parts[0], false, a)) return "The first day is not valid: use YYYY-MM-DD (2025 or later)";
    if (parts.size() == 1) {
        if (!parse_day(parts[0], true, b)) return "The day is not valid";
    } else if (!parse_day(parts[1], true, b)) {
        return "The second day is not valid: use YYYY-MM-DD";
    }
    if (b < a) return "The second day is before the first";
    from = a;
    to = b;
    return "";
}

std::string fmt_date_range(uint32_t from, uint32_t to)
{
    if (from == 0 && to == 0) return "Any date";
    const std::string a = from ? format_local_datetime(from).substr(0, 10) : std::string("...");
    const std::string b = to ? format_local_datetime(to).substr(0, 10) : std::string("now");
    return a == b ? a : a + " to " + b;
}

} // namespace meshzero
