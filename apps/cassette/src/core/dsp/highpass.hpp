/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: optional 80 Hz second-order Butterworth high-pass (Q = 1/sqrt(2), the bilinear "Audio EQ Cookbook"
 * biquad, transposed direct form II, double state per channel) against handling noise and wind. OFF by default in
 * ProcessingSink (D18). -3 dB at the cutoff, -12 dB/octave below it. Up to kMaxChannels channels (extra channels
 * pass through). No allocation. LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cassette {

class HighPass {
public:
    static constexpr int kMaxChannels = 8;
    static constexpr double kDefaultCutoffHz = 80.0;

    explicit HighPass(uint32_t rate = 48000, uint16_t channels = 1, double cutoff_hz = kDefaultCutoffHz);
    void reset();
    void process(float *x, size_t nframes);

private:
    uint16_t ch_;
    double b0_, b1_, b2_, a1_, a2_;
    double z1_[kMaxChannels];
    double z2_[kMaxChannels];
};

}  // namespace cassette
