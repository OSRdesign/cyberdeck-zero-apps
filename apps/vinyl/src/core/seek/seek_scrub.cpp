/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl seek bar: tap and drag (see seek_scrub.hpp).
 */
#include "seek_scrub.hpp"

#include <cmath>

namespace vinyl {

double seek_fraction(double x, double bar_x, double bar_w) {
    if (!(bar_w > 0)) return 0.0;
    const double f = (x - bar_x) / bar_w;
    return f < 0 ? 0.0 : (f > 1 ? 1.0 : f);
}

void SeekScrub::press(double x) {
    active_ = true;
    press_x_ = last_x_ = x;
    target_ = seek_fraction(x, bar_x_, bar_w_);
}

void SeekScrub::move(double x) {
    if (!active_) return;
    last_x_ = x;
    target_ = seek_fraction(x, bar_x_, bar_w_);
}

bool SeekScrub::release(double x, double &frac) {
    if (!active_) return false;
    move(x);
    active_ = false;
    frac = target_;
    return true;
}

bool SeekScrub::dragged(double slop) const { return std::fabs(last_x_ - press_x_) > slop; }

}  // namespace vinyl
