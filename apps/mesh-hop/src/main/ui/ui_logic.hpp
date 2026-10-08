/*
 * SPDX-License-Identifier: MIT
 *
 * The UI-independent logic of Mesh Hop: key translation (evdev code to text), the filters of the text editors, number
 * formatting, the rows of the chat list and of the contacts table, sorting and filtering, the Back rule, the popups (choice with
 * arrows, Yes / No, menu), the layout of the Settings list and the settings edits. No LVGL, no I/O: everything here is unit
 * tested on the PC (tests/test_ui.cpp).
 */

#pragma once

#include "channel_key.hpp"
#include "client.hpp"
#include "features.hpp"
#include "model.hpp"
#include "presets.hpp"

#include <cstdint>
#include <string>
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
    /* A tap on a column title: the same column reverses, another one sorts by it. */
    void tap_column(SortKey k);
    void cycle_sort();                              // the Sort button / S key: next key, ascending
    void toggle_reverse() { reverse = !reverse; }
    bool operator==(const ContactView &o) const
    {
        return sort == o.sort && reverse == o.reverse && filter.type == o.filter.type && filter.age == o.filter.age;
    }
    bool operator!=(const ContactView &o) const { return !(*this == o); }
};

/* Sorted rows. Heard: newest first; Snr: best first; Name / Type / Hops / Distance: ascending (unknown values last).
 * reverse flips the order. filter drops the rows that do not match; now 0 = the app clock. */
std::vector<ContactRow> build_contact_rows(const meshzero::Model &model, SortKey sort, bool reverse = false, const ContactFilter &filter = ContactFilter(),
                                           uint32_t now = 0);

/* The rows as they are on screen. A tap resolves the row from THIS list (never from a list rebuilt later), so the row that opens is
 * the row that was tapped, in whatever order it is shown. */
struct ContactSnapshot {
    ContactView view;
    std::vector<ContactRow> rows;
    std::string key_at(int index) const;            // "" for a bad index
    int index_of(const std::string &key_hex) const; // -1 when not shown
};
ContactSnapshot make_contact_snapshot(const meshzero::Model &model, const ContactView &view, uint32_t now = 0);
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
enum class ChoiceField { Bandwidth, Sf, Cr, Tx, Preset, RetryAttempts, ResetAfter };
Choice make_choice(ChoiceField f, const meshzero::RadioSettings &s, const meshzero::RetrySettings &retry);
/* Applies the accepted index to the edited settings (or to the retry settings for the two retry fields). */
void apply_choice(ChoiceField f, int index, meshzero::RadioSettings &s, meshzero::RetrySettings &retry);

enum class ChoiceResult { None, Moved, Accept, Cancel };
/* Left / Right (also Up / Down) step, Enter accepts, Esc cancels. A repeat of Enter or Esc is ignored. */
ChoiceResult choice_key(const KeyEvent &e, Choice &c);

/* ---- the Yes / No box. focus: 0 = No, 1 = Yes. Y and N answer at once, Left / Right move, Enter takes the focus, Esc is No. */
enum class ConfirmResult { None, Moved, Yes, No };
ConfirmResult confirm_key(const KeyEvent &e, int &focus);

/* ---- a menu of big buttons: Up / Down move, Enter takes, Esc cancels. */
enum class MenuResult { None, Moved, Accept, Cancel };
MenuResult menu_key(const KeyEvent &e, int &selected, int count);

/* ---- the Settings list. Save to radio and Undo changes are the LAST rows; the fixed Public channel (slot 0) is not listed. */
enum class SRowKind {
    Header, Info, Name, Preset, Freq, Bw, Sf, Cr, Tx, RetryAttempts, ResetAfter, SyncClock, BoardGps, GpsNotice, Channel, AddChannel,
    AddPublic, ChannelsNotice, Undo, Save,
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
};
std::vector<SRowSpec> settings_layout(const SettingsContext &ctx);
/* The channels listed in Settings: every non-empty slot except 0. */
struct ChannelEntry {
    int idx = 0;
    std::string name;
    meshzero::ChannelKind kind = meshzero::ChannelKind::Private;
    bool muted = false;
    bool has_key = false;           // the key was read from the board in this session: it can be shown
};
std::vector<ChannelEntry> build_channel_entries(const meshzero::Model &model);

/* True when the frequency is inside the EU 868 band. */
bool in_eu868(double mhz);
constexpr double kEu868PresetMhz = 869.525;

/* ------------------------------------------------------------------ navigation */

enum class Tab { Chats, Map, Contacts, Terminal, Settings };
const char *tab_name(Tab t);
Tab step_tab(Tab t, int delta);

struct NavState {
    Tab tab = Tab::Chats;
    bool popup_open = false;        // a choice, Yes / No, menu, info or Loading box
    bool editor_open = false;       // the modal text editor (settings values, channel name, clock)
    bool detail_open = false;       // the contact detail panel
    bool compose_focus = false;     // Chats: the keyboard types into the message entry
};
enum class BackAction { ClosePopup, CancelEditor, CloseDetail, FocusList, ExitHint };
/* A short Esc and every Back button. Never quits: only the launcher's 3 s hold ends the app. */
BackAction back_action(const NavState &s);

/* ------------------------------------------------------------------ messages */

enum class Tone { Normal, Muted, Gold, Green, Blue, Red };
struct StatusText {
    std::string text;            // "sending", "sent", "delivered", "no ack", "failed: ..."
    Tone tone = Tone::Muted;
};
/* The delivery status of one of our messages (empty text for a received one). A channel message ends at "sent" (or "sent
 * (unconfirmed)" when the board never answered): a channel has no acknowledgement, so it is never "delivered". */
StatusText message_status(const meshzero::Message &m, bool channel);

/* The one line of the status footer for the board. */
struct BoardLine {
    std::string text;
    Tone tone = Tone::Muted;
};
BoardLine board_line(const meshzero::Client &client, const meshzero::Model &model);

} // namespace meshhop
