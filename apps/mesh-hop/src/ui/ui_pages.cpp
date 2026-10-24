/*
 * SPDX-License-Identifier: MIT
 *
 * The texts and layout rules of the pages that follow the screen (see ui_pages.hpp).
 */

#include "ui_pages.hpp"

#include <algorithm>

namespace meshhop {

namespace {
const char *const kDot = "  \xC2\xB7  ";           // "  ·  " (U+00B7, in the launcher's Montserrat)
const char *const kDash = "\xE2\x80\x93";          // "–" (U+2013)
} // namespace

PageColumn page_column(int screen_w, int inset, int min_w, int cap_w)
{
    PageColumn c;
    const int margin = inset + std::min(inset, std::max(0, (screen_w - min_w) / 2));
    c.w = std::max(1, std::min(cap_w, screen_w - 2 * margin));
    c.x = (screen_w - c.w) / 2;
    return c;
}

std::vector<int> fit_columns(const std::vector<int> &widths, int gap, int avail)
{
    std::vector<int> kept;
    int used = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        const int need = widths[i] + (kept.empty() ? 0 : gap);
        if (!kept.empty() && used + need > avail) continue;
        kept.push_back(static_cast<int>(i));
        used += need;
    }
    return kept;
}

Aff settings_row_aff(SRowKind k)
{
    switch (k) {
    case SRowKind::Header: return Aff::None;
    case SRowKind::Info:
    case SRowKind::GpsNotice:
    case SRowKind::StatsNotice:
    case SRowKind::PathHashNotice:
    case SRowKind::AutoAddNotice:
    case SRowKind::ChannelsNotice: return Aff::Info;
    case SRowKind::Position:
    case SRowKind::Stats:
    case SRowKind::PacketLog:
    case SRowKind::Channel:
    case SRowKind::AddChannel:
    case SRowKind::MyName:                          // a link: the highlight goes to RADIO > Node name
    case SRowKind::MyKey: return Aff::Open;         // the whole key in a box
    case SRowKind::Name:
    case SRowKind::Preset:
    case SRowKind::Freq:
    case SRowKind::Bw:
    case SRowKind::Sf:
    case SRowKind::Cr:
    case SRowKind::Tx:
    case SRowKind::RetryAttempts:
    case SRowKind::ResetAfter:
    case SRowKind::BoardGps:
    case SRowKind::PathHash:
    case SRowKind::Repeat:
    case SRowKind::AdvertEvery:
    case SRowKind::AdvertKind:
    case SRowKind::ManualAdd:
    case SRowKind::AutoAddTypes: return Aff::Edit;
    case SRowKind::SyncClock:
    case SRowKind::AddPublic:
    case SRowKind::HistoryAll:
    case SRowKind::HistoryOlder:
    case SRowKind::HistoryForget:
    case SRowKind::HistoryOthers:
    case SRowKind::Reboot:
    case SRowKind::FactoryReset:
    case SRowKind::Undo:
    case SRowKind::Save: return Aff::Action;
    }
    return Aff::None;
}

int keep_selected_row(const std::vector<SRowSpec> &before, int selected, const std::vector<SRowSpec> &after)
{
    const int n = static_cast<int>(after.size());
    std::vector<bool> ok;
    for (const SRowSpec &r : after) ok.push_back(settings_row_selectable(r.kind));
    if (selected < 0 || selected >= static_cast<int>(before.size())) {
        for (int i = 0; i < n; ++i)
            if (ok[static_cast<size_t>(i)]) return i;
        return -1;
    }
    const SRowSpec &was = before[static_cast<size_t>(selected)];
    for (int i = 0; i < n; ++i)
        if (ok[static_cast<size_t>(i)] && after[static_cast<size_t>(i)].kind == was.kind && after[static_cast<size_t>(i)].arg == was.arg) return i;
    if (n == 0) return -1;
    const int at = std::min(selected, n - 1);
    for (int d = 0; d < n; ++d) {
        if (at + d < n && ok[static_cast<size_t>(at + d)]) return at + d;
        if (at - d >= 0 && ok[static_cast<size_t>(at - d)]) return at - d;
    }
    return -1;
}

const std::vector<SortKey> &sort_menu_keys()
{
    static const std::vector<SortKey> keys = {SortKey::Name, SortKey::Heard, SortKey::Type, SortKey::Snr, SortKey::Hops, SortKey::Distance};
    return keys;
}

const char *sort_field_name(SortKey k)
{
    switch (k) {
    case SortKey::Name: return "Name";
    case SortKey::Heard: return "Heard";
    case SortKey::Type: return "Type";
    case SortKey::Snr: return "SNR";
    case SortKey::Hops: return "Hops";
    case SortKey::Distance: return "Distance";
    }
    return "";
}

std::string sort_direction_text(SortKey k, bool reverse)
{
    switch (k) {
    case SortKey::Heard: return reverse ? "oldest first" : "newest first";
    case SortKey::Name: return reverse ? "Z to A" : "A to Z";
    case SortKey::Type: return reverse ? "sensors first" : "chats first";
    case SortKey::Snr: return reverse ? "worst first" : "best first";
    case SortKey::Hops: return reverse ? "most first" : "fewest first";
    case SortKey::Distance: return reverse ? "farthest first" : "nearest first";
    }
    return "";
}

std::string sort_button_text(const ContactView &v)
{
    return std::string("Sort: ") + sort_field_name(v.sort);
}

const char *type_filter_label(TypeFilter t)
{
    return t == TypeFilter::All ? "All types" : type_filter_name(t);
}

const char *age_filter_label(AgeFilter a)
{
    switch (a) {
    case AgeFilter::Any: return "Any time";
    case AgeFilter::Hour: return "Last hour";
    case AgeFilter::Day: return "Last 24 h";
    case AgeFilter::Week: return "Last 7 days";
    }
    return "";
}

std::string contact_view_summary(size_t shown, size_t total, const ContactView &v)
{
    std::string s = shown == total ? std::to_string(total) + (total == 1 ? " contact" : " contacts")
                                   : std::to_string(shown) + " of " + std::to_string(total);
    s += kDot;
    s += type_filter_label(v.filter.type);
    s += kDot;
    s += age_filter_label(v.filter.age);
    if (!v.group.empty()) s += kDot + std::string("Group ") + truncate_ellipsis(v.group, 16);
    if (!v.filter.search.empty()) s += kDot + contact_search_summary(v.filter.search);
    return s;
}

std::string contact_search_summary(const std::string &query)
{
    std::string hex;
    if (search_key_prefix(query, &hex)) return hex.empty() ? std::string("any key") : "key " + hex + "...";
    return "\"" + truncate_ellipsis(query, 16) + "\" in the name";
}

std::string contact_marked_summary(size_t marked, size_t hidden)
{
    std::string s = "Manage contacts: " + std::to_string(marked) + " selected";
    if (hidden) s += " (+" + std::to_string(hidden) + " hidden by the filters)";
    return s;
}

std::string contact_row_caption(const ContactRow &r, bool with_snr)
{
    std::string s = r.type_name;
    s += kDot;
    s += r.heard == 0 ? std::string("never heard") : "heard " + fmt_age_coarse(r.heard);
    if (r.hops >= 0) s += kDot + fmt_hops(r.hops);
    if (with_snr && r.has_snr) s += kDot + std::string("SNR ") + fmt_snr(true, r.snr);
    return s;
}

std::string contact_row_distance(const ContactRow &r)
{
    return r.has_dist ? fmt_distance(r.dist_km) : std::string(kDash);
}

Choice make_send_advert_choice(bool last_flood)
{
    ChoiceExtra extra;
    Choice c = make_choice(ChoiceField::AdvertKind, meshzero::RadioSettings(), meshzero::RetrySettings(), &extra);
    c.title = "Send advert";
    c.labels = {"Flood (whole mesh)", "Zero-hop (neighbours)"};
    c.index = c.initial = last_flood ? 0 : 1;
    return c;
}

std::string nearby_summary(size_t shown, size_t pending, bool showing_ignored)
{
    std::string s = std::to_string(shown) + (showing_ignored ? (shown == 1 ? " ignored node" : " ignored nodes") : (shown == 1 ? " node heard" : " nodes heard"));
    if (pending) s += kDot + std::to_string(pending) + " waiting";
    return s;
}

std::string nearby_row_caption(const NearbyRow &r)
{
    return r.type_name + kDot + r.detail;
}

} // namespace meshhop
