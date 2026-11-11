/* SPDX-License-Identifier: MIT */
#include "fake_pulse.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace cassette {

namespace {

std::string underscored(std::string mac) {
    std::replace(mac.begin(), mac.end(), ':', '_');
    return mac;
}

}  // namespace

/* The server end of one fake stream: every call under the fake's lock; close() detaches it from the stream. */
class FakeLink final : public PulseStreamLink {
public:
    FakeLink(FakePulse *f, size_t i) : f_(f), i_(i) {}
    std::string device_name() override {
        std::lock_guard<std::recursive_mutex> g(f_->mu_);
        FakeStream *s = f_->at(i_);
        if (!s) return "";
        ++s->identity_queries;
        return s->attached;
    }
    int latency(PulseLatency &out) override {
        std::lock_guard<std::recursive_mutex> g(f_->mu_);
        FakeStream *s = f_->at(i_);
        if (!s || !s->open) return kPulseState;
        out.total_ms = s->latency_us / 1000.0;
        out.device_ms = out.total_ms / 2;
        out.transport_ms = 0.5;
        out.timing_info = true;
        return 0;
    }
    void flush() override {
        std::lock_guard<std::recursive_mutex> g(f_->mu_);
        FakeStream *s = f_->at(i_);
        if (s && s->play) s->play->discard_queued();
    }
    int drain(int) override {
        /* the fake server plays out everything at once */
        std::lock_guard<std::recursive_mutex> g(f_->mu_);
        FakeStream *s = f_->at(i_);
        if (!s || !s->play) return kPulseState;
        std::vector<unsigned char> buf(4096);
        const size_t per = buf.size() / (2u * (s->channels ? s->channels : 1u));
        for (;;) {
            const size_t got = s->play->take(buf.data(), per);
            if (got == 0) break;
            const int16_t *p = reinterpret_cast<const int16_t *>(buf.data());
            s->drained.insert(s->drained.end(), p, p + got * (s->channels ? s->channels : 1u));
        }
        return 0;
    }
    void close() override {
        std::lock_guard<std::recursive_mutex> g(f_->mu_);
        FakeStream *s = f_->at(i_);
        if (!s) return;
        s->open = false;
        s->cap = nullptr;
        s->play = nullptr;
    }

private:
    FakePulse *f_;
    size_t i_;
};

FakePulse::FakePulse() {
    server.server_name = "PulseAudio (on PipeWire 1.4.2)";
    server.server_version = "15.0.0";
    server.on_pipewire = true;
    server.rate = 48000;
    server.library_version = "fake";
}

FakePulse::~FakePulse() = default;

FakeStream *FakePulse::at(size_t i) { return i < streams.size() ? &streams[i] : nullptr; }

void FakePulse::notify(PulseChange::What what, PulseChange::Kind kind, uint32_t index) {
    if (!cb_) return;
    PulseChange ch;
    ch.what = what;
    ch.kind = kind;
    ch.index = index;
    ++callbacks;
    cb_(ch);
}

void FakePulse::nap() {
    const int d = call_delay_ms.load();
    if (d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
}

int FakePulse::connect(const std::string &, int) {
    nap();
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (lib_missing) {
        err_ = "libpulse not installed (libpulse0): libpulse.so.0: cannot open shared object file";
        return kPulseMissing;
    }
    if (server_down) {
        err_ = "Connection refused";
        return kPulseNoServer;
    }
    if (server_hangs) {
        err_ = "the sound server did not answer in time";
        return kPulseTimeout;
    }
    connected_ = true;
    err_.clear();
    return 0;
}

void FakePulse::disconnect() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    cb_ = nullptr;
    if (!connected_) return;
    connected_ = false;
    for (FakeStream &s : streams) {
        if (s.cap) s.cap->feed_end(kPulseDisconnected);
        if (s.play) s.play->feed_end(kPulseDisconnected);
    }
}

bool FakePulse::connected() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return connected_;
}

std::string FakePulse::last_error() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return err_;
}

int FakePulse::server_info(PulseServerInfo &out) {
    nap();
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!connected_) return kPulseState;
    out = server;
    return 0;
}

int FakePulse::list_sources(std::vector<PulseDevice> &out) {
    nap();
    std::lock_guard<std::recursive_mutex> g(mu_);
    out.clear();
    if (!connected_) return kPulseState;
    out = sources;
    return 0;
}

int FakePulse::list_sinks(std::vector<PulseDevice> &out) {
    nap();
    std::lock_guard<std::recursive_mutex> g(mu_);
    out.clear();
    if (!connected_) return kPulseState;
    out = sinks;
    return 0;
}

int FakePulse::list_cards(std::vector<PulseCard> &out) {
    nap();
    std::lock_guard<std::recursive_mutex> g(mu_);
    out.clear();
    if (!connected_) return kPulseState;
    out = cards;
    return 0;
}

int FakePulse::card_by_name(const std::string &name, PulseCard &out) {
    nap();
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!connected_) return kPulseState;
    for (const PulseCard &c : cards) {
        if (c.name == name) {
            out = c;
            return 0;
        }
    }
    return kPulseNoDevice;
}

void FakePulse::set_change_callback(PulseChangeCallback cb) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    cb_ = std::move(cb);
}

int FakePulse::open_capture(const PulseCaptureRequest &req, std::unique_ptr<PulseCapture> &out, std::string *why) {
    out.reset();
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!connected_) return kPulseState;
    const PulseDevice *src = nullptr;
    for (const PulseDevice &d : sources)
        if (d.name == req.source) src = &d;
    PulseDevice guess;
    bool missing = false;
    if (!req.lookup_first && !req.source.empty()) {
        /* the diagnostics path: no lookup; when the name does not exist the "server" decides below */
        missing = src == nullptr;
        guess.name = req.source;
        guess.rate = req.rate ? req.rate : 48000;
        guess.channels = req.channels ? req.channels : 1;
        src = &guess;
    }
    if (!src) {
        if (why) *why = "no source named \"" + req.source + "\"";
        return kPulseNoDevice;
    }
    PulseStreamFormat fmt;
    std::string w;
    if (!pulse_capture_format(req, *src, fmt, &w)) {
        if (why) *why = w;
        return kPulseRefused;
    }
    FakeStream rec;
    rec.capture = true;
    rec.requested = req.source;
    rec.attached = attach_elsewhere.empty() ? req.source : attach_elsewhere;
    attach_elsewhere.clear();
    if (missing) /* DONT_MOVE honoured: no device, no stream (refused); else the server falls back (P5 bt6) */
        rec.attached = honour_dont_move && (fmt.flags & 0x200u) ? std::string() : server.default_source;
    rec.rate = fmt.rate;
    rec.channels = fmt.channels;
    rec.flags = fmt.flags;
    rec.attr = fmt.requested;
    rec.props = pulse_stream_props(req.stream_name, req.role, "Cassette");
    streams.push_back(rec);
    const size_t i = streams.size() - 1;
    std::unique_ptr<PulseCapture> cap(new PulseCapture(req, fmt, [this] { return clock.load(); }));
    cap->attach(std::unique_ptr<PulseStreamLink>(new FakeLink(this, i)));
    streams[i].cap = cap.get();
    if (connect_error_stream) {
        const int e = connect_error_stream;
        connect_error_stream = 0;
        cap->feed_end(e);
    } else if (missing && streams[i].attached.empty()) {
        cap->feed_end(kPulseNoDevice);
    } else if (!defer_ready) {
        cap->feed_ready(streams[i].attached, streams[i].attr);
    }
    if (cap->ended()) {
        const int code = cap->end_code();
        PulseStreamEvent ev;
        std::string moved;
        while (cap->poll_event(ev))
            if (ev.kind == PulseEvent::Moved) moved = ev.device;
        if (why) *why = code == kPulseMoved ? "attached to \"" + moved + "\"" : pulse_error_text(code);
        cap->close();
        return code;
    }
    out = std::move(cap);
    return 0;
}

int FakePulse::open_playback(const PulsePlaybackRequest &req, std::unique_ptr<PulsePlayback> &out, std::string *why) {
    out.reset();
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!connected_) return kPulseState;
    const PulseDevice *sink = nullptr;
    for (const PulseDevice &d : sinks)
        if (d.name == req.sink) sink = &d;
    if (!sink) {
        if (why) *why = "no sink named \"" + req.sink + "\"";
        return kPulseNoDevice;
    }
    PulseStreamFormat fmt;
    std::string w;
    if (!pulse_playback_format(req, *sink, fmt, &w)) {
        if (why) *why = w;
        return kPulseRefused;
    }
    FakeStream rec;
    rec.capture = false;
    rec.requested = req.sink;
    rec.attached = attach_elsewhere.empty() ? req.sink : attach_elsewhere;
    attach_elsewhere.clear();
    rec.rate = fmt.rate;
    rec.channels = fmt.channels;
    rec.flags = fmt.flags;
    rec.attr = fmt.requested;
    rec.props = pulse_stream_props(req.stream_name, req.role, "Cassette");
    streams.push_back(rec);
    const size_t i = streams.size() - 1;
    std::unique_ptr<PulsePlayback> pb(new PulsePlayback(req, fmt, [this] { return clock.load(); }));
    pb->attach(std::unique_ptr<PulseStreamLink>(new FakeLink(this, i)));
    streams[i].play = pb.get();
    if (connect_error_stream) {
        const int e = connect_error_stream;
        connect_error_stream = 0;
        pb->feed_end(e);
    } else if (!defer_ready) {
        pb->feed_ready(streams[i].attached, streams[i].attr);
    }
    if (pb->ended()) {
        const int code = pb->end_code();
        if (why) *why = pulse_error_text(code);
        pb->close();
        return code;
    }
    out = std::move(pb);
    return 0;
}

bool FakePulse::deliver(size_t i, const int16_t *samples, size_t frames) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (!s) return false;
    if (!s->cap) {
        s->delivered_after_close += frames;
        return false;
    }
    s->cap->feed_frames(samples, frames);
    return true;
}

bool FakePulse::deliver_value(size_t i, int16_t value, size_t frames) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (!s) return false;
    std::vector<int16_t> buf(frames * (s->channels ? s->channels : 1), value);
    return deliver(i, buf.data(), frames);
}

void FakePulse::hole(size_t i, size_t frames) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (s && s->cap) s->cap->feed_hole(frames);
}

void FakePulse::ready(size_t i) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (!s) return;
    if (s->cap) s->cap->feed_ready(s->attached, s->attr);
    if (s->play) s->play->feed_ready(s->attached, s->attr);
}

size_t FakePulse::consume(size_t i, size_t frames, std::vector<int16_t> *out) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (!s || !s->play) return 0;
    std::vector<int16_t> buf(frames * (s->channels ? s->channels : 1));
    const size_t got = s->play->take(buf.data(), frames);
    if (out) out->insert(out->end(), buf.begin(), buf.begin() + static_cast<long>(got * (s->channels ? s->channels : 1)));
    if (got < frames) s->play->feed_underflow();
    return got;
}

void FakePulse::underflow(size_t i) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (s && s->play) s->play->feed_underflow();
}

void FakePulse::add_source(const PulseDevice &d) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    PulseDevice v = d;
    if (v.index == kPulseNoIndex) v.index = next_index_++;
    sources.push_back(v);
    notify(PulseChange::What::Source, PulseChange::Kind::New, v.index);
}

void FakePulse::add_sink(const PulseDevice &d) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    PulseDevice v = d;
    if (v.index == kPulseNoIndex) v.index = next_index_++;
    sinks.push_back(v);
    notify(PulseChange::What::Sink, PulseChange::Kind::New, v.index);
}

void FakePulse::lose(const std::string &device, bool sink) {
    std::vector<PulseDevice> &list = sink ? sinks : sources;
    uint32_t index = kPulseNoIndex;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->name == device) {
            index = it->index;
            list.erase(it);
            break;
        }
    }
    const std::string fallback = sink ? server.default_sink : server.default_source;
    std::string other = fallback != device ? fallback : std::string();
    if (other.empty()) {
        for (const PulseDevice &d : list) {
            if (d.name != device) {
                other = d.name;
                break;
            }
        }
    }
    for (FakeStream &s : streams) {
        if (s.attached != device || (!s.cap && !s.play) || s.capture == sink) continue;
        /* a server that honours DONT_MOVE kills the stream; one that does not (P5) moves it */
        const bool kill = honour_dont_move && (s.flags & 0x200u /* PA_STREAM_DONT_MOVE */);
        if (kill || other.empty()) {
            s.attached.clear();
            if (s.cap) s.cap->feed_end(kPulseKilled);
            if (s.play) s.play->feed_end(kPulseKilled);
        } else {
            s.attached = other;
            if (s.cap) s.cap->feed_moved(other);
            if (s.play) s.play->feed_moved(other);
        }
    }
    notify(sink ? PulseChange::What::Sink : PulseChange::What::Source, PulseChange::Kind::Remove, index);
}

void FakePulse::remove_source(const std::string &name) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    lose(name, false);
}

void FakePulse::remove_sink(const std::string &name) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    lose(name, true);
}

void FakePulse::suspend(const std::string &device, bool on) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (FakeStream &s : streams) {
        if (s.attached != device) continue;
        const bool fail_on = (s.flags & 0x20000u /* PA_STREAM_FAIL_ON_SUSPEND */) != 0;
        if (on && fail_on) {
            if (s.cap) s.cap->feed_end(kPulseKilled);
            if (s.play) s.play->feed_end(kPulseKilled);
            continue;
        }
        if (s.cap) s.cap->feed_suspended(on);
        if (s.play) s.play->feed_suspended(on);
    }
}

void FakePulse::move(size_t i, const std::string &to) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (!s) return;
    s->attached = to;
    if (s->cap) s->cap->feed_moved(to);
    if (s->play) s->play->feed_moved(to);
}

void FakePulse::fail(size_t i, int code) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    FakeStream *s = at(i);
    if (!s) return;
    if (s->cap) s->cap->feed_end(code);
    if (s->play) s->play->feed_end(code);
}

void FakePulse::set_profile(const std::string &card, const std::string &profile) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (PulseCard &c : cards) {
        if (c.name == card) {
            c.active_profile = profile;
            notify(PulseChange::What::Card, PulseChange::Kind::Change, c.index);
        }
    }
}

void FakePulse::crash() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    connected_ = false;
    for (FakeStream &s : streams) {
        if (s.cap) s.cap->feed_end(kPulseDisconnected);
        if (s.play) s.play->feed_end(kPulseDisconnected);
    }
    notify(PulseChange::What::Server, PulseChange::Kind::Disconnected, kPulseNoIndex);
}

/* ---- presets */

PulseDevice fake_bt_source(const std::string &mac, uint32_t rate, uint32_t card) {
    PulseDevice d;
    d.name = "bluez_input." + underscored(mac) + ".0";
    d.description = "Evolve 65";
    d.rate = rate;
    d.channels = 1;
    d.format = "s16le";
    d.card = card;
    d.state = PulseDeviceState::Idle;
    d.bt_address = mac;
    d.form_factor = "headset";
    d.bus = "bluetooth";
    d.bt_profile = "headset-head-unit";
    d.bt_codec = rate == 8000 ? "cvsd" : "msbc";
    d.device_description = "Evolve 65";
    d.device_api = "bluez5";
    return d;
}

PulseDevice fake_bt_sink(const std::string &mac, uint32_t card, const std::string &form) {
    PulseDevice d;
    d.name = "bluez_output." + underscored(mac) + ".1";
    d.description = form == "speaker" ? "XWF-M18-M28-M38" : "Evolve 65";
    d.rate = 48000;
    d.channels = 2;
    d.format = "s16le";
    d.card = card;
    d.state = PulseDeviceState::Suspended;
    d.bt_address = mac;
    d.form_factor = form;
    d.bus = "bluetooth";
    d.bt_profile = "a2dp-sink";
    d.bt_codec = "sbc";
    d.device_description = d.description;
    d.device_api = "bluez5";
    return d;
}

PulseDevice fake_bt_monitor(const std::string &mac, uint32_t card) {
    PulseDevice d = fake_bt_sink(mac, card);
    d.monitor_of = d.name;
    d.name += ".monitor";
    d.description = "Monitor of " + d.description;
    d.is_monitor = true;
    return d;
}

PulseDevice fake_usb_source(const std::string &id, uint32_t rate, unsigned channels, uint32_t card) {
    PulseDevice d;
    d.name = "alsa_input.usb-" + id + (channels == 1 ? ".mono-fallback" : ".analog-stereo");
    d.description = id + " Mono";
    d.rate = rate;
    d.channels = channels;
    d.format = "s16le";
    d.card = card;
    d.bus = "usb";
    d.form_factor = "headset";
    d.device_api = "alsa";
    return d;
}

PulseDevice fake_usb_sink(const std::string &id, uint32_t card) {
    PulseDevice d;
    d.name = "alsa_output.usb-" + id + ".analog-stereo";
    d.description = id + " Analog Stereo";
    d.rate = 48000;
    d.channels = 2;
    d.format = "s16le";
    d.card = card;
    d.bus = "usb";
    d.device_api = "alsa";
    return d;
}

PulseCard fake_bt_card(const std::string &mac, uint32_t index, bool music_available, const std::string &active,
                       bool has_call) {
    PulseCard c;
    c.index = index;
    c.name = "bluez_card." + underscored(mac);
    c.description = "Evolve 65";
    c.driver = "module-bluez5-device.c";
    c.bt_address = mac;
    c.bus = "bluetooth";
    c.form_factor = "headset";
    c.device_api = "bluez5";
    auto add = [&](const char *n, const char *d, bool avail, uint32_t sinks, uint32_t sources) {
        PulseProfile p;
        p.name = n;
        p.description = d;
        p.available = avail;
        p.n_sinks = sinks;
        p.n_sources = sources;
        c.profiles.push_back(p);
    };
    add("a2dp-sink", "High Fidelity Playback (A2DP Sink, codec SBC)", music_available, 1, 0);
    add("a2dp-sink-sbc_xq", "High Fidelity Playback (A2DP Sink, codec SBC-XQ)", music_available, 1, 0);
    if (has_call) {
        add("headset-head-unit", "Headset Head Unit (HSP/HFP, codec mSBC)", true, 1, 1);
        add("headset-head-unit-cvsd", "Headset Head Unit (HSP/HFP, codec CVSD)", true, 1, 1);
    }
    add("off", "Off", true, 0, 0);
    c.active_profile = active;
    return c;
}

}  // namespace cassette
