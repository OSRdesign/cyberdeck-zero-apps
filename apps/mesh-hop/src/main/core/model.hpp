/*
 * SPDX-License-Identifier: MIT
 *
 * Application state: contacts, channels, conversations, device and radio information. No I/O, no LVGL: the
 * Client (protocol events) writes it, the UI reads it, a Listener (the history Store) persists it.
 */

#pragma once

#include "channel_key.hpp"
#include "clock_policy.hpp"
#include "groups.hpp"
#include "protocol.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace meshzero {

enum class Dir { In, Out };
enum class MsgState { Received, Pending, Sent, Delivered, Failed, NoAck };
const char *msg_state_name(MsgState s);

struct Message {
    uint32_t seq = 0;            // local, strictly increasing, never reused (also the order of the history)
    std::string conv;            // "d:<12 hex>" (direct, first 6 key bytes) or "c:<idx>" (channel)
    Dir dir = Dir::In;
    MsgState state = MsgState::Received;
    uint32_t ts = 0;             // deck clock when received / sent: the time shown
    uint32_t sender_ts = 0;      // the sender's clock (identity for de-duplication only)
    std::string sender;          // display name ("You" for outgoing)
    std::string text;
    bool has_snr = false;
    double snr = 0;
    int hops = -1;               // -1: direct / unknown
    std::string note;            // failure reason
};

struct ContactRec {
    Contact c;
    uint32_t heard_local = 0;    // deck clock of the last advert / message seen from it (0: never since cached)
    bool has_snr = false;
    double snr = 0;              // SNR of the last message received from it

    std::string key_hex() const { return to_hex(c.key); }
    KeyPrefix prefix() const;
    /* The best "last heard" time: our clock when we saw it, else what the radio stored, else the node's clock. */
    uint32_t last_heard() const;
};

struct ChannelRec {
    uint8_t idx = 0;
    std::string name;
    bool empty = true;
    bool app_added = false;
    bool key_ok = true;          // read live from the board: its key is the hashtag key of its name (cached slots: true)
    bool has_secret = false;     // the key was read from the board in this session (never written to the history files)
    ChannelSecret secret{};
    /* public / hashtag / private; Private when the key is not known (a cached slot) and the name is no hashtag. */
    ChannelKind kind() const;
};

struct ConvSummary {
    std::string key;
    std::string title;
    bool channel = false;
    int channel_idx = -1;
    int unread = 0;              // 0 for a muted conversation
    bool muted = false;
    uint32_t last_ts = 0;
    std::string last_text;
};

/* What the connected board can do, learned from its answers (D10: a feature the firmware lacks is hidden or disabled with a notice). */
enum class Cap { Unknown, No, Yes };
struct BoardCaps {
    Cap custom_vars = Cap::Unknown;   // GET_CUSTOM_VARS answered (No: "unsupported" error)
    Cap channels = Cap::Unknown;      // GET_CHANNEL answered
    bool gps_listed = false;          // the custom variable "gps" exists (the board has a GPS)
    bool gps_on = false;
    Cap autoadd = Cap::Unknown;       // GET_AUTOADD_CONFIG answered (phase 2)
    Cap stats = Cap::Unknown;         // GET_STATS answered
    Cap discover = Cap::Unknown;      // SEND_CONTROL_DATA (the zero-hop discover) accepted
    Cap repeat_freqs = Cap::Unknown;  // GET_ALLOWED_REPEAT_FREQ answered
    bool autoadd_known = false;
    AutoaddConfig autoadd_config;     // the flags the board reported
    std::vector<RepeatRange> repeat_ranges;   // where the board may repeat (kHz); empty = nowhere (or not asked)
};

/* One node seen on the air: from an ADVERT push, an advert in the radio log, a NEW_ADVERT (pending, manual add mode) or the answer to a
 * zero-hop discover. The list lives for the session (it is not saved); "ignored" keys are kept in prefs.txt. */
struct NearbyRec {
    std::string key_hex;              // 64 hex digits, or 16 when only the 8 byte prefix of a discover answer is known
    bool full_key = false;
    PubKey key{};                     // valid when full_key
    uint8_t type = 0;                 // ADV_TYPE_*; 0 = not known yet
    std::string name;                 // empty = not known yet
    bool has_pos = false;
    double lat = 0, lon = 0;
    bool has_snr = false;
    double snr = 0;                   // the latest
    bool has_rssi = false;
    int rssi = 0;
    int hops = -1;                    // hops the latest packet took (0 = heard direct); -1 = not known
    int hash_size = 0;                // bytes per hop hash of that packet (0 = not known)
    uint32_t first_heard = 0, last_heard = 0;     // app clock
    bool pending = false;             // the board told us about it (NEW_ADVERT) but did not add it: manual add mode
    bool discovered = false;          // answered an active zero-hop discover
    bool has_contact = false;         // `contact` holds the whole record from a NEW_ADVERT: Add sends exactly that
    Contact contact;
};
struct NearbyObs {
    std::string key_hex;
    bool full_key = true;
    uint8_t type = 0;
    std::string name;
    bool has_pos = false;
    double lat = 0, lon = 0;
    bool has_snr = false;
    double snr = 0;
    bool has_rssi = false;
    int rssi = 0;
    int hops = -1;
    int hash_size = 0;
    bool discovered = false;
};
constexpr size_t kMaxNearby = 150;

/* The statistics screen (D9): the last answer of each sub type. */
struct StatsSnapshot {
    std::optional<CoreStats> core;
    std::optional<RadioStats> radio;
    std::optional<PacketStats> packets;
    uint32_t updated = 0;             // app clock of the latest answer
};

/* Retry rules for direct messages (M3). Channels have no acknowledgement and are never retried. */
struct RetrySettings {
    int attempts = 3;                 // total sends, 1..4 (the wire carries the attempt number in two bits)
    int reset_after = 2;              // RESET_PATH before this attempt number (the flood fallback), 0 = never
};
constexpr int kMaxRetryAttempts = 4;
/* Clamps to the valid range; reset_after must be below attempts (a path reset before the first send is pointless). */
RetrySettings normalize_retry(RetrySettings r);

/* The editable radio settings (a copy of what the device reported, plus the user's edits). */
struct RadioSettings {
    std::string name;
    double freq_mhz = 0;
    double bw_khz = 0;
    int sf = 0;
    int cr = 0;
    int tx_power = 0;
    int max_tx_power = 0;

    bool same_radio(const RadioSettings &o) const;
};
/* Empty string when valid, else the reason (shown in the settings screen). */
std::string validate_radio(const RadioSettings &s);
std::string validate_name(const std::string &name);
/* The bandwidths the SX126x radios offer, in kHz. */
const std::vector<double> &lora_bandwidths();

/* Where the app's time comes from (shown on Status). */
struct ClockInfo {
    ClockSource source = ClockSource::NotSet;
    uint32_t board_time = 0;     // the board clock when it was read / written (0: unknown)
    bool gps_known = false;      // the board answered the custom variables
    bool gps = false;            // custom variable "gps" says enabled
};

class Listener {
public:
    virtual ~Listener() = default;
    virtual void message_added(const Message &m) = 0;
    virtual void message_state(uint32_t seq, MsgState state, const std::string &note) = 0;
    virtual void contacts_changed() = 0;
    virtual void channels_changed() = 0;
    virtual void read_changed() = 0;
    virtual void prefs_changed() {}          // muted conversations, retry settings, the advert schedule, ignored nodes
    virtual void messages_removed() {}       // messages were deleted (a conversation, everything, or the old ones): rewrite the history
    virtual void groups_changed() {}         // contact groups (phase 2)
    /* A board with another identity (public key) is connected: everything the model held for the previous board was dropped, the new
     * board's saved data is to be loaded now (load_* calls). Called once per change, never for the same board again. */
    virtual void board_changed(const std::string & /*key_hex*/) {}
    /* The app data of the current board (messages, read marks, groups, mute flags, ignored nodes, the advert schedule) was forgotten on purpose. */
    virtual void board_data_forgotten() {}
};

/* The scheduled self advert (D2): the board has no advert interval, so the app sends the advert itself. Default off. */
struct AdvertSchedule {
    int interval_hours = 0;           // 0 = off; 1, 3, 6 or 12
    bool flood = true;                // flood advert (reaches the whole mesh) or zero-hop (direct neighbours only)
    bool operator==(const AdvertSchedule &o) const { return interval_hours == o.interval_hours && flood == o.flood; }
};
const std::vector<int> &advert_interval_options();            // 0, 1, 3, 6, 12
AdvertSchedule normalize_schedule(AdvertSchedule s);

class Model {
public:
    static constexpr size_t kMaxPerConversation = 200;
    static constexpr size_t kMaxMessages = 1500;

    void set_listener(Listener *l) { listener_ = l; }
    /* ---- which board the data belongs to. Messages, read marks, mute flags, groups, ignored nodes, the advert schedule, the contact and
     * channel caches and the Nearby list all belong to ONE board, identified by its public key (SELF_INFO). The first SELF_INFO, and any
     * later one with another key, drops all of it and tells the listener (the store) to load what it saved for that board. Before the
     * first SELF_INFO nothing is loaded: the model is empty ("no board"). Local preferences about the deck (retry rules) stay. */
    bool board_known() const { return !board_key_.empty(); }
    const std::string &board_key() const { return board_key_; }          // 64 hex digits, empty while no board was seen
    uint32_t board_epoch() const { return board_epoch_; }                // +1 at every change of board (the UI resets its selections)
    bool select_board(const PubKey &key);                                // true when it was another board
    /* Forget the app data of the current board on purpose (history, read marks, groups, mute flags, ignored nodes, the advert schedule).
     * The board's contacts and channels, which mirror the board itself, stay. */
    void forget_board_app_data();
    uint32_t revision() const { return revision_; }
    void touch() { ++revision_; }

    // ---- device / radio
    void set_self(const SelfInfo &s);
    void set_device(const DeviceInfo &d);
    void set_battery(const Battery &b);
    void clear_device();
    const ClockInfo &clock() const { return clock_; }
    void set_clock(const ClockInfo &c) { clock_ = c; ++revision_; }                              // on disconnect: the cached contacts and history stay
    const std::optional<SelfInfo> &self() const { return self_; }
    const std::optional<DeviceInfo> &device() const { return device_; }
    const std::optional<Battery> &battery() const { return battery_; }
    const BoardCaps &caps() const { return caps_; }
    void set_caps(const BoardCaps &c) { caps_ = c; ++revision_; }
    std::string self_name() const;
    RadioSettings radio_settings() const;             // from SelfInfo; zeros while unknown

    // ---- contacts
    void replace_contacts(const std::vector<Contact> &list);     // a complete GET_CONTACTS
    void upsert_contact(const Contact &c);
    void remove_contact(const PubKey &key);
    void heard(const PubKey &key, uint32_t when);                // an advert push
    void note_snr(const KeyPrefix &prefix, double snr, uint32_t when);
    void load_contact(const ContactRec &rec);                    // from the cache (no listener call)
    const ContactRec *find_contact(const std::string &key_hex) const;
    const ContactRec *find_by_prefix(const KeyPrefix &prefix) const;
    std::vector<const ContactRec *> contacts_sorted() const;      // most recently heard first
    size_t contact_count() const { return contacts_.size(); }
    /* While a batch is open (a bulk delete) the contact cache is not rewritten after every change, only once at the end. */
    void begin_contact_batch() { batch_ = true; batch_dirty_ = false; }
    void end_contact_batch();

    // ---- nearby nodes (phase 2): adverts heard, pending contacts of the manual add mode, discover answers
    void note_nearby(const NearbyObs &obs, uint32_t when);
    void note_heard_advert(const HeardAdvert &h, uint32_t when);
    /* A NEW_ADVERT in manual add mode: a node the board did not add. */
    void add_pending(const Contact &c, uint32_t when);
    const NearbyRec *find_nearby(const std::string &key_hex) const;
    std::vector<const NearbyRec *> nearby_sorted() const;          // pending first, then newest
    size_t nearby_count() const { return nearby_.size(); }
    size_t pending_count() const;
    void remove_nearby(const std::string &key_hex);
    void clear_nearby();
    bool is_ignored(const std::string &key_hex) const { return ignored_.count(key_hex) != 0; }
    void set_ignored(const std::string &key_hex, bool ignored);
    void load_ignored(const std::string &key_hex) { ignored_.insert(key_hex); }
    const std::set<std::string> &ignored() const { return ignored_; }
    static constexpr size_t kMaxIgnored = 300;
    /* The board's contacts, channels and what was heard are gone (used by tests; a factory reset now shows up as a board with a new identity). */
    void clear_board_data();

    // ---- statistics (D9)
    void set_stats(const StatsReply &s, uint32_t when);
    const StatsSnapshot &stats() const { return stats_; }

    // ---- contact groups (C1) and the advert schedule (D2): local to the deck
    const ContactGroups &groups() const { return groups_; }
    ContactGroups &groups_mut() { return groups_; }              // after a change the caller calls groups_changed()
    void groups_changed();
    void load_groups(const std::string &text) { groups_.parse(text); ++revision_; }
    const AdvertSchedule &advert_schedule() const { return schedule_; }
    void set_advert_schedule(AdvertSchedule s);
    void load_advert_schedule(AdvertSchedule s) { schedule_ = normalize_schedule(s); }

    // ---- channels
    void set_channel(const ChannelRec &c);
    void load_channel(const ChannelRec &c);
    const std::vector<ChannelRec> &channels() const { return channels_; }
    const ChannelRec *find_channel(int idx) const;
    std::string channel_title(int idx) const;
    /* Names (with the #) of the hashtag channels the app added; persisted by the store. */
    void mark_added(const std::string &name);
    void unmark_added(const std::string &name);
    void load_added(const std::string &name) { added_.insert(name); }
    bool is_added(const std::string &name) const { return added_.count(name) != 0; }
    const std::set<std::string> &added_names() const { return added_; }
    /* First free slot (index >= 1), or -1 when none is known to be free / the slots were not read yet. */
    int free_channel_slot() const;
    /* Drops the "added by this app" markers that no longer match the board's channel table: no slot with that name, or the
     * slot holds that name with another key. Called after all slots were read at connect. Returns how many were dropped. */
    int reconcile_added();
    const ChannelRec *find_channel_by_name(const std::string &name) const;

    // ---- conversations
    static std::string conv_direct(const KeyPrefix &prefix);
    static std::string conv_channel(int idx);
    /* Adds a received message. Returns its seq, or 0 when it duplicates a recent one. */
    uint32_t add_incoming(const IncomingMessage &m, uint32_t local_time);
    uint32_t add_outgoing(const std::string &conv, const std::string &text, uint32_t local_time, int hops = -1);
    void load_message(const Message &m);                         // from the history (no listener call)
    void set_state(uint32_t seq, MsgState state, const std::string &note = "");
    const Message *find_message(uint32_t seq) const;
    std::vector<const Message *> messages(const std::string &conv) const;   // oldest first
    std::vector<ConvSummary> conversations() const;               // channels, then directs by last activity
    std::string conv_title(const std::string &conv) const;
    int unread(const std::string &conv) const;                    // 0 while the conversation is muted
    int unread_raw(const std::string &conv) const;                // the count whatever the mute flag says
    int unread_total() const;                                     // muted conversations are not counted
    void mark_read(const std::string &conv);
    void load_read(const std::string &conv, uint32_t last_read_seq)
    {
        read_[conv] = last_read_seq;
        if (last_read_seq >= next_seq_) next_seq_ = last_read_seq + 1;      // a deleted message must not let its number be reused
    }
    const std::map<std::string, uint32_t> &read_marks() const { return read_; }
    uint32_t next_seq() const { return next_seq_; }
    size_t message_count() const { return messages_.size(); }

    // ---- local preferences (kept by the store in prefs.txt)
    void set_muted(const std::string &conv, bool muted);
    bool is_muted(const std::string &conv) const { return muted_.count(conv) != 0; }
    const std::set<std::string> &muted() const { return muted_; }
    void load_muted(const std::string &conv) { muted_.insert(conv); }
    const RetrySettings &retry() const { return retry_; }
    void set_retry(RetrySettings r);
    void load_retry(RetrySettings r) { retry_ = normalize_retry(r); }
    const std::vector<Message> &all_messages() const { return messages_; }

    // ---- deleting history (phase 1b). Contacts, channels, settings, mute flags and the board are never touched.
    size_t message_count_in(const std::string &conv) const;
    /* All messages of one conversation and its read mark. Returns how many were removed. */
    size_t delete_conversation(const std::string &conv);
    /* Every message of every conversation (read marks cleared). */
    size_t delete_all_messages();
    /* Messages with a plausible time before `cutoff` (a UNIX time). A message whose time is unknown (clock was not set) is kept. */
    size_t count_older_than(uint32_t cutoff) const;
    size_t delete_older_than(uint32_t cutoff);

private:
    void trim(const std::string &conv);
    void reset_board_scoped();

    void notify_contacts();
    void refresh_pending();

    std::string board_key_;
    uint32_t board_epoch_ = 0;
    ClockInfo clock_;
    std::optional<SelfInfo> self_;
    std::optional<DeviceInfo> device_;
    std::optional<Battery> battery_;
    BoardCaps caps_;
    std::set<std::string> muted_;
    RetrySettings retry_;
    AdvertSchedule schedule_;
    ContactGroups groups_;
    StatsSnapshot stats_;
    std::map<std::string, NearbyRec> nearby_;
    std::set<std::string> ignored_;
    bool batch_ = false, batch_dirty_ = false;
    std::map<std::string, ContactRec> contacts_;
    std::vector<ChannelRec> channels_;
    std::set<std::string> added_;
    std::vector<Message> messages_;                               // in seq order
    std::map<std::string, uint32_t> read_;                        // conv -> highest seq read
    uint32_t next_seq_ = 1;
    uint32_t revision_ = 1;
    Listener *listener_ = nullptr;
};

} // namespace meshzero
