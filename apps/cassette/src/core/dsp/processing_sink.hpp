/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: ProcessingSink, a FrameSink adapter between the capture thread and the recording sink (the ring).
 * On the capture thread, on a private copy of each block (the caller's buffer is never written), in this order:
 *   1. DC blocker            (on by default)
 *   2. software gain "SOFT"  (0..+24 dB, 0 by default)
 *   3. 80 Hz high-pass       (off by default, D18)
 *   4. peak limiter          (off by default, D18)
 *   5. back to S16_LE / S24_3LE: rounding, saturation at full scale, clip_count() counts saturated samples
 *   6. the meter taps the processed frames (what is recorded)
 *   7. the processed frames go to the wrapped sink.
 * The work runs in float on chunks of at most `chunk_frames` frames, so any block size works; a larger block is
 * passed to the wrapped sink in several writes, and processing stops at the first chunk the wrapped sink does not
 * fully accept. write() returns the sum of what the wrapped sink accepted (unchanged for one chunk).
 * Settings are atomics: the UI thread changes them at any time without locks; the capture thread picks them up at
 * the next chunk (a filter turned on starts from a cleared state). Unsupported formats (bits other than 16 or 24)
 * pass to the wrapped sink unchanged and are not metered. All buffers are allocated in the constructor; write() never
 * allocates. LVGL-free.
 */
#pragma once

#include "../frame_sink.hpp"
#include "dc_blocker.hpp"
#include "gain.hpp"
#include "highpass.hpp"
#include "limiter.hpp"
#include "meter.hpp"

#include <atomic>
#include <vector>

namespace cassette {

class ProcessingSink final : public FrameSink {
public:
    static constexpr size_t kDefaultChunkFrames = 2048;

    ProcessingSink(const AudioFormat &fmt, FrameSink *next, size_t chunk_frames = kDefaultChunkFrames);

    size_t write(const void *frames, size_t nframes) override;

    /* UI thread (any thread), lock-free. */
    void set_dc_blocker(bool on) { dc_on_.store(on, std::memory_order_relaxed); }
    void set_highpass(bool on) { hpf_on_.store(on, std::memory_order_relaxed); }
    void set_limiter(bool on) { lim_on_.store(on, std::memory_order_relaxed); }
    void set_soft_gain_db(double db) { gain_.set_db(db); }
    bool dc_blocker() const { return dc_on_.load(std::memory_order_relaxed); }
    bool highpass() const { return hpf_on_.load(std::memory_order_relaxed); }
    bool limiter() const { return lim_on_.load(std::memory_order_relaxed); }
    float soft_gain_db() const { return gain_.db(); }
    uint64_t clip_count() const { return clips_.load(std::memory_order_relaxed); }
    MeterSnapshot levels() const { return meter_.snapshot(); }
    const Meter &meter() const { return meter_; }
    const AudioFormat &format() const { return fmt_; }

private:
    AudioFormat fmt_;
    FrameSink *next_;
    size_t chunk_;
    size_t bpf_;
    std::vector<uint8_t> bytes_;
    std::vector<float> work_;
    DcBlocker dc_;
    SoftGain gain_;
    HighPass hpf_;
    Limiter lim_;
    Meter meter_;
    std::atomic<bool> dc_on_{true}, hpf_on_{false}, lim_on_{false};
    bool dc_was_ = true, hpf_was_ = false, lim_was_ = false;
    std::atomic<uint64_t> clips_{0};
};

}  // namespace cassette
