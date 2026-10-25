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
#include "logdata.hpp"
#include "model.hpp"
#include "packet_log.hpp"
#include "passwords.hpp"
#include "relay.hpp"
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
    /* The node position (SET_ADVERT_LATLON, decimal degrees; 0, 0 clears it), then SELF_INFO is read back so the screen shows what the
     * board holds. Refused (notice, nothing sent) outside -90..90 / -180..180. The board shares it in its next advert; none is sent here. */
    bool set_position(double lat, double lon);
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

    // ---- phase 3: the radio log (PUSH_LOG_RX_DATA, every packet the board hears; no command enables it)
    /* "Heard back by N repeaters" (Message::heard_back): every channel text we send is registered with an EchoTracker (logdata.hpp), every
     * packet of the radio log is fed to it. N = the distinct NON-empty paths our payload came back on (a packet with an empty path was heard
     * straight from its sender, which says nothing about repeaters). N moves while the window is open (kEchoWindowS after the send), then
     * it is final and saved with the message. Only an approximation: see Message::heard_back and EchoTracker. Direct messages are not
     * tracked (they have acks). Nothing is registered when the channel key was not read from the board in this session (no hash). */
    static constexpr double kEchoWindowS = 60.0;
    /* The capture of the packet log (D8): off at every start, switched by the user. */
    PacketLog &packet_log() { return packet_log_; }
    const PacketLog &packet_log() const { return packet_log_; }
    /* Channel sends still inside their window (tests, diagnostics). */
    size_t echoes_open() const { return echoes_.size(); }

    // ---- the CLI relay (report 033, option (b), phase P3)
    /* A meshcore-cli session shares the node through Mesh Hop's own link (USB, BLE or IP alike: the relay sits above the
     * transport). start_relay() opens a listener on 127.0.0.1 (RelayServer: ephemeral port, one client, the framing of the
     * companion's TCP interface) for `meshcli -t 127.0.0.1 -p <port>`, and pauses Mesh Hop's own commands: the companion
     * protocol has no request ids, so two programs must not have commands in flight at once.
     *   Draining     Mesh Hop's command in flight gets its answer (or its timeout); the console's frames wait.
     *   Passthrough  the console's frames go to the node as they are; every frame of the node goes to the console. Mesh Hop
     *                still reads them all but applies only the pushes (adverts, radio log, acks, deleted contacts...) and
     *                the messages (Chats, unread, notices go on; a CLI reply, text type 1, never becomes a message); the
     *                answers to the console's own commands (contacts, channels, self info, ...) are not applied, and a
     *                MESSAGES_WAITING push does not make Mesh Hop fetch: the console fetches and Mesh Hop reads along.
     *                The second APP_START / DEVICE_QUERY the console sends go to the node and are answered by it: the
     *                firmware keeps no per-client state for them (one link, no client identity), so nothing is faked.
     *   Settling     the session ended (stop_relay(), the console closed its connection, it never connected): the node's last
     *                answers to the console are let through (kRelaySettleMs of quiet, kRelaySettleMaxMs at most) so none
     *                of them is taken for the answer to a Mesh Hop command. Then Mesh Hop resumes: its held commands go
     *                out, its retry timers start again where they stopped, and when the console sent anything it re-reads
     *                the node (device info, self info, contacts, channels, variables, auto-add, waiting messages) and
     *                resets the flood scope if the console set one.
     * A lost link ends the session at once (the reconnect re-reads everything). Mesh Hop's own sends wait while a session runs:
     * leaving the CLI page must call stop_relay() (D6). */
    enum class RelayState { Off, Draining, Passthrough, Settling };
    static constexpr uint32_t kRelayAcceptTimeoutMs = 60000;   // the console must connect within this (a first Python start is slow)
    static constexpr uint32_t kRelaySettleMs = 600;
    static constexpr uint32_t kRelaySettleMaxMs = 4000;
    /* Opens the listener and pauses Mesh Hop's commands. Returns the port on 127.0.0.1, or 0 when refused (not connected, a
     * session still runs or settles, socket error: notice() says why). The console may connect at once: its frames wait until
     * relay_ready(); meshcore_py gives its APP_START 2 s, so start the console when relay_ready() is true. */
    int start_relay();
    /* Ends the session (closes the console's connection and the listener, then Settling). Nothing when none runs. */
    void stop_relay();
    /* A session runs (Draining, Passthrough or Settling): Mesh Hop's own commands are held. */
    bool relay_active() const { return relay_state_ != RelayState::Off; }
    /* The console's frames go to the node. */
    bool relay_ready() const { return relay_state_ == RelayState::Passthrough; }
    RelayState relay_state() const { return relay_state_; }
    bool relay_client_connected() const { return relay_.connected(); }
    int relay_port() const { return relay_.port(); }
    /* Why the last session ended ("" while one runs or before any) and a counter that grows when one ends: the page sees a
     * session that ended by itself (the console quit, the link was lost). */
    const std::string &relay_end_reason() const { return relay_end_reason_; }
    uint32_t relay_sessions_ended() const { return relay_ended_id_; }
    size_t relay_frames_to_node() const { return relay_to_node_; }
    size_t relay_frames_to_console() const { return relay_to_console_; }

    // ---- remote administration of a repeater or room (report 036, slice S1: log in, log out)
    /* A remote request is a normal queued command (SEND_LOGIN 26) answered at once by the companion with SENT (a flood flag and
     * est_timeout), followed by a REMOTE WAIT: a timer of this Client for the push that carries the remote node's answer
     * (LOGIN_SUCCESS 0x85). The wait holds no command in flight, so Chats, direct messages, retries and the sync go on during it.
     *   - One remote request at a time, like the companion (clearPendingReqs() on every login or request: a second one makes the first
     *     one's answer unmatchable). A new request cancels the pending one (its result: Cancelled), and so does cancel_remote().
     *   - Refused while a CLI session runs (relay_active(): Busy); start_relay() and a lost link cancel the pending one (Cancelled,
     *     LinkLost). A push that arrives when nothing is pending, or for another node, is ignored (the late answer of a cancelled or
     *     timed out request).
     *   - The wait: kRemoteWaitFactor x est_timeout, as meshcore_py's est_timeout / 800 s (binary.py), bounded by kRemoteMinWaitMs and
     *     kRemoteMaxWaitMs; kRemoteDefaultWaitMs when SENT carries no estimate. A wrong password gets NO answer from the firmware
     *     (simple_repeater handleLoginReq returns 0), so it ends as Timeout like an out-of-reach node; LOGIN_FAIL (0x86) only comes from
     *     a legacy node (Refused).
     *   - Nothing here sets notice(): the Repeater page shows the result. The password is never logged; it is kept only while a login
     *     with `remember` waits, to be written to the PasswordStore after a success, and wiped then.
     * The companion supports SEND_LOGIN and LOGOUT from companion v1.4.0 (level 3) on; before v1.10.0 (level 8) the success push has no
     * clock, ACL byte or firmware level (has_clock / has_fw_level false, the role from the admin bit). A board without the commands
     * answers "unsupported" (Unsupported).
     * 0.8.0, the role: the ACL byte only counts when the node sends its FIRMWARE_VER_LEVEL (level 1+: repeater and room server
     * v1.9.0+). An older node has no ACL byte in its reply (a repeater: a reserved 0, a room: its count of unsynced posts), so the
     * role comes from byte 1 (repeater: 1 admin, else guest; room: 1 admin, 0 read-write, 2 read-only). A legacy "OK" answer (an
     * 8 byte push from a companion of level 8+) carries no role at all: RemoteRole::None ("Logged in"). */
    enum class RemoteOp { None, Login, Logout, Status, Neighbours, Owner };
    /* The role in the remote node's access list (ACL permission bits 0-1). A repeater gives Guest or Admin; a room also ReadOnly
     * (a blank password when `allow.read.only` is on) and ReadWrite (the room password). */
    enum class RemoteRole { None, Guest, ReadOnly, ReadWrite, Admin };
    enum class RemoteError {
        None,
        /* SENT came but no answer within the wait, or the board never answered SENT (sent = false). The firmware answers NOTHING to a
         * wrong password, to a request from a client it does not list (its session ended: a reboot forgets the guests), to a request
         * type it does not know (an older repeater) and to an anonymous request that arrived by flood or over its limit: all of these
         * look exactly like a node out of reach. */
        Timeout,
        NotFound,        // ERR not found: the node is not a contact on the board; or refused at once: not in the model's contacts (err_code -1)
        TableFull,       // ERR table full: the board could not send it (its send queue is full: "busy")
        /* ERR unsupported: the board's firmware lacks the command (SEND_LOGIN before companion v1.4.0, BINARY_REQ before v1.8.0,
         * ANON_REQ before v1.12.0); or refused at once (err_code -1): the node cannot answer it: not a repeater or room, a room asked
         * for neighbours or the owner, or the level of its login answer is too low (neighbours: kRepeaterLevelNeighbours, owner:
         * kRepeaterLevelOwnerInfo): "firmware too old". */
        Unsupported,
        Failed,          // another ERR code (err_code)
        Refused,         // LOGIN_FAIL from a legacy node
        Busy,            // refused: a CLI session holds the node
        NotConnected,    // refused: no board
        BadPassword,     // refused: longer than kMaxLoginPassword bytes, or a NUL in it
        Cancelled,       // a newer request, cancel_remote(), start_relay(), logout() of the same node
        LinkLost,        // the link to the board went away
        NotLoggedIn,     // refused: status and neighbours need a session (remote_sessions(), guest or admin) with that node
        NoDirectRoute,   // refused: the owner without a session uses the anonymous request, which the node answers only on a direct route
        BadReply,        // the answer with the right tag could not be read (too short, malformed)
    };
    /* NotAsked: no `remember`, or a blank password (never written); Skipped: a password the file cannot hold (spaces around it, a line
     * end); Failed: no PasswordStore set, or an I/O error. */
    enum class PasswordSave { NotAsked, Saved, Skipped, Failed };
    struct RemoteState {
        enum class Phase { Idle, Sending, Waiting };
        Phase phase = Phase::Idle;       // Sending: the command is queued or waits for SENT; Waiting: the timer runs
        RemoteOp op = RemoteOp::None;
        uint32_t req = 0;                // the id start_login() returned
        PubKey key{};
        uint64_t since_ms = 0;           // poll() clock at the request
        uint64_t deadline_ms = 0;        // Waiting: when it ends as Timeout
        bool flood = false;              // SENT said: sent by flood (no known route)
        uint32_t est_timeout_ms = 0;     // as SENT gave it
        // a request (Status, Neighbours, Owner): the tag of SENT that its push 0x8C carries, and what is needed to read the answer
        std::array<uint8_t, 4> tag{};
        bool anonymous = false;          // Owner by the anonymous request (no session)
        bool room = false;               // Status of a room: the room layout
        uint16_t nb_offset = 0;          // Neighbours: the page asked for
        uint8_t nb_prefix_len = 0;
    };
    struct RemoteResult {
        uint32_t id = 0;                 // grows with every result: the UI shows each once
        uint32_t req = 0;                // the request it ends (0: refused before it got an id)
        RemoteOp op = RemoteOp::None;
        PubKey key{};
        bool ok = false;
        RemoteError error = RemoteError::None;
        int err_code = -1;               // the board's ERR code for NotFound / TableFull / Unsupported / Failed
        bool sent = false;               // the board confirmed the send (SENT)
        bool flood = false;
        uint32_t waited_ms = 0;          // from the request to the end
        // a login success
        RemoteRole role = RemoteRole::None;
        uint8_t permissions = 0;         // the ACL byte (or the legacy admin byte when has_acl is false)
        bool has_acl = false;
        bool has_clock = false;
        uint32_t clock = 0;              // the remote node's clock at its answer
        uint32_t deck_time = 0;          // deck_unix() when the answer came: the skew is clock - deck_time
        bool has_fw_level = false;       // a login success; an Owner result: the level from the session (none when anonymous)
        uint8_t fw_level = 0;
        PasswordSave password = PasswordSave::NotAsked;
        // a request's answer (ok, op Status / Neighbours / Owner)
        bool anonymous = false;          // Owner read (0.8.0: or tried and failed) with the anonymous request
        RepeaterStatus status;           // Status (status.room: a room's layout)
        NeighboursPage neighbours;       // Neighbours (names: neighbour_name() in model.hpp)
        OwnerInfo owner;                 // Owner (fw_version only from OWNER_INFO 0x07)
    };
    /* What this node connection knows of its logins (the session table). The REMOTE access list survives our link (a guest until the
     * node reboots, an admin until `setperm`), but this table is cleared when the link to the board drops or changes (another board,
     * a factory reset): the knowledge belongs to one board identity and one connection. A CLI session leaves it as it is (the CLI
     * shares the board's identity, so its logins are the same entries). logout() removes the node at once. A failed or timed out
     * login leaves an entry as it was (a wrong password does not touch the remote list). */
    struct RemoteSession {
        PubKey key{};
        RemoteRole role = RemoteRole::None;
        uint8_t permissions = 0;
        uint32_t since = 0;              // deck_unix() at the login
        bool has_clock = false;
        uint32_t clock = 0;              // the remote clock at the login, and the deck time then
        uint32_t deck_time = 0;
        bool has_fw_level = false;
        uint8_t fw_level = 0;
    };
    static constexpr uint32_t kRemoteDefaultWaitMs = 30000;
    static constexpr uint32_t kRemoteMinWaitMs = 2000;
    static constexpr uint32_t kRemoteMaxWaitMs = 120000;
    static constexpr double kRemoteWaitFactor = 1.25;
    /* A NEIGHBOURS page after the first (offset > 0) waits at least this long: the pages are asked close together and their answers
     * are big, so the repeater's duty cycle may hold them back (meshcore_py fetch_all_neighbours: min_timeout + 25 s for the next
     * pages). Bounded by kRemoteMaxWaitMs like every wait. */
    static constexpr uint32_t kRemoteNeighboursPageMinWaitMs = 25000;
    /* The wait after SENT for an est_timeout (0 = none given). */
    static uint32_t remote_wait_ms(uint32_t est_timeout_ms);
    /* Where a successful login with `remember` writes the password (nullptr: never written). Not owned. */
    void set_password_store(PasswordStore *store) { passwords_ = store; }
    /* Logs in to a repeater or room (its full key). guest: a blank password (password is ignored): the firmware's default guest
     * password is empty, and a node that has this board in its access list gives its role back. remember: after a SUCCESS with a
     * non-blank password, the password goes to the PasswordStore (RemoteResult::password says how it went). Returns the request id,
     * 0 when refused at once (remote_result() then holds the reason: NotConnected, Busy, BadPassword). */
    uint32_t start_login(const PubKey &key, const std::string &password, bool guest, bool remember = false);
    /* "Log out (this deck)": forgets the session of this node here, cancels a pending request to it, and sends LOGOUT (the companion
     * stops its keep-alive; the node still lists us). The board's answer ends as a RemoteResult with op Logout. Returns its id, 0 when
     * refused (NotConnected, Busy). */
    uint32_t logout(const PubKey &key);
    /* Stops waiting for the pending request (Cancelled). Its answer, if it still comes, is ignored. */
    void cancel_remote();
    const RemoteState &remote_state() const { return remote_; }
    bool remote_busy() const { return remote_.phase != RemoteState::Phase::Idle; }
    const RemoteResult &remote_result() const { return remote_result_; }
    const std::vector<RemoteSession> &remote_sessions() const { return sessions_; }
    const RemoteSession *remote_session(const PubKey &key) const;

    // ---- the read-only view of a repeater or room (report 036, slice S2)
    /* Requests on the same remote queue as start_login(): one at a time (a new one, a login or logout of that node, cancel_remote(),
     * start_relay() cancel the pending one; a lost link ends it: Cancelled / LinkLost), refused while a CLI session runs (Busy), the
     * same wait after SENT (remote_wait_ms(), no automatic retry). The command is BINARY_REQ (50) or ANON_REQ (57); the companion's
     * SENT gives a tag, and the answer is the push BINARY_RESPONSE (0x8C) with that tag: a push with another tag, or one that comes
     * when nothing waits (a cancelled or timed out request), is ignored. The answer ends as a RemoteResult of op Status, Neighbours or
     * Owner (ok: its data in status / neighbours / owner; BadReply when it cannot be read). Nothing sets notice().
     * Refused at once (0 returned, remote_result() says why, err_code -1): NotConnected, Busy, NotFound (not one of the model's
     * contacts), Unsupported (a chat node or sensor; a room: status only; a session whose level is too low), NotLoggedIn (status and
     * neighbours need a session in remote_sessions(): guest or admin, or a room's read-only / read-write), NoDirectRoute (below).
     * Returns the request id. */
    uint32_t request_status(const PubKey &key);
    /* One page of the repeater's zero-hop neighbours from `offset`: up to `count` entries (the repeater stops at 14 with 4 byte
     * prefixes), in `order`. The page says the total and whether more remain (NeighboursPage::more: ask again from offset +
     * entries.size()). Repeaters only, from level 1 (v1.9.0). */
    uint32_t request_neighbours(const PubKey &key, uint16_t offset = 0, uint8_t count = 255, NeighbourOrder order = NeighbourOrder::NewestFirst);
    /* The owner info. With a session: OWNER_INFO (0x07, level 2: repeater v1.12.0+), which also gives the firmware version. Without
     * one: the anonymous OWNER request (0x02), sent only when the contact has a direct route (zero hop or a known path; the reply path
     * is that route reversed); a flood route is refused at once (NoDirectRoute: the repeater ignores an anonymous request by flood).
     * The repeater answers at most 4 anonymous requests per 3 minutes from everybody together: past that it is silent (Timeout).
     * Repeaters only. */
    uint32_t request_owner(const PubKey &key);

    // ---- the route of a contact (0.7.2)
    /* 0.8.0: the companion's push PATH_UPDATED (0x81, the contact's key) says it learnt a new route to that contact (a flood
     * exchange brought one back: a login, a request, a message); the contact is read back (GET_CONTACT_BY_KEY), so the route texts
     * and the anonymous owner request (direct only) follow at once. One read per key at a time; none while a CLI session runs (the
     * console reads its own, and the end of the session re-reads the contacts when the console sent anything). */
    size_t path_lookups_pending() const { return path_lookups_.size(); }
    /* "Reset path": RESET_PATH (13, the contact's 32-byte key). The companion forgets its stored route to the contact
     * (out_path_len = unknown) and answers OK, or ERR not found when the key is not one of its contacts. After OK the model's contact
     * is set to flood at once and read back from the board (GET_CONTACT_BY_KEY), so every route text says "flood". Refused (result
     * at once, false): not connected, a CLI session holds the node (Busy), a reset already on its way (Busy). Nothing here sets
     * notice(): the page shows path_reset_result(). The DM retry's own reset (RetrySettings::reset_after) goes the same way. */
    enum class PathResetError { None, NotConnected, Busy, NotFound, Failed, Timeout, LinkLost };
    struct PathResetResult {
        uint32_t id = 0;                 // grows with every result: the UI shows each once
        PubKey key{};
        bool ok = false;
        PathResetError error = PathResetError::None;
        int err_code = -1;               // the board's ERR code (NotFound, Failed)
    };
    bool reset_path(const PubKey &key);
    bool path_reset_busy() const { return path_reset_busy_; }
    const PathResetResult &path_reset_result() const { return path_reset_result_; }

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
        uint32_t remote_req = 0;          // a remote request's command: dropped from the queue when that request is cancelled
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
    void register_echo(uint32_t seq, int channel_idx, const std::string &text);
    void on_log_packet(const LogPacket &pkt);
    void tick_echoes();
    void tick_relay();
    void relay_to_node(const Bytes &payload);
    void end_relay(const std::string &why);
    void finish_relay();
    void reset_flood_scope();
    RemoteResult refuse_remote(RemoteOp op, const PubKey &key, RemoteError e);
    void end_remote(RemoteError e, int err_code = -1);
    void drop_queued_remote(uint32_t req);
    void tick_remote();
    void on_login_reply(const KeyPrefix &prefix, const LoginSuccess *ok);
    /* SENT (or the error) of a pending remote command: the wait starts (Waiting, the tag kept), or the request ends. */
    void on_remote_sent(uint32_t req, Outcome o, const Packet *p, int err);
    /* The checks of request_status / _neighbours / _owner: RemoteError::None when the request may go (rec: the model's contact). */
    RemoteError check_request(RemoteOp op, const PubKey &key, const ContactRec *&rec) const;
    uint32_t start_request(RemoteOp op, const PubKey &key, Bytes payload, const RemoteState &extra, const std::string &what);
    void on_binary_response(const BinaryResponse &b);
    void publish_remote(RemoteResult r);
    void wipe_remote_password();
    /* RESET_PATH for `key`; on OK the model's contact goes to flood and is read back; `then` gets the outcome (may be empty). */
    void queue_reset_path(const PubKey &key, std::function<void(Outcome, int)> then);
    void publish_path_reset(const PubKey &key, bool ok, PathResetError e, int err_code = -1);
    void on_path_updated(const PubKey &key);

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

    // phase 3: the radio log
    struct ChannelEcho {
        EchoTracker::Id id = EchoTracker::kNone;
        uint32_t seq = 0;
        uint32_t epoch = 0;                   // Model::board_epoch() at the send: another board's messages are never touched
        std::vector<Bytes> routes;            // hash size byte + path, non-empty paths only
    };
    EchoTracker echo_{kEchoWindowS};
    std::vector<ChannelEcho> echoes_;
    PacketLog packet_log_;

    // the CLI relay
    RelayServer relay_;
    RelayState relay_state_ = RelayState::Off;
    std::vector<Bytes> relay_hold_;           // the console's frames while Draining
    uint64_t relay_started_ms_ = 0, relay_accept_until_ = 0, relay_settle_until_ = 0, relay_settle_cap_ = 0;
    bool relay_scope_set_ = false;            // the console sent SET_FLOOD_SCOPE in this session
    bool scope_reset_pending_ = false;        // ... and the link was lost before Mesh Hop could reset it
    size_t relay_to_node_ = 0, relay_to_console_ = 0;
    std::string relay_end_reason_;
    uint32_t relay_ended_id_ = 0;

    // remote administration (report 036)
    RemoteState remote_;
    RemoteResult remote_result_;
    uint32_t remote_req_counter_ = 0;
    bool remote_remember_ = false;
    std::string remote_password_;              // only while a login with `remember` waits; wiped at its end
    std::vector<RemoteSession> sessions_;
    PasswordStore *passwords_ = nullptr;

    // the route of a contact (0.7.2)
    bool path_reset_busy_ = false;
    PathResetResult path_reset_result_;
    std::vector<PubKey> path_lookups_;           // 0.8.0: contacts read back after a PATH_UPDATED push (their answer pending)
};

} // namespace meshzero
