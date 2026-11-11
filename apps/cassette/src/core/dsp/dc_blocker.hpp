/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: DC blocker, a first-order high-pass  y[n] = x[n] - x[n-1] + R * y[n-1]  per channel, with
 * R = 1 - 2*pi*fc/rate and fc = 3.5 Hz by default (R = 0.99954 at 48 kHz). The -3 dB point sits near 3.5 Hz, the
 * response at 50 Hz is -0.02 dB and at 1 kHz under 0.001 dB: no audible change on normal signals; a constant offset
 * decays with a 45 ms time constant. On by default in ProcessingSink. State in double. Up to kMaxChannels channels
 * (extra channels pass through). No allocation. LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cassette {

class DcBlocker {
public:
    static constexpr int kMaxChannels = 8;
    static constexpr double kDefaultCutoffHz = 3.5;

    explicit DcBlocker(uint32_t rate = 48000, uint16_t channels = 1, double cutoff_hz = kDefaultCutoffHz);
    void reset();
    /* interleaved normalised float samples, in place */
    void process(float *x, size_t nframes);
    double coefficient() const { return r_; }

private:
    uint16_t ch_;
    double r_;
    double x1_[kMaxChannels];
    double y1_[kMaxChannels];
};

}  // namespace cassette
