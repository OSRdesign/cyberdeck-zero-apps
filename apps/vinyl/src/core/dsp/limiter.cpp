/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl's look-ahead limiter (see limiter.hpp).
 */
#include "limiter.hpp"

#include <algorithm>
#include <cmath>

namespace vinyl {

void Limiter::init(uint32_t rate, double release_ms, double ceiling_db) {
    if (rate == 0) rate = 44100;
    d_ = std::max<size_t>(1, static_cast<size_t>(std::lround(rate * kLimiterLookaheadMs / 1000.0)));
    w_ = d_ + 1;
    ceil_ = static_cast<float>(std::pow(10.0, ceiling_db / 20.0));
    rel_inc_ = static_cast<float>(1.0 / (rate * release_ms / 1000.0));
    xl_.assign(w_, 0.0f);
    xr_.assign(w_, 0.0f);
    qv_.assign(w_, 1.0f);
    qi_.assign(w_, 0);
    rbuf_.assign(w_, 1.0f);
    reset();
}

void Limiter::reset() {
    std::fill(xl_.begin(), xl_.end(), 0.0f);
    std::fill(xr_.begin(), xr_.end(), 0.0f);
    std::fill(rbuf_.begin(), rbuf_.end(), 1.0f);
    rsum_ = static_cast<double>(w_);
    qh_ = qn_ = 0;
    rprev_ = 1.0f;
    last_gain_ = 1.0f;
    n_ = 0;
    stats_ = Stats();
}

void Limiter::process(float *io, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        const float l = io[2 * f], r = io[2 * f + 1];
        const size_t slot = static_cast<size_t>(n_ % w_);
        /* the frame leaving is the one d_ frames old: in the slot after ours (the ring holds w_ = d_ + 1 frames) */
        const size_t old = static_cast<size_t>((n_ + 1) % w_);
        const float ol = xl_[old], orr = xr_[old];
        xl_[slot] = l;
        xr_[slot] = r;
        /* the gain this frame needs, into the sliding minimum over the last w_ frames */
        const float pk = std::max(std::fabs(l), std::fabs(r));
        const float q = pk > ceil_ ? ceil_ / pk : 1.0f;
        while (qn_ > 0 && qv_[(qh_ + qn_ - 1) % w_] >= q) --qn_;
        qv_[(qh_ + qn_) % w_] = q;
        qi_[(qh_ + qn_) % w_] = n_;
        ++qn_;
        if (qi_[qh_] + w_ <= n_) { /* the oldest left the window */
            qh_ = (qh_ + 1) % w_;
            --qn_;
        }
        const float m = qv_[qh_];
        /* release: the gain rises at most rel_inc_ a frame (it falls at once: the look-ahead and the boxcar ramp it) */
        const float rg = std::min(m, rprev_ + rel_inc_);
        rprev_ = rg;
        const size_t rs = static_cast<size_t>(n_ % w_);
        rsum_ += static_cast<double>(rg) - static_cast<double>(rbuf_[rs]);
        rbuf_[rs] = rg;
        const float g = static_cast<float>(rsum_ / static_cast<double>(w_));
        last_gain_ = g;
        /* the frame d_ old gets the gain of now: its required gain is in the window of the minimum */
        io[2 * f] = ol * g;
        io[2 * f + 1] = orr * g;
        stats_.peak_in = std::max(stats_.peak_in, pk);
        stats_.peak_out = std::max(stats_.peak_out, std::max(std::fabs(io[2 * f]), std::fabs(io[2 * f + 1])));
        ++stats_.frames;
        if (g < 0.9999f) {
            ++stats_.limited;
            stats_.red_sum_db += -20.0 * std::log10(static_cast<double>(g));
            stats_.min_gain = std::min(stats_.min_gain, g);
        }
        ++n_;
    }
}

void Limiter::flush(float *out) {
    std::vector<float> z(2 * d_, 0.0f);
    process(z.data(), d_);
    std::copy(z.begin(), z.end(), out);
    reset();
}

}  // namespace vinyl
