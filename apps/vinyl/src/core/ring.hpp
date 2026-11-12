/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette core: single-producer single-consumer lock-free ring of PCM frames. The producer side is the capture
 * thread (FrameSink::write: never blocks, counts what it had to drop), the consumer side is the writer thread.
 * LVGL-free.
 */
#pragma once

#include "frame_sink.hpp"

#include <atomic>
#include <vector>

namespace vinyl {

class Ring final : public FrameSink {
public:
    /* seconds of audio the ring holds (8 by default: 768,000 bytes at 48 kHz mono S16). */
    explicit Ring(const AudioFormat &fmt, double seconds = 8.0);

    /* Producer thread. Accepts min(nframes, free) frames; the rest is counted as dropped. */
    size_t write(const void *frames, size_t nframes) override;
    /* Consumer thread. Copies up to max_frames frames out, returns the number copied. */
    size_t read(void *dst, size_t max_frames);

    /* Any thread. */
    size_t available() const; /* frames waiting */
    size_t capacity_frames() const { return cap_; }
    size_t frame_bytes() const { return fb_; }
    double fill_ratio() const { return cap_ ? static_cast<double>(available()) / static_cast<double>(cap_) : 0.0; }
    uint64_t frames_written() const { return w_.load(std::memory_order_acquire); } /* accepted, ever */
    uint64_t frames_read() const { return r_.load(std::memory_order_acquire); }
    uint64_t frames_dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    size_t fb_;
    size_t cap_;
    std::vector<unsigned char> buf_;
    alignas(64) std::atomic<uint64_t> w_{0};
    alignas(64) std::atomic<uint64_t> r_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
};

}  // namespace vinyl
