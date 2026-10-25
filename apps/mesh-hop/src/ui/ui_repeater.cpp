/*
 * SPDX-License-Identifier: MIT
 *
 * 0.7.0: Home, Tools, the contact's Options, MY CONTACT, the Repeater page and the CLI's `to` line (see ui_repeater.hpp).
 * 0.8.0: the Repeater page's read-only view (STATUS, NEIGHBOURS, OWNER) and the queue of its reads.
 */

#include "ui_repeater.hpp"

#include "protocol.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace meshzero;

namespace meshhop {

namespace {
const char *const kDot = "  \xC2\xB7  ";           // "  ·  " (U+00B7, in the launcher's Montserrat)
const char *const kEllipsis = "\xE2\x80\xA6";      // "…" (U+2026)

bool is_hex64(const std::string &s)
{
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

/* ------------------------------------------------------------------ Home and the page loop */

const std::vector<HomeItem> &home_items()
{
    static const std::vector<HomeItem> items = {HomeItem::Chats, HomeItem::Contacts, HomeItem::Tools, HomeItem::Settings, HomeItem::Node};
    return items;
}

const char *home_item_name(HomeItem i)
{
    switch (i) {
    case HomeItem::Chats: return "Chats";
    case HomeItem::Contacts: return "Contacts";
    case HomeItem::Tools: return "Tools";
    case HomeItem::Settings: return "Settings";
    case HomeItem::Node: return "Node";
    }
    return "";
}

int home_index(HomeItem i)
{
    const auto &v = home_items();
    const auto it = std::find(v.begin(), v.end(), i);
    return it == v.end() ? -1 : static_cast<int>(it - v.begin());
}

int home_row_of(Tab t)
{
    switch (t) {
    case Tab::Contacts: return home_index(HomeItem::Contacts);
    case Tab::Tools:
    case Tab::Cli: return home_index(HomeItem::Tools);
    case Tab::Settings: return home_index(HomeItem::Settings);
    default: return home_index(HomeItem::Chats);
    }
}

Tab next_page(Tab t, int dir)
{
    static const Tab loop[] = {Tab::Chats, Tab::Contacts, Tab::Tools, Tab::Settings};
    constexpr int n = 4;
    int at = 0;
    if (t == Tab::Cli) t = Tab::Tools;
    for (int i = 0; i < n; ++i)
        if (loop[i] == t) at = i;
    const int step = dir > 0 ? 1 : dir < 0 ? -1 : 0;
    return loop[(at + step + n) % n];
}

std::string tools_home_value(size_t pending)
{
    return pending ? std::to_string(pending) + " nearby waiting" : std::string();
}

/* ------------------------------------------------------------------ Tools */

std::vector<ToolRow> tools_rows(const ToolsContext &c)
{
    std::vector<ToolRow> rows;
    ToolRow r;
    r.item = ToolItem::Nearby;
    r.title = "Nearby";
    r.caption = c.pending ? std::to_string(c.pending) + " waiting" + kDot + "Scan asks the nodes in range" : std::string("Scan asks the nodes in range");
    r.tone = c.pending ? Tone::Gold : Tone::Muted;
    rows.push_back(r);
    r = ToolRow();
    r.item = ToolItem::PacketLog;
    r.title = "Packet log";
    const std::string n = std::to_string(c.log_n) + (c.log_n == 1 ? " packet" : " packets");
    r.caption = c.capturing ? "capturing, " + n : c.log_n ? "stopped, " + n : std::string("capture off");
    r.tone = c.capturing ? Tone::Green : Tone::Muted;
    rows.push_back(r);
    r = ToolRow();
    r.item = ToolItem::Statistics;
    r.title = "Statistics";
    r.caption = c.stats.notice.empty() ? std::string("battery, noise floor, airtime") : c.stats.notice;
    r.tone = c.stats.notice.empty() ? Tone::Muted : Tone::Gold;
    rows.push_back(r);
    r = ToolRow();
    r.item = ToolItem::Cli;
    r.title = "CLI";
    r.caption = c.cli_needs_python ? std::string("needs python3") : !c.connected ? std::string("meshcore-cli  \xC2\xB7  not connected")
                                                                                 : std::string("meshcore-cli, for admin work");
    r.tone = c.cli_needs_python ? Tone::Gold : Tone::Muted;
    rows.push_back(r);
    return rows;
}

/* ------------------------------------------------------------------ Contacts */

ContactOpen contact_open_target(int type)
{
    if (type == advtype::kRepeater || type == advtype::kRoom) return ContactOpen::Repeater;
    if (opens_chat(type)) return ContactOpen::Chat;
    return ContactOpen::Details;
}

ContactOptions contact_options(const std::string &name, int type)
{
    ContactOptions o;
    o.title = "Options for " + truncate_ellipsis(name, 24);
    auto add = [&](const char *label, ContactAction a, Aff aff) {
        o.labels.push_back(label);
        o.actions.push_back(a);
        o.affs.push_back(aff);
    };
    const ContactOpen t = contact_open_target(type);
    if (t == ContactOpen::Repeater) add("Manage", ContactAction::Manage, Aff::Open);
    if (t == ContactOpen::Chat) add("Message", ContactAction::Message, Aff::Open);
    add("Details", ContactAction::Details, Aff::Open);
    add("Groups", ContactAction::Groups, Aff::Open);
    add("Delete", ContactAction::Delete, Aff::Action);
    return o;
}

/* ------------------------------------------------------------------ MY CONTACT */

std::string key_short_grouped(const std::string &hex64)
{
    if (!is_hex64(hex64)) return "";
    const std::string h = lower(hex64);
    return h.substr(0, 4) + " " + h.substr(4, 4) + " " + kEllipsis + " " + h.substr(60, 4);
}

std::string key_grouped_lines(const std::string &hex64)
{
    if (!is_hex64(hex64)) return "";
    const std::string h = lower(hex64);
    std::string out;
    for (int g = 0; g < 16; ++g) {
        if (g) out += g == 8 ? "\n" : " ";
        out += h.substr(static_cast<size_t>(g) * 4, 4);
    }
    return out;
}

/* ------------------------------------------------------------------ the Repeater page */

std::string repeater_route_text(int hops)
{
    if (hops < 0) return "flood";
    if (hops == 0) return "direct";
    return "direct, " + fmt_hops(hops);
}

std::string repeater_status_line(int type, int hops, uint32_t heard)
{
    std::string s = type == advtype::kRoom ? "Room" : type == advtype::kRepeater ? "Repeater" : contact_type_name(type);
    s += kDot + repeater_route_text(hops) + kDot;
    s += heard == 0 ? std::string("never heard") : "heard " + fmt_age_coarse(heard) + " ago";
    return s;
}

const char *remote_role_name(Client::RemoteRole r)
{
    switch (r) {
    case Client::RemoteRole::Guest: return "Guest";
    case Client::RemoteRole::ReadOnly: return "Read-only";
    case Client::RemoteRole::ReadWrite: return "Read-write";
    case Client::RemoteRole::Admin: return "Admin";
    case Client::RemoteRole::None: break;
    }
    return "Logged in";
}

std::string clock_skew_text(int64_t skew_s)
{
    if (skew_s > -2 && skew_s < 2) return "in step with the deck";
    return format_age(skew_s < 0 ? -skew_s : skew_s) + (skew_s > 0 ? " ahead" : " behind");
}

std::string logout_note(bool room)
{
    return std::string("Log out ends the session on this deck only: the ") + (room ? "room" : "repeater") +
           " keeps this deck as admin until setperm, as guest until it reboots.";
}

/* ------------------------------------------------------------------ 0.8.0: the read-only view */

const char *rep_section_name(RepSection s)
{
    switch (s) {
    case RepSection::Status: return "Status";
    case RepSection::Neighbours: return "Neighbours";
    case RepSection::Owner: return "Owner";
    case RepSection::None: break;
    }
    return "";
}

const RepeaterReads::Section &RepeaterReads::section(RepSection s) const
{
    static const Section none;
    switch (s) {
    case RepSection::Status: return st_;
    case RepSection::Neighbours: return nb_;
    case RepSection::Owner: return own_;
    case RepSection::None: break;
    }
    return none;
}

RepeaterReads::Section &RepeaterReads::sec(RepSection s)
{
    return const_cast<Section &>(static_cast<const RepeaterReads *>(this)->section(s));
}

void RepeaterReads::clear()
{
    *this = RepeaterReads();
}

bool RepeaterReads::refresh_all(bool logged_in, bool room, bool direct)
{
    if (running()) return false;
    if (logged_in) {
        queue_.push_back({RepSection::Status, 0});
        if (!room) {
            queue_.push_back({RepSection::Neighbours, 0});
            queue_.push_back({RepSection::Owner, 0});
        }
    } else if (!room && direct) {
        queue_.push_back({RepSection::Owner, 0});                 // the anonymous request: the only read without a login
    }
    return !queue_.empty();
}

bool RepeaterReads::refresh(RepSection s)
{
    if (running() || s == RepSection::None) return false;
    queue_.push_back({s, 0});
    return true;
}

bool RepeaterReads::more_neighbours()
{
    if (running() || !nb_.has || !nb_more) return false;
    queue_.push_back({RepSection::Neighbours, nb_next});
    return true;
}

bool RepeaterReads::auto_read(RepSection s, bool logged_in)
{
    if (!logged_in || s == RepSection::None || running() || section(s).tried) return false;
    return refresh(s);
}

bool RepeaterReads::busy(RepSection s) const
{
    if (s == RepSection::None) return false;
    if (flying_ && flight_.section == s) return true;
    return std::any_of(queue_.begin(), queue_.end(), [s](const Next &n) { return n.section == s; });
}

void RepeaterReads::cancel()
{
    queue_.clear();
}

bool RepeaterReads::pop(Next &n)
{
    if (flying_ || queue_.empty()) return false;
    flight_ = queue_.front();
    queue_.pop_front();
    flying_ = true;
    req_ = 0;
    sec(flight_.section).tried = true;
    n = flight_;
    return true;
}

bool RepeaterReads::stops_chain(Client::RemoteError e)
{
    using E = Client::RemoteError;
    switch (e) {
    case E::Timeout:            // this section had no answer: the next may still have one
    case E::Unsupported:
    case E::NoDirectRoute:
    case E::BadReply:
    case E::Failed:
    case E::None: return false;
    default: return true;       // NotLoggedIn (the login is lost), Busy, NotConnected, LinkLost, NotFound, TableFull, Cancelled, ...
    }
}

void RepeaterReads::merge(const NeighboursPage &pg, uint32_t deck_now)
{
    if (pg.offset == 0) neighbours.clear();                      // the first page: a new list
    for (const NeighbourEntry &e : pg.entries) {
        Neighbour n;
        n.prefix = e.prefix;
        n.heard_at = deck_now > e.heard_secs_ago ? deck_now - e.heard_secs_ago : 0;
        n.snr = e.snr;
        const auto same = std::find_if(neighbours.begin(), neighbours.end(), [&](const Neighbour &x) { return x.prefix == n.prefix; });
        if (same != neighbours.end()) {                          // the list moved between two pages: one row per neighbour, the newer
            if (n.heard_at >= same->heard_at) *same = n;
            continue;
        }
        neighbours.push_back(n);
    }
    std::stable_sort(neighbours.begin(), neighbours.end(), [](const Neighbour &a, const Neighbour &b) { return a.heard_at > b.heard_at; });
    nb_total = pg.total;
    nb_more = pg.more;
    nb_next = static_cast<uint16_t>(pg.offset + pg.entries.size());
}

std::string RepeaterReads::on_result(const Client::RemoteResult &r, uint32_t deck_now, const std::string &name)
{
    using E = Client::RemoteError;
    using RO = Client::RemoteOp;
    if (!flying_) return "";
    const RO want = flight_.section == RepSection::Status ? RO::Status : flight_.section == RepSection::Neighbours ? RO::Neighbours : RO::Owner;
    if (r.op != want || (req_ != 0 && r.req != req_) || (req_ == 0 && r.req != 0)) return "";     // not the read in flight
    const Next done = flight_;
    flying_ = false;
    req_ = 0;
    Section &s = sec(done.section);
    if (r.ok) {
        switch (done.section) {
        case RepSection::Status: status = r.status; break;
        case RepSection::Neighbours: merge(r.neighbours, deck_now); break;
        case RepSection::Owner: owner = r.owner; break;
        case RepSection::None: break;
        }
        s.has = true;
        s.read_at = deck_now;
        s.error = E::None;
        s.anonymous = done.section == RepSection::Owner && r.owner.anonymous;
        s.offset = done.offset;
        return "";
    }
    if (r.error == E::Cancelled) {                               // Cancel, a login or logout, the CLI: the section stays as it was
        queue_.clear();
        return "";
    }
    s.error = r.error;
    s.offset = done.offset;
    // the anonymous request: sent without a session (the client chose it), or refused for want of a direct route
    s.anonymous = done.section == RepSection::Owner && (r.anonymous || r.error == E::NoDirectRoute);
    if (stops_chain(r.error)) queue_.clear();
    return read_error_text(r.error, r.err_code, name, s.anonymous);
}

std::string read_error_text(Client::RemoteError e, int err_code, const std::string &name_in, bool anonymous)
{
    using E = Client::RemoteError;
    const std::string name = truncate_ellipsis(name_in.empty() ? std::string("the node") : name_in, 24);
    switch (e) {
    case E::None:
    case E::Cancelled: return "";
    case E::Timeout:
        return anonymous ? "No answer. " + name + " may be out of reach."
                         : "No answer. " + name + " may be out of reach, or its session ended (a reboot): log in again.";
    case E::NotLoggedIn: return "Log in first";
    case E::Unsupported: return "Firmware too old for this";
    case E::NoDirectRoute: return "No direct route: log in to read the owner";
    case E::BadReply: return "Unreadable answer";
    case E::Busy: return "The CLI holds the node";
    case E::NotFound: return name + " is not in the node's contacts.";
    case E::TableFull: return "The node is busy: try again in a moment.";
    case E::NotConnected: return "Not connected to a node.";
    case E::LinkLost: return "The link to the node was lost.";
    case E::Failed: return "The node refused the request" + (err_code >= 0 ? " (error " + std::to_string(err_code) + ")." : std::string("."));
    case E::Refused:
    case E::BadPassword: return "The node refused the request.";       // (a login's errors: not a read's)
    }
    return "";
}

std::string read_error_state(Client::RemoteError e)
{
    using E = Client::RemoteError;
    switch (e) {
    case E::Timeout: return "no answer";
    case E::NotLoggedIn: return "needs login";
    case E::Unsupported: return "firmware too old";
    case E::NoDirectRoute: return "direct route only";
    case E::BadReply: return "unreadable answer";
    default: return "not read";
    }
}

std::string section_wait_text(const Client::RemoteState &s, uint64_t now_ms)
{
    switch (s.phase) {
    case Client::RemoteState::Phase::Idle: return "";
    case Client::RemoteState::Phase::Sending: return "sending\xE2\x80\xA6";
    case Client::RemoteState::Phase::Waiting: {
        const uint64_t left = s.deadline_ms > now_ms ? s.deadline_ms - now_ms : 0;
        const uint64_t secs = std::max<uint64_t>(1, (left + 999) / 1000);
        return "waiting\xE2\x80\xA6 up to " + std::to_string(secs) + " s";
    }
    }
    return "";
}

std::string fmt_volts(uint32_t mv)
{
    const uint32_t cv = (mv + 5) / 10;                           // hundredths of a volt, rounded: 4996 mV is "5.00 V"
    char b[32];
    std::snprintf(b, sizeof(b), "%u.%02u V", cv / 100, cv % 100);
    return b;
}

std::string fmt_rep_uptime(uint32_t s)
{
    if (s < 60) return std::to_string(s) + " s";
    const uint32_t d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d) return std::to_string(d) + " d " + std::to_string(h) + " h " + std::to_string(m) + " m";
    if (h) return std::to_string(h) + " h " + std::to_string(m) + " m";
    return std::to_string(m) + " m";
}

std::string fmt_rep_uptime_short(uint32_t s)
{
    if (s < 60) return std::to_string(s) + " s";
    const uint32_t d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d) return std::to_string(d) + " d " + std::to_string(h) + " h";
    if (h) return std::to_string(h) + " h " + std::to_string(m) + " m";
    return std::to_string(m) + " m";
}

std::string fmt_rep_airtime(uint32_t s)
{
    if (s < 60) return std::to_string(s) + " s";
    const uint32_t h = s / 3600, m = s % 3600 / 60;
    if (h) return std::to_string(h) + " h " + std::to_string(m) + " m";
    return std::to_string(m) + " m";
}

std::string fmt_dbm(int dbm)
{
    return std::to_string(dbm) + " dBm";
}

std::string fmt_snr1(double db)
{
    const long tenths = std::lround(db * 10.0);                 // half away from zero: -6.25 dB is "-6.3"
    char b[32];
    std::snprintf(b, sizeof(b), "%s%ld.%ld", tenths < 0 ? "-" : "", std::labs(tenths) / 10, std::labs(tenths) % 10);
    return b;
}

std::string fmt_db(double db)
{
    return fmt_snr1(db) + " dB";
}

std::string fmt_ago(uint32_t secs)
{
    return format_age(secs) + " ago";
}

std::string neighbour_label(const NeighbourName &n, const Bytes &prefix)
{
    std::string s = n.known && n.text != to_hex(prefix) ? n.text : "<" + to_hex(prefix);
    if (n.ambiguous) s += " (ambiguous " + std::to_string(n.matches) + ")";
    return s;
}

std::string neighbour_value(double snr, uint32_t ago_s)
{
    return "SNR " + fmt_snr1(snr) + kDot + fmt_ago(ago_s);
}

namespace {

const RepeaterReads &reads_of(const RepeaterView &v)
{
    static const RepeaterReads none;
    return v.reads ? *v.reads : none;
}

/* The section of the page's read in flight (sent or about to be). */
bool waiting_on(const RepeaterView &v, RepSection s)
{
    return s != RepSection::None && reads_of(v).in_flight() == s;
}

/* Logged in, or (the owner) a direct route: the section can be read. */
bool readable(const RepeaterView &v, RepSection s)
{
    if (v.session) return true;
    return s == RepSection::Owner && v.hops >= 0;
}

RepeaterRow item(RepSection sec, const std::string &id, const std::string &label, const std::string &value, Tone tone = Tone::Muted)
{
    RepeaterRow r;
    r.kind = RepeaterRow::Item;
    r.section = sec;
    r.id = id;
    r.label = label;
    r.value = value;
    r.tone = tone;
    r.aff = Aff::Info;
    return r;
}

RepeaterRow action(RepSection sec, const std::string &id, const std::string &label, RepAction a, const std::string &value = "")
{
    RepeaterRow r = item(sec, id, label, value);
    r.aff = Aff::Action;
    r.action = a;
    return r;
}

RepeaterRow note(RepSection sec, const std::string &id, const std::string &text, Tone tone)
{
    RepeaterRow r;
    r.kind = RepeaterRow::Note;
    r.section = sec;
    r.id = id;
    r.label = text;
    r.tone = tone;
    r.aff = Aff::None;
    return r;
}

std::string pair_text(uint32_t a, uint32_t b)
{
    return std::to_string(a) + " / " + std::to_string(b);
}

void status_rows(const RepeaterStatus &s, std::vector<RepeaterRow> &rows)
{
    const RepSection S = RepSection::Status;
    rows.push_back(item(S, "st.battery", "Battery", fmt_volts(s.battery_mv)));
    rows.push_back(item(S, "st.uptime", "Uptime", fmt_rep_uptime(s.uptime_s)));
    rows.push_back(item(S, "st.noise", "Noise floor", fmt_dbm(s.noise_floor)));
    rows.push_back(item(S, "st.signal", "Last RSSI / SNR", fmt_dbm(s.last_rssi) + " / " + fmt_db(s.last_snr)));
    rows.push_back(item(S, "st.packets", "Packets received / sent", pair_text(s.packets_rx, s.packets_tx)));
    rows.push_back(item(S, "st.rxsplit", "Received flood / direct", pair_text(s.flood_rx, s.direct_rx)));
    rows.push_back(item(S, "st.txsplit", "Sent flood / direct", pair_text(s.flood_tx, s.direct_tx)));
    if (s.has_airtime_rx) rows.push_back(item(S, "st.airtime", "Airtime TX / RX", fmt_rep_airtime(s.airtime_tx_s) + " / " + fmt_rep_airtime(s.airtime_rx_s)));
    else rows.push_back(item(S, "st.airtime", "Airtime TX", fmt_rep_airtime(s.airtime_tx_s)));
    rows.push_back(item(S, "st.dups", "Duplicates flood / direct", pair_text(s.flood_dups, s.direct_dups)));
    if (s.has_rx_errors) rows.push_back(item(S, "st.rxerr", "RX errors", std::to_string(s.rx_errors)));
    if (s.room) rows.push_back(item(S, "st.posts", "Posts stored / pushed", pair_text(s.posted, s.post_pushes)));
    rows.push_back(item(S, "st.txq", "TX queue", std::to_string(s.tx_queue)));
}

} // namespace

std::vector<RepSection> rep_sections(bool room)
{
    return room ? std::vector<RepSection>{RepSection::Status} : std::vector<RepSection>{RepSection::Status, RepSection::Neighbours, RepSection::Owner};
}

RepState rep_section_state(const RepeaterView &v, RepSection s)
{
    RepState st;
    const RepeaterReads &rd = reads_of(v);
    const RepeaterReads::Section &sec = rd.section(s);
    if (waiting_on(v, s)) {
        if (v.remote && rd.sent() && v.remote->req == rd.req()) st.text = section_wait_text(*v.remote, v.now_ms);
        if (st.text.empty()) st.text = "sending\xE2\x80\xA6";
        return st;
    }
    if (sec.error != Client::RemoteError::None) {
        st.text = read_error_state(sec.error);
        if (sec.has) st.text += ", read " + fmt_clock(sec.read_at);
        st.tone = Tone::Gold;
        return st;
    }
    if (sec.has) {
        st.text = "read " + fmt_clock(sec.read_at);
        return st;
    }
    if (!readable(v, s)) st.text = "needs login";
    return st;
}

RepState rep_summary(const RepeaterView &v, RepSection s)
{
    RepState st;
    const RepeaterReads &rd = reads_of(v);
    const RepeaterReads::Section &sec = rd.section(s);
    if (waiting_on(v, s)) return rep_section_state(v, s);         // "waiting… up to 12 s"
    if (sec.error != Client::RemoteError::None) {                 // the last attempt failed: that is the news
        st.text = read_error_state(sec.error);
        st.tone = Tone::Gold;
        return st;
    }
    if (!sec.has) {
        st.text = readable(v, s) ? "not read yet" : "needs login";
        return st;
    }
    switch (s) {
    case RepSection::Status:
        st.text = fmt_volts(rd.status.battery_mv) + kDot + "up " + fmt_rep_uptime_short(rd.status.uptime_s);
        break;
    case RepSection::Neighbours: {
        if (rd.neighbours.empty()) {
            st.text = "none heard";
            break;
        }
        uint32_t newest = rd.neighbours.front().heard_at;            // (newest first)
        const int heard = std::max(rd.nb_total, static_cast<int>(rd.neighbours.size()));
        st.text = std::to_string(heard) + " heard" + kDot + "newest " + fmt_ago(v.now > newest ? v.now - newest : 0);
        break;
    }
    case RepSection::Owner: {
        std::string who = rd.owner.owner_info.substr(0, rd.owner.owner_info.find('\n'));
        if (who.empty()) who = rd.owner.name;
        if (who.empty()) who = "no owner info";
        st.text = truncate_ellipsis(who, 20);
        std::string fw = rd.owner.fw_version;
        if (!fw.empty() && (fw[0] == 'v' || fw[0] == 'V')) fw.erase(0, 1);
        if (!fw.empty()) st.text += std::string(kDot) + "fw " + fw;
        break;
    }
    case RepSection::None: break;
    }
    return st;
}

RepState rep_section_line(const RepeaterView &v, RepSection s)
{
    RepState st;
    const RepeaterReads &rd = reads_of(v);
    const RepeaterReads::Section &sec = rd.section(s);
    if (waiting_on(v, s)) return rep_section_state(v, s);
    if (sec.error != Client::RemoteError::None) {
        st.text = read_error_text(sec.error, -1, v.name, sec.anonymous);
        st.tone = Tone::Gold;
        return st;
    }
    if (sec.has) {
        st.text = "read " + fmt_clock(sec.read_at) + kDot + "tap Refresh to read again";
        return st;
    }
    if (!readable(v, s)) {
        st.text = s == RepSection::Owner ? "Log in, or get a direct route" : "Log in first: Esc, then Log in";
        return st;
    }
    st.text = "not read yet" + std::string(kDot) + "tap Refresh";
    return st;
}

std::vector<RepeaterRow> repeater_rows(const RepeaterView &v)
{
    std::vector<RepeaterRow> rows;
    RepeaterRow head;
    head.kind = RepeaterRow::Section;
    head.label = "ACCESS";
    head.id = "access";
    rows.push_back(head);
    RepeaterRow access;
    access.id = "access.role";
    access.label = v.session ? std::string(remote_role_name(v.session->role)) + " since " + fmt_clock(v.session->since) : std::string("Not logged in");
    access.value = v.password_saved ? "password saved" : "";
    access.tone = Tone::Muted;
    rows.push_back(access);
    if (v.session) {
        RepeaterRow clock;
        clock.id = "access.clock";
        clock.label = "Clock at login";
        bool ahead = false;
        if (v.session->has_clock) {
            const int64_t skew = static_cast<int64_t>(v.session->clock) - static_cast<int64_t>(v.session->deck_time);
            ahead = skew > kClockAheadWarnS;
            clock.value = fmt_hms(v.session->clock) + kDot + clock_skew_text(skew);
            clock.tone = ahead ? Tone::Gold : Tone::Muted;
        } else {
            clock.value = "not sent (older firmware)";
        }
        rows.push_back(clock);
        if (ahead)
            rows.push_back(note(RepSection::None, "access.ahead", std::string("Its clock is more than a minute ahead of this deck: the ") + (v.room ? "room" : "repeater") +
                                                                     " may drop the commands sent from here.", Tone::Gold));
        rows.push_back(note(RepSection::None, "access.logout", logout_note(v.room), Tone::Muted));
    }
    // 0.8.1: READ, a row per section: its screen opens from it, its summary at the right
    RepeaterRow read;
    read.kind = RepeaterRow::Section;
    read.label = "READ";
    read.id = "read";
    rows.push_back(read);
    for (RepSection s : rep_sections(v.room)) {
        const RepState sum = rep_summary(v, s);
        RepeaterRow r = item(s, std::string("open.") + (s == RepSection::Status ? "st" : s == RepSection::Neighbours ? "nb" : "own"), rep_section_name(s), sum.text,
                             sum.tone);
        r.aff = Aff::Open;
        r.action = RepAction::Open;
        rows.push_back(r);
    }
    return rows;
}

std::vector<RepeaterRow> repeater_section_rows(const RepeaterView &v, RepSection s)
{
    std::vector<RepeaterRow> rows;
    if (s == RepSection::None) return rows;
    const RepeaterReads &rd = reads_of(v);
    const RepeaterReads::Section &sec = rd.section(s);
    const std::string key = s == RepSection::Status ? "st" : s == RepSection::Neighbours ? "nb" : "own";
    const bool anon_owner = s == RepSection::Owner && !v.session;          // the anonymous request is the only way
    const bool failed = sec.error != Client::RemoteError::None && !waiting_on(v, s);
    if (failed) rows.push_back(note(s, key + ".err", read_error_text(sec.error, -1, v.name, sec.anonymous), Tone::Gold));     // whole
    if (s == RepSection::Status) {
        if (sec.has) status_rows(rd.status, rows);
    } else if (s == RepSection::Neighbours) {
        if (sec.has) {
            if (rd.neighbours.empty()) rows.push_back(item(s, "nb.empty", "None heard", ""));
            for (const RepeaterReads::Neighbour &n : rd.neighbours) {
                const NeighbourName nm = v.model ? neighbour_name(*v.model, n.prefix) : NeighbourName{};
                const uint32_t ago = v.now > n.heard_at ? v.now - n.heard_at : 0;
                rows.push_back(item(s, "nb." + to_hex(n.prefix), neighbour_label(nm, n.prefix), neighbour_value(n.snr, ago)));
            }
            if (rd.nb_more)                                           // (after a failed page too: it asks that page again)
                rows.push_back(action(s, "nb.more", "More neighbours", RepAction::MoreNeighbours,
                                      std::to_string(rd.neighbours.size()) + " of " + std::to_string(rd.nb_total) + " shown"));
        }
    } else if (sec.has) {
        rows.push_back(item(s, "own.name", "Name", rd.owner.name.empty() ? std::string("-") : rd.owner.name));
        if (!rd.owner.anonymous) rows.push_back(item(s, "own.fw", "Firmware", rd.owner.fw_version.empty() ? std::string("-") : rd.owner.fw_version));
        if (rd.owner.owner_info.empty()) rows.push_back(item(s, "own.info", "Owner info", "none set"));
        else rows.push_back(note(s, "own.info", rd.owner.owner_info, Tone::Normal));
    }
    if (anon_owner) {
        if (v.hops >= 0) rows.push_back(action(s, "own.read", "Read owner (direct route)", RepAction::ReadOwner));
        else rows.push_back(item(s, "own.login", "Log in to read", "or a direct route"));
        if (sec.anonymous && (sec.error == Client::RemoteError::Timeout || sec.error == Client::RemoteError::NoDirectRoute))
            rows.push_back(note(s, "own.limit", "Without a login the repeater answers this 4 times in 3 minutes, for everyone together.", Tone::Muted));
    } else if (!sec.has && !failed) {
        rows.push_back(item(s, key + ".none", v.session ? "Not read yet" : "Log in to read", ""));
    }
    return rows;
}

std::string login_route_note(int hops)
{
    return hops < 0 ? "No known route: the login is sent to the whole mesh" : "";
}

std::string login_route_line(int hops)
{
    return hops < 0 ? std::string("Route: flood: the login goes to the whole mesh") : "Route: " + repeater_route_text(hops);
}

bool login_offers_reset(int hops)
{
    return hops >= 0;
}

std::string path_reset_text(const Client::PathResetResult &r, const std::string &name)
{
    using E = Client::PathResetError;
    if (r.id == 0) return "";
    if (r.ok) return "Path reset: the next message goes by flood";
    switch (r.error) {
    case E::None: return "";
    case E::NotConnected: return "Not connected to a node.";
    case E::Busy: return "The node is busy (the CLI, or a reset on its way): try again in a moment.";
    case E::NotFound: return truncate_ellipsis(name, 24) + " is not in the node's contacts.";
    case E::Failed: return "The path was not reset" + (r.err_code >= 0 ? " (error " + std::to_string(r.err_code) + ")." : std::string("."));
    case E::Timeout: return "The node did not answer: the path may be unchanged.";
    case E::LinkLost: return "The link to the node was lost.";
    }
    return "";
}

std::vector<RepeaterMoreItem> repeater_more_items(bool password_saved, bool reading)
{
    std::vector<RepeaterMoreItem> v = {
        {RepeaterMore::Details, "Details", Aff::Open},
        {RepeaterMore::OpenCli, "Open in CLI", Aff::Open},
        {RepeaterMore::ResetPath, "Reset path", Aff::Action},
        {RepeaterMore::RefreshAll, reading ? "Stop reading" : "Refresh all", Aff::Action},     // 0.8.1: the chained read, from the hub
    };
    if (password_saved) v.push_back({RepeaterMore::ForgetPassword, "Forget password", Aff::Action});
    return v;
}

std::string remote_wait_text(const Client::RemoteState &s, uint64_t now_ms)
{
    switch (s.phase) {
    case Client::RemoteState::Phase::Idle: return "";
    case Client::RemoteState::Phase::Sending: return "Sending\xE2\x80\xA6";
    case Client::RemoteState::Phase::Waiting: {
        const uint64_t left = s.deadline_ms > now_ms ? s.deadline_ms - now_ms : 0;
        const uint64_t secs = std::max<uint64_t>(1, (left + 999) / 1000);
        return "Waiting\xE2\x80\xA6 up to " + std::to_string(secs) + " s";
    }
    }
    return "";
}

std::string login_error_text(const Client::RemoteResult &r, const std::string &name)
{
    using E = Client::RemoteError;
    switch (r.error) {
    case E::None: return "";
    case E::Timeout: return "No answer. Wrong password, or " + name + " is out of reach.";
    case E::NotFound: return name + " is not in the node's contacts.";
    case E::TableFull: return "The node is busy: try again in a moment.";
    case E::Unsupported: return "The node's firmware is too old for logins.";
    case E::Failed: return "The node refused the login" + (r.err_code >= 0 ? " (error " + std::to_string(r.err_code) + ")." : std::string("."));
    case E::Refused: return name + " refused the password.";
    case E::Busy: return "The CLI has the node: leave the CLI first.";
    case E::NotConnected: return "Not connected to a node.";
    case E::BadPassword: return "The password is too long: " + std::to_string(kMaxLoginPassword) + " bytes at most.";
    case E::Cancelled: return "Cancelled.";
    case E::LinkLost: return "The link to the node was lost.";
    case E::NotLoggedIn:                                    // the read-only requests' errors (report 036 S2): never a login's
    case E::NoDirectRoute:
    case E::BadReply: return "";
    }
    return "";
}

std::string login_success_text(const Client::RemoteResult &r, const std::string &name)
{
    std::string s = "Logged in to " + truncate_ellipsis(name, 24);
    if (r.role != Client::RemoteRole::None) s += " as " + lower(remote_role_name(r.role));     // (a legacy answer says no role)
    switch (r.password) {
    case Client::PasswordSave::Saved: s += ", password saved"; break;
    case Client::PasswordSave::Failed: s += ": the password could not be saved"; break;
    case Client::PasswordSave::Skipped: s += ": this password cannot be saved (spaces at its ends)"; break;
    case Client::PasswordSave::NotAsked: break;
    }
    return s;
}

/* ------------------------------------------------------------------ the CLI */

std::string cli_to_line(const std::string &key_hex)
{
    if (!is_hex64(key_hex)) return "";
    return "to " + lower(key_hex);
}

std::string cli_to_shown(const std::string &name, const std::string &key_hex)
{
    if (!is_hex64(key_hex)) return "";
    const std::string tail = "<" + lower(key_hex.substr(0, 6));
    return name.empty() ? "to " + tail : "to " + name + " " + tail;
}

std::string cli_pick_hint()
{
    return "Contacts > a repeater > More > Open in CLI, or type: to <name>";
}

} // namespace meshhop
