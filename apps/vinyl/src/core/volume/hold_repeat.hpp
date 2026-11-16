/*
 * SPDX-License-Identifier: MIT
 *
 * Hold to repeat for the volume group's "-" and "+" (report 060 3f, D24): a fine "Volume steps" mode has 121 or 601
 * levels over 60 dB, so a held button must keep stepping. The first step is the press itself (the caller's); after
 * kHoldDelayMs the repeat starts at 10 steps a second, and from kHoldFastMs of holding at 30 a second. Pure: the caller
 * passes a monotonic clock in ms (the app's 100 ms tick; the tests a fake clock) and gets the number of steps due.
 */
#pragma once

#include <cstdint>

namespace vinyl {

constexpr uint64_t kHoldDelayMs = 500;  /* held this long before the first repeat */
constexpr uint64_t kHoldFastMs = 2000;  /* held this long: the fast rate */
constexpr uint64_t kHoldSlowGapMs = 100; /* 10 a second */
constexpr uint64_t kHoldFastGapMs = 33;  /* about 30 a second */
constexpr int kHoldMaxPerPoll = 8;       /* after a stall the repeat never floods */

class HoldRepeat {
public:
    void start(int dir, uint64_t now_ms) {
        active_ = true;
        dir_ = dir;
        t0_ = now_ms;
        next_ = now_ms + kHoldDelayMs;
    }
    void stop() { active_ = false; }
    bool active() const { return active_; }
    int dir() const { return dir_; }
    /* the steps due at now_ms (each one a press in dir()) */
    int poll(uint64_t now_ms) {
        if (!active_) return 0;
        int n = 0;
        while (next_ <= now_ms && n < kHoldMaxPerPoll) {
            ++n;
            next_ += now_ms - t0_ >= kHoldFastMs ? kHoldFastGapMs : kHoldSlowGapMs;
        }
        if (next_ <= now_ms) next_ = now_ms + 1; /* the rest of a stall is dropped */
        return n;
    }

private:
    bool active_ = false;
    int dir_ = 0;
    uint64_t t0_ = 0, next_ = 0;
};

}  // namespace vinyl
