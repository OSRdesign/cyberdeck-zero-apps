/* SPDX-License-Identifier: MIT */
#include "pulse_pcm.hpp"

#include <cerrno>
#include <cstring>

namespace cassette {

int pulse_end_errno(int e) {
    switch (e) {
    case kPulseKilled:
    case kPulseNoDevice:
    case kPulseMoved:
    case kPulseDisconnected: return -ENODEV;
    default: return -EIO;
    }
}

PulseCapturePcm::PulseCapturePcm(std::shared_ptr<PulseCapture> stream, int read_timeout_ms,
                                 std::shared_ptr<PulseLeadGate> gate)
    : s_(std::move(stream)), timeout_ms_(read_timeout_ms > 0 ? read_timeout_ms : 1), gate_(std::move(gate)) {
    if (s_) channels_ = s_->format().channels ? s_->format().channels : 1;
}

PulseCapturePcm::~PulseCapturePcm() { close(); }

long PulseCapturePcm::readi(void *buf, size_t frames) {
    if (!s_) return -ENODEV;
    const long n = s_->read(buf, frames, timeout_ms_);
    if (n < 0) return pulse_end_errno(static_cast<int>(n));
    if (n == 0 || !gate_ || gate_->open.load(std::memory_order_acquire)) return n;
    /* the gate is closed: find the first non-zero sample; the frames before it are dropped */
    int16_t *p = static_cast<int16_t *>(buf);
    const size_t total = static_cast<size_t>(n) * channels_;
    size_t i = 0;
    while (i < total && p[i] == 0) ++i;
    if (i == total) { /* all zeros: nothing real yet (the caller naps and asks again) */
        gate_->dropped_frames.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        return 0;
    }
    const size_t zero_frames = i / channels_;
    if (zero_frames) {
        gate_->dropped_frames.fetch_add(zero_frames, std::memory_order_relaxed);
        std::memmove(buf, p + zero_frames * channels_, (static_cast<size_t>(n) - zero_frames) * channels_ * sizeof(int16_t));
    }
    gate_->by_audio.store(true, std::memory_order_relaxed);
    gate_->open.store(true, std::memory_order_release);
    return n - static_cast<long>(zero_frames);
}

int PulseCapturePcm::prepare() {
    if (!s_ || s_->ended()) return -ENODEV;
    return 0;
}

void PulseCapturePcm::close() {
    if (s_) s_->close();
}

OpenResult pulse_open_result(std::shared_ptr<PulseCapture> stream, int read_timeout_ms,
                             std::shared_ptr<PulseLeadGate> gate) {
    OpenResult r;
    if (!stream) {
        r.status = OpenStatus::NoCard;
        r.err = -ENODEV;
        r.message = "No sound server stream";
        return r;
    }
    const PulseStreamFormat f = stream->format();
    r.status = OpenStatus::Ok;
    r.via_server = true;
    r.device = stream->request().source;
    r.hw.rate = f.rate;
    r.hw.channels = f.channels;
    r.hw.format = SampleFormat::S16_LE;
    r.hw.period_frames = static_cast<uint32_t>(pulse_bytes_to_frames(f.granted.fragsize == kPulseDefault ? 0 : f.granted.fragsize, f.channels));
    r.hw.buffer_frames = static_cast<uint32_t>(stream->stats().ring_frames);
    r.conv = Conversion::None;
    r.format.rate = f.rate;
    r.format.channels = static_cast<uint16_t>(f.channels);
    r.format.bits = 16;
    r.pcm.reset(new PulseCapturePcm(std::move(stream), read_timeout_ms, std::move(gate)));
    return r;
}

}  // namespace cassette
