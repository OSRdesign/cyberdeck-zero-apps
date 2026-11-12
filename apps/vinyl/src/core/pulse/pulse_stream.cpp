/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette sound server: the parts every PulseApi implementation shares. The error texts, the property and profile
 * helpers, the open parameters (flags, properties, format, buffer attributes from milliseconds) and the two stream
 * classes: the bounded ring between the server's thread and the owner, the event logic, the silence detector and the
 * start timeout. The server end (real_pulse.cpp, the test fake) only feeds them.
 */
#include "pulse_api.hpp"

#include "../ring.hpp"
#include "pulse_abi.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace vinyl {

int pulse_volume_pct(const uint32_t *values, unsigned channels) { /* VINYL */
    if (!values || channels == 0) return -1;
    uint32_t top = 0;
    for (unsigned c = 0; c < channels && c < 32; ++c) top = std::max(top, values[c]);
    return static_cast<int>((static_cast<uint64_t>(top) * 100 + 0x8000) / 0x10000);
}

/* ------------------------------------------------------------------------------------------------ texts */

std::string pulse_error_text(int err) {
    switch (err) {
    case 0: return "no error";
    case kPulseMissing: return "libpulse not installed (libpulse0)";
    case kPulseNoServer: return "No sound server is running (PipeWire or PulseAudio)";
    case kPulseNoDevice: return "The sound device is not there";
    case kPulseKilled: return "The sound device went away";
    case kPulseMoved: return "The sound server switched to another device";
    case kPulseDisconnected: return "The connection to the sound server was lost";
    case kPulseTimeout: return "The sound server did not answer in time";
    case kPulseFailed: return "The sound server reported an error";
    case kPulseState: return "Sound server call out of order";
    case kPulseRefused: return "The sound server refused the format";
    default: break;
    }
    return err > 0 ? "no error" : "sound server error " + std::to_string(err);
}

int pulse_error_from_pa(int e) {
    switch (e) {
    case pa::kOk: return 0;
    case pa::kErrNoEntity: return kPulseNoDevice;
    case pa::kErrConnectionRefused:
    case pa::kErrInvalidServer: return kPulseNoServer;
    case pa::kErrTimeout: return kPulseTimeout;
    case pa::kErrConnectionTerminated: return kPulseDisconnected;
    case pa::kErrKilled: return kPulseKilled;
    case pa::kErrInvalid:
    case pa::kErrNotSupported:
    case pa::kErrTooLarge: return kPulseRefused;
    case pa::kErrBadState: return kPulseState;
    default: return kPulseFailed;
    }
}

const char *pulse_event_name(PulseEvent e) {
    switch (e) {
    case PulseEvent::Connecting: return "Connecting";
    case PulseEvent::Running: return "Running";
    case PulseEvent::Suspended: return "Suspended";
    case PulseEvent::Moved: return "Moved";
    case PulseEvent::Killed: return "Killed";
    case PulseEvent::Error: return "Error";
    case PulseEvent::StartTimeout: return "StartTimeout";
    case PulseEvent::SilenceDetected: return "SilenceDetected";
    case PulseEvent::SilenceCleared: return "SilenceCleared";
    }
    return "?";
}

const char *pulse_state_name(PulseStreamState s) {
    switch (s) {
    case PulseStreamState::Connecting: return "connecting";
    case PulseStreamState::Running: return "running";
    case PulseStreamState::Suspended: return "suspended";
    case PulseStreamState::Ended: return "ended";
    }
    return "?";
}

const char *pulse_format_name(int f) {
    switch (f) {
    case pa::kSampleU8: return "u8";
    case pa::kSampleAlaw: return "aLaw";
    case pa::kSampleUlaw: return "uLaw";
    case pa::kSampleS16LE: return "s16le";
    case pa::kSampleS16BE: return "s16be";
    case pa::kSampleFloat32LE: return "float32le";
    case pa::kSampleFloat32BE: return "float32be";
    case pa::kSampleS32LE: return "s32le";
    case pa::kSampleS32BE: return "s32be";
    case pa::kSampleS24LE: return "s24le";
    case pa::kSampleS24BE: return "s24be";
    case pa::kSampleS24_32LE: return "s24-32le";
    case pa::kSampleS24_32BE: return "s24-32be";
    default: return "invalid";
    }
}

/* ------------------------------------------------------------------------------------------------ names and profiles */

namespace {

bool starts_with(const std::string &s, const char *p) { return s.compare(0, std::strlen(p), p) == 0; }

}  // namespace

bool pulse_profile_is_music(const std::string &p) { return starts_with(p, "a2dp-sink") || starts_with(p, "a2dp_sink"); }

bool pulse_profile_is_call(const std::string &p) {
    return starts_with(p, "headset-head-unit") || starts_with(p, "headset_head_unit") ||
           starts_with(p, "handsfree_head_unit") || starts_with(p, "handsfree-head-unit");
}

std::string pulse_parse_mac(const std::string &s) {
    if (s.size() != 17) return "";
    for (size_t i = 0; i < 17; ++i) {
        const char c = s[i];
        if (i % 3 == 2) {
            if (c != ':') return "";
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return "";
        }
    }
    return s;
}

std::string pulse_mac_from_name(const std::string &name) {
    /* "bluez_input.70_BF_92_53_6B_8B.0", "bluez_output.70_BF_92_53_6B_8B.1.monitor", "bluez_card.70_BF_92_53_6B_8B";
     * PulseAudio: "bluez_source.70_BF_92_53_6B_8B.headset_head_unit" */
    for (size_t i = 0; i + 17 <= name.size(); ++i) {
        if (i > 0 && name[i - 1] != '.' && name[i - 1] != '_') continue;
        std::string cand = name.substr(i, 17);
        for (size_t k = 2; k < 17; k += 3) {
            if (cand[k] == '_') cand[k] = ':';
        }
        if (!pulse_parse_mac(cand).empty() && (i + 17 == name.size() || name[i + 17] == '.')) return cand;
    }
    return "";
}

/* ------------------------------------------------------------------------------------------------ open parameters */

uint32_t pulse_ms_to_bytes(uint32_t ms, uint32_t rate, unsigned channels, unsigned sample_bytes) {
    const uint64_t frames = (static_cast<uint64_t>(ms) * rate + 999) / 1000;
    const uint64_t b = frames * (sample_bytes ? sample_bytes : 2u) * (channels ? channels : 1u);
    return b > 0xFFFFFFF0u ? 0xFFFFFFF0u : static_cast<uint32_t>(b);
}

uint64_t pulse_bytes_to_frames(uint64_t bytes, unsigned channels, unsigned sample_bytes) {
    return bytes / ((sample_bytes ? sample_bytes : 2u) * (channels ? channels : 1u));
}

uint32_t pulse_capture_flags(const PulseCaptureRequest &req) {
    uint32_t f = pa::kStreamDontMove | pa::kStreamAdjustLatency | pa::kStreamInterpolateTiming |
                 pa::kStreamAutoTimingUpdate;
    if (req.fail_on_suspend) f |= pa::kStreamFailOnSuspend;
    return f;
}

uint32_t pulse_playback_flags(const PulsePlaybackRequest &req) {
    uint32_t f = pa::kStreamDontMove | pa::kStreamAdjustLatency | pa::kStreamInterpolateTiming |
                 pa::kStreamAutoTimingUpdate | pa::kStreamStartUnmuted; /* VINYL: START_UNMUTED */
    if (req.fail_on_suspend) f |= pa::kStreamFailOnSuspend;
    return f;
}

std::vector<std::pair<std::string, std::string>> pulse_context_props(const std::string &app_name) { /* VINYL */
    return {{"application.name", app_name.empty() ? std::string("Vinyl") : app_name},
            {"application.id", kPulseAppId},
            {"application.process.binary", kPulseAppId},
            {"application.icon_name", kPulseAppId}};
}

PulseVolume pulse_norm_volume(unsigned channels) { /* VINYL */
    PulseVolume v;
    v.channels = std::min(std::max(channels, 1u), 32u);
    for (unsigned c = 0; c < v.channels; ++c) v.values[c] = 0x10000; /* PA_VOLUME_NORM */
    return v;
}

std::vector<std::pair<std::string, std::string>> pulse_stream_props(const std::string &stream_name, const std::string &role,
                                                                    const std::string &app_name, bool playback) {
    std::vector<std::pair<std::string, std::string>> p;
    p.emplace_back("media.name", stream_name);
    if (!role.empty()) p.emplace_back("media.role", role);
    p.emplace_back("application.name", app_name.empty() ? std::string("Vinyl") : app_name);
    /* VINYL: its own identity and restore key, no restored volume or target (see pulse_api.hpp) */
    p.emplace_back("application.id", kPulseAppId);
    p.emplace_back("module-stream-restore.id",
                   std::string(playback ? "sink-input" : "source-output") + "-by-application-id:" + kPulseAppId);
    p.emplace_back("state.restore-props", "false");
    p.emplace_back("state.restore-target", "false");
    /* PipeWire: pipewire-pulse copies the stream's properties onto its node. WirePlumber then destroys the stream when its
     * device goes away (dont-reconnect) and refuses to link it to another device when the target is missing
     * (dont-fallback) instead of the silent relink measured in P5. */
    p.emplace_back("node.dont-reconnect", "true");
    p.emplace_back("node.dont-fallback", "true");
    p.emplace_back("node.dont-move", "true");
    return p;
}

namespace {

bool bad_name(const std::string &n) { return n.empty() || n == "default" || n == "@DEFAULT_SOURCE@" || n == "@DEFAULT_SINK@"; }

}  // namespace

bool pulse_capture_format(const PulseCaptureRequest &req, const PulseDevice &src, PulseStreamFormat &fmt,
                          std::string *why) {
    auto no = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    if (bad_name(req.source)) return no("a capture stream needs the exact source name");
    if (src.is_monitor) return no("a monitor source is not a microphone");
    fmt = PulseStreamFormat();
    fmt.device_rate = src.rate;
    fmt.device_channels = src.channels;
    fmt.rate = req.rate ? req.rate : src.rate;
    if (fmt.rate == 0) return no("the source reports no sample rate");
    if (fmt.rate < 1000 || fmt.rate > 384000) return no("unsupported sample rate");
    unsigned ch = req.channels ? req.channels : src.channels;
    if (ch == 0) ch = 1;
    fmt.channels = std::min(ch, 2u);
    fmt.requested = pulse_capture_attr(req, fmt.rate, fmt.channels);
    fmt.granted = fmt.requested;
    fmt.flags = pulse_capture_flags(req);
    return true;
}

bool pulse_playback_format(const PulsePlaybackRequest &req, const PulseDevice &sink, PulseStreamFormat &fmt,
                           std::string *why) {
    auto no = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    if (bad_name(req.sink)) return no("a playback stream needs the exact sink name");
    if (req.rate < 1000 || req.rate > 384000) return no("unsupported sample rate");
    if (req.channels < 1 || req.channels > 2) return no("playback takes 1 or 2 channels");
    if (req.sample_bytes != 2 && req.sample_bytes != 4) return no("playback takes 16 or 32 bit samples"); /* VINYL */
    fmt = PulseStreamFormat();
    fmt.sample_bytes = req.sample_bytes; /* VINYL */
    fmt.device_rate = sink.rate;
    fmt.device_channels = sink.channels;
    fmt.rate = req.rate;
    fmt.channels = req.channels;
    fmt.requested = pulse_playback_attr(req, fmt.rate, fmt.channels);
    fmt.granted = fmt.requested;
    fmt.flags = pulse_playback_flags(req);
    return true;
}

PulseBufferAttr pulse_capture_attr(const PulseCaptureRequest &req, uint32_t rate, unsigned channels) {
    PulseBufferAttr a;
    const uint32_t frag = std::max<uint32_t>(req.fragment_ms, 1);
    a.fragsize = pulse_ms_to_bytes(frag, rate, channels);
    a.maxlength = pulse_ms_to_bytes(std::max<uint32_t>(req.server_max_ms, frag * 2), rate, channels);
    return a;
}

PulseBufferAttr pulse_playback_attr(const PulsePlaybackRequest &req, uint32_t rate, unsigned channels) {
    PulseBufferAttr a;
    a.tlength = pulse_ms_to_bytes(std::max<uint32_t>(req.buffer_ms, 1), rate, channels, req.sample_bytes); /* VINYL */
    return a;
}

/* ------------------------------------------------------------------------------------------------ capture */

namespace {

constexpr size_t kQueueMax = 64; /* state events are rare; a flood beyond this is counted, not kept */

std::unique_ptr<Ring> make_ring(uint32_t rate, unsigned channels, uint32_t ms, unsigned sample_bytes = 2) {
    AudioFormat af;
    af.rate = rate ? rate : 48000;
    af.channels = static_cast<uint16_t>(channels ? channels : 1);
    af.bits = static_cast<uint16_t>(8 * (sample_bytes ? sample_bytes : 2)); /* VINYL: 32 for s32le playback */
    return std::unique_ptr<Ring>(new Ring(af, std::max<uint32_t>(ms, 10) / 1000.0));
}

bool killed_kind(int code) { return code == kPulseKilled || code == kPulseNoDevice || code == kPulseDisconnected; }

}  // namespace

PulseCapture::PulseCapture(const PulseCaptureRequest &req, const PulseStreamFormat &fmt, std::function<double()> now)
    : req_(req), fmt_(fmt), now_(std::move(now)) {
    if (!now_) {
        now_ = [] {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        };
    }
    t0_ = now_();
    channels_ = fmt_.channels ? fmt_.channels : 1;
    silence_frames_ = req_.silence_seconds > 0 ? static_cast<uint64_t>(req_.silence_seconds * fmt_.rate + 0.5) : 0;
    if (req_.silence_seconds > 0 && silence_frames_ == 0) silence_frames_ = 1;
    ring_ = make_ring(fmt_.rate, channels_, req_.ring_ms);
}

PulseCapture::~PulseCapture() { close(); }

void PulseCapture::attach(std::unique_ptr<PulseStreamLink> link) { link_ = std::move(link); }

void PulseCapture::close() {
    if (closed_.exchange(true)) return;
    if (link_) link_->close();
    wait_cv_.notify_all();
}

void PulseCapture::feed_frames(const void *frames, size_t n) {
    if (n == 0) return;
    if (closed_.load(std::memory_order_acquire) || ended_.load(std::memory_order_acquire)) {
        after_end_.fetch_add(n, std::memory_order_relaxed);
        return;
    }
    if (!first_frame_.load(std::memory_order_relaxed)) {
        first_frame_t_.store(now_s(), std::memory_order_relaxed);
        first_frame_.store(true, std::memory_order_release);
    }
    const uint64_t before = frames_in_.fetch_add(n, std::memory_order_relaxed);
    if (silence_frames_) {
        const int16_t *s = static_cast<const int16_t *>(frames);
        const size_t samples = n * channels_;
        size_t last = samples; /* the last non-zero sample, `samples` when none */
        for (size_t i = samples; i-- > 0;) {
            if (s[i] != 0) {
                last = i;
                break;
            }
        }
        uint64_t run = zero_run_.load(std::memory_order_relaxed);
        if (last == samples) {
            run += n;
        } else {
            /* the first non-zero sample of the chunk ends a detected silence where it is */
            if (silent_.load(std::memory_order_relaxed)) {
                size_t first = 0;
                while (s[first] == 0) ++first;
                flip_frame_.store(before + first / channels_, std::memory_order_relaxed);
                silent_.store(false, std::memory_order_relaxed);
                silence_flips_.fetch_add(1, std::memory_order_release);
            }
            run = n - (last / channels_ + 1);
        }
        zero_run_.store(run, std::memory_order_relaxed);
        if (!silent_.load(std::memory_order_relaxed) && run >= silence_frames_) {
            flip_frame_.store(before + n - (run - silence_frames_), std::memory_order_relaxed);
            silent_.store(true, std::memory_order_relaxed);
            silence_flips_.fetch_add(1, std::memory_order_release);
        }
    }
    if (ring_->write(frames, n) > 0) wait_cv_.notify_one();
}

void PulseCapture::feed_hole(size_t n) { holes_.fetch_add(n, std::memory_order_relaxed); }

void PulseCapture::end_with(PulseEvent kind, int code, const std::string &device) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (ended_.load(std::memory_order_acquire)) return; /* the first end wins */
        end_code_.store(code, std::memory_order_release);
        ended_.store(true, std::memory_order_release);
        PulseStreamEvent ev;
        ev.kind = kind;
        ev.code = code;
        ev.device = device;
        ev.t = now_s();
        ev.frame = frames_in_.load(std::memory_order_relaxed);
        if (queue_.size() < kQueueMax + 4) queue_.push_back(ev); /* an end always gets in */
        else ++queue_dropped_;
    }
    wait_cv_.notify_all();
}

bool PulseCapture::feed_ready(const std::string &device, const PulseBufferAttr &granted) {
    if (closed_.load() || ended_.load()) return false;
    {
        std::lock_guard<std::mutex> g(mu_);
        fmt_.granted = granted;
        fmt_.device = device;
    }
    if (!device.empty() && device != req_.source) {
        end_with(PulseEvent::Moved, kPulseMoved, device);
        return false;
    }
    return true;
}

bool PulseCapture::feed_moved(const std::string &device) {
    if (closed_.load()) return false;
    end_with(PulseEvent::Moved, kPulseMoved, device);
    return false;
}

void PulseCapture::feed_suspended(bool on) {
    if (closed_.load() || ended_.load()) return;
    if (suspended_.exchange(on) == on) return;
    std::lock_guard<std::mutex> g(mu_);
    PulseStreamEvent ev;
    ev.kind = on ? PulseEvent::Suspended : PulseEvent::Running;
    ev.device = req_.source;
    ev.t = now_s();
    ev.frame = frames_in_.load(std::memory_order_relaxed);
    if (queue_.size() < kQueueMax) queue_.push_back(ev);
    else ++queue_dropped_;
}

void PulseCapture::feed_end(int code) {
    if (closed_.load()) return;
    if (code >= 0) code = kPulseFailed;
    end_with(killed_kind(code) ? PulseEvent::Killed : PulseEvent::Error, code, req_.source);
}

long PulseCapture::read(void *dst, size_t max_frames, int timeout_ms) {
    if (closed_.load(std::memory_order_acquire)) return kPulseState;
    if (max_frames == 0) return 0;
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        const size_t n = ring_->read(dst, max_frames);
        if (n > 0) {
            frames_read_.fetch_add(n, std::memory_order_relaxed);
            return static_cast<long>(n);
        }
        if (ended_.load(std::memory_order_acquire)) {
            /* the ring may have been filled just before the end: read again before reporting the end */
            const size_t m = ring_->read(dst, max_frames);
            if (m > 0) {
                frames_read_.fetch_add(m, std::memory_order_relaxed);
                return static_cast<long>(m);
            }
            const int c = end_code_.load(std::memory_order_acquire);
            return c < 0 ? c : kPulseKilled;
        }
        if (closed_.load(std::memory_order_acquire)) return kPulseState;
        const auto left = deadline - clock::now();
        if (timeout_ms <= 0 || left <= clock::duration::zero()) return 0;
        std::unique_lock<std::mutex> lk(wait_mu_);
        wait_cv_.wait_for(lk, std::min<clock::duration>(left, std::chrono::milliseconds(5)));
    }
}

bool PulseCapture::poll_event(PulseStreamEvent &ev) {
    std::lock_guard<std::mutex> pg(poll_mu_);
    const double t = now_s();
    ev = PulseStreamEvent();
    ev.device = req_.source;
    if (!rep_connecting_) {
        rep_connecting_ = true;
        ev.kind = PulseEvent::Connecting;
        ev.t = 0;
        return true;
    }
    if (first_frame_.load(std::memory_order_acquire) && !rep_running_) {
        rep_running_ = true;
        ev.kind = PulseEvent::Running;
        ev.t = first_frame_t_.load(std::memory_order_relaxed);
        return true;
    }
    if (!rep_timeout_ && !first_frame_.load(std::memory_order_acquire) && !ended_.load(std::memory_order_acquire) &&
        req_.first_frame_timeout_ms > 0 && t * 1000.0 >= req_.first_frame_timeout_ms) {
        rep_timeout_ = true;
        ev.kind = PulseEvent::StartTimeout;
        ev.code = kPulseTimeout;
        ev.t = t;
        return true;
    }
    const uint32_t flips = silence_flips_.load(std::memory_order_acquire);
    if (rep_flips_ < flips) {
        /* the flips alternate, starting with a detection: the count says which one comes next */
        ev.kind = rep_flips_ % 2 == 0 ? PulseEvent::SilenceDetected : PulseEvent::SilenceCleared;
        ++rep_flips_;
        ev.frame = rep_flips_ == flips ? flip_frame_.load(std::memory_order_relaxed)
                                       : frames_in_.load(std::memory_order_relaxed);
        ev.t = first_frame_t_.load(std::memory_order_relaxed) + (fmt_.rate ? double(ev.frame) / fmt_.rate : 0.0);
        return true;
    }
    std::lock_guard<std::mutex> g(mu_);
    if (queue_.empty()) return false;
    ev = queue_.front();
    queue_.pop_front();
    return true;
}

PulseStreamState PulseCapture::state() const {
    if (ended_.load(std::memory_order_acquire) || closed_.load(std::memory_order_acquire)) return PulseStreamState::Ended;
    if (!first_frame_.load(std::memory_order_acquire)) return PulseStreamState::Connecting;
    if (suspended_.load(std::memory_order_acquire)) return PulseStreamState::Suspended;
    return PulseStreamState::Running;
}

double PulseCapture::connecting_seconds() const {
    return state() == PulseStreamState::Connecting ? std::max(0.0, now_s()) : 0.0;
}

bool PulseCapture::source_identity_ok(std::string *actual) {
    if (actual) actual->clear();
    if (closed_.load() || !link_) return false;
    const std::string dev = link_->device_name();
    if (actual) *actual = dev;
    if (ended_.load(std::memory_order_acquire)) return false;
    return !dev.empty() && dev == req_.source;
}

int PulseCapture::latency(PulseLatency &out) {
    out = PulseLatency();
    if (closed_.load() || !link_) return kPulseState;
    if (ended_.load()) return end_code_.load();
    return link_->latency(out);
}

PulseStreamFormat PulseCapture::format() const {
    std::lock_guard<std::mutex> g(mu_);
    return fmt_;
}

PulseCaptureStats PulseCapture::stats() const {
    PulseCaptureStats s;
    s.state = state();
    s.frames_in = frames_in_.load(std::memory_order_relaxed);
    s.frames_read = frames_read_.load(std::memory_order_relaxed);
    s.frames_overflow = ring_->frames_dropped();
    s.frames_hole = holes_.load(std::memory_order_relaxed);
    s.frames_after_end = after_end_.load(std::memory_order_relaxed);
    s.zero_run = zero_run_.load(std::memory_order_relaxed);
    s.silent = silent_.load(std::memory_order_relaxed);
    s.first_frame_s = first_frame_.load(std::memory_order_acquire) ? first_frame_t_.load(std::memory_order_relaxed) : -1.0;
    s.end_code = end_code_.load(std::memory_order_acquire);
    s.ring_frames = ring_->capacity_frames();
    return s;
}

/* ------------------------------------------------------------------------------------------------ playback */

PulsePlayback::PulsePlayback(const PulsePlaybackRequest &req, const PulseStreamFormat &fmt, std::function<double()> now)
    : req_(req), fmt_(fmt), now_(std::move(now)) {
    if (!now_) {
        now_ = [] {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        };
    }
    t0_ = now_();
    ring_ = make_ring(fmt_.rate, fmt_.channels, req_.ring_ms ? req_.ring_ms : req_.buffer_ms, fmt_.sample_bytes); /* VINYL */
}

PulsePlayback::~PulsePlayback() { close(); }

void PulsePlayback::attach(std::unique_ptr<PulseStreamLink> link) { link_ = std::move(link); }

void PulsePlayback::close() {
    if (closed_.exchange(true)) return;
    if (link_) link_->close();
    wait_cv_.notify_all();
}

void PulsePlayback::push(PulseEvent kind, int code, const std::string &device) {
    std::lock_guard<std::mutex> g(mu_);
    PulseStreamEvent ev;
    ev.kind = kind;
    ev.code = code;
    ev.device = device;
    ev.t = now_s();
    if (queue_.size() < kQueueMax) queue_.push_back(ev);
}

void PulsePlayback::end_with(PulseEvent kind, int code, const std::string &device) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (ended_.load(std::memory_order_acquire)) return;
        end_code_.store(code, std::memory_order_release);
        ended_.store(true, std::memory_order_release);
        PulseStreamEvent ev;
        ev.kind = kind;
        ev.code = code;
        ev.device = device;
        ev.t = now_s();
        queue_.push_back(ev);
    }
    wait_cv_.notify_all();
}

size_t PulsePlayback::take(void *dst, size_t max_frames) {
    if (closed_.load(std::memory_order_acquire)) return 0;
    const size_t n = ring_->read(dst, max_frames);
    if (n > 0) {
        to_server_.fetch_add(n, std::memory_order_relaxed);
        wait_cv_.notify_one();
    }
    return n;
}

size_t PulsePlayback::discard_queued() {
    unsigned char buf[4096];
    const size_t per = std::max<size_t>(1, sizeof buf / ring_->frame_bytes());
    size_t total = 0;
    for (;;) {
        const size_t n = ring_->read(buf, per);
        if (n == 0) break;
        total += n;
    }
    if (total) {
        dropped_.fetch_add(total, std::memory_order_relaxed);
        wait_cv_.notify_all();
    }
    return total;
}

size_t PulsePlayback::queued_frames() const { return ring_->available(); }

void PulsePlayback::feed_underflow() {
    if (closed_.load() || ended_.load()) return;
    underruns_.fetch_add(1, std::memory_order_relaxed);
}

bool PulsePlayback::feed_ready(const std::string &device, const PulseBufferAttr &granted) {
    if (closed_.load() || ended_.load()) return false;
    {
        std::lock_guard<std::mutex> g(mu_);
        fmt_.granted = granted;
        fmt_.device = device;
    }
    if (!device.empty() && device != req_.sink) {
        end_with(PulseEvent::Moved, kPulseMoved, device);
        return false;
    }
    if (!ready_.exchange(true)) push(PulseEvent::Running, 0, req_.sink);
    return true;
}

bool PulsePlayback::feed_moved(const std::string &device) {
    if (closed_.load()) return false;
    end_with(PulseEvent::Moved, kPulseMoved, device);
    return false;
}

void PulsePlayback::feed_suspended(bool on) {
    if (closed_.load() || ended_.load()) return;
    if (suspended_.exchange(on) == on) return;
    push(on ? PulseEvent::Suspended : PulseEvent::Running, 0, req_.sink);
}

void PulsePlayback::feed_end(int code) {
    if (closed_.load()) return;
    if (code >= 0) code = kPulseFailed;
    end_with(killed_kind(code) ? PulseEvent::Killed : PulseEvent::Error, code, req_.sink);
}

long PulsePlayback::write(const void *frames, size_t n, int timeout_ms) {
    if (closed_.load(std::memory_order_acquire)) return kPulseState;
    if (n == 0) return 0;
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        if (ended_.load(std::memory_order_acquire)) {
            const int c = end_code_.load(std::memory_order_acquire);
            return c < 0 ? c : kPulseKilled;
        }
        if (closed_.load(std::memory_order_acquire)) return kPulseState;
        const size_t room = ring_->capacity_frames() - ring_->available();
        if (room > 0) {
            const size_t k = ring_->write(frames, std::min(n, room));
            written_.fetch_add(k, std::memory_order_relaxed);
            if (link_) link_->kick();
            return static_cast<long>(k);
        }
        if (link_) link_->kick();
        const auto left = deadline - clock::now();
        if (timeout_ms <= 0 || left <= clock::duration::zero()) return 0;
        std::unique_lock<std::mutex> lk(wait_mu_);
        wait_cv_.wait_for(lk, std::min<clock::duration>(left, std::chrono::milliseconds(5)));
    }
}

int PulsePlayback::drain(int timeout_ms) {
    if (closed_.load()) return kPulseState;
    if (ended_.load()) return end_code_.load();
    if (!link_) return ring_->available() ? kPulseTimeout : 0;
    const int r = link_->drain(timeout_ms); /* hands the ring over, then waits for the server to play it */
    if (ended_.load()) return end_code_.load();
    return r;
}

void PulsePlayback::drop() {
    if (closed_.load()) return;
    if (link_) link_->flush();
    else discard_queued();
}

long PulsePlayback::delay_frames() {
    long d = static_cast<long>(ring_->available());
    PulseLatency l;
    if (link_ && !closed_.load() && !ended_.load() && link_->latency(l) == 0)
        d += static_cast<long>(l.total_ms * fmt_.rate / 1000.0 + 0.5);
    return d;
}

bool PulsePlayback::poll_event(PulseStreamEvent &ev) {
    std::lock_guard<std::mutex> g(mu_);
    ev = PulseStreamEvent();
    ev.device = req_.sink;
    if (!rep_connecting_) {
        rep_connecting_ = true;
        ev.kind = PulseEvent::Connecting;
        return true;
    }
    if (queue_.empty()) return false;
    ev = queue_.front();
    queue_.pop_front();
    return true;
}

PulseStreamState PulsePlayback::state() const {
    if (ended_.load() || closed_.load()) return PulseStreamState::Ended;
    if (!ready_.load()) return PulseStreamState::Connecting;
    if (suspended_.load()) return PulseStreamState::Suspended;
    return PulseStreamState::Running;
}

bool PulsePlayback::sink_identity_ok(std::string *actual) {
    if (actual) actual->clear();
    if (closed_.load() || !link_) return false;
    const std::string dev = link_->device_name();
    if (actual) *actual = dev;
    if (ended_.load()) return false;
    return !dev.empty() && dev == req_.sink;
}

int PulsePlayback::latency(PulseLatency &out) {
    out = PulseLatency();
    if (closed_.load() || !link_) return kPulseState;
    if (ended_.load()) return end_code_.load();
    return link_->latency(out);
}

PulseStreamFormat PulsePlayback::format() const {
    std::lock_guard<std::mutex> g(mu_);
    return fmt_;
}

PulsePlaybackStats PulsePlayback::stats() const {
    PulsePlaybackStats s;
    s.state = state();
    s.frames_written = written_.load();
    s.frames_to_server = to_server_.load();
    s.frames_dropped = dropped_.load();
    s.underruns = underruns_.load();
    s.end_code = end_code_.load();
    s.ring_frames = ring_->capacity_frames();
    return s;
}

}  // namespace vinyl
