/* SPDX-License-Identifier: MIT */
#include "gain.hpp"

#include "pcm.hpp"

#include <cmath>

namespace cassette {

void SoftGain::set_db(double db) {
    if (!(db == db)) db = 0.0;
    set_steps(int(std::lround(db / double(kStepDb))));
}

void SoftGain::set_steps(int s) {
    if (s < 0) s = 0;
    if (s > kMaxSteps) s = kMaxSteps;
    steps_.store(s, std::memory_order_relaxed);
}

float SoftGain::factor_for_steps(int s) { return float(std::pow(10.0, double(s) * double(kStepDb) / 20.0)); }

float SoftGain::factor() {
    const int s = steps_.load(std::memory_order_relaxed);
    if (s != cached_steps_) {
        cached_steps_ = s;
        cached_factor_ = factor_for_steps(s);
    }
    return cached_factor_;
}

void SoftGain::process(float *x, size_t n) {
    const float g = factor();
    if (g == 1.0f) return;
    for (size_t i = 0; i < n; ++i) x[i] *= g;
}

size_t SoftGain::apply(void *frames, size_t nframes, const AudioFormat &fmt) {
    if (!pcm::supported(fmt.bits) || frames == nullptr) return 0;
    const double g = factor();
    if (g == 1.0) return 0;
    const int32_t hi = pcm::max_pos(fmt.bits), lo = pcm::min_neg(fmt.bits);
    const size_t bps = pcm::bytes_per_sample(fmt.bits);
    const size_t n = nframes * fmt.channels;
    uint8_t *p = static_cast<uint8_t *>(frames);
    size_t clipped = 0;
    for (size_t i = 0; i < n; ++i, p += bps) {
        const double y = std::nearbyint(double(pcm::read(p, fmt.bits)) * g);
        int32_t v;
        if (y > hi) { v = hi; ++clipped; }
        else if (y < lo) { v = lo; ++clipped; }
        else v = int32_t(y);
        pcm::write(p, fmt.bits, v);
    }
    if (clipped) clips_.fetch_add(clipped, std::memory_order_relaxed);
    return clipped;
}

}  // namespace cassette
