/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "pulse_sink.hpp"

#include <algorithm>
#include <cerrno>

namespace vinyl {

PulseSinkOptions pulse_monitor_sink_options() {
    PulseSinkOptions o;
    o.buffer_ms = 40;
    o.ring_ms = 20;
    o.period_ms = 16;
    o.write_timeout_ms = 10;
    o.stream_name = "Cassette monitor";
    o.role = "production";
    return o;
}

PulsePlaybackSink::PulsePlaybackSink(PulseApi &api, PulseSinkOptions opt) : api_(api), opt_(std::move(opt)) {}

PulsePlaybackSink::~PulsePlaybackSink() { close(); }

void PulsePlaybackSink::set_sink(const std::string &name) {
    std::lock_guard<std::mutex> g(mu_);
    sink_ = name;
}

std::string PulsePlaybackSink::sink_name() const {
    std::lock_guard<std::mutex> g(mu_);
    return sink_;
}

std::shared_ptr<PulsePlayback> PulsePlaybackSink::get() const {
    std::lock_guard<std::mutex> g(mu_);
    return pb_;
}

int PulsePlaybackSink::open(const AudioFormat &src, SinkFormat &granted) {
    close();
    std::string name;
    {
        std::lock_guard<std::mutex> g(mu_);
        name = sink_;
    }
    if (name.empty() || src.rate == 0 || src.channels == 0) return -ENODEV;
    PulsePlaybackRequest rq;
    rq.sink = name;
    rq.rate = src.rate;
    rq.channels = src.channels == 1 ? 1u : 2u; /* more than two: the first two (the Player maps) */
    rq.sample_bytes = src.bits > 16 ? 4u : 2u; /* VINYL: 24-bit and 32-bit sources play as s32le (D13) */
    rq.buffer_ms = opt_.buffer_ms;
    rq.ring_ms = opt_.ring_ms;
    rq.ready_timeout_ms = opt_.ready_timeout_ms;
    rq.stream_name = opt_.stream_name;
    rq.role = opt_.role;
    std::unique_ptr<PulsePlayback> pb;
    std::string why;
    const int r = api_.open_playback(rq, pb, &why);
    if (r != 0 || !pb) {
        if (r == kPulseNoDevice || r == kPulseMoved || r == kPulseKilled) return -ENODEV;
        if (r == kPulseRefused) return -EINVAL;
        if (r == kPulseTimeout) return -ETIMEDOUT;
        return -EIO;
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        pb_ = std::move(pb);
        rate_ = src.rate;
        seen_underruns_ = pb_->underruns();
        fresh_ = true;
        progress_ = std::chrono::steady_clock::now();
    }
    granted.rate = src.rate;
    granted.channels = rq.channels;
    granted.format = src.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE; /* VINYL */
    granted.period_frames = std::max<uint32_t>(64, static_cast<uint32_t>(static_cast<uint64_t>(src.rate) * opt_.period_ms / 1000));
    granted.buffer_frames = static_cast<uint32_t>(static_cast<uint64_t>(src.rate) * opt_.buffer_ms / 1000);
    return 0;
}

long PulsePlaybackSink::write(const void *frames, size_t nframes) {
    std::shared_ptr<PulsePlayback> pb = get();
    if (!pb) return -ENODEV;
    if (pb->ended()) return -ENODEV;
    const uint64_t u = pb->underruns();
    if (fresh_) { /* after open, drop or drain: underflows while nothing was written are not underruns */
        seen_underruns_ = u;
        fresh_ = false;
    } else if (u != seen_underruns_) {
        seen_underruns_ = u;
        return -EPIPE;
    }
    const long r = pb->write(frames, nframes, opt_.write_timeout_ms);
    if (r < 0) return -ENODEV;
    const auto now = std::chrono::steady_clock::now();
    if (r > 0) {
        progress_ = now;
        return r;
    }
    if (now - progress_ > std::chrono::milliseconds(opt_.stuck_ms)) return -EIO;
    return 0;
}

int PulsePlaybackSink::recover(int err) { return err == -EPIPE || err == -EINTR ? 0 : err; }

long PulsePlaybackSink::delay() {
    std::shared_ptr<PulsePlayback> pb = get();
    return pb ? pb->delay_frames() : 0;
}

void PulsePlaybackSink::drop() {
    std::shared_ptr<PulsePlayback> pb = get();
    if (!pb) return;
    pb->drop();
    fresh_ = true;
    progress_ = std::chrono::steady_clock::now();
}

void PulsePlaybackSink::drain() {
    std::shared_ptr<PulsePlayback> pb = get();
    if (!pb) return;
    double ms = 0;
    delay_ms(ms);
    pb->drain(static_cast<int>(std::min(2000.0, ms + 500.0)));
    fresh_ = true;
    progress_ = std::chrono::steady_clock::now();
}

void PulsePlaybackSink::close() {
    std::shared_ptr<PulsePlayback> pb;
    {
        std::lock_guard<std::mutex> g(mu_);
        pb = std::move(pb_);
        pb_.reset();
    }
    if (pb) pb->close();
}

bool PulsePlaybackSink::is_open() const { return get() != nullptr; }

bool PulsePlaybackSink::ended() const {
    std::shared_ptr<PulsePlayback> pb = get();
    return pb && pb->ended();
}

int PulsePlaybackSink::end_code() const {
    std::shared_ptr<PulsePlayback> pb = get();
    return pb ? pb->end_code() : 0;
}

uint64_t PulsePlaybackSink::underruns() const {
    std::shared_ptr<PulsePlayback> pb = get();
    return pb ? pb->underruns() : 0;
}

bool PulsePlaybackSink::delay_ms(double &ms) const {
    ms = 0;
    std::shared_ptr<PulsePlayback> pb = get();
    uint32_t rate;
    {
        std::lock_guard<std::mutex> g(mu_);
        rate = rate_;
    }
    if (!pb || rate == 0) return false;
    const long d = pb->delay_frames();
    ms = 1000.0 * static_cast<double>(d) / rate;
    return true;
}

}  // namespace vinyl
