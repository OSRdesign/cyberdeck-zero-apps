/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl hold tracker (see hold_tracker.hpp).
 */
#include "hold_tracker.hpp"

#include <cstdlib>

namespace vinyl {

void HoldTracker::begin(int target, int x, int y, uint64_t now) {
    active_ = true;
    swallow_ = false;
    target_ = target;
    x_ = x;
    y_ = y;
    start_ = now;
}

void HoldTracker::move(int x, int y) {
    if (!active_) return;
    if (std::abs(x - x_) > kSlop || std::abs(y - y_) > kSlop) active_ = false;
}

double HoldTracker::progress(uint64_t now) const {
    if (!active_) return 0;
    const double p = static_cast<double>(now - start_) / static_cast<double>(kFireMs);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

bool HoldTracker::poll(uint64_t now) {
    if (!active_ || now - start_ < kFireMs) return false;
    active_ = false;
    swallow_ = true;
    return true;
}

bool HoldTracker::take_swallow() {
    const bool s = swallow_;
    swallow_ = false;
    return s;
}

}  // namespace vinyl
