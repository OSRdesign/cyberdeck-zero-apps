/* SPDX-License-Identifier: MIT */
#include "limiter.hpp"

#include <cmath>

namespace cassette {

Limiter::Limiter(uint32_t rate, uint16_t channels, float ceiling_db, double release_ms) : ch_(channels) {
    if (ceiling_db > 0.0f) ceiling_db = 0.0f;
    ceiling_ = float(std::pow(10.0, double(ceiling_db) / 20.0));
    const double fs = rate ? double(rate) : 48000.0;
    release_ = float(std::exp(-1.0 / (fs * release_ms / 1000.0)));
}

float Limiter::gain_reduction_db() const { return float(-20.0 * std::log10(double(gain_))); }

void Limiter::process(float *x, size_t nframes) {
    if (ch_ == 0) return;
    for (size_t f = 0; f < nframes; ++f) {
        float *fr = x + f * ch_;
        float m = 0.0f;
        for (uint16_t c = 0; c < ch_; ++c) {
            const float a = std::fabs(fr[c]);
            if (a > m) m = a;
        }
        gain_ = 1.0f - (1.0f - gain_) * release_;
        if (m * gain_ > ceiling_) gain_ = ceiling_ / m;
        for (uint16_t c = 0; c < ch_; ++c) {
            float y = fr[c] * gain_;
            if (y > ceiling_) y = ceiling_; /* rounding guard: never above the ceiling */
            if (y < -ceiling_) y = -ceiling_;
            fr[c] = y;
        }
    }
}

}  // namespace cassette
