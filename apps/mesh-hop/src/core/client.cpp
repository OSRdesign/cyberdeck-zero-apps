/*
 * SPDX-License-Identifier: MIT
 */

#include "client.hpp"

#include "features.hpp"
#include "position.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace meshzero {

namespace {
constexpr const char *kUnconfirmedNote = "no confirmation from the board";

char hexdigit(int v) { return "0123456789abcdef"[v & 15]; }
std::string code_text(uint8_t code)
{
    std::string s = "0x";
    s.push_back(hexdigit(code >> 4));
    s.push_back(hexdigit(code));
    return s;
}

bool is_push(uint8_t code)
{
    return code >= 0x80;
}

template <typename T> const T *pkt_as(const Packet *p)
{
    return p ? std::get_if<T>(p) : nullptr;
}

/* One word for the log: what the board answered to a command. */
std::string answer_text(int outcome, int err)
{
    switch (outcome) {
    case 0: return "ok";                                   // Outcome::Ok
    case 1: return "error " + std::to_string(err);         // Outcome::Error
    case 2: return "no answer";                            // Outcome::Timeout
    default: return "link lost";                           // Outcome::Aborted
    }
}
} // namespace

Client::Client(ITransport &transport, Model &model) : transport_(transport), model_(model) {}

/* ------------------------------------------------------------------ status text */

std::string Client::link_text() const
{
    switch (link_) {
    case Link::Searching: return lost_reason_.empty() ? "No board found" : "Board disconnected";
    case Link::Denied: return "Permission denied";
    case Link::Busy: return "Port in use";
    case Link::Error: return "Cannot open the port";
    case Link::Handshake: return "Connecting...";
    case Link::NotCompanion: return "Not a companion radio";
    case Link::Ready: return "Connected";
    }
    return "";
}

std::string Client::link_hint() const
{
    switch (link_) {
    case Link::Searching:
        return lost_reason_.empty() ? "Plug the MeshCore board into the USB port" : "Plug it back in, it reconnects by itself";
    case Link::Denied: return "Add the user to the dialout group, then log in again";
    case Link::Busy: return "Close the other program that uses the port";
    case Link::NotCompanion: return "The board must run the MeshCore companion radio firmware (USB)";
    case Link::Error: return transport_.last_error();
    default: return "";
    }
}

void Client::set_notice(bool ok, const std::string &text)
{
    if (ok && now_ < notice_hold_until_) return;          // the message about a factory reset stays readable; an error always shows
    notice_.id++;
    notice_.ok = ok;
    notice_.text = text;
    model_.touch();
}

/* ------------------------------------------------------------------ connection */

void Client::try_open()
{
    last_open_ = transport_.open();
    switch (last_open_) {
    case OpenStatus::Ok:
        lost_reason_.clear();
        start_handshake();
        return;
    case OpenStatus::NotFound: link_ = Link::Searching; next_open_ = now_ + 2000; break;
    case OpenStatus::PermissionDenied: link_ = Link::Denied; next_open_ = now_ + 4000; break;
    case OpenStatus::Busy: link_ = Link::Busy; next_open_ = now_ + 3000; break;
    case OpenStatus::Error: link_ = Link::Error; next_open_ = now_ + 3000; break;
    }
    model_.touch();
}

void Client::drop_link(const std::string &why, uint64_t retry_in_ms)
{
    const bool was_ready = link_ == Link::Ready;
    if (relay_active()) {                                  // the console loses the node too: its session ends now (the reconnect re-reads all)
        if (relay_scope_set_) scope_reset_pending_ = true;
        relay_.close();
        relay_hold_.clear();
        relay_state_ = RelayState::Off;
        relay_end_reason_ = "the link to the node was lost";
        ++relay_ended_id_;
        log("console relay: session ended, the link to the node was lost");
    }
    transport_.close();
    parser_.reset();
    // abort what is waiting; callbacks may set states, so detach first
    std::vector<Request> aborted;
    if (inflight_) { aborted.push_back(std::move(*inflight_)); inflight_.reset(); }
    for (auto &r : urgent_) aborted.push_back(std::move(r));
    for (auto &r : normal_) aborted.push_back(std::move(r));
    urgent_.clear();
    normal_.clear();
    for (auto &r : aborted)
        if (r.done) r.done(Outcome::Aborted, nullptr, -1);
    end_remote(RemoteError::LinkLost);                     // a remote wait ends with the link (a pending command ended above)
    sessions_.clear();                                     // what we knew of our logins belongs to this connection (report 036)
    for (const Outgoing &o : outgoing_) model_.set_state(o.seq, MsgState::NoAck, "board disconnected");
    outgoing_.clear();
    chan_pending_.clear();
    if (bulk_.active && !bulk_inflight_) end_bulk("the board disconnected");     // an in-flight one ends through its own Aborted callback
    discover_.active = false;
    stats_pending_ = 0;
    building_contacts_ = false;
    building_.clear();
    want_sync_ = false;
    key_lookup_queued_ = false;
    clock_stage_ = ClockStage::Idle;
    consecutive_timeouts_ = 0;
    model_.clear_device();
    link_ = Link::Searching;
    lost_reason_ = was_ready || !why.empty() ? (why.empty() ? "lost" : why) : "";
    next_open_ = now_ + retry_in_ms;
    model_.touch();
}

void Client::start_handshake()
{
    link_ = Link::Handshake;
    parser_.reset();
    attempts_ = 0;
    consecutive_timeouts_ = 0;
    model_.touch();
    send_query();
}

/* The board may still be booting after the port opened (opening it can reset it): the query is repeated. */
void Client::send_query()
{
    Request r;
    r.payload = build_device_query();
    r.accept = {resp::kDeviceInfo};
    r.timeout_ms = 2000;
    r.done = [this](Outcome o, const Packet *p, int) {
        if (link_ != Link::Handshake) return;
        if (o == Outcome::Aborted) return;
        if (o == Outcome::Timeout) {
            if (++attempts_ < 5) { send_query(); return; }
            drop_link("", 0);
            link_ = Link::NotCompanion;
            next_open_ = now_ + 8000;
            return;
        }
        // an old firmware may answer "unsupported": the app start below still works
        (void)p;
        attempts_ = 0;
        send_app_start();
    };
    enqueue(std::move(r), true);
}

void Client::send_app_start()
{
    Request r;
    r.payload = build_app_start(app_name());
    r.accept = {resp::kSelfInfo};
    r.timeout_ms = 3000;
    r.done = [this](Outcome o, const Packet *, int) {
        if (link_ != Link::Handshake) return;
        if (o == Outcome::Aborted) return;
        if (o == Outcome::Ok) {
            link_ = Link::Ready;
            model_.touch();
            after_ready();
            return;
        }
        if (o == Outcome::Timeout && ++attempts_ < 3) { send_app_start(); return; }
        drop_link("", 0);
        link_ = Link::NotCompanion;
        next_open_ = now_ + 8000;
    };
    enqueue(std::move(r), true);
}

void Client::after_ready()
{
    last_battery_ = last_sync_ = now_;
    if (const auto &d = model_.device()) {
        log("board: " + d->model + " " + d->version + ", firmware level " + std::to_string(d->fw_ver) + ", path hash mode " +
            (d->path_hash_mode >= 0 ? std::to_string(d->path_hash_mode) : std::string("not reported")) + ", repeat " +
            (d->has_repeat ? (d->repeat ? "on" : "off") : "not reported") + (model_.self() ? std::string(", manual add ") + (model_.self()->manual_add_contacts ? "on" : "off") : std::string()));
    }
    if (reset_phase_ == ResetPhase::Waiting) {                 // the board is back after a factory reset: is it another board now?
        const bool new_keys = model_.self() && model_.self()->key != reset_prev_key_;
        reset_phase_ = new_keys ? ResetPhase::Done : ResetPhase::KeptKeys;
        reset_until_ms_ = now_ + 120000;
        log(std::string("factory reset: the board is back and reports ") +
            (new_keys ? "a NEW identity (public key " + to_hex(model_.self()->key).substr(0, 12) + "... instead of " + to_hex(reset_prev_key_).substr(0, 12) + "...): the reset happened"
                      : "the SAME identity (public key " + (model_.self() ? to_hex(model_.self()->key).substr(0, 12) : std::string("?")) + "...): nothing was erased"));
        set_notice(new_keys, new_keys ? "Board reset: new keys, no contacts or channels"
                                      : "Restarted with the same keys: not reset");
        notice_hold_until_ = now_ + 8000;                       // the clock notices of the new connection must not wipe it at once
    }
    if (scope_reset_pending_) reset_flood_scope();          // a console session set a flood scope, then the link went before it was undone
    start_clock_sync();
    Request b;
    b.payload = build_battery();
    b.accept = {resp::kBattery};
    b.done = [](Outcome, const Packet *, int) {};
    enqueue(std::move(b));
    refresh_contacts();
    const int max_channels = model_.device() && model_.device()->max_channels ? model_.device()->max_channels : 8;
    queue_channels(std::min(max_channels, 40));
    refresh_autoadd();                                      // phase 2: what the board adds by itself
    read_repeat_ranges();                                   // phase 2: where the board may repeat
    want_sync_ = true;
}

void Client::set_deck_clock(std::function<DeckClock()> state, std::function<void()> refresh)
{
    deck_state_ = std::move(state);
    deck_refresh_ = std::move(refresh);
}

/* ---- board clock policy (clock_policy.hpp). The only place that writes the board clock is write_time(). */

bool Client::sync_clock()
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (clock_stage_ == ClockStage::Busy || clock_stage_ == ClockStage::WaitDeck) { set_notice(true, "Clock sync already running"); return true; }
    start_clock_sync();
    return true;
}

void Client::start_clock_sync()
{
    clock_stage_ = ClockStage::Busy;
    if (!options_.set_device_time) {                       // policy off: read only
        read_custom_vars();                                // still needed for the GPS switch and the firmware check
        use_board_clock(ClockSource::BoardKept, "");
        return;
    }
    // GPS: the companion protocol has no GPS field; the custom variables ("key:value,...") of the board may list one called "gps".
    Request r;
    r.payload = build_get_custom_vars();
    r.accept = {resp::kCustomVars};
    r.done = [this](Outcome o, const Packet *p, int err) {
        if (o == Outcome::Aborted) return;
        apply_custom_vars(o, p, err);
        clock_stage_ = ClockStage::WaitDeck;
        clock_wait_until_ = now_ + 6000;
        if (deck_refresh_) deck_refresh_();
    };
    enqueue(std::move(r));
}

/* The custom variables of the board: the clock policy needs "gps", the Settings screen shows the GPS switch and the firmware check
 * (D10) learns that a board without the command is too old for it. */
void Client::apply_custom_vars(Outcome o, const Packet *p, int err)
{
    gps_known_ = o == Outcome::Ok;
    gps_ = false;
    BoardCaps caps = model_.caps();
    caps.gps_listed = false;
    caps.gps_on = false;
    if (o == Outcome::Ok) caps.custom_vars = Cap::Yes;
    else if (o == Outcome::Error && err == err::kUnsupported) caps.custom_vars = Cap::No;
    if (const auto *cv = pkt_as<CustomVars>(p))
        if (const std::string *v = cv->find("gps")) {
            gps_ = gps_value_enabled(*v);
            caps.gps_listed = true;
            caps.gps_on = gps_;
        }
    model_.set_caps(caps);
    ClockInfo ci = model_.clock();
    ci.gps_known = gps_known_;
    ci.gps = gps_;
    model_.set_clock(ci);
}

void Client::read_custom_vars()
{
    Request r;
    r.payload = build_get_custom_vars();
    r.accept = {resp::kCustomVars};
    r.done = [this](Outcome o, const Packet *p, int err) {
        if (o != Outcome::Aborted) apply_custom_vars(o, p, err);
    };
    enqueue(std::move(r));
}

void Client::tick_clock()
{
    if (clock_stage_ != ClockStage::WaitDeck) return;
    DeckClock st = deck_state_ ? deck_state_() : DeckClock::Unsynced;
    if (st == DeckClock::Unknown && now_ < clock_wait_until_) return;      // the check is still running
    const ClockAction action = decide_clock(st, gps_);
    clock_stage_ = ClockStage::Busy;
    switch (action) {
    case ClockAction::SetFromDeck: write_time(deck_unix(), ClockSource::DeckNtp, 0); break;
    case ClockAction::UseBoardClock: use_board_clock(gps_ ? ClockSource::BoardGps : ClockSource::BoardKept, ""); break;
    case ClockAction::PromptUser:
        clock_stage_ = ClockStage::Prompt;                // the UI opens the typed editor
        model_.touch();
        break;
    }
}

void Client::clock_set_user(uint32_t ts)
{
    if (clock_stage_ != ClockStage::Prompt) return;
    clock_stage_ = ClockStage::Busy;
    write_time(ts, ClockSource::SetByUser, static_cast<int64_t>(ts) - static_cast<int64_t>(deck_unix()));
}

void Client::clock_skip()
{
    if (clock_stage_ != ClockStage::Prompt) return;
    clock_stage_ = ClockStage::Busy;
    use_board_clock(ClockSource::NotSet, "Board clock left unchanged");
}

void Client::write_time(uint32_t ts, ClockSource source, int64_t offset_after)
{
    Request t;
    t.payload = build_set_time(ts);
    t.accept = {resp::kOk};
    t.done = [this, ts, source, offset_after](Outcome o, const Packet *, int err) {
        if (o == Outcome::Aborted) return;
        if (o == Outcome::Ok) {
            set_time_offset(offset_after);
            ClockInfo ci = model_.clock();
            ci.source = source;
            ci.board_time = ts;
            model_.set_clock(ci);
            clock_stage_ = ClockStage::Idle;
            set_notice(true, std::string("Board clock set: ") + format_local_datetime(ts));
            return;
        }
        if (o == Outcome::Error) {
            // the firmware only moves its clock forward: a time in the past is refused (ERR_CODE_ILLEGAL_ARG)
            use_board_clock(ClockSource::BoardKept, err == err::kIllegalArg
                                                        ? "The board refused that time: its own clock is ahead (it only moves forward). Using the board clock"
                                                        : "The board refused the clock: " + error_text(err) + ". Using the board clock");
            return;
        }
        clock_stage_ = ClockStage::Idle;
        set_notice(false, "The board did not answer the clock setting");
    };
    enqueue(std::move(t), true);
}

void Client::use_board_clock(ClockSource source, const std::string &note)
{
    Request r;
    r.payload = build_get_time();
    r.accept = {resp::kCurrentTime};
    r.done = [this, source, note](Outcome o, const Packet *p, int) {
        if (o == Outcome::Aborted) return;
        clock_stage_ = ClockStage::Idle;
        const auto *ct = pkt_as<CurrentTime>(p);
        if (o != Outcome::Ok || !ct) {
            set_notice(false, "Could not read the board clock");
            return;
        }
        ClockInfo ci = model_.clock();
        ci.board_time = ct->time;
        if (ct->time < kMinPlausibleTime) {      // an unset board clock is not a time base: the deck clock stays
            set_time_offset(0);
            ci.source = ClockSource::NotSet;
            model_.set_clock(ci);
            set_notice(false, gps_ ? "GPS is on but the board clock looks unset (no fix yet?)" : "The board clock is not set");
            return;
        }
        set_time_offset(static_cast<int64_t>(ct->time) - static_cast<int64_t>(deck_unix()));
        ci.source = source;
        model_.set_clock(ci);
        if (!note.empty()) set_notice(note.find("refused") == std::string::npos, note);
        else set_notice(true, std::string("Using the board clock: ") + format_local_datetime(ct->time));
    };
    enqueue(std::move(r), true);
}

void Client::queue_channels(int count)
{
    for (int i = 0; i < count; ++i) {
        Request r;
        r.payload = build_get_channel(static_cast<uint8_t>(i));
        r.accept = {resp::kChannelInfo};
        const bool last = i == count - 1;
        r.done = [this, i, last](Outcome o, const Packet *, int err) {
            if (i == 0 && o == Outcome::Error && err == err::kUnsupported) {       // a firmware without channels
                BoardCaps caps = model_.caps();
                caps.channels = Cap::No;
                model_.set_caps(caps);
            }
            if (last && o == Outcome::Ok) model_.reconcile_added();   // all slots read: forget the "added by this app" marks the board does not back
        };
        enqueue(std::move(r));
    }
}

bool Client::refresh_contacts()
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    Request r;
    r.payload = build_get_contacts();
    r.accept = {resp::kContactEnd};
    r.progress = {resp::kContactStart, resp::kContact};
    r.timeout_ms = 8000;
    r.done = [this](Outcome o, const Packet *, int) {
        if (o == Outcome::Timeout) { building_contacts_ = false; building_.clear(); }
    };
    enqueue(std::move(r));
    return true;
}

/* ------------------------------------------------------------------ queue */

void Client::enqueue(Request r, bool urgent)
{
    (urgent ? urgent_ : normal_).push_back(std::move(r));
    pump_queue();
}

void Client::pump_queue()
{
    if (inflight_ || !transport_.is_open() || relay_active()) return;      // a console session holds Mesh Hop's commands
    std::deque<Request> &q = !urgent_.empty() ? urgent_ : normal_;
    if (q.empty()) return;
    inflight_ = std::make_unique<Request>(std::move(q.front()));
    q.pop_front();
    const Bytes frame = encode_frame(inflight_->payload);
    if (frame.empty() || !transport_.write(frame.data(), frame.size())) {
        finish(Outcome::Error, nullptr, -1);
        return;
    }
    deadline_ = now_ + inflight_->timeout_ms;
    if (!inflight_->payload.empty() && inflight_->payload[0] == cmd::kSendLogin && inflight_->payload.size() > 33)
        std::fill(inflight_->payload.begin() + 33, inflight_->payload.end(), 0);      // the password is on its way: no copy stays here
    if (inflight_->no_reply) finish(Outcome::Ok, nullptr, 0);       // the board answers nothing (reboot)
}

void Client::finish(Outcome o, const Packet *p, int err)
{
    if (!inflight_) return;
    std::unique_ptr<Request> r = std::move(inflight_);
    if (o == Outcome::Timeout) ++consecutive_timeouts_;
    else if (o == Outcome::Ok || o == Outcome::Error) consecutive_timeouts_ = 0;
    if (r->done) r->done(o, p, err);
    if (o == Outcome::Timeout && consecutive_timeouts_ >= 3 && link_ == Link::Ready) {
        drop_link("the radio stopped answering", 1000);
        return;
    }
    pump_queue();
}

/* ------------------------------------------------------------------ frames */

void Client::on_frame(const Bytes &frame)
{
    ++frames_rx_;
    consecutive_timeouts_ = 0;
    const uint8_t code = packet_code(frame);
    // ---- the CLI relay: the console gets every frame of the node as it is (pushes only while Mesh Hop's last command is answered)
    if (relay_state_ == RelayState::Passthrough || (relay_state_ == RelayState::Draining && is_push(code))) {
        if (relay_.connected()) {
            relay_.send(frame);
            ++relay_to_console_;
            if (options_.log_frames) log("relay rx " + code_text(code) + " len=" + std::to_string(frame.size()) + " -> console");
        }
    }
    const auto parsed = parse_packet(frame);
    if (!parsed) { ++bad_frames_; return; }
    const Packet &p = *parsed;
    if (!is_push(code)) last_head_ = to_hex(frame.data(), std::min<size_t>(frame.size(), 16));
    // While the console owns the node, an answer (not a push) belongs to one of ITS commands: Mesh Hop applies only the messages it
    // pulled (Chats keep working), never its contacts, channels, self info... (an incremental contact list would wipe Mesh Hop's).
    const bool console_owns = relay_state_ == RelayState::Passthrough || relay_state_ == RelayState::Settling;
    if (console_owns && !is_push(code)) {
        if (relay_state_ == RelayState::Settling) relay_settle_until_ = now_ + kRelaySettleMs;
        if (!std::holds_alternative<IncomingMessage>(p)) return;
    }

    // ---- effects on the model, whatever command is in flight
    if (const auto *m = std::get_if<IncomingMessage>(&p)) {
        // A repeater's or room server's answer to a remote CLI command (text type CLI_DATA, report 033 section 2.3) is console output for
        // the program that sent the command, never a chat message: no Chats entry, no unread count, no notice. Type 2 (signed, a room
        // server's posts) stays a message.
        if (!m->channel && m->txt_type == txttype::kCliData) {
            if (options_.log_frames) log("rx CLI reply (text type 1) from " + to_hex(m->prefix) + ", " + std::to_string(m->text.size()) + " bytes: kept out of Chats");
        } else {
            model_.add_incoming(*m, now_unix());
        }
    } else if (const auto *s = std::get_if<SelfInfo>(&p)) {
        model_.set_self(*s);
    } else if (const auto *d = std::get_if<DeviceInfo>(&p)) {
        model_.set_device(*d);
    } else if (const auto *b = std::get_if<Battery>(&p)) {
        model_.set_battery(*b);
    } else if (const auto *c = std::get_if<ChannelInfo>(&p)) {
        ChannelRec rec;
        rec.idx = c->idx;
        rec.name = c->name;
        rec.empty = c->empty();
        rec.key_ok = !rec.empty && c->secret == hashtag_secret(c->name);
        rec.has_secret = !rec.empty;
        rec.secret = c->secret;
        if (model_.caps().channels != Cap::Yes) {
            BoardCaps caps = model_.caps();
            caps.channels = Cap::Yes;
            model_.set_caps(caps);
        }
        model_.set_channel(rec);
    } else if (std::get_if<ContactStart>(&p)) {
        building_contacts_ = true;
        building_.clear();
    } else if (const auto *ct = std::get_if<Contact>(&p)) {
        if (building_contacts_) building_.push_back(*ct);
        else model_.upsert_contact(*ct);
    } else if (std::get_if<ContactEnd>(&p)) {
        if (building_contacts_) model_.replace_contacts(building_);
        building_contacts_ = false;
        building_.clear();
    } else if (const auto *adv = std::get_if<Advertisement>(&p)) {
        model_.heard(adv->key, now_unix());
        if (const ContactRec *kc = model_.find_contact(to_hex(adv->key))) {      // a known node advertised: it is in the Nearby list too
            NearbyObs o;
            o.key_hex = to_hex(adv->key);
            o.type = kc->c.type;
            o.name = kc->c.name;
            model_.note_nearby(o, now_unix());
        }
        if (ready() && !relay_active() && !key_lookup_queued_ && normal_.size() < 8) {    // fetch the new name / position
            key_lookup_queued_ = true;
            Request r;
            r.payload = build_get_contact_by_key(adv->key);
            r.accept = {resp::kContact};
            r.done = [this](Outcome, const Packet *, int) { key_lookup_queued_ = false; };
            enqueue(std::move(r));
        }
    } else if (const auto *na = std::get_if<NewAdvert>(&p)) {
        if (model_.self() && model_.self()->manual_add_contacts && !model_.find_contact(to_hex(na->contact.key))) {
            model_.add_pending(na->contact, now_unix());          // manual add: the board did not keep it, the user decides (Nearby)
        } else {
            model_.upsert_contact(na->contact);
            model_.heard(na->contact.key, now_unix());
        }
    } else if (const auto *ld = std::get_if<LogData>(&p)) {
        // every packet the board hears is in the radio log: an ADVERT in it gives the key, name, position, SNR and hop count of a node
        if (const auto h = parse_heard_advert(*ld)) {
            if (!model_.self() || h->key != model_.self()->key) model_.note_heard_advert(*h, now_unix());
        }
        LogPacket lp;
        if (parse_log_rx(frame, lp)) on_log_packet(lp);
    } else if (const auto *st = std::get_if<StatsReply>(&p)) {
        model_.set_stats(*st, now_unix());
    } else if (const auto *cd = std::get_if<ControlData>(&p)) {
        if (cd->discover && discover_.tag != 0 && std::equal(cd->tag.begin(), cd->tag.end(), discover_tag_bytes_.begin())) {
            NearbyObs o;
            o.key_hex = to_hex(cd->pubkey.data(), cd->pubkey.size());
            o.type = cd->node_type;
            o.has_snr = true;
            o.snr = cd->snr;
            o.has_rssi = true;
            o.rssi = cd->rssi;
            o.discovered = true;
            const bool fresh = !model_.find_nearby(o.key_hex) || !model_.find_nearby(o.key_hex)->discovered;
            model_.note_nearby(o, now_unix());
            if (fresh) ++discover_.found;
            if (cd->pubkey.size() == 32) {
                PubKey k{};
                std::copy_n(cd->pubkey.begin(), 32, k.begin());
                model_.heard(k, now_unix());
            }
        }
    } else if (const auto *del = std::get_if<ContactDeleted>(&p)) {
        model_.remove_contact(del->key);
    } else if (std::get_if<ContactsFull>(&p)) {
        set_notice(false, "The radio's contact list is full");
    } else if (std::get_if<MessagesWaiting>(&p)) {
        if (!console_owns) {                                   // during a console session the console fetches; Mesh Hop reads along
            want_sync_ = true;
            sync_burst_ = 0;
        }
    } else if (const auto *ack = std::get_if<Ack>(&p)) {
        handle_ack(*ack);
    } else if (const auto *ls = std::get_if<LoginSuccess>(&p)) {
        on_login_reply(ls->prefix, ls);                        // report 036: the remote node's verdict on a pending login
    } else if (const auto *lf = std::get_if<LoginFail>(&p)) {
        on_login_reply(lf->prefix, nullptr);
    } else if (const auto *br = std::get_if<BinaryResponse>(&p)) {
        on_binary_response(*br);                               // report 036 S2: the answer to a pending request, matched by its tag
    } else if (const auto *pu = std::get_if<PathUpdate>(&p)) {
        on_path_updated(pu->key);                              // 0.8.0: a new route to a contact: read it back
    }

    // ---- diagnostics: what the board sends while a channel message waits for its answer
    if (inflight_ && (options_.log_frames || (!inflight_->payload.empty() && (inflight_->payload[0] == cmd::kSendChannelTxt || inflight_->payload[0] == cmd::kFactoryReset))))
        log("rx " + code_text(code) + " len=" + std::to_string(frame.size()) + " while waiting for command " +
            std::to_string(inflight_->payload.empty() ? 0 : inflight_->payload[0]) +
            (!inflight_->payload.empty() && inflight_->payload[0] == cmd::kFactoryReset ? ", first bytes " + to_hex(frame.data(), std::min<size_t>(frame.size(), 16)) : std::string()));

    // ---- match against the command in flight (pushes never answer a command)
    if (is_push(code) || !inflight_) return;
    last_code_ = code;
    if (code == resp::kError) {
        finish(Outcome::Error, &p, std::get<Error>(p).code);
        return;
    }
    const Request &r = *inflight_;
    if (r.accept_any || std::find(r.accept.begin(), r.accept.end(), code) != r.accept.end()) {
        finish(Outcome::Ok, &p, 0);
    } else if (std::find(r.progress.begin(), r.progress.end(), code) != r.progress.end()) {
        deadline_ = now_ + r.timeout_ms;
    }
}

/* ------------------------------------------------------------------ poll */

void Client::poll(uint64_t now_ms)
{
    now_ = now_ms;
    if (reset_phase_ != ResetPhase::None && now_ > reset_until_ms_) reset_phase_ = ResetPhase::None;
    tick_echoes();                                     // the heard-back windows close even while the board is away
    if (!transport_.is_open()) {
        if (now_ >= next_open_) try_open();
        return;
    }
    uint8_t buf[512];
    for (int i = 0; i < 8; ++i) {
        const long n = transport_.read(buf, sizeof(buf));
        if (n < 0) {
            const std::string why = transport_.last_error();
            drop_link(why.empty() ? "lost" : why, 1000);
            return;
        }
        if (n == 0) break;
        std::vector<Bytes> frames;
        parser_.feed(buf, static_cast<size_t>(n), frames);
        for (const Bytes &f : frames) {
            on_frame(f);
            if (!transport_.is_open()) return;
        }
    }
    if (inflight_ && now_ > deadline_) finish(Outcome::Timeout, nullptr, 0);
    if (!transport_.is_open()) return;
    tick_remote();                                     // a remote wait is a timer, not a command in flight
    if (relay_active()) tick_relay();
    pump_queue();
    if (link_ == Link::NotCompanion) return;
    if (ready() && !relay_active()) {                  // the timers that send stop while a console session runs
        tick_outgoing();
        tick_clock();
        tick_discover();
        tick_schedule();
        maybe_sync_and_poll();
    }
}

void Client::maybe_sync_and_poll()
{
    if (inflight_ || !urgent_.empty() || !normal_.empty()) return;
    if (now_ - last_sync_ > 30000) want_sync_ = true, sync_burst_ = 0;
    if (want_sync_ && sync_burst_ < 100) {
        ++sync_burst_;
        last_sync_ = now_;
        Request r;
        r.payload = build_sync_next();
        r.accept = {resp::kContactMsg, resp::kContactMsgV3, resp::kChannelMsg, resp::kChannelMsgV3, resp::kChannelData,
                    resp::kNoMoreMsgs};
        r.done = [this](Outcome o, const Packet *p, int) {
            if (o == Outcome::Ok && pkt_as<NoMoreMsgs>(p)) want_sync_ = false;
            if (o != Outcome::Ok) want_sync_ = false;
        };
        enqueue(std::move(r));
        return;
    }
    if (now_ - last_battery_ > 60000) {       // also the liveness check of an otherwise quiet link
        last_battery_ = now_;
        Request b;
        b.payload = build_battery();
        b.accept = {resp::kBattery};
        b.done = [](Outcome, const Packet *, int) {};
        enqueue(std::move(b));
    }
}

/* ------------------------------------------------------------------ the CLI relay (report 033, P3) */

int Client::start_relay()
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return 0; }
    if (relay_active()) { set_notice(false, "A console session is already running"); return 0; }
    const int port = relay_.open();
    if (port == 0) {
        log("console relay: " + relay_.error());
        set_notice(false, "The console could not start: " + relay_.error());
        return 0;
    }
    end_remote(RemoteError::Cancelled);                       // the console's requests would clear the companion's pending one (report 036)
    relay_state_ = inflight_ ? RelayState::Draining : RelayState::Passthrough;
    relay_hold_.clear();
    relay_scope_set_ = false;
    relay_to_node_ = relay_to_console_ = 0;
    relay_end_reason_.clear();
    relay_started_ms_ = now_;
    relay_accept_until_ = now_ + kRelayAcceptTimeoutMs;
    log("console relay: listening on 127.0.0.1:" + std::to_string(port) + ", Mesh Hop's own commands wait" +
        (inflight_ ? " (command " + std::to_string(inflight_->payload.empty() ? 0 : inflight_->payload[0]) + " still in flight)" : std::string()));
    model_.touch();
    return port;
}

void Client::stop_relay()
{
    if (relay_state_ == RelayState::Draining || relay_state_ == RelayState::Passthrough) end_relay("closed by Mesh Hop");
}

/* One frame of the console to the node, framed as the Client frames its own (every transport takes '<' + length + payload). The length is not
 * limited to Mesh Hop's own kMaxTxFrame: the console may know newer commands, the firmware decides. */
void Client::relay_to_node(const Bytes &payload)
{
    if (payload.empty()) return;
    if (payload[0] == cmd::kSetFloodScope) relay_scope_set_ = true;
    Bytes f = {kFrameToDevice, static_cast<uint8_t>(payload.size() & 0xFF), static_cast<uint8_t>(payload.size() >> 8)};
    f.insert(f.end(), payload.begin(), payload.end());
    const bool ok = transport_.write(f.data(), f.size());
    ++relay_to_node_;
    if (options_.log_frames || !ok)
        log("relay tx command " + std::to_string(payload[0]) + " len=" + std::to_string(payload.size()) + (ok ? " -> node" : ": the link refused it"));
}

void Client::tick_relay()
{
    std::vector<Bytes> frames;
    const RelayServer::Io io = relay_.poll(frames);           // sends what is queued for the console, reads what it sent
    if (relay_state_ == RelayState::Settling) {
        if (now_ >= relay_settle_until_ || now_ >= relay_settle_cap_) finish_relay();
        return;
    }
    if (io == RelayServer::Io::Gone) {                         // what it sent with its goodbye is dropped: nobody would read the answers
        end_relay("the console closed its connection");
        return;
    }
    if (io == RelayServer::Io::Idle && now_ >= relay_accept_until_) {
        end_relay("the console did not connect");
        return;
    }
    if (relay_state_ == RelayState::Draining) {
        relay_hold_.insert(relay_hold_.end(), frames.begin(), frames.end());
        if (inflight_) return;
        relay_state_ = RelayState::Passthrough;                // Mesh Hop's last command is answered: the node is the console's now
        log("console relay: passthrough" + (relay_hold_.empty() ? std::string() : ", " + std::to_string(relay_hold_.size()) + " frames of the console were waiting"));
        frames.swap(relay_hold_);
        relay_hold_.clear();
        model_.touch();
    }
    for (const Bytes &f : frames) relay_to_node(f);
}

void Client::end_relay(const std::string &why)
{
    relay_.close();
    relay_hold_.clear();
    relay_end_reason_ = why;
    if (relay_to_node_ == 0) {                                 // nothing of the console reached the node: no answer to wait for
        finish_relay();
        return;
    }
    relay_state_ = RelayState::Settling;
    relay_settle_until_ = now_ + kRelaySettleMs;
    relay_settle_cap_ = now_ + kRelaySettleMaxMs;
    model_.touch();
}

void Client::finish_relay()
{
    const uint64_t paused = now_ > relay_started_ms_ ? now_ - relay_started_ms_ : 0;
    const bool touched = relay_to_node_ > 0;
    relay_state_ = RelayState::Off;
    relay_.close();
    ++relay_ended_id_;
    log("console relay: session ended (" + relay_end_reason_ + ") after " + std::to_string(paused / 1000) + " s, " + std::to_string(relay_to_node_) +
        " frames to the node, " + std::to_string(relay_to_console_) + " to the console; Mesh Hop resumes" + (touched ? " and reads the node again" : ""));
    // the timers of what Mesh Hop sent before the session start again where they stopped
    for (Outgoing &o : outgoing_)
        if (o.awaiting) o.ack_deadline += paused;
    for (ChannelSend &c : chan_pending_) c.deadline += paused;
    if (ready() && touched) {
        // the console may have changed the node under Mesh Hop: scope, name, radio, contacts, channels, variables, protocol level
        if (relay_scope_set_) reset_flood_scope();
        read_device_info();                                    // urgent: the console sent its own DEVICE_QUERY (the level the node answers in)
        refresh_self();                                        // urgent
        refresh_contacts();
        const int max_channels = model_.device() && model_.device()->max_channels ? model_.device()->max_channels : 8;
        queue_channels(std::min(max_channels, 40));
        read_custom_vars();
        refresh_autoadd();
    }
    relay_scope_set_ = false;
    want_sync_ = true;                                         // what the console left on the node
    sync_burst_ = 0;
    model_.touch();
    pump_queue();
}

void Client::reset_flood_scope()
{
    scope_reset_pending_ = false;
    Request r;
    r.payload = build_reset_flood_scope();
    r.accept = {resp::kOk};
    r.done = [this](Outcome o, const Packet *, int err) { log("flood scope reset: " + answer_text(static_cast<int>(o), err)); };
    enqueue(std::move(r), true);
}

/* ------------------------------------------------------------------ remote administration (report 036, slice S1) */

namespace {
Client::RemoteError remote_error_of(int err)
{
    switch (err) {
    case err::kNotFound: return Client::RemoteError::NotFound;
    case err::kTableFull: return Client::RemoteError::TableFull;
    case err::kUnsupported:
    case -1: return Client::RemoteError::Unsupported;       // an old firmware answers an unknown command with a bare error frame
    default: return Client::RemoteError::Failed;
    }
}

const char *remote_error_name(Client::RemoteError e)
{
    switch (e) {
    case Client::RemoteError::None: return "ok";
    case Client::RemoteError::Timeout: return "no answer";
    case Client::RemoteError::NotFound: return "not a contact on the board";
    case Client::RemoteError::TableFull: return "the board's send queue is full";
    case Client::RemoteError::Unsupported: return "unsupported by the board";
    case Client::RemoteError::Failed: return "error";
    case Client::RemoteError::Refused: return "refused (LOGIN_FAIL)";
    case Client::RemoteError::Busy: return "refused: a CLI session holds the node";
    case Client::RemoteError::NotConnected: return "refused: not connected";
    case Client::RemoteError::BadPassword: return "refused: password longer than 15 bytes or with a NUL";
    case Client::RemoteError::Cancelled: return "cancelled";
    case Client::RemoteError::LinkLost: return "the link to the board was lost";
    case Client::RemoteError::NotLoggedIn: return "refused: no session with the node (log in first)";
    case Client::RemoteError::NoDirectRoute: return "refused: no direct route for an anonymous request";
    case Client::RemoteError::BadReply: return "the answer could not be read";
    }
    return "?";
}

const char *remote_op_name(Client::RemoteOp op)
{
    switch (op) {
    case Client::RemoteOp::Login: return "login";
    case Client::RemoteOp::Logout: return "logout";
    case Client::RemoteOp::Status: return "status";
    case Client::RemoteOp::Neighbours: return "neighbours";
    case Client::RemoteOp::Owner: return "owner";
    default: return "request";
    }
}

bool is_request(Client::RemoteOp op)
{
    return op == Client::RemoteOp::Status || op == Client::RemoteOp::Neighbours || op == Client::RemoteOp::Owner;
}

Client::RemoteRole role_of_acl(uint8_t acl)
{
    switch (acl & 0x03) {                                      // ClientACL.h PERM_ACL_ROLE_MASK
    case 0: return Client::RemoteRole::Guest;
    case 1: return Client::RemoteRole::ReadOnly;
    case 2: return Client::RemoteRole::ReadWrite;
    default: return Client::RemoteRole::Admin;
    }
}

/* The role of a login success (see client.hpp start_login: which byte to trust on which firmware). companion_level: the board's
 * DEVICE_INFO level (-1 unknown). */
Client::RemoteRole role_of_login(const LoginSuccess &s, bool room, int companion_level)
{
    if (s.has_acl && s.has_fw_level && s.fw_level >= 1) return role_of_acl(s.acl);     // repeater / room server v1.9.0+
    // companion v1.10.0+ (level 8) pushes 14 bytes for a current answer, 8 for the legacy "OK" one, whose byte 1 is always 0
    if (!s.has_acl && companion_level >= 8) return s.permissions == 1 ? Client::RemoteRole::Admin : Client::RemoteRole::None;
    if (room) {                                                // room server before v1.9.0: RoomPermission ADMIN 1, GUEST 0, READ_ONLY 2
        switch (s.permissions) {
        case 1: return Client::RemoteRole::Admin;
        case 0: return Client::RemoteRole::ReadWrite;      // the room password: may post
        case 2: return Client::RemoteRole::ReadOnly;
        default: return Client::RemoteRole::None;
        }
    }
    return (s.permissions & 1) ? Client::RemoteRole::Admin : Client::RemoteRole::Guest;     // repeater before v1.9.0: is_admin
}

const char *role_name(Client::RemoteRole r)
{
    switch (r) {
    case Client::RemoteRole::Guest: return "guest";
    case Client::RemoteRole::ReadOnly: return "read only";
    case Client::RemoteRole::ReadWrite: return "read-write";
    case Client::RemoteRole::Admin: return "admin";
    default: return "role unknown";
    }
}
} // namespace

uint32_t Client::remote_wait_ms(uint32_t est_timeout_ms)
{
    if (est_timeout_ms == 0) return kRemoteDefaultWaitMs;
    const double w = static_cast<double>(est_timeout_ms) * kRemoteWaitFactor;
    if (w < kRemoteMinWaitMs) return kRemoteMinWaitMs;
    if (w > kRemoteMaxWaitMs) return kRemoteMaxWaitMs;
    return static_cast<uint32_t>(w);
}

const Client::RemoteSession *Client::remote_session(const PubKey &key) const
{
    for (const RemoteSession &s : sessions_)
        if (s.key == key) return &s;
    return nullptr;
}

void Client::publish_remote(RemoteResult r)
{
    r.id = remote_result_.id + 1;
    remote_result_ = std::move(r);
    model_.touch();
}

Client::RemoteResult Client::refuse_remote(RemoteOp op, const PubKey &key, RemoteError e)
{
    RemoteResult r;
    r.op = op;
    r.key = key;
    r.error = e;
    log(std::string("remote ") + remote_op_name(op) + " " + to_hex(key).substr(0, 8) + ": " + remote_error_name(e));
    publish_remote(r);
    return remote_result_;
}

void Client::wipe_remote_password()
{
    for (char &c : remote_password_) *static_cast<volatile char *>(&c) = 0;
    remote_password_.clear();
    remote_password_.shrink_to_fit();
    remote_remember_ = false;
}

void Client::drop_queued_remote(uint32_t req)
{
    if (req == 0) return;
    for (std::deque<Request> *q : {&urgent_, &normal_})
        q->erase(std::remove_if(q->begin(), q->end(), [req](const Request &r) { return r.remote_req == req; }), q->end());
}

void Client::end_remote(RemoteError e, int err_code)
{
    if (remote_.phase == RemoteState::Phase::Idle) return;
    const RemoteState st = remote_;
    remote_ = RemoteState();
    drop_queued_remote(st.req);                               // not sent yet: it never goes out
    wipe_remote_password();
    RemoteResult r;
    r.req = st.req;
    r.op = st.op;
    r.key = st.key;
    r.error = e;
    r.err_code = err_code;
    r.sent = st.phase == RemoteState::Phase::Waiting;
    r.flood = st.flood;
    r.anonymous = st.anonymous;                               // 0.8.0: an anonymous owner request that failed says so too
    r.waited_ms = static_cast<uint32_t>(now_ > st.since_ms ? now_ - st.since_ms : 0);
    log(std::string("remote ") + remote_op_name(st.op) + " " + to_hex(st.key).substr(0, 8) + ": " + remote_error_name(e) +
        (err_code >= 0 && (e == RemoteError::Failed || e == RemoteError::Unsupported) ? " (error " + std::to_string(err_code) + ")" : std::string()) +
        " after " + std::to_string(r.waited_ms) + " ms");
    publish_remote(r);
}

void Client::cancel_remote()
{
    end_remote(RemoteError::Cancelled);
}

uint32_t Client::start_login(const PubKey &key, const std::string &password_in, bool guest, bool remember)
{
    if (!ready()) { refuse_remote(RemoteOp::Login, key, RemoteError::NotConnected); return 0; }
    if (relay_active()) { refuse_remote(RemoteOp::Login, key, RemoteError::Busy); return 0; }
    const std::string password = guest ? std::string() : password_in;
    if (password.size() > kMaxLoginPassword || password.find('\0') != std::string::npos) {
        refuse_remote(RemoteOp::Login, key, RemoteError::BadPassword);
        return 0;
    }
    if (remote_busy()) end_remote(RemoteError::Cancelled);     // one at a time: the companion forgets the older one as well
    const uint32_t req = ++remote_req_counter_;
    remote_ = RemoteState();
    remote_.phase = RemoteState::Phase::Sending;
    remote_.op = RemoteOp::Login;
    remote_.req = req;
    remote_.key = key;
    remote_.since_ms = now_;
    remote_remember_ = remember && !password.empty();
    if (remote_remember_) remote_password_ = password;
    log("remote login " + to_hex(key).substr(0, 8) + ": " + (password.empty() ? "blank password (guest)" : "with a password") +
        (remote_remember_ ? ", remember it" : ""));
    Request r;
    r.payload = build_send_login(key, password);
    r.accept = {resp::kMsgSent};
    r.remote_req = req;
    r.done = [this, req](Outcome o, const Packet *p, int err) { on_remote_sent(req, o, p, err); };
    enqueue(std::move(r), true);
    model_.touch();
    return req;
}

void Client::on_remote_sent(uint32_t req, Outcome o, const Packet *p, int err)
{
    if (remote_.req != req || remote_.phase != RemoteState::Phase::Sending) return;     // cancelled meanwhile, or answered already
    if (o == Outcome::Ok) {
        if (const auto *s = pkt_as<MsgSent>(p)) {
            remote_.phase = RemoteState::Phase::Waiting;
            remote_.flood = s->type == 1;
            remote_.est_timeout_ms = s->suggested_timeout_ms;
            remote_.tag = s->expected_ack;                     // a request's answer (0x8C) carries it; a login's push is matched by key
            uint32_t wait = remote_wait_ms(s->suggested_timeout_ms);
            if (remote_.op == RemoteOp::Neighbours && remote_.nb_offset > 0)     // a later page: as meshcore_py, 25 s at least
                wait = std::min(kRemoteMaxWaitMs, std::max(wait, kRemoteNeighboursPageMinWaitMs));
            remote_.deadline_ms = now_ + wait;
            log(std::string("remote ") + remote_op_name(remote_.op) + " " + to_hex(remote_.key).substr(0, 8) + ": sent " +
                (remote_.flood ? "by flood" : "direct") + ", est_timeout " + std::to_string(s->suggested_timeout_ms) + " ms, waiting up to " +
                std::to_string(wait) + " ms");
            model_.touch();
            return;
        }
        end_remote(RemoteError::Failed);
    } else if (o == Outcome::Error) {
        end_remote(remote_error_of(err), err);
    } else if (o == Outcome::Timeout) {
        end_remote(RemoteError::Timeout);                      // the board did not even confirm the send (sent = false)
    } else {
        end_remote(RemoteError::LinkLost);
    }
}

/* ------------------------------------------------------------------ the read-only view (report 036, slice S2) */

Client::RemoteError Client::check_request(RemoteOp op, const PubKey &key, const ContactRec *&rec) const
{
    rec = nullptr;
    if (!ready()) return RemoteError::NotConnected;
    if (relay_active()) return RemoteError::Busy;
    rec = model_.find_contact(to_hex(key));
    if (!rec) return RemoteError::NotFound;
    // rooms answer the status only (simple_room_server handleRequest: no neighbours, no owner info, no anonymous request types)
    const uint8_t type = rec->c.type;
    if (type != advtype::kRepeater && !(type == advtype::kRoom && op == RemoteOp::Status)) return RemoteError::Unsupported;
    const RemoteSession *ses = remote_session(key);
    if (!ses && op != RemoteOp::Owner) return RemoteError::NotLoggedIn;
    if (ses && ses->has_fw_level) {
        if (op == RemoteOp::Neighbours && ses->fw_level < kRepeaterLevelNeighbours) return RemoteError::Unsupported;
        if (op == RemoteOp::Owner && ses->fw_level < kRepeaterLevelOwnerInfo) return RemoteError::Unsupported;
    }
    return RemoteError::None;
}

uint32_t Client::start_request(RemoteOp op, const PubKey &key, Bytes payload, const RemoteState &extra, const std::string &what)
{
    if (remote_busy()) end_remote(RemoteError::Cancelled);     // one at a time: the companion forgets the older one as well
    const uint32_t req = ++remote_req_counter_;
    remote_ = RemoteState();
    remote_.phase = RemoteState::Phase::Sending;
    remote_.op = op;
    remote_.req = req;
    remote_.key = key;
    remote_.since_ms = now_;
    remote_.anonymous = extra.anonymous;
    remote_.room = extra.room;
    remote_.nb_offset = extra.nb_offset;
    remote_.nb_prefix_len = extra.nb_prefix_len;
    log(std::string("remote ") + remote_op_name(op) + " " + to_hex(key).substr(0, 8) + ": " + what);
    Request r;
    r.payload = std::move(payload);
    r.accept = {resp::kMsgSent};
    r.remote_req = req;
    r.done = [this, req](Outcome o, const Packet *p, int err) { on_remote_sent(req, o, p, err); };
    enqueue(std::move(r), true);
    model_.touch();
    return req;
}

uint32_t Client::request_status(const PubKey &key)
{
    const ContactRec *rec = nullptr;
    const RemoteError e = check_request(RemoteOp::Status, key, rec);
    if (e != RemoteError::None) { refuse_remote(RemoteOp::Status, key, e); return 0; }
    RemoteState extra;
    extra.room = rec->c.type == advtype::kRoom;
    return start_request(RemoteOp::Status, key, build_status_request(key), extra, extra.room ? "a room's status" : "a repeater's status");
}

uint32_t Client::request_neighbours(const PubKey &key, uint16_t offset, uint8_t count, NeighbourOrder order)
{
    const ContactRec *rec = nullptr;
    const RemoteError e = check_request(RemoteOp::Neighbours, key, rec);
    if (e != RemoteError::None) { refuse_remote(RemoteOp::Neighbours, key, e); return 0; }
    tag_counter_ += 0x9E3779B9u;                               // the nonce only makes the packet hash unique (the firmware ignores it)
    const uint32_t nonce = (static_cast<uint32_t>(now_) * 2654435761u ^ tag_counter_) | 1u;
    RemoteState extra;
    extra.nb_offset = offset;
    extra.nb_prefix_len = static_cast<uint8_t>(kNeighbourPrefixLen);
    return start_request(RemoteOp::Neighbours, key, build_neighbours_request(key, count, offset, order, extra.nb_prefix_len, nonce), extra,
                         "from " + std::to_string(offset) + ", up to " + std::to_string(count) + ", order " + std::to_string(static_cast<int>(order)));
}

uint32_t Client::request_owner(const PubKey &key)
{
    const ContactRec *rec = nullptr;
    const RemoteError e = check_request(RemoteOp::Owner, key, rec);
    if (e != RemoteError::None) { refuse_remote(RemoteOp::Owner, key, e); return 0; }
    if (remote_session(key)) return start_request(RemoteOp::Owner, key, build_owner_info_request(key), RemoteState(), "OWNER_INFO (logged in)");
    const auto path = anon_reply_path(rec->c.out_path_len, rec->c.out_path);
    if (!path) { refuse_remote(RemoteOp::Owner, key, RemoteError::NoDirectRoute); return 0; }
    RemoteState extra;
    extra.anonymous = true;
    return start_request(RemoteOp::Owner, key, build_anon_request(key, anontype::kOwner, *path), extra,
                         "anonymous OWNER, reply path of " + std::to_string(path_hops(rec->c.out_path_len)) + " hops");
}

/* A push 0x8C: the answer to the pending request when it carries that request's tag; anything else is ignored. */
void Client::on_binary_response(const BinaryResponse &b)
{
    const bool pending = remote_.phase == RemoteState::Phase::Waiting && is_request(remote_.op) && b.tag == remote_.tag;
    if (!pending) {
        if (!relay_active()) log("remote answer (0x8C, tag " + to_hex(b.tag.data(), b.tag.size()) + ", " + std::to_string(b.data.size()) +
                                 " bytes) ignored: no request with that tag is pending");
        return;
    }
    const RemoteState st = remote_;
    const RemoteSession *ses = remote_session(st.key);
    RemoteResult r;
    r.req = st.req;
    r.op = st.op;
    r.key = st.key;
    r.sent = true;
    r.flood = st.flood;
    r.anonymous = st.anonymous;
    r.waited_ms = static_cast<uint32_t>(now_ > st.since_ms ? now_ - st.since_ms : 0);
    bool ok = false;
    std::string what;
    if (st.op == RemoteOp::Status) {
        if (auto s = parse_repeater_status(b.data, st.room)) {
            r.status = *s;
            // the 52 byte reply of a repeater before v1.12.0 is padded to look 56 long: its "error counter" is padding
            if (ses && ses->has_fw_level && ses->fw_level < kRepeaterLevelOwnerInfo) r.status.has_rx_errors = false;
            ok = true;
            what = "battery " + std::to_string(s->battery_mv) + " mV, up " + std::to_string(s->uptime_s) + " s";
        }
    } else if (st.op == RemoteOp::Neighbours) {
        if (auto pg = parse_neighbours(b.data, st.nb_prefix_len, st.nb_offset)) {
            r.neighbours = std::move(*pg);
            ok = true;
            what = std::to_string(r.neighbours.entries.size()) + " of " + std::to_string(r.neighbours.total) + " from " +
                   std::to_string(r.neighbours.offset) + (r.neighbours.more ? ", more" : "");
        }
    } else if (auto o = st.anonymous ? parse_anon_owner(b.data) : parse_owner_info(b.data)) {
        r.owner = std::move(*o);
        if (ses && ses->has_fw_level) {
            r.has_fw_level = true;
            r.fw_level = ses->fw_level;
        }
        ok = true;
        what = (st.anonymous ? std::string("anonymous answer") : "firmware " + r.owner.fw_version) + ", " +
               std::to_string(r.owner.owner_info.size()) + " bytes of owner info";
    }
    if (!ok) {
        log(std::string("remote ") + remote_op_name(st.op) + " " + to_hex(st.key).substr(0, 8) + ": an answer of " + std::to_string(b.data.size()) +
            " bytes that cannot be read");
        end_remote(RemoteError::BadReply);
        return;
    }
    r.ok = true;
    remote_ = RemoteState();
    drop_queued_remote(st.req);
    log(std::string("remote ") + remote_op_name(st.op) + " " + to_hex(st.key).substr(0, 8) + ": " + what + " after " + std::to_string(r.waited_ms) + " ms");
    publish_remote(std::move(r));
}

/* The verdict of the remote node (0x85, or 0x86 when s is null): it ends the pending login when it is from that node. */
void Client::on_login_reply(const KeyPrefix &prefix, const LoginSuccess *s)
{
    const bool pending = remote_.phase != RemoteState::Phase::Idle && remote_.op == RemoteOp::Login &&
                         std::equal(prefix.begin(), prefix.end(), remote_.key.begin());
    if (!pending) {
        if (!relay_active()) log("remote login answer from " + to_hex(prefix) + " ignored: no login to that node is pending");
        return;
    }
    if (!s) {
        end_remote(RemoteError::Refused);
        return;
    }
    const RemoteState st = remote_;
    RemoteResult r;
    r.req = st.req;
    r.op = RemoteOp::Login;
    r.key = st.key;
    r.ok = true;
    r.sent = true;
    r.flood = st.flood;
    r.waited_ms = static_cast<uint32_t>(now_ > st.since_ms ? now_ - st.since_ms : 0);
    r.has_acl = s->has_acl;
    r.permissions = s->has_acl ? s->acl : s->permissions;
    {
        const ContactRec *node = model_.find_contact(to_hex(st.key));
        const bool room = node && node->c.type == advtype::kRoom;
        r.role = role_of_login(*s, room, model_.device() ? static_cast<int>(model_.device()->fw_ver) : -1);
    }
    r.has_clock = s->has_clock;
    r.clock = s->clock;
    r.deck_time = deck_unix();
    r.has_fw_level = s->has_fw_level;
    r.fw_level = s->fw_level;
    if (remote_remember_) {
        if (!passwords_) {
            r.password = PasswordSave::Failed;
        } else {
            switch (passwords_->save(to_hex(st.key), remote_password_)) {
            case PasswordStore::SaveResult::Saved: r.password = PasswordSave::Saved; break;
            case PasswordStore::SaveResult::IoError: r.password = PasswordSave::Failed; break;
            default: r.password = PasswordSave::Skipped; break;
            }
        }
    }
    remote_ = RemoteState();
    wipe_remote_password();
    drop_queued_remote(st.req);
    RemoteSession ses;
    ses.key = st.key;
    ses.role = r.role;
    ses.permissions = r.permissions;
    ses.since = r.deck_time;
    ses.has_clock = r.has_clock;
    ses.clock = r.clock;
    ses.deck_time = r.deck_time;
    ses.has_fw_level = r.has_fw_level;
    ses.fw_level = r.fw_level;
    const auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const RemoteSession &x) { return x.key == st.key; });
    if (it != sessions_.end()) *it = ses;
    else sessions_.push_back(ses);
    log("remote login " + to_hex(st.key).substr(0, 8) + ": " + role_name(r.role) + " after " + std::to_string(r.waited_ms) + " ms" +
        (r.has_fw_level ? ", remote firmware level " + std::to_string(r.fw_level) : std::string(", legacy answer")) +
        (r.password == PasswordSave::Saved ? ", password saved" : r.password == PasswordSave::Failed ? ", password NOT saved" :
         r.password == PasswordSave::Skipped ? ", password not storable" : ""));
    publish_remote(r);
}

void Client::tick_remote()
{
    if (remote_.phase == RemoteState::Phase::Waiting && now_ >= remote_.deadline_ms) end_remote(RemoteError::Timeout);
}

uint32_t Client::logout(const PubKey &key)
{
    if (!ready()) { refuse_remote(RemoteOp::Logout, key, RemoteError::NotConnected); return 0; }
    if (relay_active()) { refuse_remote(RemoteOp::Logout, key, RemoteError::Busy); return 0; }
    if (remote_busy() && remote_.key == key) end_remote(RemoteError::Cancelled);
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const RemoteSession &s) { return s.key == key; }), sessions_.end());
    const uint32_t req = ++remote_req_counter_;
    const uint64_t since = now_;
    log("remote logout " + to_hex(key).substr(0, 8) + ": the session here is forgotten (the node keeps its access list)");
    Request r;
    r.payload = build_logout(key);
    r.accept = {resp::kOk};
    r.done = [this, req, key, since](Outcome o, const Packet *, int err) {
        RemoteResult res;
        res.req = req;
        res.op = RemoteOp::Logout;
        res.key = key;
        res.ok = o == Outcome::Ok;
        res.sent = o == Outcome::Ok;
        res.error = o == Outcome::Ok ? RemoteError::None : o == Outcome::Error ? remote_error_of(err) : o == Outcome::Timeout ? RemoteError::Timeout : RemoteError::LinkLost;
        res.err_code = o == Outcome::Error ? err : -1;
        res.waited_ms = static_cast<uint32_t>(now_ > since ? now_ - since : 0);
        log("remote logout " + to_hex(key).substr(0, 8) + ": " + answer_text(static_cast<int>(o), err));
        publish_remote(res);
    };
    enqueue(std::move(r), true);
    model_.touch();
    return req;
}

/* ------------------------------------------------------------------ sending */

size_t Client::max_text(const std::string &conv) const
{
    if (conv.rfind("c:", 0) == 0) {
        // the firmware prepends "<node name>: " to a channel message and the whole text is length limited
        const size_t overhead = model_.self_name().size() + 2;
        return std::max<size_t>(40, std::min<size_t>(kMaxDirectText, 150 - std::min<size_t>(overhead, 110)));
    }
    return kMaxDirectText;
}

static std::string trim_text(const std::string &t)
{
    size_t a = 0, b = t.size();
    while (a < b && (t[a] == ' ' || t[a] == '\n' || t[a] == '\t')) ++a;
    while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\n' || t[b - 1] == '\t')) --b;
    return t.substr(a, b - a);
}

bool Client::send_advert(bool flood)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    send_advert_impl(flood, false);
    return true;
}

void Client::send_advert_impl(bool flood, bool scheduled)
{
    Request r;
    r.payload = build_send_advert(flood);
    r.accept = {resp::kOk};
    r.done = [this, flood, scheduled](Outcome o, const Packet *, int err) {
        const std::string what = std::string(scheduled ? "Scheduled advert" : "Advert");
        if (scheduled) log(std::string("scheduled advert (") + (flood ? "flood" : "zero-hop") + "): " + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) set_notice(true, what + (scheduled ? " sent" : " sent") + (flood ? " (flood)" : " (zero-hop)"));
        else if (o != Outcome::Aborted) set_notice(false, what + " failed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
    };
    enqueue(std::move(r), true);
}

uint32_t Client::send_direct(const std::string &contact_key_hex, const std::string &text_in)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return 0; }
    const std::string text = sanitize_utf8(trim_text(text_in));
    if (text.empty()) return 0;
    if (text.size() > max_text("d:")) { set_notice(false, "Message too long"); return 0; }
    const ContactRec *rec = model_.find_contact(contact_key_hex);
    if (!rec) { set_notice(false, "Unknown contact"); return 0; }
    Outgoing o;
    o.key = rec->c.key;
    o.prefix = rec->prefix();
    o.text = text;
    o.ts = now_unix();
    o.seq = model_.add_outgoing(Model::conv_direct(o.prefix), text, o.ts, path_hops(rec->c.out_path_len));
    outgoing_.push_back(o);
    transmit(o.seq);
    return o.seq;
}

void Client::transmit(uint32_t seq)
{
    const auto it = std::find_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing &o) { return o.seq == seq; });
    if (it == outgoing_.end()) return;
    Request r;
    r.payload = build_send_txt(it->prefix, it->text, it->ts, static_cast<uint8_t>(it->attempt));
    r.accept = {resp::kMsgSent};
    r.done = [this, seq](Outcome o, const Packet *p, int err) {
        const auto cur = std::find_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing &x) { return x.seq == seq; });
        if (cur == outgoing_.end()) return;
        if (o == Outcome::Ok) {
            if (const auto *s = pkt_as<MsgSent>(p)) {
                cur->acks.push_back(s->expected_ack);
                const uint32_t wait = std::max<uint32_t>(3000, static_cast<uint32_t>(s->suggested_timeout_ms * 1.2));
                cur->ack_deadline = now_ + wait;
                cur->awaiting = true;
                model_.set_state(seq, MsgState::Sent);
                return;
            }
        }
        model_.set_state(seq, MsgState::Failed,
                         o == Outcome::Error ? error_text(err) : (o == Outcome::Aborted ? "board disconnected" : "the radio did not answer"));
        outgoing_.erase(cur);
    };
    enqueue(std::move(r), true);
}

void Client::tick_outgoing()
{
    // a channel message has no acknowledgement: whatever the board answers (or not) within the fallback time, it ends as "sent"
    for (size_t i = 0; i < chan_pending_.size(); ++i) {
        if (now_ < chan_pending_[i].deadline) continue;
        const uint32_t seq = chan_pending_[i].seq;
        chan_pending_.erase(chan_pending_.begin() + static_cast<long>(i));
        --i;
        const Message *m = model_.find_message(seq);
        if (m && m->state == MsgState::Pending) {
            log("channel send seq=" + std::to_string(seq) + ": no answer within " + std::to_string(kChannelSendFallbackMs) + " ms, shown as sent (unconfirmed)");
            model_.set_state(seq, MsgState::Sent, kUnconfirmedNote);
        }
    }
    const RetrySettings rs = model_.retry();
    for (size_t i = 0; i < outgoing_.size(); ++i) {
        Outgoing &o = outgoing_[i];
        if (!o.awaiting || now_ < o.ack_deadline) continue;
        if (o.attempt + 1 >= rs.attempts) {
            model_.set_state(o.seq, MsgState::NoAck, "no acknowledgement");
            outgoing_.erase(outgoing_.begin() + static_cast<long>(i));
            --i;
            continue;
        }
        o.awaiting = false;
        ++o.attempt;
        model_.set_state(o.seq, MsgState::Pending, "retry " + std::to_string(o.attempt));
        const ContactRec *c = model_.find_contact(to_hex(o.key));
        if (rs.reset_after > 0 && o.attempt == rs.reset_after && c && c->c.out_path_len != kPathFlood) {
            // the stored route may be dead: fall back to flooding from this attempt on (the same command as "Reset path")
            queue_reset_path(o.key, {});
        }
        transmit(o.seq);
    }
}

void Client::queue_reset_path(const PubKey &key, std::function<void(Outcome, int)> then)
{
    Request rp;
    rp.payload = build_reset_path(key);
    rp.accept = {resp::kOk};
    rp.done = [this, key, then](Outcome o, const Packet *, int err) {
        log("reset path " + to_hex(key.data(), 4) + ": " + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) {
            if (const ContactRec *c = model_.find_contact(to_hex(key))) {
                Contact flood = c->c;                            // the board forgot the route: flood from now on
                flood.out_path_len = kPathFlood;
                flood.out_path.fill(0);
                model_.upsert_contact(flood);
            }
            Request g;                                           // and its record read back (what the board holds now)
            g.payload = build_get_contact_by_key(key);
            g.accept = {resp::kContact};
            g.done = [](Outcome, const Packet *, int) {};
            enqueue(std::move(g));
        }
        if (then) then(o, err);
    };
    enqueue(std::move(rp), true);
}

/* PATH_UPDATED (0x81): the companion stored a new route to this contact (companion MyMesh.cpp onContactPathUpdated). The model's
 * copy still has the old one: read the contact back (its CONTACT answer goes through upsert_contact like any other). */
void Client::on_path_updated(const PubKey &key)
{
    if (!ready() || relay_active()) return;                    // a CLI session: the console reads it (the session's end re-reads)
    if (!model_.find_contact(to_hex(key))) return;            // not one of ours (a contact only the board knows comes with the sync)
    if (std::find(path_lookups_.begin(), path_lookups_.end(), key) != path_lookups_.end()) return;     // already asked
    path_lookups_.push_back(key);
    log("path updated " + to_hex(key.data(), 4) + ": reading the contact back");
    Request g;
    g.payload = build_get_contact_by_key(key);
    g.accept = {resp::kContact};
    g.done = [this, key](Outcome o, const Packet *, int err) {
        path_lookups_.erase(std::remove(path_lookups_.begin(), path_lookups_.end(), key), path_lookups_.end());
        if (o != Outcome::Ok) log("path updated " + to_hex(key.data(), 4) + ": the contact was not read back (" + answer_text(static_cast<int>(o), err) + ")");
    };
    enqueue(std::move(g));
}

void Client::publish_path_reset(const PubKey &key, bool ok, PathResetError e, int err_code)
{
    PathResetResult r;
    r.id = path_reset_result_.id + 1;
    r.key = key;
    r.ok = ok;
    r.error = e;
    r.err_code = err_code;
    path_reset_result_ = r;
}

bool Client::reset_path(const PubKey &key)
{
    if (!ready()) { publish_path_reset(key, false, PathResetError::NotConnected); return false; }
    if (relay_active() || path_reset_busy_) { publish_path_reset(key, false, PathResetError::Busy); return false; }
    path_reset_busy_ = true;
    queue_reset_path(key, [this, key](Outcome o, int err) {
        path_reset_busy_ = false;
        switch (o) {
        case Outcome::Ok: publish_path_reset(key, true, PathResetError::None); break;
        case Outcome::Error: publish_path_reset(key, false, err == err::kNotFound ? PathResetError::NotFound : PathResetError::Failed, err); break;
        case Outcome::Timeout: publish_path_reset(key, false, PathResetError::Timeout); break;
        case Outcome::Aborted: publish_path_reset(key, false, PathResetError::LinkLost); break;
        }
    });
    return true;
}

void Client::handle_ack(const Ack &a)
{
    for (size_t i = 0; i < outgoing_.size(); ++i) {
        const auto &acks = outgoing_[i].acks;
        if (std::find(acks.begin(), acks.end(), a.code) == acks.end()) continue;
        model_.set_state(outgoing_[i].seq, MsgState::Delivered);
        outgoing_.erase(outgoing_.begin() + static_cast<long>(i));
        return;
    }
}

uint32_t Client::send_channel(int idx, const std::string &text_in)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return 0; }
    const std::string text = sanitize_utf8(trim_text(text_in));
    if (text.empty()) return 0;
    if (text.size() > max_text(Model::conv_channel(idx))) { set_notice(false, "Message too long"); return 0; }
    const uint32_t ts = now_unix();
    const uint32_t seq = model_.add_outgoing(Model::conv_channel(idx), text, ts);
    chan_pending_.push_back({seq, now_ + kChannelSendFallbackMs});
    register_echo(seq, idx, text);
    log("channel send seq=" + std::to_string(seq) + " channel=" + std::to_string(idx) + " bytes=" + std::to_string(text.size()));
    Request r;
    r.payload = build_send_channel_txt(static_cast<uint8_t>(idx), text, ts);
    // The report on the real board says "OK or MSG_SENT"; the first test on the deck stayed on "sending". So: any answer that is not an
    // error completes the command, and the fallback timer above ends it even without one. A channel has no ack: it ends at "sent".
    r.accept = {resp::kOk, resp::kMsgSent};
    r.accept_any = true;
    r.timeout_ms = kChannelSendFallbackMs;
    r.done = [this, seq](Outcome o, const Packet *, int err) {
        const auto it = std::find_if(chan_pending_.begin(), chan_pending_.end(), [&](const ChannelSend &c) { return c.seq == seq; });
        if (it != chan_pending_.end()) chan_pending_.erase(it);
        if (o == Outcome::Ok) {
            log("channel send seq=" + std::to_string(seq) + ": answer " + code_text(last_code_) + " -> sent");
            model_.set_state(seq, MsgState::Sent);
        } else if (o == Outcome::Timeout) {
            log("channel send seq=" + std::to_string(seq) + ": no answer, shown as sent (unconfirmed)");
            const Message *m = model_.find_message(seq);
            if (m && m->state == MsgState::Pending) model_.set_state(seq, MsgState::Sent, kUnconfirmedNote);
        } else {
            log("channel send seq=" + std::to_string(seq) + ": " + (o == Outcome::Error ? "error " + std::to_string(err) : std::string("aborted")));
            model_.set_state(seq, MsgState::Failed, o == Outcome::Error ? error_text(err) : "board disconnected");
        }
    };
    enqueue(std::move(r), true);
    return seq;
}

/* ------------------------------------------------------------------ the radio log: heard back, packet log */

void Client::register_echo(uint32_t seq, int idx, const std::string &text)
{
    const ChannelRec *ch = model_.find_channel(idx);
    if (!ch || ch->empty || !ch->has_secret) {
        log("channel send seq=" + std::to_string(seq) + ": channel key not known, no heard-back count");
        return;
    }
    // what the firmware puts on the air: the channel hash (first byte of sha256 of the 16 byte secret) and "<node name>: <text>" encrypted;
    // the lengths are UTF-8 bytes (std::string sizes). Without SELF_INFO the name is unknown: any size is accepted then.
    const int hash = sha256(ch->secret.data(), ch->secret.size())[0];
    const size_t size = model_.self() ? expected_grp_txt_payload_size(model_.self()->name.size(), text.size()) : 0;
    ChannelEcho e;
    e.id = echo_.register_sent(hash, static_cast<double>(now_) / 1000.0, size);
    e.seq = seq;
    e.epoch = model_.board_epoch();
    echoes_.push_back(std::move(e));
}

void Client::on_log_packet(const LogPacket &pkt)
{
    packet_log_.add(pkt, now_unix());
    const EchoTracker::Id id = echo_.on_packet(pkt, static_cast<double>(now_) / 1000.0);
    if (id == EchoTracker::kNone || pkt.path.empty()) return;          // an empty path: heard straight from the sender, no repeater
    for (ChannelEcho &e : echoes_) {
        if (e.id != id) continue;
        Bytes route;
        route.push_back(pkt.hash_size);
        route.insert(route.end(), pkt.path.begin(), pkt.path.end());
        if (std::find(e.routes.begin(), e.routes.end(), route) != e.routes.end()) return;
        e.routes.push_back(std::move(route));
        if (e.epoch == model_.board_epoch()) model_.set_heard_back(e.seq, static_cast<int>(e.routes.size()), false);
        return;
    }
}

void Client::tick_echoes()
{
    const double now_s = static_cast<double>(now_) / 1000.0;
    for (size_t i = 0; i < echoes_.size(); ++i) {
        const ChannelEcho &e = echoes_[i];
        if (!echo_.expired(e.id, now_s)) continue;       // (a send pushed out of the tracker's last 16 counts as expired too)
        if (!e.routes.empty() && e.epoch == model_.board_epoch()) {
            model_.set_heard_back(e.seq, static_cast<int>(e.routes.size()), true);
            log("channel send seq=" + std::to_string(e.seq) + ": heard back on " + std::to_string(e.routes.size()) + " routes");
        }
        echoes_.erase(echoes_.begin() + static_cast<long>(i));
        --i;
    }
}

/* ------------------------------------------------------------------ settings */

bool Client::save_settings(const RadioSettings &e)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const RadioSettings cur = model_.radio_settings();
    std::string bad = validate_name(e.name);
    if (bad.empty()) bad = validate_radio(e);
    if (!bad.empty()) { set_notice(false, bad); return false; }

    struct Progress { int errors = 0; std::string first; };
    auto prog = std::make_shared<Progress>();
    auto step = [this, prog](Bytes payload) {
        Request r;
        r.payload = std::move(payload);
        r.accept = {resp::kOk};
        r.done = [prog](Outcome o, const Packet *, int err) {
            if (o == Outcome::Ok) return;
            if (prog->errors++ == 0) prog->first = o == Outcome::Error ? error_text(err) : "no answer";
        };
        enqueue(std::move(r), true);
    };
    int steps = 0;
    if (e.name != cur.name) { step(build_set_name(e.name)); ++steps; }
    if (!e.same_radio(cur) && (std::abs(e.freq_mhz - cur.freq_mhz) >= 0.0005 || std::abs(e.bw_khz - cur.bw_khz) >= 0.0005 || e.sf != cur.sf || e.cr != cur.cr)) {
        step(build_set_radio(e.freq_mhz, e.bw_khz, static_cast<uint8_t>(e.sf), static_cast<uint8_t>(e.cr)));
        ++steps;
    }
    if (e.tx_power != cur.tx_power) { step(build_set_tx_power(e.tx_power)); ++steps; }
    if (steps == 0) { set_notice(true, "Nothing to save"); return true; }
    // read the settings back: what the radio reports is what the screen shows
    Request r;
    r.payload = build_app_start(app_name());
    r.accept = {resp::kSelfInfo};
    r.done = [this, prog](Outcome o, const Packet *, int) {
        if (prog->errors) set_notice(false, "Not saved: " + prog->first);
        else if (o == Outcome::Ok) set_notice(true, "Settings saved to the radio");
        else set_notice(false, "Saved, but the radio did not confirm");
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::add_public_channel()
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    Request r;
    r.payload = build_set_channel(0, "Public", kPublicChannelSecret);
    r.accept = {resp::kOk};
    r.done = [this](Outcome o, const Packet *, int err) {
        if (o != Outcome::Ok) { set_notice(false, "Public channel: " + (o == Outcome::Error ? error_text(err) : std::string("no answer"))); return; }
        set_notice(true, "Public channel added");
        Request g;
        g.payload = build_get_channel(0);
        g.accept = {resp::kChannelInfo};
        g.done = [](Outcome, const Packet *, int) {};
        enqueue(std::move(g), true);
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::add_channel_in_free_slot(const std::string &name, const ChannelSecret &key, bool hashtag)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (model_.caps().channels == Cap::No) { set_notice(false, "Firmware too old for this feature: channel management"); return false; }
    if (model_.find_channel_by_name(name)) { set_notice(false, name + " is already on the radio"); return false; }
    const int slot = model_.free_channel_slot();
    if (slot < 0) { set_notice(false, "No free channel slot on the radio"); return false; }
    if (hashtag) model_.mark_added(name);
    Request r;
    r.payload = build_set_channel(static_cast<uint8_t>(slot), name, key);
    r.accept = {resp::kOk};
    r.done = [this, name, slot, hashtag](Outcome o, const Packet *, int err) {
        if (o != Outcome::Ok) {
            if (hashtag) model_.unmark_added(name);
            set_notice(false, name + ": " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
            return;
        }
        set_notice(true, name + " added (slot " + std::to_string(slot) + ")");
        Request g;
        g.payload = build_get_channel(static_cast<uint8_t>(slot));
        g.accept = {resp::kChannelInfo};
        g.done = [](Outcome, const Packet *, int) {};
        enqueue(std::move(g), true);
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::add_hashtag_channel(const std::string &name)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const std::string bad = validate_hashtag(name);
    if (!bad.empty()) { set_notice(false, bad); return false; }
    return add_channel_in_free_slot(name, hashtag_secret(name), true);
}

bool Client::add_private_channel(const std::string &name, const ChannelSecret &key)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const std::string bad = validate_private_name(name);
    if (!bad.empty()) { set_notice(false, bad); return false; }
    bool zero = true;
    for (uint8_t b : key)
        if (b) zero = false;
    if (zero) { set_notice(false, "The key cannot be all zeros"); return false; }
    return add_channel_in_free_slot(name, key, false);
}

bool Client::remove_channel(int idx)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const ChannelRec *c = model_.find_channel(idx);
    if (idx == 0) { set_notice(false, "The Public channel cannot be removed"); return false; }
    if (!c || c->empty) { set_notice(false, "No such channel"); return false; }
    const std::string name = c->name;
    Request r;
    r.payload = build_set_channel(static_cast<uint8_t>(idx), "", std::array<uint8_t, 16>{});
    r.accept = {resp::kOk};
    r.done = [this, name, idx](Outcome o, const Packet *, int err) {
        if (o != Outcome::Ok) { set_notice(false, "Remove failed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer"))); return; }
        model_.unmark_added(name);
        model_.set_muted(Model::conv_channel(idx), false);       // the slot may be reused by another channel
        set_notice(true, name + " removed");
        Request g;
        g.payload = build_get_channel(static_cast<uint8_t>(idx));
        g.accept = {resp::kChannelInfo};
        g.done = [](Outcome, const Packet *, int) {};
        enqueue(std::move(g), true);
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::set_board_gps(bool on)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const BoardCaps &caps = model_.caps();
    if (caps.custom_vars == Cap::No) { set_notice(false, "Firmware too old for this feature: Board GPS"); return false; }
    if (!caps.gps_listed) { set_notice(false, "This board reports no GPS"); return false; }
    Request r;
    r.payload = build_set_custom_var("gps", on ? "1" : "0");
    r.accept = {resp::kOk};
    r.done = [this, on](Outcome o, const Packet *, int err) {
        if (o == Outcome::Ok) {
            set_notice(true, on ? "Board GPS switched on" : "Board GPS switched off");
            read_custom_vars();
        } else if (o != Outcome::Aborted) {
            set_notice(false, "Board GPS: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::set_position(double lat, double lon)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const std::string bad = check_position(lat, lon);
    if (!bad.empty()) { set_notice(false, bad); return false; }
    const bool clear = !position_is_set(lat, lon);
    const std::string text = clear ? "Position cleared" : "Position set to " + fmt_coordinate(lat) + ", " + fmt_coordinate(lon);
    Request r;
    r.payload = build_set_advert_latlon(lat, lon);
    r.accept = {resp::kOk};
    r.done = [this, text](Outcome o, const Packet *, int err) {
        log("set position: " + answer_text(static_cast<int>(o), err));          // never the coordinates
        if (o == Outcome::Ok) {
            // read the position back: what the board reports is what the screen shows
            Request g;
            g.payload = build_app_start(app_name());
            g.accept = {resp::kSelfInfo};
            g.done = [this, text](Outcome og, const Packet *, int) {
                if (og == Outcome::Ok) set_notice(true, text);
                else if (og != Outcome::Aborted) set_notice(false, "Position sent, but the board did not confirm");
            };
            enqueue(std::move(g), true);
        } else if (o != Outcome::Aborted) {
            set_notice(false, o == Outcome::Error && err == err::kUnsupported ? "Firmware too old for this feature: Position"
                                                                                : "Position not set: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

/* ================================================================== phase 2: contacts */

bool Client::remove_contacts(const std::vector<PubKey> &keys)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (bulk_.active) { set_notice(false, "A delete is already running"); return false; }
    if (keys.empty()) { set_notice(false, "No contact selected"); return false; }
    const uint32_t id = bulk_.id + 1;
    bulk_ = BulkProgress();
    bulk_.id = id;
    bulk_.active = true;
    bulk_.total = keys.size();
    bulk_queue_.assign(keys.begin(), keys.end());
    bulk_removed_.clear();
    bulk_inflight_ = false;
    model_.begin_contact_batch();
    log("bulk delete: " + std::to_string(keys.size()) + " contacts");
    pump_bulk();
    return true;
}

void Client::pump_bulk()
{
    if (!bulk_.active || bulk_inflight_) return;
    if (bulk_queue_.empty() || bulk_.cancelled) {
        end_bulk(bulk_.cancelled ? "stopped" : "");
        return;
    }
    const PubKey key = bulk_queue_.front();
    bulk_queue_.pop_front();
    bulk_inflight_ = true;
    Request r;
    r.payload = build_remove_contact(key);
    r.accept = {resp::kOk};
    r.done = [this, key](Outcome o, const Packet *, int err) {
        bulk_inflight_ = false;
        if (o == Outcome::Aborted) {
            bulk_.cancelled = true;
            end_bulk("the board disconnected");
            return;
        }
        if (o == Outcome::Ok || (o == Outcome::Error && err == err::kNotFound)) {      // not found: it is gone from the board anyway
            model_.remove_contact(key);
            bulk_removed_.push_back(to_hex(key));
            ++bulk_.done;
        } else {
            ++bulk_.failed;
            log("bulk delete: " + std::string(o == Outcome::Error ? error_text(err) : "no answer") + " for " + to_hex(key).substr(0, 8));
        }
        pump_bulk();
    };
    enqueue(std::move(r));
}

void Client::end_bulk(const std::string &why)
{
    if (!bulk_.active) return;
    bulk_.active = false;
    bulk_queue_.clear();
    model_.end_contact_batch();
    if (!bulk_removed_.empty()) {                                  // a deleted contact leaves its groups
        if (model_.groups_mut().remove_everywhere(bulk_removed_) > 0) model_.groups_changed();
    }
    std::string text = "Deleted " + std::to_string(bulk_.done) + (bulk_.done == 1 ? " contact" : " contacts");
    if (bulk_.failed) text += ", " + std::to_string(bulk_.failed) + " failed";
    if (!why.empty()) text += " (" + why + ")";
    log("bulk delete finished: " + text);
    set_notice(bulk_.failed == 0 && !bulk_.cancelled, text);
    bulk_removed_.clear();
}

bool Client::add_nearby(const std::string &key_hex)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const NearbyRec *n = model_.find_nearby(key_hex);
    if (!n) { set_notice(false, "That node is not in the list any more"); return false; }
    if (model_.find_contact(key_hex)) { set_notice(true, "Already a contact"); return false; }
    if (!n->full_key) { set_notice(false, "Only a part of its key is known: wait for its next advert"); return false; }
    Contact c;
    if (n->has_contact) {
        c = n->contact;                                          // the record the board sent in NEW_ADVERT
    } else {
        c.key = n->key;
        c.type = n->type;
        c.name = n->name;
        c.out_path_len = kPathFlood;
        c.last_advert = now_unix();
        if (n->has_pos) { c.lat = n->lat; c.lon = n->lon; }
    }
    if (c.type == 0) { set_notice(false, "Its type is not known yet: wait for its next advert"); return false; }
    if (c.name.empty()) c.name = "Node " + key_hex.substr(0, 6);       // the name arrives with its next advert
    const std::string name = c.name;
    Request r;
    r.payload = build_add_update_contact(c);
    r.accept = {resp::kOk};
    r.done = [this, c, name, key_hex](Outcome o, const Packet *, int err) {
        log("add contact " + key_hex.substr(0, 8) + " (type " + std::to_string(c.type) + "): " + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) {
            model_.upsert_contact(c);
            model_.heard(c.key, now_unix());
            set_notice(true, name + " added to the contacts");
            Request g;                                           // read it back: the board fills in what it keeps (modification time)
            g.payload = build_get_contact_by_key(c.key);
            g.accept = {resp::kContact};
            g.done = [](Outcome, const Packet *, int) {};
            enqueue(std::move(g));
        } else if (o == Outcome::Error && err == err::kTableFull) {
            set_notice(false, "The radio's contact list is full: delete some contacts first");
        } else if (o != Outcome::Aborted) {
            set_notice(false, "Not added: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

void Client::send_other_params(bool manual, bool with_multi_acks)
{
    const SelfInfo *s = model_.self() ? &*model_.self() : nullptr;
    if (!s) return;
    Request r;
    r.payload = build_set_other_params(manual, s->telemetry_mode, s->adv_loc_policy, s->multi_acks, with_multi_acks);
    r.accept = {resp::kOk};
    r.done = [this, manual, with_multi_acks](Outcome o, const Packet *, int err) {
        log(std::string("manual add ") + (manual ? "on" : "off") + (with_multi_acks ? " (5 bytes): " : " (4 bytes): ") + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) {
            set_notice(true, manual ? "New nodes now wait in Nearby until you add them" : "The board adds new nodes to the contacts by itself");
            refresh_self();
        } else if (o == Outcome::Error && with_multi_acks && (err == err::kIllegalArg || err == err::kUnsupported)) {
            send_other_params(manual, false);                    // an older firmware knows only the 4 byte form
        } else if (o != Outcome::Aborted) {
            set_notice(false, "Not changed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
}

bool Client::set_manual_add(bool manual)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (!model_.self()) { set_notice(false, "The board settings are not read yet"); return false; }
    send_other_params(manual, true);
    return true;
}

bool Client::refresh_autoadd()
{
    if (!ready()) return false;
    Request r;
    r.payload = build_get_autoadd_config();
    r.accept = {resp::kAutoaddConfig};
    r.done = [this](Outcome o, const Packet *p, int err) {
        if (o == Outcome::Aborted) return;
        BoardCaps caps = model_.caps();
        if (const auto *a = pkt_as<AutoaddConfig>(p)) {
            caps.autoadd = Cap::Yes;
            caps.autoadd_known = true;
            caps.autoadd_config = *a;
            log("autoadd: config 0x" + code_text(a->config).substr(2) + (a->has_max_hops ? ", max hops " + std::to_string(a->max_hops) : std::string(", no max hops byte")));
        } else if (o == Outcome::Error && (err == err::kUnsupported || err == -1)) {
            caps.autoadd = Cap::No;
            log("autoadd: the board does not know GET_AUTOADD_CONFIG (error " + std::to_string(err) + ")");
        } else {
            log("autoadd: " + answer_text(static_cast<int>(o), err));
            return;
        }
        model_.set_caps(caps);
    };
    enqueue(std::move(r));
    return true;
}

bool Client::set_autoadd_flags(uint8_t flags)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (model_.caps().autoadd == Cap::No) { set_notice(false, "Firmware too old for this feature: Auto-add contacts"); return false; }
    Request r;
    r.payload = build_set_autoadd_config(flags);
    r.accept = {resp::kOk};
    r.done = [this, flags](Outcome o, const Packet *, int err) {
        log("autoadd set 0x" + code_text(flags).substr(2) + ": " + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) {
            set_notice(true, "Auto-add types saved on the board");
            refresh_autoadd();
        } else if (o != Outcome::Aborted) {
            set_notice(false, "Not saved: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

/* ---- the zero-hop discover */

bool Client::start_discover(uint8_t type_filter)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (discover_.active) { set_notice(true, "A scan is already running"); return false; }
    if (model_.caps().discover == Cap::No) { set_notice(false, "Firmware too old for this feature: Discover nearby nodes"); return false; }
    // a tag the answers will carry back: any non-zero number that differs from scan to scan
    tag_counter_ += 0x9E3779B9u;
    uint32_t tag = static_cast<uint32_t>(now_) * 2654435761u ^ tag_counter_;
    if (tag == 0) tag = 1;
    discover_.tag = tag;
    for (int i = 0; i < 4; ++i) discover_tag_bytes_[static_cast<size_t>(i)] = static_cast<uint8_t>(tag >> (8 * i));
    discover_.active = true;
    discover_.found = 0;
    discover_.until_ms = now_ + 5000 + kDiscoverWindowMs;          // the window starts when the board has accepted the request
    Request r;
    r.payload = build_discover_request(type_filter, tag, false);   // full keys, so a node found can be added
    r.accept_any = true;                                           // OK or whatever non-error answer the firmware gives
    r.done = [this](Outcome o, const Packet *, int err) {
        log("discover request: " + answer_text(static_cast<int>(o), err) + (o == Outcome::Ok ? " (code " + code_text(last_code_) + ")" : std::string()));
        if (o == Outcome::Ok) {
            BoardCaps caps = model_.caps();
            caps.discover = Cap::Yes;
            model_.set_caps(caps);
            discover_.until_ms = now_ + kDiscoverWindowMs;
            set_notice(true, "Scanning for nodes in direct range...");
            return;
        }
        discover_.active = false;
        if (o == Outcome::Aborted) return;
        if (o == Outcome::Error && (err == err::kUnsupported || err == -1)) {
            BoardCaps caps = model_.caps();
            caps.discover = Cap::No;
            model_.set_caps(caps);
            set_notice(false, "Firmware too old for this feature: Discover nearby nodes");
        } else {
            set_notice(false, "Scan failed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

void Client::tick_discover()
{
    if (!discover_.active || now_ < discover_.until_ms) return;
    discover_.active = false;
    ++discover_.finished_id;
    log("discover: " + std::to_string(discover_.found) + " nodes answered");
    set_notice(true, discover_.found == 0 ? "Scan finished: no node answered" : "Scan finished: " + std::to_string(discover_.found) +
                                                                                      (discover_.found == 1 ? " node answered" : " nodes answered"));
}

/* ---- the scheduled advert (D2): the board has no advert interval, the app sends the advert itself */

void Client::tick_schedule()
{
    const AdvertSchedule s = model_.advert_schedule();
    if (!(s == sched_seen_)) {                                    // changed by the user: the first advert comes one interval from now
        sched_seen_ = s;
        next_advert_ms_ = s.interval_hours > 0 ? now_ + static_cast<uint64_t>(s.interval_hours) * 3600000ull : 0;
        return;
    }
    if (s.interval_hours <= 0) return;
    if (next_advert_ms_ == 0) next_advert_ms_ = now_ + static_cast<uint64_t>(s.interval_hours) * 3600000ull;
    if (now_ < next_advert_ms_) return;
    next_advert_ms_ = now_ + static_cast<uint64_t>(s.interval_hours) * 3600000ull;
    send_advert_impl(s.flood, true);
}

/* ================================================================== phase 2: the board */

bool Client::reboot()
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    Request r;
    r.payload = build_reboot();
    r.no_reply = true;
    r.done = [this](Outcome o, const Packet *, int) {
        if (o == Outcome::Ok) set_notice(true, "Reboot sent: the board restarts and the link comes back by itself");
        else if (o != Outcome::Aborted) set_notice(false, "The reboot command could not be sent");
    };
    log("reboot requested");
    enqueue(std::move(r), true);
    return true;
}

bool Client::factory_reset()
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    Request r;
    r.payload = build_factory_reset();
    r.accept_any = true;                                      // whatever the board answers is logged; an error frame still ends it as an error
    r.timeout_ms = kFactoryResetWaitMs;                       // the firmware formats its file system before it answers
    reset_phase_ = ResetPhase::Waiting;
    reset_until_ms_ = now_ + 10 * 60 * 1000;
    reset_prev_key_ = model_.self() ? model_.self()->key : PubKey{};
    r.done = [this](Outcome o, const Packet *, int err) {
        if (o == Outcome::Ok) {
            log("factory reset: the board answered with code " + code_text(last_code_) + ", first bytes " + last_head_ +
                " (OK is 0x00); it restarts by itself in about a second");
            set_notice(true, "Board reset: it restarts by itself");
        } else if (o == Outcome::Error) {
            reset_phase_ = ResetPhase::None;
            log("factory reset refused: the board answered with code " + code_text(last_code_) + ", first bytes " + last_head_ + " (error " + std::to_string(err) + ")");
            set_notice(false, err == err::kUnsupported ? "Firmware too old for this feature: Factory reset" : "Factory reset refused: " + error_text(err));
        } else if (o == Outcome::Timeout) {
            log("factory reset: no answer within " + std::to_string(kFactoryResetWaitMs / 1000) + " s; if the board restarts by itself the identity it reports decides");
            set_notice(false, "No answer: if the board restarts, it was reset");
        } else {
            log("factory reset: the link went away while waiting for the answer (the board is restarting); the identity it reports when it is back decides");
            set_notice(true, "Board reset: it restarts by itself");
        }
    };
    const Bytes wire = encode_frame(r.payload);
    set_notice(true, "Erasing the board, this can take a minute...");
    log("factory reset requested: frame " + to_hex(r.payload) + " (" + std::to_string(r.payload.size()) + " bytes), on the wire " + to_hex(wire) +
        ", waiting up to " + std::to_string(kFactoryResetWaitMs / 1000) + " s for the answer");
    enqueue(std::move(r), true);
    return true;
}

void Client::read_device_info()
{
    Request r;
    r.payload = build_device_query();
    r.accept = {resp::kDeviceInfo};
    r.done = [](Outcome, const Packet *, int) {};
    enqueue(std::move(r), true);
}

void Client::read_repeat_ranges()
{
    if (!ready()) return;
    const DeviceInfo *dev = model_.device() ? &*model_.device() : nullptr;
    if (!dev || dev->fw_ver < feature_min_level(Feature::ClientRepeat)) return;
    Request r;
    r.payload = build_get_allowed_repeat_freq();
    r.accept = {resp::kAllowedRepeatFreq};
    r.done = [this](Outcome o, const Packet *p, int err) {
        if (o == Outcome::Aborted) return;
        BoardCaps caps = model_.caps();
        if (const auto *a = pkt_as<AllowedRepeatFreq>(p)) {
            caps.repeat_freqs = Cap::Yes;
            caps.repeat_ranges = a->ranges;
            std::string raw;
            for (const RepeatRange &r : a->ranges) raw += (raw.empty() ? "" : ", ") + std::to_string(r.lo_khz) + "-" + std::to_string(r.hi_khz);
            log("repeat: allowed frequency ranges as the board sent them (read as kHz): " + (raw.empty() ? std::string("none") : raw));
        } else if (o == Outcome::Error && (err == err::kUnsupported || err == -1)) {
            caps.repeat_freqs = Cap::No;
            log("repeat: the board does not know GET_ALLOWED_REPEAT_FREQ (error " + std::to_string(err) + ")");
        } else {
            log("repeat: allowed frequencies: " + answer_text(static_cast<int>(o), err));
            return;
        }
        model_.set_caps(caps);
    };
    enqueue(std::move(r));
}

bool Client::set_path_hash_mode(int mode)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    if (mode < 0 || mode > 2) { set_notice(false, "The path hash size is 1, 2 or 3 bytes"); return false; }
    const FeatureState fs = feature_state(Feature::PathHashMode, model_.device() ? &*model_.device() : nullptr, model_.caps());
    if (!fs.available) { set_notice(false, fs.notice.empty() ? "Path hash size is not available" : fs.notice); return false; }
    Request r;
    r.payload = build_set_path_hash_mode(mode);
    r.accept = {resp::kOk};
    r.done = [this, mode](Outcome o, const Packet *, int err) {
        log("path hash mode " + std::to_string(mode) + ": " + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) {
            set_notice(true, std::string("Path hash size set to ") + path_hash_text(mode));
            read_device_info();                                  // what the board reports now is what the screen shows
        } else if (o != Outcome::Aborted) {
            set_notice(false, o == Outcome::Error && err == err::kUnsupported ? "Firmware too old for this feature: Path hash size"
                                                                                : "Not changed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::set_client_repeat(bool on)
{
    if (!ready()) { set_notice(false, "Not connected to a radio"); return false; }
    const SelfInfo *s = model_.self() ? &*model_.self() : nullptr;
    const RepeatState rs = repeat_state(model_.device() ? &*model_.device() : nullptr, model_.caps(), s ? s->freq_mhz() : 0);
    if (!s || !rs.available) { set_notice(false, rs.note.empty() ? "Repeat is not available" : rs.note); return false; }
    Request r;
    r.payload = build_set_radio(s->freq_mhz(), s->bw_khz(), s->sf, s->cr, on ? 1 : 0);
    r.accept = {resp::kOk};
    r.done = [this, on](Outcome o, const Packet *, int err) {
        log(std::string("repeat ") + (on ? "on" : "off") + ": " + answer_text(static_cast<int>(o), err));
        if (o == Outcome::Ok) {
            set_notice(true, on ? "Repeat is on: this board now forwards the packets it hears" : "Repeat is off");
            read_device_info();
            refresh_self();
        } else if (o != Outcome::Aborted) {
            set_notice(false, "Not changed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
        }
    };
    enqueue(std::move(r), true);
    return true;
}

bool Client::refresh_stats()
{
    if (!ready()) return false;
    if (stats_pending_ > 0) return false;
    if (model_.caps().stats == Cap::No) return false;
    stats_pending_ = 3;
    for (uint8_t sub = 0; sub < 3; ++sub) {
        Request r;
        r.payload = build_get_stats(sub);
        r.accept = {resp::kStats};
        r.done = [this, sub](Outcome o, const Packet *, int err) {
            if (o != Outcome::Ok || model_.caps().stats != Cap::Yes) log("stats " + std::to_string(sub) + ": " + answer_text(static_cast<int>(o), err));
            if (stats_pending_ > 0) --stats_pending_;
            if (o == Outcome::Aborted) { stats_pending_ = 0; return; }
            BoardCaps caps = model_.caps();
            if (o == Outcome::Ok) {
                if (caps.stats != Cap::Yes) { caps.stats = Cap::Yes; model_.set_caps(caps); }
            } else if (o == Outcome::Error && (err == err::kUnsupported || err == -1) && sub == 0) {
                caps.stats = Cap::No;
                model_.set_caps(caps);
                stats_pending_ = 0;
            }
        };
        enqueue(std::move(r));
    }
    return true;
}

bool Client::refresh_self()
{
    if (!ready()) return false;
    Request r;
    r.payload = build_app_start(app_name());
    r.accept = {resp::kSelfInfo};
    r.done = [](Outcome, const Packet *, int) {};
    enqueue(std::move(r), true);
    return true;
}

} // namespace meshzero
