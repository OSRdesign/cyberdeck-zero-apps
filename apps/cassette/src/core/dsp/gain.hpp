/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: the software gain "SOFT", 0 to +24 dB in 0.5 dB steps, added on top of the sound card's own level.
 * The setting is stored as a step count (half dB) in an atomic, so the UI thread can change it while the capture
 * thread applies it; the multiplier 10^(dB/20) is recomputed on the audio thread only when the step changes.
 *
 * Two ways to apply it:
 *   - process(float*): multiplies normalised float samples (used inside ProcessingSink, which saturates once at the
 *     end of its chain so the optional limiter can catch what the gain pushed over full scale);
 *   - apply(pcm): in place on S16_LE or S24_3LE, rounding and saturating at full scale, counting saturated samples.
 * set_db() rounds to the nearest 0.5 dB and clamps to 0..24 (so 10.8 dB becomes 11.0 dB).
 * No allocation. LVGL-free.
 */
#pragma once

#include "../frame_sink.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cassette {

class SoftGain {
public:
    static constexpr int kMaxSteps = 48; /* 24 dB */
    static constexpr float kStepDb = 0.5f;

    /* Any thread. */
    void set_db(double db);
    void set_steps(int half_db_steps);
    int steps() const { return steps_.load(std::memory_order_relaxed); }
    float db() const { return float(steps()) * kStepDb; }
    uint64_t clip_count() const { return clips_.load(std::memory_order_relaxed); }
    void reset_clips() { clips_.store(0, std::memory_order_relaxed); }

    static float factor_for_steps(int steps);

    /* Audio thread. `nsamples` = frames * channels. */
    void process(float *samples, size_t nsamples);
    /* Audio thread, in place; returns the samples saturated in this call (also added to clip_count()).
     * Unsupported formats are left untouched. */
    size_t apply(void *frames, size_t nframes, const AudioFormat &fmt);

    /* Audio thread: the multiplier currently in use (refreshes it from the atomic step). */
    float factor();

private:
    std::atomic<int> steps_{0};
    std::atomic<uint64_t> clips_{0};
    int cached_steps_ = 0;
    float cached_factor_ = 1.0f;
};

}  // namespace cassette
