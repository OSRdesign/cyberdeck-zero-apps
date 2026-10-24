/*
 * SPDX-License-Identifier: MIT
 *
 * 0.7.0 (report 036, slice S1, with the regrouping the owner chose, option B): the texts and rules of Home and its page loop, the
 * Tools page, what a tap on a contact opens and its Options menu, MY CONTACT in Settings, the Repeater page (its rows, the login
 * box's texts, the error and waiting texts) and the CLI's `to` line (0.7.2: the To... picker is gone). No LVGL, no I/O: unit tested on the PC
 * (tests/unit/test_repeater_ui.inc). The LVGL side is mesh_app.cpp and repeater_page.cpp.
 *
 * Slice S2 (Status, Neighbours, Owner) adds its sections to repeater_rows() (RepeaterView carries what they need) and replaces
 * the grey "next version" row; the Refresh cell of the page's bar becomes live then.
 */

#pragma once

#include "client.hpp"
#include "features.hpp"
#include "ui_logic.hpp"
#include "ui_pages.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace meshhop {

/* ------------------------------------------------------------------ Home and the page loop */

enum class HomeItem { Chats, Contacts, Tools, Settings, Node };
/* Home's rows, in order: Chats, Contacts, Tools, Settings, Node (0.7.0: Tools took the CLI row's place among the pages). */
const std::vector<HomeItem> &home_items();
const char *home_item_name(HomeItem i);
int home_index(HomeItem i);                       // the row of an item (-1: none)
/* The Home row a page goes back to (Esc at its root): Chats, Contacts, Tools (and the CLI, which lives under it), Settings. */
int home_row_of(Tab t);
/* Tab / Shift+Tab on a page: Chats, Contacts, Tools, Settings in a loop (dir > 0 forwards). Map and the CLI are never stops:
 * from the CLI the loop goes on as from Tools. */
Tab next_page(Tab t, int dir);
/* Home's Tools row: "3 nearby waiting", or "" (nothing waiting). */
std::string tools_home_value(size_t pending);

/* ------------------------------------------------------------------ Tools */

enum class ToolItem { Nearby, PacketLog, Statistics, Cli };
struct ToolRow {
    ToolItem item = ToolItem::Nearby;
    std::string title, caption;       // the name; its second line (what it holds or does)
    Tone tone = Tone::Muted;          // the second line's colour (Gold: something waits, or the firmware is too old)
};
struct ToolsContext {
    size_t pending = 0;               // Nearby: the nodes waiting to be added (manual add mode)
    bool capturing = false;           // the packet log
    size_t log_n = 0;
    meshzero::FeatureState stats;     // Statistics (a notice: the firmware is too old)
    bool connected = false;
    bool cli_needs_python = false;    // the CLI's warm-up found no python3
};
/* The rows of Tools, in order: Nearby, Packet log, Statistics, CLI (no Trace / Ping yet: a later slice). */
std::vector<ToolRow> tools_rows(const ToolsContext &c);

/* ------------------------------------------------------------------ Contacts: what a tap opens, the Options menu */

/* A tap on the selected row (or Enter): a repeater or a room opens its Repeater page, a chat node its conversation (made when it
 * has none yet), anything else (a sensor, an unknown type) its details. A node of type 0 (none) counts as a chat, as in the
 * Chat filter and in opens_chat(). */
enum class ContactOpen { Repeater, Chat, Details };
ContactOpen contact_open_target(int type);
/* The Options of a contact (a 3 s hold on its row, or I): "Options for <name>": Manage (a repeater or room), Message (a chat
 * node), Details, Groups, Delete. No favourite, no Ping, no Reset path in this slice. */
enum class ContactAction { Manage, Message, Details, Groups, Delete };
struct ContactOptions {
    std::string title;
    std::vector<std::string> labels;
    std::vector<ContactAction> actions;
    std::vector<Aff> affs;            // Manage, Message, Details, Groups: a chevron; Delete: the action dot (it asks first)
};
ContactOptions contact_options(const std::string &name, int type);

/* ------------------------------------------------------------------ Settings: MY CONTACT */

/* The public key on its row: "a0a1 a2a3 ... bebf" (the first 8 and the last 4 hex digits, in groups of 4). "" for a bad key. */
std::string key_short_grouped(const std::string &hex64);
/* The whole key in a box: 16 groups of 4, two lines of 8. "" for a bad key. */
std::string key_grouped_lines(const std::string &hex64);

/* ------------------------------------------------------------------ the Repeater page */

/* The route of a contact: "flood" (no known path), "direct" (zero hop), "direct, 2 hops" (a known path over 2 repeaters). */
std::string repeater_route_text(int hops);
/* The line under the title: "Repeater  ·  direct, 2 hops  ·  heard 3 min ago" ("Room ..." for a room; "never heard"). */
std::string repeater_status_line(int type, int hops, uint32_t heard);
/* "Guest", "Read-only", "Read-write", "Admin" (a repeater gives Guest or Admin, a room any of them). */
const char *remote_role_name(meshzero::Client::RemoteRole r);
/* The remote clock against this deck's at the login: "in step with the deck" (under 2 s), "1 s ahead", "3 min behind". */
std::string clock_skew_text(int64_t skew_s);
/* Ahead by more than this, its replay rule may drop the commands sent from here (report 036 section 4.3): a warning. */
constexpr int64_t kClockAheadWarnS = 60;

struct RepeaterRow {
    enum Kind { Section, Item, Note } kind = Item;
    std::string label, value;         // Item: the label at the left, the value at the right; Note: the label wraps, grey or Gold
    Tone tone = Tone::Muted;          // the value's (Item) or the text's (Note) colour
};
struct RepeaterView {
    bool room = false;
    const meshzero::Client::RemoteSession *session = nullptr;   // the login of this connection to the node; nullptr: none
    bool password_saved = false;      // the PasswordStore holds one for it
};
/* ACCESS: "Not logged in" / "Admin since 14:02" (value "password saved" when one is), then, logged in, "Clock at login" (its time
 * and the skew; Gold with a note when it is ahead by more than a minute) and the note of Log out; then the grey row of what comes
 * next ("Status, neighbours and owner: next version"). */
std::vector<RepeaterRow> repeater_rows(const RepeaterView &v);
/* Log out's help line: the remote node keeps this deck in its access list until setperm (admin) or a reboot (guest). */
std::string logout_note(bool room);

/* The login box: "No known route: the login is sent to the whole mesh" when the contact has no path (hops < 0), else "". */
std::string login_route_note(int hops);
/* 0.7.2: the login box's line under its title (the page's line is under the box): "Route: direct, 2 hops", "Route: direct", and
 * with no route "Route: flood: the login goes to the whole mesh" (one line at 640x480: it stands for login_route_note). */
std::string login_route_line(int hops);
/* The box's Reset path button: only when a route is known (a flood route has nothing to forget). */
bool login_offers_reset(int hops);
/* After Client::reset_path: "Path reset: the next message goes by flood", or why not. "" for no result. */
std::string path_reset_text(const meshzero::Client::PathResetResult &r, const std::string &name);

/* 0.7.2: the page's More, in order: Details, Open in CLI, Reset path, Forget password (only when one is saved) LAST. */
enum class RepeaterMore { Details, OpenCli, ResetPath, ForgetPassword };
struct RepeaterMoreItem {
    RepeaterMore action = RepeaterMore::Details;
    std::string label;
    Aff aff = Aff::Open;              // Details, Open in CLI: a chevron; Reset path, Forget password: the action dot (no confirm box)
};
std::vector<RepeaterMoreItem> repeater_more_items(bool password_saved);
/* While a request waits: "Sending…" until the board confirmed it, then "Waiting… up to N s" (the seconds left of the wait, at
 * least 1); "" when idle. */
std::string remote_wait_text(const meshzero::Client::RemoteState &s, uint64_t now_ms);
/* Why a login ended without success, for the box's message line (a timeout: "No answer. Wrong password, or <name> is out of
 * reach."). "" for a success or no error. */
std::string login_error_text(const meshzero::Client::RemoteResult &r, const std::string &name);
/* After a success: "Logged in to <name> as admin", ", password saved" / ": the password could not be saved" when it was asked. */
std::string login_success_text(const meshzero::Client::RemoteResult &r, const std::string &name);

/* ------------------------------------------------------------------ the CLI: Open in CLI */

/* What the CLI types for a picked repeater or room: "to <its 64 hex digits, lower case>" (no line end; a full key matches one
 * contact only in meshcore-cli, whatever the names). "" for a key that is not 64 hex digits. */
std::string cli_to_line(const std::string &key_hex);
/* 0.7.2: what the output shows for that automatic line instead of its echo: "to Hilltop repeater <909192" (the name, then the
 * first 6 key digits: two of the same name differ there); "to <909192" without a name. "" for a bad key. */
std::string cli_to_shown(const std::string &name, const std::string &key_hex);
/* 0.7.2: the CLI's caption when it was opened from Tools and nothing was sent yet (the To... picker is gone). */
std::string cli_pick_hint();

} // namespace meshhop
