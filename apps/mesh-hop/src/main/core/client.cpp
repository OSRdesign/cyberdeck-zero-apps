/*
 * SPDX-License-Identifier: MIT
 */

#include "client.hpp"

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
    for (const Outgoing &o : outgoing_) model_.set_state(o.seq, MsgState::NoAck, "board disconnected");
    outgoing_.clear();
    chan_pending_.clear();
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
    start_clock_sync();
    Request b;
    b.payload = build_battery();
    b.accept = {resp::kBattery};
    b.done = [](Outcome, const Packet *, int) {};
    enqueue(std::move(b));
    refresh_contacts();
    const int max_channels = model_.device() && model_.device()->max_channels ? model_.device()->max_channels : 8;
    queue_channels(std::min(max_channels, 40));
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
    if (inflight_ || !transport_.is_open()) return;
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
    const auto parsed = parse_packet(frame);
    if (!parsed) { ++bad_frames_; return; }
    const Packet &p = *parsed;
    const uint8_t code = packet_code(frame);

    // ---- effects on the model, whatever command is in flight
    if (const auto *m = std::get_if<IncomingMessage>(&p)) {
        model_.add_incoming(*m, now_unix());
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
        if (ready() && !key_lookup_queued_ && normal_.size() < 8) {    // fetch the new name / position
            key_lookup_queued_ = true;
            Request r;
            r.payload = build_get_contact_by_key(adv->key);
            r.accept = {resp::kContact};
            r.done = [this](Outcome, const Packet *, int) { key_lookup_queued_ = false; };
            enqueue(std::move(r));
        }
    } else if (const auto *na = std::get_if<NewAdvert>(&p)) {
        model_.upsert_contact(na->contact);
        model_.heard(na->contact.key, now_unix());
    } else if (const auto *del = std::get_if<ContactDeleted>(&p)) {
        model_.remove_contact(del->key);
    } else if (std::get_if<ContactsFull>(&p)) {
        set_notice(false, "The radio's contact list is full");
    } else if (std::get_if<MessagesWaiting>(&p)) {
        want_sync_ = true;
        sync_burst_ = 0;
    } else if (const auto *ack = std::get_if<Ack>(&p)) {
        handle_ack(*ack);
    }

    // ---- diagnostics: what the board sends while a channel message waits for its answer
    if (inflight_ && (options_.log_frames || (!inflight_->payload.empty() && inflight_->payload[0] == cmd::kSendChannelTxt)))
        log("rx " + code_text(code) + " len=" + std::to_string(frame.size()) + " while waiting for command " +
            std::to_string(inflight_->payload.empty() ? 0 : inflight_->payload[0]));

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
    pump_queue();
    if (link_ == Link::NotCompanion) return;
    if (ready()) {
        tick_outgoing();
        tick_clock();
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
    Request r;
    r.payload = build_send_advert(flood);
    r.accept = {resp::kOk};
    r.done = [this, flood](Outcome o, const Packet *, int err) {
        if (o == Outcome::Ok) set_notice(true, flood ? "Advert sent (flood)" : "Advert sent (zero-hop)");
        else set_notice(false, "Advert failed: " + (o == Outcome::Error ? error_text(err) : std::string("no answer")));
    };
    enqueue(std::move(r), true);
    return true;
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
            // the stored route may be dead: fall back to flooding from this attempt on
            Request rp;
            rp.payload = build_reset_path(o.key);
            rp.accept = {resp::kOk};
            rp.done = [](Outcome, const Packet *, int) {};
            enqueue(std::move(rp), true);
        }
        transmit(o.seq);
    }
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

} // namespace meshzero
