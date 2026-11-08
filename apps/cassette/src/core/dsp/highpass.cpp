/* SPDX-License-Identifier: MIT */
#include "highpass.hpp"

#include <cmath>

namespace cassette {

HighPass::HighPass(uint32_t rate, uint16_t channels, double fc) : ch_(channels) {
    const double pi = 3.14159265358979323846;
    const double fs = rate ? double(rate) : 48000.0;
    if (fc >= fs * 0.45) fc = fs * 0.45;
    const double w0 = 2.0 * pi * fc / fs;
    const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * std::sqrt(0.5));
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 + cw) / 2.0 / a0;
    b1_ = -(1.0 + cw) / a0;
    b2_ = b0_;
    a1_ = -2.0 * cw / a0;
    a2_ = (1.0 - alpha) / a0;
    reset();
}

void HighPass::reset() {
    for (int c = 0; c < kMaxChannels; ++c) z1_[c] = z2_[c] = 0.0;
}

void HighPass::process(float *x, size_t nframes) {
    const uint16_t n = ch_ < kMaxChannels ? ch_ : uint16_t(kMaxChannels);
    for (size_t f = 0; f < nframes; ++f) {
        float *fr = x + f * ch_;
        for (uint16_t c = 0; c < n; ++c) {
            const double in = fr[c];
            const double y = b0_ * in + z1_[c];
            z1_[c] = b1_ * in - a1_ * y + z2_[c];
            z2_[c] = b2_ * in - a2_ * y;
            fr[c] = float(y);
        }
    }
}

}  // namespace cassette
