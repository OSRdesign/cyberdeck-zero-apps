/*
 * SPDX-License-Identifier: MIT
 *
 * The companion radio session: connection management, the command queue (one command in flight, matched to its
 * response by code, with timeouts), the start-up sequence, message sync, delivery tracking, and the user actions.
 * It is driven by poll() from the UI timer and never blocks. It talks to an ITransport and writes a Model.
 */

#pragma once

#include "clock_policy.hpp"
#include "frame.hpp"
#include "model.hpp"
#include "transport.hpp"

#include <deque>
#include <functional>
#include <memory>

namespace meshzero {

enum class Link {
    Searching,      // no board found / the board went away: retrying
    Denied,         // the port cannot be opened (dialout group)
    Busy,           // another program has the port
    Error,          // any other open failure
    Handshake,      // port open, asking the board who it is
    NotCompanion,   // the port answers nothing a companion radio would (wrong firmware, wrong device)
    Ready,
};

struct Notice {
    uint32_t id = 0;            // grows with every notice: the UI shows a new one when it changes
    bool ok = true;
    std::string text;
};

class Client {
public:
    /* Behaviour switches (one line each to change a decision). */
    struct Options {
        /* Run the clock policy at connect (clock_policy.hpp: write the deck time, ask the user, or use the board's clock).
         * false: never write the board clock and never ask; the board's own clock is read and used. Default: true. */
        bool set_device_time = true;
        /* Log every frame the board sends (not only the answers to a channel send). MESHHOP_DEBUG=1 in the app. */
        bool log_frames = false;
    };
    /* Seconds a channel message may stay "sending" without an error before it is shown as sent (unconfirmed). */
    static constexpr uint32_t kChannelSendFallbackMs = 5000;

    Client(ITransport &transport, Model &model);
    Options &options() { return options_; }
    /* Where the debug lines go (the app's log file); nothing is logged without it. No message text is ever logged. */
    void set_log(std::function<void(const std::string &)> fn) { log_ = std::move(fn); }

    /* The check of the deck clock (DeckClockProbe in the app, a fake in the tests). state() may answer Unknown while it runs. */
    void set_deck_clock(std::function<DeckClock()> state, std::function<void()> refresh);
    /* Re-runs the clock policy ("Sync clock now"). */
    bool sync_clock();
    /* The policy needs the user to type the start date and time (deck offline, board without GPS). */
    bool clock_prompt_pending() const { return clock_stage_ == ClockStage::Prompt; }
    void clock_set_user(uint32_t unix_time);     // the typed time: written to the board
    void clock_skip();                           // Esc: the board clock stays as it is

    /* Call often (every 20-100 ms) with a monotonic millisecond clock. */
    void poll(uint64_t now_ms);

    Link link() const { return link_; }
    bool ready() const { return link_ == Link::Ready; }
    /* One line for the status screen, e.g. "Connected", "No board found". */
    std::string link_text() const;
    /* A second line with the hint (group, port in use, ...). */
    std::string link_hint() const;
    const Notice &notice() const { return notice_; }
    ITransport &transport() { return transport_; }

    // ---- user actions (return false / 0 and set a notice when refused)
    bool send_advert(bool flood);
    uint32_t send_direct(const std::string &contact_key_hex, const std::string &text);
    uint32_t send_channel(int channel_idx, const std::string &text);
    bool save_settings(const RadioSettings &edited);
    bool add_public_channel();
    /* Adds the hashtag channel `name` (with the #) in the first free slot; its key is sha256(name)[0:16]. */
    bool add_hashtag_channel(const std::string &name);
    /* Adds a private channel (a name and a 16-byte key) in the first free slot. */
    bool add_private_channel(const std::string &name, const ChannelSecret &key);
    /* Clears the slot of a channel (empty name, zero key). Slot 0 (Public) is never removed. */
    bool remove_channel(int idx);
    /* Board GPS on/off: SET_CUSTOM_VAR gps:1|0, then the variables are read back. Needs the board to list the variable. */
    bool set_board_gps(bool on);
    bool refresh_contacts();
    /* The most text that can be sent in this conversation (bytes). */
    size_t max_text(const std::string &conv) const;

    // ---- phase 2: contacts
    /* Deletes contacts on the board one by one (REMOVE_CONTACT per key, the next one only after the answer to the last). Progress in
     * bulk(); the model and the contact cache are updated as each one goes, the cache file is rewritten once at the end. */
    struct BulkProgress {
        bool active = false;
        size_t total = 0, done = 0, failed = 0;
        bool cancelled = false;          // the user stopped it, or the link was lost
        uint32_t id = 0;                 // grows with every job: the UI shows the end of a job once
    };
    bool remove_contacts(const std::vector<PubKey> &keys);
    void cancel_bulk() { if (bulk_.active) bulk_.cancelled = true; }
    const BulkProgress &bulk() const { return bulk_; }
    /* Adds a node of the Nearby list to the board (ADD_UPDATE_CONTACT). */
    bool add_nearby(const std::string &key_hex);
    /* Manual add mode (SET_OTHER_PARAMS manual_add_contacts): true = new nodes wait in Nearby until added, false = the board adds them. */
    bool set_manual_add(bool manual);
    /* The auto-add filter (SET_AUTOADD_CONFIG): which types the board still adds by itself in manual mode, and overwrite-oldest. */
    bool refresh_autoadd();
    bool set_autoadd_flags(uint8_t flags);
    /* The zero-hop discover (SEND_CONTROL_DATA): the answers arrive for kDiscoverWindowMs and fill the Nearby list. */
    static constexpr uint32_t kDiscoverWindowMs = 8000;
    struct DiscoverState {
        bool active = false;
        uint32_t tag = 0;
        uint64_t until_ms = 0;
        int found = 0;                   // distinct nodes that answered
        uint32_t finished_id = 0;        // grows when a scan ends
    };
    bool start_discover(uint8_t type_filter = kDiscoverAllTypes);
    const DiscoverState &discover() const { return discover_; }

    // ---- phase 2: the board
    bool reboot();
    /* FACTORY_RESET: the firmware formats its file system (this can take a good while on an ESP32 board: the wait is kFactoryResetWaitMs), answers
     * OK, waits a second and restarts with a new identity. The answer may be missing when the link drops first: the outcome is judged by the identity
     * the board reports when it is back (reset_phase()). */
    bool factory_reset();
    static constexpr uint32_t kFactoryResetWaitMs = 60000;
    enum class ResetPhase { None, Waiting, Done, KeptKeys };
    /* Waiting: asked, the board has not come back yet. Done: it came back with another identity (shown for 2 minutes). KeptKeys: it came back with
     * the same keys, so nothing was erased (shown for 2 minutes). */
    ResetPhase reset_phase() const { return reset_phase_; }
    /* Path hash size: mode 0, 1, 2 = 1, 2, 3 bytes per hash in the paths of the packets the board sends. */
    bool set_path_hash_mode(int mode);
    bool set_client_repeat(bool on);
    bool refresh_stats();                 // core, radio and packet statistics, one after the other
    bool stats_busy() const { return stats_pending_ > 0; }
    bool refresh_self();                  // APP_START again: position, name, flags as the board holds them now

    // ---- counters (tests, diagnostics)
    size_t frames_received() const { return frames_rx_; }
    size_t bad_frames() const { return bad_frames_ + parser_.bad_headers(); }
    size_t junk_bytes() const { return parser_.junk_bytes(); }
    size_t queued() const { return urgent_.size() + normal_.size() + (inflight_ ? 1 : 0); }

private:
    enum class Outcome { Ok, Error, Timeout, Aborted };
    using Done = std::function<void(Outcome, const Packet *, int err)>;
    struct Request {
        Bytes payload;
        std::vector<uint8_t> accept;      // response codes that complete the command
        std::vector<uint8_t> progress;    // codes that keep it alive (the contact stream)
        uint32_t timeout_ms = 5000;
        bool accept_any = false;          // any answer that is not an error and not a push completes it (channel send)
        bool no_reply = false;            // the board sends no answer (reboot): the command completes when it is written
        Done done;
    };
    struct Outgoing {
        uint32_t seq = 0;
        PubKey key{};
        KeyPrefix prefix{};
        std::string text;
        uint32_t ts = 0;
        int attempt = 0;
        bool awaiting = false;
        uint64_t ack_deadline = 0;
        std::vector<std::array<uint8_t, 4>> acks;
    };

    void enqueue(Request r, bool urgent = false);
    void pump_queue();
    void finish(Outcome o, const Packet *p, int err);
    void on_frame(const Bytes &frame);
    void drop_link(const std::string &why, uint64_t retry_in_ms);
    void try_open();
    void start_handshake();
    void send_query();
    void send_app_start();
    void after_ready();
    enum class ClockStage { Idle, Busy, WaitDeck, Prompt };
    void start_clock_sync();
    void tick_clock();
    void write_time(uint32_t ts, ClockSource source, int64_t offset_after);
    void use_board_clock(ClockSource source, const std::string &note);
    void queue_channels(int count);
    void maybe_sync_and_poll();
    void transmit(uint32_t seq);
    void handle_ack(const Ack &a);
    void tick_outgoing();
    void set_notice(bool ok, const std::string &text);
    void log(const std::string &text) const { if (log_) log_(text); }
    void apply_custom_vars(Outcome o, const Packet *p, int err);
    void read_custom_vars();
    bool add_channel_in_free_slot(const std::string &name, const ChannelSecret &key, bool hashtag);
    std::string app_name() const { return "mcdeck"; }
    void pump_bulk();
    void end_bulk(const std::string &why);
    void send_other_params(bool manual, bool with_multi_acks);
    void read_device_info();
    void read_repeat_ranges();
    void tick_discover();
    void tick_schedule();
    void send_advert_impl(bool flood, bool scheduled);

    ITransport &transport_;
    Model &model_;
    FrameParser parser_;
    Link link_ = Link::Searching;
    OpenStatus last_open_ = OpenStatus::NotFound;
    std::string lost_reason_;
    uint64_t now_ = 0;
    uint64_t next_open_ = 0;
    int attempts_ = 0;

    std::deque<Request> urgent_, normal_;
    std::unique_ptr<Request> inflight_;
    uint64_t deadline_ = 0;
    int consecutive_timeouts_ = 0;

    bool building_contacts_ = false;
    std::vector<Contact> building_;
    bool want_sync_ = false;
    int sync_burst_ = 0;
    uint64_t last_sync_ = 0, last_battery_ = 0;
    bool key_lookup_queued_ = false;

    struct ChannelSend {
        uint32_t seq = 0;
        uint64_t deadline = 0;
    };
    std::vector<Outgoing> outgoing_;
    std::vector<ChannelSend> chan_pending_;
    uint8_t last_code_ = 0;               // the code of the frame that completed the request being finished
    std::string last_head_;               // the first bytes of the latest answer frame (hex), for the log
    ResetPhase reset_phase_ = ResetPhase::None;
    PubKey reset_prev_key_{};
    uint64_t notice_hold_until_ = 0;      // good news does not replace the notice about a factory reset before this time
    uint64_t reset_until_ms_ = 0;         // Waiting: give up judging after this; Done / KeptKeys: stop showing after this
    std::function<void(const std::string &)> log_;
    Options options_;
    std::function<DeckClock()> deck_state_;
    std::function<void()> deck_refresh_;
    ClockStage clock_stage_ = ClockStage::Idle;
    uint64_t clock_wait_until_ = 0;
    bool gps_ = false, gps_known_ = false;
    Notice notice_;
    size_t frames_rx_ = 0, bad_frames_ = 0;

    // phase 2
    BulkProgress bulk_;
    std::deque<PubKey> bulk_queue_;
    std::vector<std::string> bulk_removed_;
    bool bulk_inflight_ = false;
    DiscoverState discover_;
    std::array<uint8_t, 4> discover_tag_bytes_{};
    uint32_t tag_counter_ = 0;
    int stats_pending_ = 0;
    AdvertSchedule sched_seen_;
    uint64_t next_advert_ms_ = 0;
};

} // namespace meshzero
