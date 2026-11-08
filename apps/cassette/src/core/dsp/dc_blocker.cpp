/* SPDX-License-Identifier: MIT */
#include "dc_blocker.hpp"

#include <cmath>

namespace cassette {

DcBlocker::DcBlocker(uint32_t rate, uint16_t channels, double fc) : ch_(channels) {
    const double pi = 3.14159265358979323846;
    r_ = rate ? 1.0 - 2.0 * pi * fc / double(rate) : 1.0;
    if (r_ < 0.0) r_ = 0.0;
    reset();
}

void DcBlocker::reset() {
    for (int c = 0; c < kMaxChannels; ++c) x1_[c] = y1_[c] = 0.0;
}

void DcBlocker::process(float *x, size_t nframes) {
    const uint16_t n = ch_ < kMaxChannels ? ch_ : uint16_t(kMaxChannels);
    for (size_t f = 0; f < nframes; ++f) {
        float *fr = x + f * ch_;
        for (uint16_t c = 0; c < n; ++c) {
            const double in = fr[c];
            const double y = in - x1_[c] + r_ * y1_[c];
            x1_[c] = in;
            y1_[c] = y;
            fr[c] = float(y);
        }
    }
}

}  // namespace cassette
