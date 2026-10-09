/*
 * SPDX-License-Identifier: MIT
 *
 * The UI-independent logic of Mesh Hop: key translation (evdev code to text), the filters of the text editors, number
 * formatting, the rows of the chat list and of the contacts table, sorting and filtering, the Back rule, the popups (choice with
 * arrows, Yes / No, menu), the layout of the Settings list and the settings edits. No LVGL, no I/O: everything here is unit
 * tested on the PC (tests/test_ui.cpp).
 */

#pragma once

#include "archive.hpp"
#include "channel_key.hpp"
#include "client.hpp"
#include "features.hpp"
#include "model.hpp"
#include "presets.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace meshhop {

/* ------------------------------------------------------------------ keys */

enum class Key {
    None, Char, Enter, Esc, Tab, BackTab, Up, Down, Left, Right, PageUp, PageDown, Home, End, Backspace, Delete, Function,
};

struct KeyEvent {
    Key key = Key::None;
    std::string text;            // UTF-8, for Key::Char
    bool ctrl = false, alt = false, shift = false;
    bool repeat = false;         // auto-repeat of a held key (Esc, Enter and Tab ignore it)
    int fn = 0;                  // Key::Function: 1..12
    int code = 0;                // the evdev code
};

enum class Layout { US, FR };
/* "us", "fr", "fr(azerty)"... (case insensitive); anything else: US. */
Layout layout_from_name(const std::string &name);
/* The XKBLAYOUT="..." line of /etc/default/keyboard (the text of that file) -> layout name, "" when absent. */
std::string xkb_layout_from_text(const std::string &text);
/* $MESHHOP_KEYMAP, else /etc/default/keyboard, else US. */
Layout detect_layout();

/* Feeds evdev EV_KEY events (value 0 release, 1 press, 2 repeat). Returns true when `out` holds a key to act on (a press or a
 * repeat of a key that does something); modifier keys only update the state. */
class KeyTranslator {
public:
    explicit KeyTranslator(Layout layout = Layout::US) : layout_(layout) {}
    bool feed(int code, int value, KeyEvent &out);
    void set_layout(Layout l) { layout_ = l; }
    void reset();                // all modifiers up (the keyboard went away)

private:
    Layout layout_;
    bool lshift_ = false, rshift_ = false, lctrl_ = false, rctrl_ = false, lalt_ = false, ralt_ = false, caps_ = false;
};

/* ------------------------------------------------------------------ text editors */

enum class EditMode { Free, Name, Number, Clock, Channel, HexKey };
/* What a typed piece of text may add to `cur`: the accepted text, "" when rejected (a character not allowed in this editor,
 * or more than limit_bytes). A comma becomes a decimal point in a Number editor. */
std::string filter_typed(EditMode mode, const std::string &cur, const std::string &add, size_t limit_bytes);

/* ------------------------------------------------------------------ formatting */

std::string fmt_clock(uint32_t ts);                       // "14:05" in local time, "--:--" when the clock is not set
std::string fmt_age(uint32_t when);                       // "5 s", "5 min", "-" when unknown
std::string fmt_age_coarse(uint32_t when);                // "< 1 min" below a minute, else as fmt_age: changes at most once a minute
std::string fmt_freq(double mhz);
std::string fmt_bw(double khz);
std::string fmt_snr(bool has, double snr);                // "-3.5 dB" or "-"
std::string fmt_hops(int hops);                           // "direct", "2 hops", "flood" (-1)
std::string fmt_distance(double km);                      // "850 m", "12.4 km", "340 km"
double distance_km(double lat1, double lon1, double lat2, double lon2);
std::string truncate_ellipsis(const std::string &s, size_t max_chars);   // by characters, adds "..."

/* ------------------------------------------------------------------ chats */

struct ChatRow {
    enum Kind { Header, Channel, Direct, Add } kind = Header;
    std::string key;             // conversation key ("c:0", "d:<hex>"), "+" for the add row, "" for a header
    std::string title;
    int unread = 0;              // 0 while muted
    bool muted = false;
    uint32_t last_ts = 0;
    std::string last_text;       // not shown in the list any more (the list shows the unread count), kept for the tests
};
/* The unread count in a pill or a tab title: "7", "99+" for 100 and more, "" for none. */
std::string fmt_count_badge(int n);
/* also_direct: a direct conversation key ("d:<hex>") to list even when it has no message yet (a chat opened from Contacts). */
std::vector<ChatRow> build_chat_rows(const meshzero::Model &model, const std::string &also_direct = "");
/* Moves the selection by delta over the selectable rows (headers are skipped); stays put at the ends. -1 when none. */
int step_selection(const std::vector<ChatRow> &rows, int current, int delta);
int first_selectable(const std::vector<ChatRow> &rows);
int find_chat_row(const std::vector<ChatRow> &rows, const std::string &key);
/* The same stepping over any list: selectable[i] says whether row i can be selected. -1 when none is. */
int step_selectable(const std::vector<bool> &selectable, int current, int delta);

/* ------------------------------------------------------------------ contacts */

enum class SortKey { Heard, Name, Type, Snr, Hops, Distance };
const char *sort_name(SortKey k);
SortKey next_sort(SortKey k);

struct ContactRow {
    std::string key_hex;
    std::string name;            // the name, or the first 6 key bytes when it has none
    int type = 0;
    std::string type_name;
    uint32_t heard = 0;
    bool has_snr = false;
    double snr = 0;
    int hops = -1;               // -1: flood / unknown
    bool has_pos = false;
    double lat = 0, lon = 0;
    bool has_dist = false;
    double dist_km = 0;
};
/* Filters of the contacts table (C2): by node type and by time since the node was last heard. */
enum class TypeFilter { All, Chat, Repeater, Room, Sensor };
enum class AgeFilter { Any, Hour, Day, Week };
const char *type_filter_name(TypeFilter t);         // "All", "Chat", "Repeater", "Room", "Sensor"
const char *age_filter_name(AgeFilter a);           // "Any time", "1 h", "24 h", "7 d"
TypeFilter next_type_filter(TypeFilter t);
AgeFilter next_age_filter(AgeFilter a);
uint32_t age_filter_seconds(AgeFilter a);           // 0 for Any
bool type_matches(TypeFilter f, int contact_type);
struct ContactFilter {
    TypeFilter type = TypeFilter::All;
    AgeFilter age = AgeFilter::Any;
    bool active() const { return type != TypeFilter::All || age != AgeFilter::Any; }
};
/* Does a row pass? `now` is the app clock; a node never heard (heard == 0) fails any time filter. */
bool passes_filter(const ContactRow &r, const ContactFilter &f, uint32_t now);

/* Sort, direction and filters of the table: what the user chose, kept while the screen is shown, left, re-entered. */
struct ContactView {
    SortKey sort = SortKey::Heard;
    bool reverse = false;
    ContactFilter filter;
    std::string group;                              // a contact group (phase 2); empty = every contact
    /* A tap on a column title: the same column reverses, another one sorts by it. */
    void tap_column(SortKey k);
    void cycle_sort();                              // the Sort button / S key: next key, ascending
    void toggle_reverse() { reverse = !reverse; }
    bool operator==(const ContactView &o) const
    {
        return sort == o.sort && reverse == o.reverse && filter.type == o.filter.type && filter.age == o.filter.age && group == o.group;
    }
    bool operator!=(const ContactView &o) const { return !(*this == o); }
};

/* Sorted rows. Heard: newest first; Snr: best first; Name / Type / Hops / Distance: ascending (unknown values last).
 * reverse flips the order. filter drops the rows that do not match; now 0 = the app clock. only_keys (when given) keeps just those
 * contacts: the members of the chosen group. */
std::vector<ContactRow> build_contact_rows(const meshzero::Model &model, SortKey sort, bool reverse = false, const ContactFilter &filter = ContactFilter(),
                                           uint32_t now = 0, const std::set<std::string> *only_keys = nullptr);

/* The rows as they are on screen. A tap resolves the row from THIS list (never from a list rebuilt later), so the row that opens is
 * the row that was tapped, in whatever order it is shown. */
struct ContactSnapshot {
    ContactView view;
    std::vector<ContactRow> rows;
    std::string key_at(int index) const;            // "" for a bad index
    int index_of(const std::string &key_hex) const; // -1 when not shown
};
/* view.group, when set, restricts the rows to the members of that group of the model. */
ContactSnapshot make_contact_snapshot(const meshzero::Model &model, const ContactView &view, uint32_t now = 0);

/* ---- phase 2: selecting contacts for a bulk delete or a group (C3, C1) */

/* The marked contacts of the select mode. Only what is SHOWN is ever acted on: a mark on a row the filter hides stays, but is not deleted. */
class ContactSelection {
public:
    void clear() { keys_.clear(); }
    bool contains(const std::string &key_hex) const { return keys_.count(key_hex) != 0; }
    bool toggle(const std::string &key_hex);                      // the new state
    size_t size() const { return keys_.size(); }
    void select_shown(const std::vector<ContactRow> &rows);
    /* Not heard for at least `days` days (a node never heard counts as stale). now 0 = the app clock. */
    void select_stale(const std::vector<ContactRow> &rows, int days, uint32_t now = 0);
    void select_never_heard(const std::vector<ContactRow> &rows);
    void invert(const std::vector<ContactRow> &rows);             // the shown rows
    std::vector<std::string> shown_selected(const std::vector<ContactRow> &rows) const;     // in row order
    size_t hidden_count(const std::vector<ContactRow> &rows) const;                           // marked, but filtered out of view
    void keep_only(const std::set<std::string> &existing);        // forget marks of contacts that are gone

private:
    std::set<std::string> keys_;
};
enum class SelectAction { All, None, Stale7, Stale30, NeverHeard, Invert };
struct SelectMenu {
    std::vector<std::string> labels;
    std::vector<SelectAction> actions;
};
SelectMenu select_menu_items();
/* The Yes / No box of a bulk delete: how many, who (the first names), what it does. */
struct BulkDeletePrompt {
    std::string title, body;
    size_t count = 0;
    bool empty = true;
};
BulkDeletePrompt bulk_delete_prompt(const meshzero::Model &model, const std::vector<ContactRow> &rows, const ContactSelection &sel);
/* The pick list of the group button: "All contacts", then every group with its size. Index 0 is "all". */
std::vector<std::string> group_filter_labels(const meshzero::ContactGroups &g);
/* What the Group button of the select mode offers. */
enum class GroupAction { AddTo, RemoveFrom, NewWith, RemoveAll, Manage };
struct GroupMenu {
    std::vector<std::string> labels;
    std::vector<GroupAction> actions;
};
GroupMenu group_assign_menu(const meshzero::ContactGroups &g, size_t selected);
/* "Friends (12)" etc. for pick lists. */
std::vector<std::string> group_pick_labels(const meshzero::ContactGroups &g);

/* ---- phase 2: the static details of a contact (C8) */

/* "no route (flood)", "direct", "3 hops (2-byte hashes): 1a2b > 3c4d > 5e6f" */
std::string fmt_route(uint8_t path_len, const std::array<uint8_t, 64> &path);
std::string fmt_node_time(uint32_t ts);                           // "2026-10-08 14:05", "-" when the clock was not set
std::string fmt_position(bool has, double lat, double lon);       // "48.85660, 2.35220" or "not shared"
struct ContactDetail {
    bool found = false;
    bool chat = false;                  // has a text conversation
    std::string title, name, type, key1, key2, route, last_advert, heard, position, distance, snr, groups;
};
ContactDetail build_contact_detail(const meshzero::Model &model, const std::string &key_hex);

/* ---- phase 2: the Nearby list (C4) */

enum class NearbyState { Pending, New, Contact, Ignored };
const char *nearby_state_name(NearbyState s);
struct NearbyRow {
    std::string key_hex;
    std::string title;                  // the name, else "Node <first 6 hex>"
    std::string type_name;              // "Chat", "Repeater", ... or "?"
    int type = 0;
    std::string detail;                 // "SNR 5.5 dB  direct  12 s ago"
    NearbyState state = NearbyState::New;
    bool can_add = false;               // a full key and a known type
    bool has_snr = false;
    double snr = 0;
};
/* Pending first, then the newest. Ignored nodes are listed only with show_ignored. */
std::vector<NearbyRow> build_nearby_rows(const meshzero::Model &model, bool show_ignored, uint32_t now = 0);
size_t ignored_nearby_count(const meshzero::Model &model);
std::string fmt_signal(bool has_snr, double snr, bool has_rssi, int rssi);       // "SNR 5.5 dB  RSSI -80"

/* ---- phase 2: the message archive search (M7) */

struct SearchState {
    std::string text;
    std::string conv;                   // a conversation key; empty = all
    meshzero::DatePreset date = meshzero::DatePreset::Any;
    uint32_t from = 0, to = 0;          // the custom range
    meshzero::SearchDir dir = meshzero::SearchDir::Any;
    meshzero::SearchQuery query(uint32_t now = 0) const;
    void cycle_dir();                   // all -> received -> sent -> all
    std::string date_label() const;
    std::string conv_label(const meshzero::Model &model) const;
};
struct SearchRow {
    uint32_t seq = 0;
    std::string conv;
    std::string head;                   // "14:05  Alice  You" style first line
    std::string snippet;                // the text around the match
    bool outgoing = false;
};
std::vector<SearchRow> build_search_rows(const meshzero::Model &model, const SearchState &st, size_t *total = nullptr, uint32_t now = 0);
/* A one line piece of `text` around the first match of `needle`, at most max_chars characters, with "..." where it was cut. */
std::string make_snippet(const std::string &text, const std::string &needle, size_t max_chars);
std::string fmt_when(uint32_t ts, uint32_t now = 0);              // "14:05" today, "09-30 14:05" before
/* The conversations to pick from (key, title): the channels and every direct conversation with messages. */
std::vector<std::pair<std::string, std::string>> search_conversations(const meshzero::Model &model);

/* ---- phase 2: the statistics screen (D9) */

struct StatLine {
    std::string label, value;
};
struct StatsView {
    std::vector<StatLine> left;         // core and radio
    std::vector<StatLine> right;        // packets
    std::string updated;                // "Updated 3 s ago" / "Not read yet"
    bool any = false;
};
StatsView build_stats_view(const meshzero::StatsSnapshot &s, uint32_t now = 0);
std::string fmt_uptime(uint32_t secs);                            // "45 s", "12 min", "2 h 05 min", "3 d 4 h"
/* True for the contacts that have a text conversation (chat nodes); repeaters, rooms and sensors open the detail panel. */
bool opens_chat(int type);

/* ------------------------------------------------------------------ settings */

/* Row numbers of the radio settings in a RadioSettings edit. */
enum SettingRow { kRowName = 0, kRowFreq = 1, kRowBw = 2, kRowSf = 3, kRowCr = 4, kRowTx = 5 };
/* The stepper of a row (touch buttons, Left/Right). */
void adjust_setting(meshzero::RadioSettings &s, int row, int delta);
/* A typed number for a row; returns "" when accepted (s changed) or the reason (s unchanged). */
std::string apply_number(meshzero::RadioSettings &s, int row, const std::string &text);
/* One line per changed setting for the Save confirmation: "Frequency 869.525 -> 868.100 MHz". Empty when nothing differs. */
std::vector<std::string> describe_radio_changes(const meshzero::RadioSettings &current, const meshzero::RadioSettings &edited);

/* ---- the choice popup: the value in the middle, an arrow on each side (bandwidth, SF, CR, TX power, preset, retries) */
struct Choice {
    std::string title;
    std::vector<std::string> labels;
    std::vector<std::string> details;       // optional second line per label (may be empty)
    std::string note;                       // an extra line (for example "the current settings match no preset")
    int index = 0;
    int initial = 0;
    bool step(int delta);                   // clamps at both ends; true when the index moved
    bool can_prev() const { return index > 0; }
    bool can_next() const { return index + 1 < static_cast<int>(labels.size()); }
    const std::string &label() const;
    std::string detail() const;
    std::string position() const;           // "3 / 26"
};
enum class ChoiceField { Bandwidth, Sf, Cr, Tx, Preset, RetryAttempts, ResetAfter, PathHash, AdvertEvery, AdvertKind };
/* What the phase 2 choices start from: the path hash mode the board reports (-1 unknown) and the advert schedule. */
struct ChoiceExtra {
    int path_hash_mode = -1;
    meshzero::AdvertSchedule schedule;
};
Choice make_choice(ChoiceField f, const meshzero::RadioSettings &s, const meshzero::RetrySettings &retry, const ChoiceExtra *extra = nullptr);
/* Applies the accepted index to the edited settings (or to the retry settings for the two retry fields). */
void apply_choice(ChoiceField f, int index, meshzero::RadioSettings &s, meshzero::RetrySettings &retry);

enum class ChoiceResult { None, Moved, Accept, Cancel };
/* Left / Right (also Up / Down) step, Enter accepts, Esc cancels. A repeat of Enter or Esc is ignored. */
ChoiceResult choice_key(const KeyEvent &e, Choice &c);

/* The mode a choice index stands for (path hash: index = mode) and the schedule an index stands for. */
int path_hash_mode_from_index(int index);
meshzero::AdvertSchedule apply_advert_choice(ChoiceField f, int index, meshzero::AdvertSchedule current);

/* ---- the Yes / No box. focus: 0 = No, 1 = Yes. Y and N answer at once, Left / Right move, Enter takes the focus, Esc is No. */
enum class ConfirmResult { None, Moved, Yes, No };
ConfirmResult confirm_key(const KeyEvent &e, int &focus);
/* A Yes that must wait (the second box of a factory reset): the seconds still to wait at `now`, 0 once it may be used. */
int confirm_wait_left(uint64_t opened_ms, uint64_t now_ms, uint32_t delay_ms);
std::string confirm_yes_text(const std::string &yes, int wait_left);       // "Yes, erase (3)" while waiting

/* ---- a pick list (a long list of rows in a box): Up / Down move, Enter takes, Esc cancels, Home / End jump. */
enum class ListResult { None, Moved, Accept, Cancel };
ListResult list_key(const KeyEvent &e, int &selected, int count);

/* ---- a menu of big buttons: Up / Down move, Enter takes, Esc cancels. */
enum class MenuResult { None, Moved, Accept, Cancel };
MenuResult menu_key(const KeyEvent &e, int &selected, int count);

/* ---- the Settings list. Save to radio and Undo changes are the LAST rows; the fixed Public channel (slot 0) is not listed. */
enum class SRowKind {
    Header, Info, Name, Preset, Freq, Bw, Sf, Cr, Tx, RetryAttempts, ResetAfter, SyncClock, BoardGps, GpsNotice, Channel, AddChannel,
    AddPublic, ChannelsNotice, HistoryAll, HistoryOlder, HistoryNote, Undo, Save,
    // phase 2 (0.2.0)
    Position, Stats, StatsNotice, PathHash, PathHashNotice, Repeat, RepeatNotice, AdvertEvery, AdvertKind, ManualAdd, AutoAddTypes,
    AutoAddNotice, Reboot, FactoryReset,
    // 0.2.2: the data kept per board
    HistoryForget, HistoryOthers,
    // phase 3: the packet log (D8)
    PacketLog,
};
struct SRowSpec {
    SRowKind kind = SRowKind::Info;
    int arg = 0;                    // Header: section number, Channel: slot, Info: which line
};
struct SettingsContext {
    bool connected = false;
    meshzero::FeatureState gps;             // BoardGps
    meshzero::FeatureState channel_admin;   // ChannelAdmin
    bool slot0_empty = false;               // the Public channel is missing: offer to add it back
    std::vector<int> channels;              // slots of the channels to list (empty slots and slot 0 are dropped by the builder)
    meshzero::FeatureState path_hash;       // PathHashMode: a row to choose 1 / 2 / 3 bytes, or the "firmware too old" notice
    meshzero::FeatureState stats;           // Stats
    meshzero::FeatureState autoadd;         // AutoAdd
    meshzero::RepeatState repeat;           // client repeat (shown / available / why not)
    bool have_self = false;                 // the board settings were read (manual add can be shown)
    bool board_known = false;               // SELF_INFO was seen: there is a board whose saved data can be forgotten
    size_t other_boards = 0;                // folders of other boards that are saved on the deck
};
/* The header titles of the Settings sections, by the number the layout gives them. */
const char *settings_section_title(int section);
std::vector<SRowSpec> settings_layout(const SettingsContext &ctx);
/* True for the rows the selection can rest on (everything except headers, info lines and notices). */
bool settings_row_selectable(SRowKind k);
/* The channels listed in Settings: every non-empty slot except 0. */
struct ChannelEntry {
    int idx = 0;
    std::string name;
    meshzero::ChannelKind kind = meshzero::ChannelKind::Private;
    bool muted = false;
    bool has_key = false;           // the key was read from the board in this session: it can be shown
};
std::vector<ChannelEntry> build_channel_entries(const meshzero::Model &model);

/* The auto-add filter of the board (SET_AUTOADD_CONFIG): the node types it still adds by itself in manual add mode, and the overwrite flag. */
struct AutoAddItem {
    std::string label;                  // "Chat nodes", "Repeaters", ...
    uint8_t flag = 0;
    bool on = false;
};
std::vector<AutoAddItem> autoadd_items(const meshzero::AutoaddConfig &c);
uint8_t autoadd_toggled(uint8_t config, uint8_t flag);
std::string autoadd_summary(const meshzero::AutoaddConfig &c);        // "Chat, Repeater" / "none"

/* ------------------------------------------------------------------ history (phase 1b): options of a conversation, bulk delete */

/* What the options box of a conversation offers. A direct chat: mute / unmute this contact, delete the conversation. A channel: mute /
 * unmute, delete its messages (the channel stays). Cancel is the button of the box. */
enum class ConvAction { Mute, Unmute, DeleteConversation, DeleteMessages };
struct ConvOptions {
    std::string title;
    std::vector<std::string> labels;
    std::vector<ConvAction> actions;       // one per label
};
ConvOptions conversation_options(const meshzero::Model &model, const std::string &conv);
/* The Yes / No box before a deletion. empty = true when there is nothing to delete (no box is shown then, only a notice). */
struct DeletePrompt {
    std::string title, body;
    size_t count = 0;
    bool empty = false;
};
DeletePrompt delete_conversation_prompt(const meshzero::Model &model, const std::string &conv);
DeletePrompt delete_all_prompt(const meshzero::Model &model);
DeletePrompt delete_older_prompt(const meshzero::Model &model, int days, uint32_t now);
/* The data kept per board (0.2.2): forget what the deck saved for the connected board, or for the boards that are not connected. */
DeletePrompt forget_board_prompt(const meshzero::Model &model, const std::string &board_id);
DeletePrompt forget_others_prompt(size_t boards, uint64_t bytes);
std::string fmt_bytes(uint64_t n);                      // "512 B", "12 KB", "1.4 MB"
std::string fmt_boards(size_t n);                       // "1 board", "3 boards"

/* "Delete messages older than": 7, 30 or 90 days. */
const std::vector<int> &history_day_options();
Choice make_days_choice(int initial_days);
/* The UNIX time before which a message is "older than N days"; 0 when the app clock is not plausible (nothing can be told). */
uint32_t history_cutoff(uint32_t now, int days);
std::string fmt_message_count(size_t n);               // "1 message", "12 messages"

/* A touch held on a name (header or list row) for 3 s opens the options. The cue (a progress bar) appears after kShowMs so a plain tap
 * or a scroll never flashes it; a move of more than kSlop pixels, a release or a lost press cancels. Pure state: the app feeds it events. */
class HoldTracker {
public:
    static constexpr uint64_t kShowMs = 500, kFireMs = 3000;
    static constexpr int kSlop = 16;
    void begin(const std::string &target, int x, int y, uint64_t now);
    void move(int x, int y);
    void cancel();                                  // release, scroll or lost press
    bool active() const { return active_; }
    const std::string &target() const { return target_; }
    bool visible(uint64_t now) const { return active_ && now - start_ >= kShowMs; }
    double progress(uint64_t now) const;            // 0..1 over kFireMs
    /* True exactly once when the hold has lasted kFireMs; the tracker is then idle and the click of the release is to be swallowed. */
    bool poll(uint64_t now);
    bool take_swallow();                            // the click that follows a completed hold: true once

private:
    bool active_ = false, swallow_ = false;
    std::string target_;
    int x_ = 0, y_ = 0;
    uint64_t start_ = 0;
};

/* True when the frequency is inside the EU 868 band. */
bool in_eu868(double mhz);
constexpr double kEu868PresetMhz = 869.525;

/* ------------------------------------------------------------------ navigation */

enum class Tab { Chats, Map, Contacts, Terminal, Settings };
const char *tab_name(Tab t);
Tab step_tab(Tab t, int delta);

struct NavState {
    Tab tab = Tab::Chats;
    bool popup_open = false;        // a choice, Yes / No, menu, info, list or progress box
    bool editor_open = false;       // the modal text editor (settings values, channel name, clock)
    bool detail_open = false;       // the contact detail panel
    bool compose_focus = false;     // Chats: the keyboard types into the message entry
    bool nearby_open = false;       // Contacts: the Nearby list
    bool search_open = false;       // Chats: the message search
    bool stats_open = false;        // Settings: the statistics screen
    bool packet_log_open = false;   // Settings: the packet log (D8)
    bool select_mode = false;       // Contacts: marking contacts for a bulk delete or a group
};
enum class BackAction { ClosePopup, CancelEditor, CloseSearch, CloseStats, CloseDetail, CloseNearby, ExitSelect, FocusList, ExitHint, ClosePacketLog };
/* A short Esc and every Back button. Never quits: only the launcher's 3 s hold ends the app. */
BackAction back_action(const NavState &s);

/* ------------------------------------------------------------------ messages */

enum class Tone { Normal, Muted, Gold, Green, Blue, Red };
struct StatusText {
    std::string text;            // "sending", "sent", "delivered", "no ack", "failed: ..."
    Tone tone = Tone::Muted;
};
/* The delivery status of one of our messages (empty text for a received one). A channel message ends at "sent" (or "sent
 * (unconfirmed)" when the board never answered): a channel has no acknowledgement, so it is never "delivered". Once the board heard it
 * come back over repeaters (Message::heard_back) a sent channel message says "heard back by N repeaters" instead: N is the count of
 * distinct routes, an approximation of repeaters (see Message::heard_back). */
StatusText message_status(const meshzero::Message &m, bool channel);

/* ------------------------------------------------------------------ phase 3: the packet log (D8) */

const char *payload_type_name(uint8_t type);              // "REQ", "GRP_TXT", "ADVERT" ... "TYPE 13" for an unknown one
std::string route_name(const meshzero::LogPacket &p);     // "FLOOD", "DIRECT", "T-FLOOD", "T-DIRECT" (T: with transport codes)
std::string fmt_hms(uint32_t ts);                         // "14:05:33" local time, "--:--:--" when the clock is not set
/* The channels whose key hashes to `hash` (first byte of sha256 of the secret): only channels whose key was read from the board in this
 * session. Two names = an ambiguous hash. */
std::vector<std::string> channels_for_hash(const meshzero::Model &model, int hash);
/* "d9", "d9 Public", "d9 Public|#test ?" (ambiguous: two channels share the hash). "" for a packet that is not a group text. */
std::string channel_hash_text(const meshzero::Model &model, const meshzero::LogPacket &p);
struct PacketRow {
    std::string time, route, type, hops, snr, rssi, size, channel;
};
PacketRow build_packet_row(const meshzero::Model &model, const meshzero::LoggedPacket &lp);
/* The raw packet in hex, 4 byte groups: "15416364 d9eb430a ...". */
std::string packet_hex(const meshzero::Bytes &raw);
/* The line above the hex of the selected packet: "path 63de (1 hop, 2-byte hashes)  transport 2211 4433  raw 89 bytes". */
std::string packet_detail(const meshzero::LogPacket &p);
/* "Capturing: 12 / 500", "Stopped: 12 / 500", ", 40 dropped" when the ring overflowed. */
std::string packet_log_status(const meshzero::PacketLog &log);

/* The one line of the status footer for the board. */
struct BoardLine {
    std::string text;
    Tone tone = Tone::Muted;
};
BoardLine board_line(const meshzero::Client &client, const meshzero::Model &model);

} // namespace meshhop
