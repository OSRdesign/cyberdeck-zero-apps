/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: a simple peak limiter without look-ahead, OFF by default in ProcessingSink (D18).
 *
 * Behaviour: one gain for all channels (stereo image kept). For each frame the frame's largest |sample| times the
 * current gain is compared with the ceiling (-1 dBFS by default). Above it, the gain drops at once to exactly
 * ceiling/|sample| (instant attack: the output never exceeds the ceiling, so never full scale). Below it, the gain
 * recovers towards 1.0 exponentially with a 100 ms release time constant. Because there is no look-ahead, the
 * instant gain change on a transient is a small distortion; it is meant as a safety net after the software gain, not
 * as a mastering tool. Signals that stay under the ceiling pass unchanged once the gain has recovered.
 * gain_reduction_db() reports the current reduction for a UI. No allocation. LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cassette {

class Limiter {
public:
    static constexpr float kDefaultCeilingDb = -1.0f;
    static constexpr double kDefaultReleaseMs = 100.0;

    explicit Limiter(uint32_t rate = 48000, uint16_t channels = 1, float ceiling_db = kDefaultCeilingDb,
                     double release_ms = kDefaultReleaseMs);
    void reset() { gain_ = 1.0f; }
    void process(float *x, size_t nframes);
    float ceiling() const { return ceiling_; }
    float gain_reduction_db() const;

private:
    uint16_t ch_;
    float ceiling_;
    float release_;   /* per-frame recovery coefficient */
    float gain_ = 1.0f;
};

}  // namespace cassette
