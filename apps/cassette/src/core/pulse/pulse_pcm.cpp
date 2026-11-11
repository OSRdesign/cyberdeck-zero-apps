/* SPDX-License-Identifier: MIT */
#include "pulse_pcm.hpp"

#include <cerrno>

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

PulseCapturePcm::PulseCapturePcm(std::shared_ptr<PulseCapture> stream, int read_timeout_ms)
    : s_(std::move(stream)), timeout_ms_(read_timeout_ms > 0 ? read_timeout_ms : 1) {}

PulseCapturePcm::~PulseCapturePcm() { close(); }

long PulseCapturePcm::readi(void *buf, size_t frames) {
    if (!s_) return -ENODEV;
    const long n = s_->read(buf, frames, timeout_ms_);
    if (n >= 0) return n;
    return pulse_end_errno(static_cast<int>(n));
}

int PulseCapturePcm::prepare() {
    if (!s_ || s_->ended()) return -ENODEV;
    return 0;
}

void PulseCapturePcm::close() {
    if (s_) s_->close();
}

OpenResult pulse_open_result(std::shared_ptr<PulseCapture> stream, int read_timeout_ms) {
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
    r.pcm.reset(new PulseCapturePcm(std::move(stream), read_timeout_ms));
    return r;
}

}  // namespace cassette
