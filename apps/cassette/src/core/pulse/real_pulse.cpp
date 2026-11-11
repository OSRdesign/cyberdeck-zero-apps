/*
 * SPDX-License-Identifier: MIT
 *
 * The production PulseApi: libpulse.so.0 through the table of pulse_lib.hpp (loaded at the first connect: no link-time
 * dependency, no pulseaudio header). One pa_threaded_mainloop per connection: the server's callbacks run on its thread
 * with its lock held; every call from the owner takes that lock, and waits for the server in 1 ms naps with the lock
 * dropped (bounded by a timeout: a hung server never hangs the app). The streams' data paths (the read callback, the
 * playback pump) only copy between libpulse and the stream's ring.
 * Not exercised by the PC unit tests (they use the fake): tests/pulse/real_pulse_test.cpp and tools/pulse_capture_test.cpp
 * run it against a real server (WSLg's PulseAudio on the build PC, PipeWire on the boards).
 */
#include "pulse_api.hpp"
#include "pulse_lib.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace cassette {

namespace {

double steady_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void nap_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

pa::BufferAttr to_pa(const PulseBufferAttr &a) { return pa::BufferAttr{a.maxlength, a.tlength, a.prebuf, a.minreq, a.fragsize}; }

PulseBufferAttr from_pa(const pa::BufferAttr *a, const PulseBufferAttr &fallback) {
    if (!a) return fallback;
    PulseBufferAttr b;
    b.maxlength = a->maxlength;
    b.tlength = a->tlength;
    b.prebuf = a->prebuf;
    b.minreq = a->minreq;
    b.fragsize = a->fragsize;
    return b;
}

PulseDeviceState state_of(int s) {
    switch (s) {
    case pa::kDeviceRunning: return PulseDeviceState::Running;
    case pa::kDeviceIdle: return PulseDeviceState::Idle;
    case pa::kDeviceSuspended: return PulseDeviceState::Suspended;
    default: return PulseDeviceState::Unknown;
    }
}

std::string str(const char *s) { return s ? s : ""; }

std::string prop(const PulseLib *L, const pa::Proplist *p, const char *key) {
    return p ? str(L->proplist_gets(p, key)) : std::string();
}

std::string first_of(const std::string &a, const std::string &b, const std::string &c = std::string()) {
    return !a.empty() ? a : (!b.empty() ? b : c);
}

std::string mac_of(const PulseLib *L, const pa::Proplist *p) {
    std::string m = pulse_parse_mac(prop(L, p, "api.bluez5.address"));
    if (m.empty()) m = pulse_parse_mac(prop(L, p, "device.string"));
    if (m.empty()) m = pulse_parse_mac(prop(L, p, "bluez5.address"));
    return m;
}

void read_props(const PulseLib *L, const pa::Proplist *p, PulseDevice &d) {
    d.bt_address = mac_of(L, p);
    d.form_factor = first_of(prop(L, p, "device.form-factor"), prop(L, p, "device.form_factor"));
    d.bus = prop(L, p, "device.bus");
    d.bt_profile = first_of(prop(L, p, "api.bluez5.profile"), prop(L, p, "bluetooth.protocol"));
    d.bt_codec = first_of(prop(L, p, "api.bluez5.codec"), prop(L, p, "bluetooth.codec"), prop(L, p, "bluez5.codec"));
    d.device_description = prop(L, p, "device.description");
    d.device_api = prop(L, p, "device.api");
}

pa::Proplist *make_props(const PulseLib *L, const std::vector<std::pair<std::string, std::string>> &kv) {
    pa::Proplist *p = L->proplist_new();
    if (!p) return nullptr;
    for (const auto &e : kv) L->proplist_sets(p, e.first.c_str(), e.second.c_str());
    return p;
}

/* One connection to the server. Shared by the RealPulse and every stream link: a stream that outlives disconnect() keeps
 * the (dead) context and the mainloop until it is closed. No callback ever holds a reference, so the destructor never runs
 * on the mainloop thread. */
struct Conn {
    const PulseLib *L = nullptr;
    pa::ThreadedMainloop *ml = nullptr;
    pa::Context *ctx = nullptr;
    bool started = false;
    bool dead = false;            /* under the lock: the context failed or was disconnected */
    PulseChangeCallback on_change; /* under the lock */

    ~Conn() {
        if (started) L->threaded_mainloop_stop(ml);
        if (ctx) {
            L->context_set_state_callback(ctx, nullptr, nullptr);
            L->context_set_subscribe_callback(ctx, nullptr, nullptr);
            if (L->context_get_state(ctx) == pa::kContextReady) L->context_disconnect(ctx);
            L->context_unref(ctx);
        }
        if (ml) L->threaded_mainloop_free(ml);
    }
    void lock() { L->threaded_mainloop_lock(ml); }
    void unlock() { L->threaded_mainloop_unlock(ml); }
    bool ready() const { return ctx && L->context_get_state(ctx) == pa::kContextReady; }
    int err() const { return ctx ? L->context_errno(ctx) : pa::kErrConnectionTerminated; }
    std::string err_text() const { return str(L->strerror(err())); }

    /* With the lock held: waits until done() (the lock dropped during 1 ms naps); false on timeout. */
    template <typename F> bool wait_locked(F done, int timeout_ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
        while (!done()) {
            if (std::chrono::steady_clock::now() >= end) return done();
            unlock();
            nap_ms(1);
            lock();
        }
        return true;
    }

    /* With the lock held: runs an operation to its end. 0, kPulseTimeout, kPulseDisconnected, or the mapped error. */
    int run_op(pa::Operation *o, int timeout_ms) {
        if (!o) return ready() ? pulse_error_from_pa(err()) : kPulseDisconnected;
        wait_locked([&] { return L->operation_get_state(o) != pa::kOperationRunning || !ready(); }, timeout_ms);
        const int st = L->operation_get_state(o);
        if (st == pa::kOperationRunning) L->operation_cancel(o); /* the callback is not called after this */
        L->operation_unref(o);
        if (st == pa::kOperationDone) return 0;
        if (!ready()) return kPulseDisconnected;
        return st == pa::kOperationRunning ? kPulseTimeout : kPulseFailed;
    }
};

constexpr int kOpTimeoutMs = 3000; /* one introspection round trip */

struct LockGuard {
    Conn &c;
    explicit LockGuard(Conn &conn) : c(conn) { c.lock(); }
    ~LockGuard() { c.unlock(); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;
};

/* ---- introspection callbacks (userdata: a struct on the caller's stack; run_op cancels before it returns) */

struct DeviceList {
    const PulseLib *L;
    std::vector<PulseDevice> *out;
};

void source_cb(pa::Context *, const pa::SourceInfo *i, int eol, void *ud) {
    if (eol || !i) return;
    auto *d = static_cast<DeviceList *>(ud);
    PulseDevice v;
    v.index = i->index;
    v.name = str(i->name);
    v.description = str(i->description);
    v.rate = i->sample_spec.rate;
    v.channels = i->sample_spec.channels;
    v.format = pulse_format_name(i->sample_spec.format);
    v.is_monitor = i->monitor_of_sink != pa::kInvalidIndex;
    v.monitor_of = str(i->monitor_of_sink_name);
    v.card = i->card;
    v.state = state_of(i->state);
    read_props(d->L, i->proplist, v);
    d->out->push_back(v);
}

void sink_cb(pa::Context *, const pa::SinkInfo *i, int eol, void *ud) {
    if (eol || !i) return;
    auto *d = static_cast<DeviceList *>(ud);
    PulseDevice v;
    v.index = i->index;
    v.name = str(i->name);
    v.description = str(i->description);
    v.rate = i->sample_spec.rate;
    v.channels = i->sample_spec.channels;
    v.format = pulse_format_name(i->sample_spec.format);
    v.card = i->card;
    v.state = state_of(i->state);
    read_props(d->L, i->proplist, v);
    d->out->push_back(v);
}

struct CardList {
    const PulseLib *L;
    std::vector<PulseCard> *out;
};

void card_cb(pa::Context *, const pa::CardInfo *i, int eol, void *ud) {
    if (eol || !i) return;
    auto *d = static_cast<CardList *>(ud);
    const PulseLib *L = d->L;
    PulseCard c;
    c.index = i->index;
    c.name = str(i->name);
    c.driver = str(i->driver);
    c.description = prop(L, i->proplist, "device.description");
    c.bt_address = mac_of(L, i->proplist);
    c.bus = prop(L, i->proplist, "device.bus");
    c.form_factor = first_of(prop(L, i->proplist, "device.form-factor"), prop(L, i->proplist, "device.form_factor"));
    c.device_api = prop(L, i->proplist, "device.api");
    if (i->profiles2) {
        for (pa::CardProfileInfo2 **p = i->profiles2; *p; ++p) {
            PulseProfile pr;
            pr.name = str((*p)->name);
            pr.description = str((*p)->description);
            pr.available = (*p)->available != 0;
            pr.priority = (*p)->priority;
            pr.n_sinks = (*p)->n_sinks;
            pr.n_sources = (*p)->n_sources;
            c.profiles.push_back(pr);
        }
    }
    if (i->active_profile2) c.active_profile = str(i->active_profile2->name);
    d->out->push_back(c);
}

struct ServerOut {
    PulseServerInfo *out;
    bool got = false;
};

void server_cb(pa::Context *, const pa::ServerInfo *i, void *ud) {
    auto *s = static_cast<ServerOut *>(ud);
    if (!i) return;
    s->got = true;
    s->out->server_name = str(i->server_name);
    s->out->server_version = str(i->server_version);
    s->out->default_source = str(i->default_source_name);
    s->out->default_sink = str(i->default_sink_name);
    s->out->rate = i->sample_spec.rate;
    s->out->on_pipewire = s->out->server_name.find("PipeWire") != std::string::npos;
}

struct IndexOut {
    uint32_t index = pa::kInvalidIndex;
};

void source_output_cb(pa::Context *, const pa::SourceOutputInfoHead *i, int eol, void *ud) {
    if (eol || !i) return;
    static_cast<IndexOut *>(ud)->index = i->source;
}

void sink_input_cb(pa::Context *, const pa::SinkInputInfoHead *i, int eol, void *ud) {
    if (eol || !i) return;
    static_cast<IndexOut *>(ud)->index = i->sink;
}

struct NameOut {
    std::string name;
};

void source_name_cb(pa::Context *, const pa::SourceInfo *i, int eol, void *ud) {
    if (eol || !i) return;
    static_cast<NameOut *>(ud)->name = str(i->name);
}

void sink_name_cb(pa::Context *, const pa::SinkInfo *i, int eol, void *ud) {
    if (eol || !i) return;
    static_cast<NameOut *>(ud)->name = str(i->name);
}

/* ---- context callbacks (userdata: the Conn) */

void ctx_state_cb(pa::Context *ctx, void *ud) {
    auto *c = static_cast<Conn *>(ud);
    const int st = c->L->context_get_state(ctx);
    if (st != pa::kContextFailed && st != pa::kContextTerminated) return;
    if (c->dead) return;
    c->dead = true;
    if (c->on_change) {
        PulseChange ch;
        ch.what = PulseChange::What::Server;
        ch.kind = PulseChange::Kind::Disconnected;
        c->on_change(ch);
    }
}

void subscribe_cb(pa::Context *, int t, uint32_t idx, void *ud) {
    auto *c = static_cast<Conn *>(ud);
    if (!c->on_change) return;
    PulseChange ch;
    ch.index = idx;
    switch (t & pa::kEventFacilityMask) {
    case pa::kEventSource: ch.what = PulseChange::What::Source; break;
    case pa::kEventSink: ch.what = PulseChange::What::Sink; break;
    case pa::kEventCard: ch.what = PulseChange::What::Card; break;
    case pa::kEventServer: ch.what = PulseChange::What::Server; break;
    default: return;
    }
    switch (t & pa::kEventTypeMask) {
    case pa::kEventNew: ch.kind = PulseChange::Kind::New; break;
    case pa::kEventRemove: ch.kind = PulseChange::Kind::Remove; break;
    default: ch.kind = PulseChange::Kind::Change; break;
    }
    c->on_change(ch);
}

/* ---- streams */

/* The common part of both links: the stream object, the device-name query (the server's own view), latency, close. */
class LinkBase : public PulseStreamLink {
public:
    LinkBase(std::shared_ptr<Conn> c, bool capture) : c_(std::move(c)), capture_(capture) {}
    ~LinkBase() override { close(); }

    std::string device_name() override {
        LockGuard g(*c_);
        if (!s_) return "";
        const PulseLib *L = c_->L;
        std::string name;
        const uint32_t idx = L->stream_get_index(s_);
        if (idx != pa::kInvalidIndex && c_->ready()) {
            IndexOut io;
            pa::Operation *o = capture_ ? L->context_get_source_output_info(c_->ctx, idx, source_output_cb, &io)
                                        : L->context_get_sink_input_info(c_->ctx, idx, sink_input_cb, &io);
            if (c_->run_op(o, kOpTimeoutMs) == 0 && io.index != pa::kInvalidIndex && s_) {
                NameOut no;
                o = capture_ ? L->context_get_source_info_by_index(c_->ctx, io.index, source_name_cb, &no)
                             : L->context_get_sink_info_by_index(c_->ctx, io.index, sink_name_cb, &no);
                if (c_->run_op(o, kOpTimeoutMs) == 0) name = no.name;
            }
        }
        if (name.empty() && s_ && L->stream_get_state(s_) == pa::kStreamReady) name = str(L->stream_get_device_name(s_));
        return name;
    }

    int latency(PulseLatency &out) override {
        LockGuard g(*c_);
        out = PulseLatency();
        if (!s_ || c_->L->stream_get_state(s_) != pa::kStreamReady) return kPulseState;
        pa::usec_t us = 0;
        int neg = 0;
        const int r = c_->L->stream_get_latency(s_, &us, &neg);
        if (r < 0) return r == -pa::kErrNoData ? kPulseTimeout : pulse_error_from_pa(-r);
        out.total_ms = neg ? 0.0 : us / 1000.0;
        if (const pa::TimingInfo *ti = c_->L->stream_get_timing_info(s_)) {
            out.timing_info = true;
            out.device_ms = (capture_ ? ti->source_usec : ti->sink_usec) / 1000.0;
            out.transport_ms = ti->transport_usec / 1000.0;
        }
        return 0;
    }

    void close() override {
        LockGuard g(*c_);
        close_locked();
    }

    /* With the lock held: the callbacks off first (no feed after this), then disconnect and release. */
    void close_locked() {
        if (!s_) return;
        const PulseLib *L = c_->L;
        L->stream_set_state_callback(s_, nullptr, nullptr);
        L->stream_set_read_callback(s_, nullptr, nullptr);
        L->stream_set_write_callback(s_, nullptr, nullptr);
        L->stream_set_moved_callback(s_, nullptr, nullptr);
        L->stream_set_suspended_callback(s_, nullptr, nullptr);
        L->stream_set_underflow_callback(s_, nullptr, nullptr);
        const int st = L->stream_get_state(s_);
        if (st == pa::kStreamCreating || st == pa::kStreamReady) L->stream_disconnect(s_);
        L->stream_unref(s_);
        s_ = nullptr;
    }

    Conn &conn() { return *c_; }
    pa::Stream *stream() const { return s_; }
    void set_stream(pa::Stream *s) { s_ = s; }

protected:
    std::shared_ptr<Conn> c_;
    bool capture_;
    pa::Stream *s_ = nullptr;
};

class CaptureLink final : public LinkBase {
public:
    CaptureLink(std::shared_ptr<Conn> c, PulseCapture *cap, unsigned channels)
        : LinkBase(std::move(c), true), cap_(cap), fb_(2u * (channels ? channels : 1u)) {}

    static void state_cb(pa::Stream *s, void *ud) {
        auto *k = static_cast<CaptureLink *>(ud);
        const PulseLib *L = k->c_->L;
        switch (L->stream_get_state(s)) {
        case pa::kStreamReady: {
            const PulseBufferAttr g = from_pa(L->stream_get_buffer_attr(s), k->cap_->format().requested);
            if (!k->cap_->feed_ready(str(L->stream_get_device_name(s)), g)) L->stream_disconnect(s);
            break;
        }
        case pa::kStreamFailed: k->cap_->feed_end(pulse_error_from_pa(L->context_errno(k->c_->ctx))); break;
        case pa::kStreamTerminated: k->cap_->feed_end(kPulseDisconnected); break; /* not by us: close() unhooks first */
        default: break;
        }
    }

    static void read_cb(pa::Stream *s, size_t, void *ud) {
        auto *k = static_cast<CaptureLink *>(ud);
        const PulseLib *L = k->c_->L;
        for (int guard = 0; guard < 256; ++guard) {
            const void *data = nullptr;
            size_t nb = 0;
            if (L->stream_peek(s, &data, &nb) < 0 || nb == 0) return; /* empty: nothing to drop */
            if (data) k->cap_->feed_frames(data, nb / k->fb_);
            else k->cap_->feed_hole(nb / k->fb_); /* a hole: the server lost this much */
            L->stream_drop(s);
        }
    }

    static void moved_cb(pa::Stream *s, void *ud) {
        auto *k = static_cast<CaptureLink *>(ud);
        if (!k->cap_->feed_moved(str(k->c_->L->stream_get_device_name(s)))) k->c_->L->stream_disconnect(s);
    }

    static void suspended_cb(pa::Stream *s, void *ud) {
        auto *k = static_cast<CaptureLink *>(ud);
        k->cap_->feed_suspended(k->c_->L->stream_is_suspended(s) > 0);
    }

private:
    PulseCapture *cap_;
    size_t fb_;
};

class PlaybackLink final : public LinkBase {
public:
    PlaybackLink(std::shared_ptr<Conn> c, PulsePlayback *pb, unsigned channels, size_t scratch_frames)
        : LinkBase(std::move(c), false), pb_(pb), fb_(2u * (channels ? channels : 1u)),
          scratch_(std::max<size_t>(scratch_frames, 256) * fb_) {}

    void kick() override {
        LockGuard g(*c_);
        pump_locked();
    }

    void flush() override {
        LockGuard g(*c_);
        pb_->discard_queued();
        if (s_ && c_->L->stream_get_state(s_) == pa::kStreamReady) {
            if (pa::Operation *o = c_->L->stream_flush(s_, nullptr, nullptr)) c_->L->operation_unref(o);
        }
    }

    int drain(int timeout_ms) override {
        LockGuard g(*c_);
        const auto start = std::chrono::steady_clock::now();
        /* the ring first: pumped as the server makes room */
        c_->wait_locked(
            [&] {
                pump_locked();
                return pb_->queued_frames() == 0 || !s_ || c_->L->stream_get_state(s_) != pa::kStreamReady;
            },
            timeout_ms);
        if (!s_ || c_->L->stream_get_state(s_) != pa::kStreamReady) return kPulseState;
        if (pb_->queued_frames() > 0) return kPulseTimeout;
        const long used = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        const int left = timeout_ms - static_cast<int>(used);
        return c_->run_op(c_->L->stream_drain(s_, nullptr, nullptr), left > 0 ? left : 0);
    }

    /* With the lock held: hands the server as much queued audio as it takes (pa_stream_write copies it). */
    void pump_locked() {
        if (!s_) return;
        const PulseLib *L = c_->L;
        if (L->stream_get_state(s_) != pa::kStreamReady) return;
        for (int guard = 0; guard < 64; ++guard) {
            const size_t want = L->stream_writable_size(s_);
            if (want == static_cast<size_t>(-1) || want < fb_) return;
            const size_t frames = std::min(want, scratch_.size()) / fb_;
            const size_t got = pb_->take(scratch_.data(), frames);
            if (got == 0) return;
            if (L->stream_write(s_, scratch_.data(), got * fb_, nullptr, 0, pa::kSeekRelative) < 0) return;
        }
    }

    static void state_cb(pa::Stream *s, void *ud) {
        auto *k = static_cast<PlaybackLink *>(ud);
        const PulseLib *L = k->c_->L;
        switch (L->stream_get_state(s)) {
        case pa::kStreamReady: {
            const PulseBufferAttr g = from_pa(L->stream_get_buffer_attr(s), k->pb_->format().requested);
            if (!k->pb_->feed_ready(str(L->stream_get_device_name(s)), g)) L->stream_disconnect(s);
            else k->pump_locked();
            break;
        }
        case pa::kStreamFailed: k->pb_->feed_end(pulse_error_from_pa(L->context_errno(k->c_->ctx))); break;
        case pa::kStreamTerminated: k->pb_->feed_end(kPulseDisconnected); break;
        default: break;
        }
    }

    static void write_cb(pa::Stream *, size_t, void *ud) { static_cast<PlaybackLink *>(ud)->pump_locked(); }

    static void underflow_cb(pa::Stream *, void *ud) { static_cast<PlaybackLink *>(ud)->pb_->feed_underflow(); }

    static void moved_cb(pa::Stream *s, void *ud) {
        auto *k = static_cast<PlaybackLink *>(ud);
        if (!k->pb_->feed_moved(str(k->c_->L->stream_get_device_name(s)))) k->c_->L->stream_disconnect(s);
    }

    static void suspended_cb(pa::Stream *s, void *ud) {
        auto *k = static_cast<PlaybackLink *>(ud);
        k->pb_->feed_suspended(k->c_->L->stream_is_suspended(s) > 0);
    }

private:
    PulsePlayback *pb_;
    size_t fb_;
    std::vector<unsigned char> scratch_;
};

class RealPulse final : public PulseApi {
public:
    ~RealPulse() override { disconnect(); }

    int connect(const std::string &app_name, int timeout_ms) override {
        std::lock_guard<std::mutex> g(mu_);
        if (conn_) {
            LockGuard lg(*conn_);
            if (conn_->ready()) return 0;
        }
        conn_.reset();
        app_ = app_name.empty() ? std::string("Cassette") : app_name;
        std::string why;
        const PulseLib *L = real_pulse_lib(&why);
        if (!L) {
            err_ = why;
            return kPulseMissing;
        }
        auto c = std::make_shared<Conn>();
        c->L = L;
        c->ml = L->threaded_mainloop_new();
        if (!c->ml) {
            err_ = "cannot create the libpulse main loop";
            return kPulseFailed;
        }
        pa::Proplist *pl = make_props(L, {{"application.name", app_}, {"application.process.binary", "cassette"}});
        c->ctx = L->context_new_with_proplist(L->threaded_mainloop_get_api(c->ml), app_.c_str(), pl);
        if (pl) L->proplist_free(pl);
        if (!c->ctx) {
            err_ = "cannot create the libpulse context";
            return kPulseFailed;
        }
        L->context_set_state_callback(c->ctx, ctx_state_cb, c.get());
        /* NOAUTOSPAWN: never start a PulseAudio daemon (an installed but stopped one would take Bluetooth from PipeWire) */
        if (L->context_connect(c->ctx, nullptr, pa::kContextNoAutospawn, nullptr) < 0) {
            const int e = c->err();
            err_ = c->err_text();
            return no_server(e) ? kPulseNoServer : pulse_error_from_pa(e);
        }
        if (L->threaded_mainloop_start(c->ml) < 0) {
            err_ = "cannot start the libpulse main loop";
            return kPulseFailed;
        }
        c->started = true;
        c->lock();
        const bool done = c->wait_locked(
            [&] {
                const int st = L->context_get_state(c->ctx);
                return st == pa::kContextReady || st == pa::kContextFailed || st == pa::kContextTerminated;
            },
            timeout_ms);
        if (!done) {
            c->unlock();
            err_ = "the sound server did not answer in time";
            return kPulseTimeout;
        }
        if (!c->ready()) {
            const int e = c->err();
            err_ = c->err_text();
            c->unlock();
            return no_server(e) ? kPulseNoServer : pulse_error_from_pa(e);
        }
        c->on_change = cb_;
        L->context_set_subscribe_callback(c->ctx, subscribe_cb, c.get());
        if (pa::Operation *o =
                L->context_subscribe(c->ctx, pa::kMaskSink | pa::kMaskSource | pa::kMaskCard | pa::kMaskServer, nullptr,
                                     nullptr))
            L->operation_unref(o);
        c->unlock();
        conn_ = c;
        err_.clear();
        return 0;
    }

    void disconnect() override {
        std::lock_guard<std::mutex> g(mu_);
        if (!conn_) return;
        {
            LockGuard lg(*conn_);
            conn_->on_change = nullptr; /* no callback after disconnect() */
            conn_->L->context_set_subscribe_callback(conn_->ctx, nullptr, nullptr);
            conn_->dead = true;
            /* the streams still open see TERMINATED: Killed(kPulseDisconnected) */
            if (conn_->ready()) conn_->L->context_disconnect(conn_->ctx);
        }
        conn_.reset(); /* the mainloop stops when the last stream releases it */
    }

    bool connected() override {
        std::lock_guard<std::mutex> g(mu_);
        if (!conn_) return false;
        LockGuard lg(*conn_);
        return conn_->ready();
    }

    std::string last_error() override {
        std::lock_guard<std::mutex> g(mu_);
        return err_;
    }

    int server_info(PulseServerInfo &out) override {
        std::lock_guard<std::mutex> g(mu_);
        out = PulseServerInfo();
        if (!conn_) return kPulseState;
        Conn &c = *conn_;
        out.library_version = c.L->version;
        LockGuard lg(c);
        if (!c.ready()) return kPulseDisconnected;
        ServerOut so{&out};
        int r = c.run_op(c.L->context_get_server_info(c.ctx, server_cb, &so), kOpTimeoutMs);
        if (r == 0 && !so.got) r = kPulseFailed;
        return r;
    }

    int list_sources(std::vector<PulseDevice> &out) override {
        std::lock_guard<std::mutex> g(mu_);
        out.clear();
        if (!conn_) return kPulseState;
        Conn &c = *conn_;
        LockGuard lg(c);
        if (!c.ready()) return kPulseDisconnected;
        DeviceList d{c.L, &out};
        return c.run_op(c.L->context_get_source_info_list(c.ctx, source_cb, &d), kOpTimeoutMs);
    }

    int list_sinks(std::vector<PulseDevice> &out) override {
        std::lock_guard<std::mutex> g(mu_);
        out.clear();
        if (!conn_) return kPulseState;
        Conn &c = *conn_;
        LockGuard lg(c);
        if (!c.ready()) return kPulseDisconnected;
        DeviceList d{c.L, &out};
        return c.run_op(c.L->context_get_sink_info_list(c.ctx, sink_cb, &d), kOpTimeoutMs);
    }

    int list_cards(std::vector<PulseCard> &out) override {
        std::lock_guard<std::mutex> g(mu_);
        out.clear();
        if (!conn_) return kPulseState;
        Conn &c = *conn_;
        LockGuard lg(c);
        if (!c.ready()) return kPulseDisconnected;
        CardList d{c.L, &out};
        return c.run_op(c.L->context_get_card_info_list(c.ctx, card_cb, &d), kOpTimeoutMs);
    }

    int card_by_name(const std::string &name, PulseCard &out) override {
        std::lock_guard<std::mutex> g(mu_);
        out = PulseCard();
        if (!conn_) return kPulseState;
        Conn &c = *conn_;
        LockGuard lg(c);
        if (!c.ready()) return kPulseDisconnected;
        std::vector<PulseCard> v;
        CardList d{c.L, &v};
        const int r = c.run_op(c.L->context_get_card_info_by_name(c.ctx, name.c_str(), card_cb, &d), kOpTimeoutMs);
        if (r == kPulseFailed || (r == 0 && v.empty())) return kPulseNoDevice;
        if (r) return r;
        out = v.front();
        return 0;
    }

    void set_change_callback(PulseChangeCallback cb) override {
        std::lock_guard<std::mutex> g(mu_);
        cb_ = cb;
        if (!conn_) return;
        LockGuard lg(*conn_); /* the callback runs under this lock: once we hold it, the old one is not running */
        conn_->on_change = std::move(cb);
    }

    int open_capture(const PulseCaptureRequest &req, std::unique_ptr<PulseCapture> &out, std::string *why) override {
        out.reset();
        std::lock_guard<std::mutex> g(mu_);
        if (!conn_) return fail(why, kPulseState, "not connected to the sound server");
        std::shared_ptr<Conn> cp = conn_;
        Conn &c = *cp;
        const PulseLib *L = c.L;
        c.lock();
        if (!c.ready()) {
            c.unlock();
            return fail(why, kPulseDisconnected, "the connection to the sound server was lost");
        }
        /* the exact source first: absent means no stream at all (study C16: never start on another device) */
        std::vector<PulseDevice> found;
        DeviceList dl{L, &found};
        int r = 0;
        if (!req.lookup_first && !req.source.empty()) {
            /* diagnostics: no lookup; the server alone decides what happens to this name */
            PulseDevice guess;
            guess.name = req.source;
            guess.rate = req.rate ? req.rate : 48000;
            guess.channels = req.channels ? req.channels : 1;
            found.assign(1, guess);
        } else {
            r = req.source.empty()
                    ? kPulseNoDevice
                    : c.run_op(L->context_get_source_info_by_name(c.ctx, req.source.c_str(), source_cb, &dl), kOpTimeoutMs);
            if (r == kPulseFailed || (r == 0 && found.empty())) r = kPulseNoDevice;
        }
        if (r) {
            c.unlock();
            return fail(why, r, "no source named \"" + req.source + "\"");
        }
        PulseStreamFormat fmt;
        std::string w;
        if (!pulse_capture_format(req, found.front(), fmt, &w)) {
            c.unlock();
            return fail(why, kPulseRefused, w);
        }
        std::unique_ptr<PulseCapture> cap(new PulseCapture(req, fmt, steady_s));
        CaptureLink *link = new CaptureLink(cp, cap.get(), fmt.channels);
        cap->attach(std::unique_ptr<PulseStreamLink>(link)); /* closes the stream if anything below fails */
        const pa::SampleSpec ss{pa::kSampleS16LE, fmt.rate, static_cast<uint8_t>(fmt.channels)};
        pa::Proplist *pl = make_props(L, pulse_stream_props(req.stream_name, req.role, app_));
        pa::Stream *s = L->stream_new_with_proplist(c.ctx, req.stream_name.c_str(), &ss, nullptr, pl);
        if (pl) L->proplist_free(pl);
        if (!s) {
            const int e = pulse_error_from_pa(c.err());
            const std::string t = c.err_text();
            c.unlock();
            return fail(why, e, "the sound server refused the stream: " + t);
        }
        link->set_stream(s);
        L->stream_set_state_callback(s, CaptureLink::state_cb, link);
        L->stream_set_read_callback(s, CaptureLink::read_cb, link);
        L->stream_set_moved_callback(s, CaptureLink::moved_cb, link);
        L->stream_set_suspended_callback(s, CaptureLink::suspended_cb, link);
        const pa::BufferAttr a = to_pa(fmt.requested);
        if (L->stream_connect_record(s, req.source.c_str(), &a, static_cast<int>(fmt.flags)) < 0) {
            const int e = pulse_error_from_pa(c.err());
            const std::string t = c.err_text();
            link->close_locked();
            c.unlock();
            return fail(why, e, "the sound server refused the stream: " + t);
        }
        c.wait_locked([&] { return L->stream_get_state(s) != pa::kStreamCreating; }, static_cast<int>(req.ready_timeout_ms));
        c.unlock();
        if (cap->ended()) return refuse(why, *cap, req.source);
        out = std::move(cap);
        return 0;
    }

    int open_playback(const PulsePlaybackRequest &req, std::unique_ptr<PulsePlayback> &out, std::string *why) override {
        out.reset();
        std::lock_guard<std::mutex> g(mu_);
        if (!conn_) return fail(why, kPulseState, "not connected to the sound server");
        std::shared_ptr<Conn> cp = conn_;
        Conn &c = *cp;
        const PulseLib *L = c.L;
        c.lock();
        if (!c.ready()) {
            c.unlock();
            return fail(why, kPulseDisconnected, "the connection to the sound server was lost");
        }
        std::vector<PulseDevice> found;
        DeviceList dl{L, &found};
        int r = req.sink.empty() ? kPulseNoDevice
                                 : c.run_op(L->context_get_sink_info_by_name(c.ctx, req.sink.c_str(), sink_cb, &dl),
                                            kOpTimeoutMs);
        if (r == kPulseFailed || (r == 0 && found.empty())) r = kPulseNoDevice;
        if (r) {
            c.unlock();
            return fail(why, r, "no sink named \"" + req.sink + "\"");
        }
        PulseStreamFormat fmt;
        std::string w;
        if (!pulse_playback_format(req, found.front(), fmt, &w)) {
            c.unlock();
            return fail(why, kPulseRefused, w);
        }
        std::unique_ptr<PulsePlayback> pb(new PulsePlayback(req, fmt, steady_s));
        const size_t scratch = pulse_bytes_to_frames(
            pulse_ms_to_bytes(std::max<uint32_t>(req.ring_ms ? req.ring_ms : req.buffer_ms, 10), fmt.rate, fmt.channels),
            fmt.channels);
        PlaybackLink *link = new PlaybackLink(cp, pb.get(), fmt.channels, scratch);
        pb->attach(std::unique_ptr<PulseStreamLink>(link));
        const pa::SampleSpec ss{pa::kSampleS16LE, fmt.rate, static_cast<uint8_t>(fmt.channels)};
        pa::Proplist *pl = make_props(L, pulse_stream_props(req.stream_name, req.role, app_));
        pa::Stream *s = L->stream_new_with_proplist(c.ctx, req.stream_name.c_str(), &ss, nullptr, pl);
        if (pl) L->proplist_free(pl);
        if (!s) {
            const int e = pulse_error_from_pa(c.err());
            const std::string t = c.err_text();
            c.unlock();
            return fail(why, e, "the sound server refused the stream: " + t);
        }
        link->set_stream(s);
        L->stream_set_state_callback(s, PlaybackLink::state_cb, link);
        L->stream_set_write_callback(s, PlaybackLink::write_cb, link);
        L->stream_set_underflow_callback(s, PlaybackLink::underflow_cb, link);
        L->stream_set_moved_callback(s, PlaybackLink::moved_cb, link);
        L->stream_set_suspended_callback(s, PlaybackLink::suspended_cb, link);
        const pa::BufferAttr a = to_pa(fmt.requested);
        if (L->stream_connect_playback(s, req.sink.c_str(), &a, static_cast<int>(fmt.flags), nullptr, nullptr) < 0) {
            const int e = pulse_error_from_pa(c.err());
            const std::string t = c.err_text();
            link->close_locked();
            c.unlock();
            return fail(why, e, "the sound server refused the stream: " + t);
        }
        c.wait_locked([&] { return L->stream_get_state(s) != pa::kStreamCreating; }, static_cast<int>(req.ready_timeout_ms));
        c.unlock();
        if (pb->ended()) {
            const int code = pb->end_code() ? pb->end_code() : kPulseFailed;
            PulseStreamEvent ev;
            std::string moved;
            while (pb->poll_event(ev))
                if (ev.kind == PulseEvent::Moved) moved = ev.device;
            pb->close();
            return fail(why, code,
                        code == kPulseMoved ? "the sound server attached the stream to \"" + moved + "\""
                                            : pulse_error_text(code));
        }
        out = std::move(pb);
        return 0;
    }

private:
    static bool no_server(int e) {
        return e == pa::kErrConnectionRefused || e == pa::kErrInvalidServer || e == pa::kErrConnectionTerminated;
    }

    static int fail(std::string *why, int code, const std::string &text) {
        if (why) *why = text;
        return code;
    }

    /* A capture stream that ended during the open: its end code, closed again. */
    static int refuse(std::string *why, PulseCapture &cap, const std::string &source) {
        const int code = cap.end_code() ? cap.end_code() : kPulseFailed;
        PulseStreamEvent ev;
        std::string moved;
        while (cap.poll_event(ev))
            if (ev.kind == PulseEvent::Moved) moved = ev.device;
        cap.close();
        if (code == kPulseMoved)
            return fail(why, code, "the sound server attached the stream to \"" + moved + "\", not \"" + source + "\"");
        return fail(why, code, pulse_error_text(code));
    }

    std::mutex mu_;
    std::shared_ptr<Conn> conn_;
    PulseChangeCallback cb_;
    std::string err_;
    std::string app_ = "Cassette";
};

}  // namespace

std::unique_ptr<PulseApi> make_real_pulse() { return std::unique_ptr<PulseApi>(new RealPulse()); }

}  // namespace cassette
