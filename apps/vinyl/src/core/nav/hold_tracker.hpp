/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl hold tracker (owner ruling 2026-11-14, B44: "on touch, 3sec is long get it down to 1.5 sec and as we did in others
 * apps (i.e markdown notes) show a progression bar and a message 'keep holding for options'"): a finger held on a Library
 * row opens its actions. Markdown Notes' HoldTracker with Vinyl's times: the cue (the bar and "Keep holding for options")
 * appears after kShowMs so a tap or a scroll never flashes it; after kFireMs the hold fires once; a move of more than kSlop
 * pixels, a release or a lost press gives it up. Pure (the caller's monotonic clock), tested in tests/test_lib_model.cpp.
 */
#pragma once

#include <cstdint>

namespace vinyl {

class HoldTracker {
public:
    static constexpr uint64_t kShowMs = 500, kFireMs = 1500;
    static constexpr int kSlop = 16;
    void begin(int target, int x, int y, uint64_t now);
    void move(int x, int y); /* beyond kSlop: given up */
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    int target() const { return target_; }
    bool visible(uint64_t now) const { return active_ && now - start_ >= kShowMs; }
    double progress(uint64_t now) const; /* 0..1 over kFireMs */
    /* true exactly once when the hold has lasted kFireMs; the click of the release is then to be swallowed */
    bool poll(uint64_t now);
    bool take_swallow();

private:
    bool active_ = false, swallow_ = false;
    int target_ = -1, x_ = 0, y_ = 0;
    uint64_t start_ = 0;
};

}  // namespace vinyl
