/*
 * SPDX-License-Identifier: MIT
 *
 * 0.7.0: Home, Tools, the contact's Options, MY CONTACT, the Repeater page and the CLI's `to` line (see ui_repeater.hpp).
 */

#include "ui_repeater.hpp"

#include "protocol.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>

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

std::vector<RepeaterRow> repeater_rows(const RepeaterView &v)
{
    std::vector<RepeaterRow> rows;
    rows.push_back({RepeaterRow::Section, "ACCESS", "", Tone::Muted});
    RepeaterRow access;
    access.label = v.session ? std::string(remote_role_name(v.session->role)) + " since " + fmt_clock(v.session->since) : std::string("Not logged in");
    access.value = v.password_saved ? "password saved" : "";
    access.tone = Tone::Muted;
    rows.push_back(access);
    if (v.session) {
        RepeaterRow clock;
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
            rows.push_back({RepeaterRow::Note, std::string("Its clock is more than a minute ahead of this deck: the ") + (v.room ? "room" : "repeater") +
                                                   " may drop the commands sent from here.", "", Tone::Gold});
        rows.push_back({RepeaterRow::Note, logout_note(v.room), "", Tone::Muted});
    }
    rows.push_back({RepeaterRow::Note, "Status, neighbours and owner: next version", "", Tone::Muted});
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

std::vector<RepeaterMoreItem> repeater_more_items(bool password_saved)
{
    std::vector<RepeaterMoreItem> v = {
        {RepeaterMore::Details, "Details", Aff::Open},
        {RepeaterMore::OpenCli, "Open in CLI", Aff::Open},
        {RepeaterMore::ResetPath, "Reset path", Aff::Action},
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
    }
    return "";
}

std::string login_success_text(const Client::RemoteResult &r, const std::string &name)
{
    std::string role = remote_role_name(r.role);
    role = lower(role);
    std::string s = "Logged in to " + truncate_ellipsis(name, 24) + " as " + role;
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
