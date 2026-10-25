/*
 * SPDX-License-Identifier: MIT
 *
 * 0.7.0 (report 036, slice S1, with the regrouping the owner chose, option B): the texts and rules of Home and its page loop, the
 * Tools page, what a tap on a contact opens and its Options menu, MY CONTACT in Settings, the Repeater page (its rows, the login
 * box's texts, the error and waiting texts) and the CLI's `to` line (0.7.2: the To... picker is gone). No LVGL, no I/O: unit tested on the PC
 * (tests/unit/test_repeater_ui.inc). The LVGL side is mesh_app.cpp and repeater_page.cpp.
 *
 * 0.8.0, slice S2 (the read-only view): the reads the page keeps while it is open and their queue (RepeaterReads: one request at
 * a time, Refresh all in a chain, Cancel), the texts of their states and errors, the formatting of every value. 0.8.1 (the owner's
 * design after 0.8.0 on the deck): the Repeater page is a hub (ACCESS, then READ: a row per section with its one-line summary,
 * repeater_rows) and each section has a screen of its own (repeater_section_rows, its line under the header rep_section_line).
 */

#pragma once

#include "client.hpp"
#include "features.hpp"
#include "ui_logic.hpp"
#include "ui_pages.hpp"

#include <cstdint>
#include <deque>
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

/* ------------------------------------------------------------------ 0.8.0 / 0.8.1: the read-only view (report 036 slice S2) */

/* The three sections the page reads (a room: STATUS only; a room has no neighbours and no owner request). 0.8.1: each has a
 * screen of its own (a sub-page of the Repeater page, the hub); the hub lists them as rows that open them. */
enum class RepSection { None, Status, Neighbours, Owner };
const char *rep_section_name(RepSection s);        // "Status", "Neighbours", "Owner" ("" for None)
/* What a row does (a tap on the selected row, or Enter): on the hub, Open (its section's screen); on a sub-page, More neighbours
 * (the next page) and Read owner (the anonymous OWNER request: not logged in, direct route only). */
enum class RepAction { None, Open, MoreNeighbours, ReadOwner };

/* What the page read of the node while it is open (nothing goes to disk: leaving the page, logging out and a new login clear it),
 * and the queue of its reads: the client takes one remote request at a time, so the page sends the next only when the last one
 * ended. Refresh all (the hub's More, or R there): Status, then Neighbours (the first page), then Owner, one after the other. A
 * soft failure (no answer, firmware too old, no direct route, an unreadable answer, another ERR) marks that section and the chain
 * goes on with the next; a hard one (not logged in any more, the CLI holds the node, the link went, not connected, the node does
 * not know the repeater, its queue is full, cancelled) stops the chain: the sections still queued keep what they showed. No
 * automatic retry. */
class RepeaterReads {
public:
    struct Section {
        bool has = false;                 // something was read (the data below)
        uint32_t read_at = 0;             // deck time of the last good read
        meshzero::Client::RemoteError error = meshzero::Client::RemoteError::None;   // why the last attempt failed (None: it did not)
        bool anonymous = false;           // Owner: the last attempt (or read) was the anonymous request
        uint16_t offset = 0;              // Neighbours: the page of the last attempt
        bool tried = false;               // 0.8.1: a read of it went (or was refused) since the last clear(): no automatic read any more
    };
    struct Neighbour {
        meshzero::Bytes prefix;
        uint32_t heard_at = 0;            // deck time: the read's time minus its "heard N s ago"
        double snr = 0;
    };
    meshzero::RepeaterStatus status;
    std::vector<Neighbour> neighbours;    // every page read since the first, newest first (one entry per prefix)
    int nb_total = 0;                     // as the last page said
    bool nb_more = false;
    uint16_t nb_next = 0;                 // the offset of the next page (the last page's offset + its entries)
    meshzero::OwnerInfo owner;

    const Section &section(RepSection s) const;
    void clear();
    /* Refresh all: logged in, Status, Neighbours (page 0), Owner (a room: Status); not logged in, Owner alone when the route is direct
     * (the anonymous request), else nothing. Returns false when nothing could be queued (or a chain already runs). */
    bool refresh_all(bool logged_in, bool room, bool direct);
    /* One section (Neighbours: from the first page again). False when a chain runs. */
    bool refresh(RepSection s);
    /* 0.8.1: a sub-page opened: its section is read by itself ONCE, when a session exists and it was never tried since the last
     * clear() (the status after a login was: it shows at once). Nothing while a chain runs, and never again after a failure (the
     * page's Refresh asks again). True when it was queued. */
    bool auto_read(RepSection s, bool logged_in);
    /* The next page of neighbours (nb_next); false when none remains or a chain runs. */
    bool more_neighbours();
    /* Stops the chain: the queue goes; the read in flight ends when its Cancelled result comes (the page cancels it at the client). */
    void cancel();
    bool running() const { return flying_ || !queue_.empty(); }
    size_t queued() const { return queue_.size(); }
    /* A read of this section is in flight or queued. */
    bool busy(RepSection s) const;
    RepSection in_flight() const { return flying_ ? flight_.section : RepSection::None; }
    bool sent() const { return flying_ && req_ != 0; }
    uint32_t req() const { return flying_ ? req_ : 0; }
    struct Next {
        RepSection section = RepSection::None;
        uint16_t offset = 0;
    };
    /* The page's pump: the head of the queue becomes the read in flight (false: nothing queued, or one is in flight already). */
    bool pop(Next &n);
    void sent_as(uint32_t req) { if (flying_) req_ = req; }
    /* The result of the read in flight (also a refusal at once, req 0, right after pop()). Returns the text for the notice line ("":
     * none: a success, Cancelled). Results of other requests are ignored (""). deck_now: read_at and the neighbours' times. */
    std::string on_result(const meshzero::Client::RemoteResult &r, uint32_t deck_now, const std::string &name);
    /* The kind of a failure for the chain: false = it goes on with the next section. */
    static bool stops_chain(meshzero::Client::RemoteError e);

private:
    Section &sec(RepSection s);
    void merge(const meshzero::NeighboursPage &pg, uint32_t deck_now);
    std::deque<Next> queue_;
    Next flight_;
    bool flying_ = false;
    uint32_t req_ = 0;
    Section st_, nb_, own_;
};

/* The text of a read's failure, for the notice line, a sub-page's line and its note (in the spirit of report 036 sections 4.3 and
 * 7): a timeout "No answer. <name> may be out of reach, or its session ended (a reboot): log in again." (the anonymous owner
 * request: "No answer. <name> may be out of reach."), "Log in first", "Firmware too old for this", "No direct route: log in to read
 * the owner", "Unreadable answer", "The CLI holds the node", ... "" for None and Cancelled. */
std::string read_error_text(meshzero::Client::RemoteError e, int err_code, const std::string &name, bool anonymous = false);
/* The short state after a failure (the hub's summaries): "no answer", "needs login", "firmware too old", "direct route only",
 * "unreadable answer", else "not read". */
std::string read_error_state(meshzero::Client::RemoteError e);
/* While a section's request runs: "sending…", "waiting… up to 12 s" ("" when idle). */
std::string section_wait_text(const meshzero::Client::RemoteState &s, uint64_t now_ms);

/* The values. */
std::string fmt_volts(uint32_t mv);                // "4.12 V"
std::string fmt_rep_uptime(uint32_t s);            // "3 d 4 h 12 m", "4 h 12 m", "12 m", "45 s"
std::string fmt_rep_uptime_short(uint32_t s);      // 0.8.1, the hub: "3 d 4 h", "4 h 12 m", "12 m", "45 s"
std::string fmt_rep_airtime(uint32_t s);           // "52 h 3 m", "12 m", "45 s" (hours and minutes)
std::string fmt_dbm(int dbm);                      // "-112 dBm"
std::string fmt_db(double db);                     // "6.5 dB", "-6.3 dB" (one decimal)
std::string fmt_snr1(double db);                   // "7.0", "-12.5" (one decimal)
std::string fmt_ago(uint32_t secs);                // "12 s ago", "5 min ago", "3 h ago", "2 d ago"
/* A neighbour's label: its contact's name, "<e0e1e2e3" (its key prefix, as the Search writes keys) when no contact (or no name)
 * matches, " (ambiguous 2)" after it when two or more contacts start with that prefix. */
std::string neighbour_label(const meshzero::NeighbourName &n, const meshzero::Bytes &prefix);
/* "SNR 7.0  ·  12 s ago". */
std::string neighbour_value(double snr, uint32_t ago_s);

struct RepeaterRow {
    enum Kind { Section, Item, Note } kind = Item;
    std::string label, value;         // Item: the label at the left, the value at the right; Section: its name; Note: the label wraps
    Tone tone = Tone::Muted;          // the value's (Item) or the text's (Note) colour
    // 0.8.0 / 0.8.1
    RepSection section = RepSection::None;     // the hub: the section a row opens; a sub-page: the section shown
    Aff aff = Aff::Info;              // Item: Info (a value), Open (the hub's rows: a chevron) or Action (a row that acts: the dot)
    RepAction action = RepAction::None;
    std::string id;                   // stable over renders: the selection follows it
    /* The keys and taps select the hub's section rows and a sub-page's rows (Info and Action); ACCESS, the headers and the notes
     * are read-only. */
    bool selectable() const { return kind == Item && section != RepSection::None; }
};
struct RepeaterView {
    bool room = false;
    const meshzero::Client::RemoteSession *session = nullptr;   // the login of this connection to the node; nullptr: none
    bool password_saved = false;      // the PasswordStore holds one for it
    // 0.8.0: the read-only view
    const RepeaterReads *reads = nullptr;          // nullptr: nothing read
    const meshzero::Model *model = nullptr;        // the neighbours' names (nullptr: their prefixes)
    const meshzero::Client::RemoteState *remote = nullptr;     // the client's remote wait (the in-flight section's state)
    uint64_t now_ms = 0;              // the poll clock (the wait's seconds left)
    uint32_t now = 0;                 // deck time (the neighbours' ages)
    int hops = -1;                    // the contact's route (-1: flood): the anonymous owner request needs a direct one
    std::string name;                 // the node's name, for the error texts
};
/* The sections of this node, in order: Status, and for a repeater Neighbours and Owner. */
std::vector<RepSection> rep_sections(bool room);
/* A section's state and its colour (Muted; Gold after a failure): "read 14:02", "waiting… up to 12 s", "no answer", "no answer,
 * read 14:02" (old values still shown), "needs login", "firmware too old", "direct route only", "" (nothing yet). */
struct RepState {
    std::string text;
    Tone tone = Tone::Muted;
};
RepState rep_section_state(const RepeaterView &v, RepSection s);
/* 0.8.1, the hub's one-line summary of a section (the value of its row; Gold after a failure):
 *   Status       "4.02 V  ·  up 3 d 4 h"
 *   Neighbours   "12 heard  ·  newest 12 s ago" ("none heard")
 *   Owner        "<the first line of the owner info>  ·  fw 1.12.0" (the anonymous answer has no version; no owner info: the
 *                node's name)
 *   else         "waiting… up to 12 s", "no answer", "firmware too old", "direct route only", "needs login", "not read yet". */
RepState rep_summary(const RepeaterView &v, RepSection s);
/* 0.8.1, the line under the header of a sub-page: "read 14:02  ·  tap Refresh to read again", "waiting… up to 12 s", the error
 * text (Gold; read_error_text), "Log in first: Esc, then Log in" (Owner on a flood route: "Log in, or get a direct route"), "not
 * read yet  ·  tap Refresh". */
RepState rep_section_line(const RepeaterView &v, RepSection s);
/* The hub (the Repeater page). ACCESS: "Not logged in" / "Admin since 14:02" (value "password saved" when one is), then, logged in,
 * "Clock at login" (its time and the skew; Gold with a note when it is ahead by more than a minute) and the note of Log out.
 * 0.8.1: then READ: one Open row (a chevron) per section, its summary as the value (rep_summary). */
std::vector<RepeaterRow> repeater_rows(const RepeaterView &v);
/* 0.8.1, a sub-page's rows (the section alone; its state is the line under the header):
 *   a failure      its whole text first (a Gold note: the line under the header may cut it);
 *   Status         Battery, Uptime, Noise floor, Last RSSI / SNR, Packets received / sent, Received flood / direct, Sent flood /
 *                  direct, Airtime TX / RX (a room: TX), Duplicates flood / direct, RX errors (when sent), Posts stored / pushed (a
 *                  room), TX queue;
 *   Neighbours     one row each (neighbour_label, neighbour_value), newest first; More neighbours (an action row, value "14 of 22
 *                  shown") while more remain; "None heard" for an empty list;
 *   Owner          Name, Firmware (logged in), the owner info (a white note; "none set"); not logged in: Read owner (direct
 *                  route) as an action row, or "Log in to read" (value "or a direct route") on a flood route; after the anonymous
 *                  request failed, a grey note about its limit (4 per 3 minutes, for everyone);
 *   nothing read   "Log in to read" (not logged in) or "Not read yet". */
std::vector<RepeaterRow> repeater_section_rows(const RepeaterView &v, RepSection s);
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

/* 0.7.2: the page's More, in order: Details, Open in CLI, Reset path, (0.8.1) Refresh all ("Stop reading" while a read runs),
 * Forget password (only when one is saved) LAST. */
enum class RepeaterMore { Details, OpenCli, ResetPath, RefreshAll, ForgetPassword };
struct RepeaterMoreItem {
    RepeaterMore action = RepeaterMore::Details;
    std::string label;
    Aff aff = Aff::Open;              // Details, Open in CLI: a chevron; Reset path, Refresh all, Forget password: the action dot (no confirm box)
};
std::vector<RepeaterMoreItem> repeater_more_items(bool password_saved, bool reading = false);
/* While a request waits: "Sending…" until the board confirmed it, then "Waiting… up to N s" (the seconds left of the wait, at
 * least 1); "" when idle. */
std::string remote_wait_text(const meshzero::Client::RemoteState &s, uint64_t now_ms);
/* Why a login ended without success, for the box's message line (a timeout: "No answer. Wrong password, or <name> is out of
 * reach."). "" for a success or no error. */
std::string login_error_text(const meshzero::Client::RemoteResult &r, const std::string &name);
/* After a success: "Logged in to <name> as admin", ", password saved" / ": the password could not be saved" when it was asked.
 * 0.8.0: a role the answer does not say (a legacy node): "Logged in to <name>". */
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
