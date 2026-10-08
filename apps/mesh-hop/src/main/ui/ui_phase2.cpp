/*
 * SPDX-License-Identifier: MIT
 *
 * UI-independent logic of phase 2 (0.2.0): selecting contacts, groups, contact details, the Nearby list, the message search, the statistics
 * view, the auto-add filter, the pick list and the waiting Yes. No LVGL, no I/O. Unit tested in tests/test_ui.cpp.
 */

#include "ui_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

using namespace meshzero;

namespace meshhop {

/* ------------------------------------------------------------------ the waiting Yes and the pick list */

int confirm_wait_left(uint64_t opened_ms, uint64_t now_ms, uint32_t delay_ms)
{
    if (delay_ms == 0 || now_ms < opened_ms) return delay_ms ? static_cast<int>((delay_ms + 999) / 1000) : 0;
    const uint64_t elapsed = now_ms - opened_ms;
    if (elapsed >= delay_ms) return 0;
    return static_cast<int>((delay_ms - elapsed + 999) / 1000);
}

std::string confirm_yes_text(const std::string &yes, int wait_left)
{
    return wait_left > 0 ? yes + " (" + std::to_string(wait_left) + ")" : yes;
}

ListResult list_key(const KeyEvent &e, int &selected, int count)
{
    if (e.key == Key::Esc) return e.repeat ? ListResult::None : ListResult::Cancel;
    if (count <= 0) return ListResult::None;
    auto go = [&](int to) {
        const int n = std::clamp(to, 0, count - 1);
        const bool moved = n != selected;
        selected = n;
        return moved ? ListResult::Moved : ListResult::None;
    };
    switch (e.key) {
    case Key::Up: return go(selected - 1);
    case Key::Down: return go(selected + 1);
    case Key::PageUp: return go(selected - 5);
    case Key::PageDown: return go(selected + 5);
    case Key::Home: return go(0);
    case Key::End: return go(count - 1);
    case Key::Enter: return e.repeat ? ListResult::None : ListResult::Accept;
    default: return ListResult::None;
    }
}

/* ------------------------------------------------------------------ selecting contacts */

bool ContactSelection::toggle(const std::string &key_hex)
{
    if (keys_.erase(key_hex)) return false;
    keys_.insert(key_hex);
    return true;
}

void ContactSelection::select_shown(const std::vector<ContactRow> &rows)
{
    for (const ContactRow &r : rows) keys_.insert(r.key_hex);
}

void ContactSelection::select_stale(const std::vector<ContactRow> &rows, int days, uint32_t now_in)
{
    const uint32_t now = now_in ? now_in : now_unix();
    const uint64_t span = static_cast<uint64_t>(days) * 86400u;
    for (const ContactRow &r : rows) {
        const bool never = r.heard < kMinPlausibleTime;
        if (never || (now >= r.heard && now - r.heard >= span)) keys_.insert(r.key_hex);
    }
}

void ContactSelection::select_never_heard(const std::vector<ContactRow> &rows)
{
    for (const ContactRow &r : rows)
        if (r.heard < kMinPlausibleTime) keys_.insert(r.key_hex);
}

void ContactSelection::invert(const std::vector<ContactRow> &rows)
{
    for (const ContactRow &r : rows) toggle(r.key_hex);
}

std::vector<std::string> ContactSelection::shown_selected(const std::vector<ContactRow> &rows) const
{
    std::vector<std::string> out;
    for (const ContactRow &r : rows)
        if (contains(r.key_hex)) out.push_back(r.key_hex);
    return out;
}

size_t ContactSelection::hidden_count(const std::vector<ContactRow> &rows) const
{
    return keys_.size() - shown_selected(rows).size();
}

void ContactSelection::keep_only(const std::set<std::string> &existing)
{
    for (auto it = keys_.begin(); it != keys_.end();) it = existing.count(*it) ? std::next(it) : keys_.erase(it);
}

SelectMenu select_menu_items()
{
    SelectMenu m;
    m.labels = {"Select all shown", "Select none", "Not heard for 7 days or more", "Not heard for 30 days or more", "Never heard", "Invert the selection"};
    m.actions = {SelectAction::All, SelectAction::None, SelectAction::Stale7, SelectAction::Stale30, SelectAction::NeverHeard, SelectAction::Invert};
    return m;
}

BulkDeletePrompt bulk_delete_prompt(const Model &model, const std::vector<ContactRow> &rows, const ContactSelection &sel)
{
    BulkDeletePrompt p;
    const std::vector<std::string> keys = sel.shown_selected(rows);
    p.count = keys.size();
    p.empty = keys.empty();
    p.title = "Delete " + std::to_string(p.count) + (p.count == 1 ? " contact?" : " contacts?");
    std::string who;
    size_t shown = 0;
    for (const std::string &k : keys) {
        if (shown == 3) break;
        const ContactRec *c = model.find_contact(k);
        if (!who.empty()) who += ", ";
        who += truncate_ellipsis(c && !c->c.name.empty() ? c->c.name : k.substr(0, 12), 18);
        ++shown;
    }
    if (p.count > shown) who += " and " + std::to_string(p.count - shown) + " more";
    p.body = who + ". They are removed from the radio board one by one. A deleted contact comes back only when its node adverts again. This cannot be undone.";
    const size_t hidden = sel.hidden_count(rows);
    if (hidden > 0) p.body += " " + std::to_string(hidden) + " marked but hidden by the filter stay.";
    return p;
}

std::vector<std::string> group_filter_labels(const ContactGroups &g)
{
    std::vector<std::string> v = {"All contacts"};
    for (const ContactGroup &grp : g.groups()) v.push_back(truncate_ellipsis(grp.name, 22) + " (" + std::to_string(grp.keys.size()) + ")");
    return v;
}

std::vector<std::string> group_pick_labels(const ContactGroups &g)
{
    std::vector<std::string> v;
    for (const ContactGroup &grp : g.groups()) v.push_back(truncate_ellipsis(grp.name, 22) + " (" + std::to_string(grp.keys.size()) + ")");
    return v;
}

GroupMenu group_assign_menu(const ContactGroups &g, size_t selected)
{
    GroupMenu m;
    const std::string n = std::to_string(selected) + (selected == 1 ? " contact" : " contacts");
    if (!g.empty()) {
        m.labels.push_back("Add " + n + " to a group");
        m.actions.push_back(GroupAction::AddTo);
        m.labels.push_back("Remove " + n + " from a group");
        m.actions.push_back(GroupAction::RemoveFrom);
    }
    m.labels.push_back("New group with " + n);
    m.actions.push_back(GroupAction::NewWith);
    if (!g.empty()) {
        m.labels.push_back("Remove " + n + " from all groups");
        m.actions.push_back(GroupAction::RemoveAll);
    }
    return m;
}

/* ------------------------------------------------------------------ contact details */

std::string fmt_node_time(uint32_t ts)
{
    return ts < kMinPlausibleTime ? "-" : format_local_datetime(ts);
}

std::string fmt_position(bool has, double lat, double lon)
{
    if (!has) return "not shared";
    char b[64];
    std::snprintf(b, sizeof(b), "%.5f, %.5f", lat, lon);
    return b;
}

std::string fmt_route(uint8_t path_len, const std::array<uint8_t, 64> &path)
{
    const int hops = path_hops(path_len);
    if (hops < 0) return "no route yet (flood)";
    if (hops == 0) return "direct (0 hops)";
    const int hash = std::min(path_hash_mode(path_len) + 1, 3);
    std::string t = std::to_string(hops) + (hops == 1 ? " hop" : " hops") + " (" + std::to_string(hash) + "-byte hashes): ";
    for (int i = 0; i < hops && (i + 1) * hash <= 64; ++i) {
        if (i) t += " > ";
        t += to_hex(path.data() + i * hash, static_cast<size_t>(hash));
    }
    return t;
}

ContactDetail build_contact_detail(const Model &model, const std::string &key_hex)
{
    ContactDetail d;
    const ContactRec *r = model.find_contact(key_hex);
    if (!r) return d;
    d.found = true;
    d.chat = opens_chat(r->c.type);
    d.name = r->c.name.empty() ? "(no name)" : r->c.name;
    d.title = truncate_ellipsis(r->c.name.empty() ? to_hex(r->prefix()) : r->c.name, 36);
    d.type = contact_type_name(r->c.type);
    const std::string hex = to_hex(r->c.key);
    d.key1 = hex.substr(0, 32);
    d.key2 = hex.substr(32);
    d.route = fmt_route(r->c.out_path_len, r->c.out_path);
    d.last_advert = fmt_node_time(r->c.last_advert);
    d.heard = r->last_heard() ? fmt_age(r->last_heard()) + " ago" : "-";
    const bool pos = !(r->c.lat == 0 && r->c.lon == 0);
    d.position = fmt_position(pos, r->c.lat, r->c.lon);
    d.distance = "-";
    if (const auto &self = model.self(); self && pos && !(self->lat == 0 && self->lon == 0))
        d.distance = fmt_distance(distance_km(self->lat, self->lon, r->c.lat, r->c.lon));
    d.snr = fmt_snr(r->has_snr, r->snr);
    const std::vector<std::string> g = model.groups().groups_of(hex);
    for (const std::string &n : g) d.groups += (d.groups.empty() ? "" : ", ") + n;
    if (d.groups.empty()) d.groups = "none";
    return d;
}

/* ------------------------------------------------------------------ nearby */

const char *nearby_state_name(NearbyState s)
{
    switch (s) {
    case NearbyState::Pending: return "waiting";
    case NearbyState::New: return "new";
    case NearbyState::Contact: return "contact";
    case NearbyState::Ignored: return "ignored";
    }
    return "";
}

std::string fmt_signal(bool has_snr, double snr, bool has_rssi, int rssi)
{
    std::string t;
    char b[48];
    if (has_snr) {
        std::snprintf(b, sizeof(b), "SNR %.1f dB", snr);
        t = b;
    }
    if (has_rssi) {
        std::snprintf(b, sizeof(b), "RSSI %d", rssi);
        t += (t.empty() ? "" : "  ") + std::string(b);
    }
    return t.empty() ? "no signal figure" : t;
}

std::vector<NearbyRow> build_nearby_rows(const Model &model, bool show_ignored, uint32_t now_in)
{
    const uint32_t now = now_in ? now_in : now_unix();
    std::vector<NearbyRow> rows;
    for (const NearbyRec *n : model.nearby_sorted()) {
        const ContactRec *known_rec = model.find_contact(n->key_hex);
        const bool known = known_rec != nullptr;
        const bool ignored = model.is_ignored(n->key_hex);
        if (ignored && !show_ignored) continue;
        NearbyRow r;
        r.key_hex = n->key_hex;
        const int type = n->type != 0 ? n->type : known_rec ? known_rec->c.type : 0;
        r.type = type;
        r.type_name = type == 0 ? "?" : contact_type_name(type);
        r.title = known_rec && !known_rec->c.name.empty() ? truncate_ellipsis(known_rec->c.name, 24)
                  : !n->name.empty() ? truncate_ellipsis(n->name, 24) : "Node " + n->key_hex.substr(0, 6);
        r.has_snr = n->has_snr;
        r.snr = n->snr;
        r.state = ignored ? NearbyState::Ignored : known ? NearbyState::Contact : n->pending ? NearbyState::Pending : NearbyState::New;
        r.can_add = !known && !ignored && n->full_key && (n->type != 0 || n->has_contact);
        std::string hops = n->hops < 0 ? "" : n->hops == 0 ? "direct" : std::to_string(n->hops) + (n->hops == 1 ? " hop" : " hops");
        if (n->hops > 0 && n->hash_size > 0) hops += " (" + std::to_string(n->hash_size) + "-byte hashes)";
        r.detail = fmt_signal(n->has_snr, n->snr, false, 0) + (hops.empty() ? "" : "  " + hops) + "  " + (n->last_heard == 0 ? std::string("-") : format_age(now >= n->last_heard ? static_cast<int64_t>(now - n->last_heard) : 0)) + " ago";
        rows.push_back(std::move(r));
    }
    return rows;
}

size_t ignored_nearby_count(const Model &model)
{
    size_t n = 0;
    for (const NearbyRec *r : model.nearby_sorted())
        if (model.is_ignored(r->key_hex)) ++n;
    return n;
}

/* ------------------------------------------------------------------ message search */

SearchQuery SearchState::query(uint32_t now_in) const
{
    SearchQuery q;
    q.text = text;
    q.conv = conv;
    q.dir = dir;
    if (date == DatePreset::Custom) {
        q.from = from;
        q.to = to;
    } else {
        date_preset_range(date, now_in ? now_in : now_unix(), q.from, q.to);
    }
    return q;
}

void SearchState::cycle_dir()
{
    dir = dir == SearchDir::Any ? SearchDir::Received : dir == SearchDir::Received ? SearchDir::Sent : SearchDir::Any;
}

std::string SearchState::date_label() const
{
    return date == DatePreset::Custom ? fmt_date_range(from, to) : date_preset_name(date);
}

std::string SearchState::conv_label(const Model &model) const
{
    return conv.empty() ? "All chats" : truncate_ellipsis(model.conv_title(conv), 16);
}

std::string fmt_when(uint32_t ts, uint32_t now_in)
{
    const uint32_t now = now_in ? now_in : now_unix();
    if (ts < kMinPlausibleTime) return "--:--";
    const std::string full = format_local_datetime(ts);                 // YYYY-MM-DD HH:MM
    const std::string today = format_local_datetime(now).substr(0, 10);
    return full.substr(0, 10) == today ? full.substr(11) : full.substr(5);
}

std::string make_snippet(const std::string &text, const std::string &needle, size_t max_chars)
{
    // one line: newlines become spaces; positions are in bytes, the cut respects UTF-8
    std::string flat = text;
    for (char &c : flat)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (utf8_length(flat) <= max_chars) return flat;
    size_t at = 0;
    if (!needle.empty()) {
        auto low = [](unsigned char c) { return static_cast<unsigned char>(c < 0x80 ? std::tolower(c) : c); };
        for (size_t i = 0; i + needle.size() <= flat.size(); ++i) {
            size_t k = 0;
            while (k < needle.size() && low(static_cast<unsigned char>(flat[i + k])) == low(static_cast<unsigned char>(needle[k]))) ++k;
            if (k == needle.size()) { at = i; break; }
        }
    }
    // start a few characters before the match
    size_t start = at;
    size_t back = 0;
    while (start > 0 && back < 12) {
        --start;
        while (start > 0 && (static_cast<unsigned char>(flat[start]) & 0xC0) == 0x80) --start;
        ++back;
    }
    // characters from `start`
    std::string tail = flat.substr(start);
    const bool cut_front = start > 0;
    const size_t room = max_chars > 6 ? max_chars - (cut_front ? 3 : 0) : max_chars;
    std::string out = truncate_ellipsis(tail, room);
    return (cut_front ? "..." : "") + out;
}

std::vector<SearchRow> build_search_rows(const Model &model, const SearchState &st, size_t *total, uint32_t now)
{
    std::vector<SearchRow> rows;
    const uint32_t t = now ? now : now_unix();
    for (const SearchHit &h : search_messages(model, st.query(t), 300, total)) {
        SearchRow r;
        r.seq = h.seq;
        r.conv = h.conv;
        r.outgoing = h.outgoing;
        r.head = fmt_when(h.ts, t) + "   " + truncate_ellipsis(h.title, 18) + (h.outgoing ? "   You" : (h.conv.rfind("c:", 0) == 0 && !h.sender.empty() ? "   " + truncate_ellipsis(h.sender, 14) : ""));
        r.snippet = make_snippet(h.text, st.text, 56);
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<std::pair<std::string, std::string>> search_conversations(const Model &model)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const ConvSummary &c : model.conversations()) out.emplace_back(c.key, c.title);
    return out;
}

/* ------------------------------------------------------------------ statistics */

std::string fmt_uptime(uint32_t secs)
{
    if (secs < 60) return std::to_string(secs) + " s";
    if (secs < 3600) return std::to_string(secs / 60) + " min";
    char b[48];
    if (secs < 86400) {
        std::snprintf(b, sizeof(b), "%u h %02u min", secs / 3600, (secs % 3600) / 60);
        return b;
    }
    std::snprintf(b, sizeof(b), "%u d %u h", secs / 86400, (secs % 86400) / 3600);
    return b;
}

StatsView build_stats_view(const StatsSnapshot &s, uint32_t now_in)
{
    StatsView v;
    char b[96];
    auto line = [](std::vector<StatLine> &to, const std::string &l, const std::string &val) { to.push_back({l, val}); };
    if (s.core) {
        v.any = true;
        std::snprintf(b, sizeof(b), "%.2f V", s.core->battery_mv / 1000.0);
        line(v.left, "Battery", b);
        line(v.left, "Uptime", fmt_uptime(s.core->uptime_secs));
        line(v.left, "Errors", std::to_string(s.core->errors));
        line(v.left, "Send queue", std::to_string(s.core->queue_len));
    }
    if (s.radio) {
        v.any = true;
        line(v.left, "Noise floor", std::to_string(s.radio->noise_floor) + " dBm");
        line(v.left, "Last RSSI", std::to_string(s.radio->last_rssi) + " dBm");
        std::snprintf(b, sizeof(b), "%.1f dB", s.radio->last_snr);
        line(v.left, "Last SNR", b);
        std::string tx = std::to_string(s.radio->tx_air_secs) + " s";
        if (s.core && s.core->uptime_secs > 0) {
            std::snprintf(b, sizeof(b), " (%.2f %%)", 100.0 * s.radio->tx_air_secs / s.core->uptime_secs);
            tx += b;
        }
        line(v.left, "TX airtime", tx);
        line(v.left, "RX airtime", std::to_string(s.radio->rx_air_secs) + " s");
    }
    if (s.packets) {
        v.any = true;
        line(v.right, "Received", std::to_string(s.packets->recv));
        line(v.right, "Sent", std::to_string(s.packets->sent));
        line(v.right, "Flood sent", std::to_string(s.packets->flood_tx));
        line(v.right, "Direct sent", std::to_string(s.packets->direct_tx));
        line(v.right, "Flood received", std::to_string(s.packets->flood_rx));
        line(v.right, "Direct received", std::to_string(s.packets->direct_rx));
        line(v.right, "Receive errors", s.packets->has_errors ? std::to_string(s.packets->recv_errors) : std::string("not reported"));
    }
    const uint32_t now = now_in ? now_in : now_unix();
    v.updated = s.updated == 0 ? "Not read yet" : "Updated " + std::string(now >= s.updated ? format_age(static_cast<int64_t>(now - s.updated)) : "0 s") + " ago";
    return v;
}

/* ------------------------------------------------------------------ the auto-add filter */

std::vector<AutoAddItem> autoadd_items(const AutoaddConfig &c)
{
    std::vector<AutoAddItem> v = {
        {"Chat nodes", kAutoaddChat, false},
        {"Repeaters", kAutoaddRepeater, false},
        {"Room servers", kAutoaddRoom, false},
        {"Sensors", kAutoaddSensor, false},
        {"Replace the oldest when the list is full", kAutoaddOverwrite, false},
    };
    for (AutoAddItem &i : v) i.on = (c.config & i.flag) != 0;
    return v;
}

uint8_t autoadd_toggled(uint8_t config, uint8_t flag)
{
    return static_cast<uint8_t>(config ^ flag);
}

std::string autoadd_summary(const AutoaddConfig &c)
{
    std::string t;
    for (const AutoAddItem &i : autoadd_items(c)) {
        if (!i.on || i.flag == kAutoaddOverwrite) continue;
        if (!t.empty()) t += ", ";
        t += i.label == "Chat nodes" ? "Chat" : i.label == "Repeaters" ? "Repeater" : i.label == "Room servers" ? "Room" : "Sensor";
    }
    if (t.empty()) t = "none";
    if (c.config & kAutoaddOverwrite) t += " + replace oldest";
    return t;
}

} // namespace meshhop
