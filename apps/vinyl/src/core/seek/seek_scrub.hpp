/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl seek bar: tap and drag (the owner, 2026-11-13: "... so should the track timeline to seek a specific point").
 *
 * A press on the seek bar's touch area starts a scrub at the press's x; while pressed the target follows the finger
 * (only its x counts, clamped to the bar's ends); the release seeks once, to the target (a tap is a press and a release
 * where it started: the same seek). Nothing is applied while the finger moves (the decoders and the SD card), playback
 * runs on (or stays paused), and the view shows the target: the knob and the fill under the finger, the elapsed time
 * as the target time, the remaining time from it. A press lost without a release (the touch area deleted, the page
 * changed) cancels: no seek. LVGL-free; tests/test_transport.cpp drives it with fake pointer events.
 */
#pragma once

#include <cstdint>

namespace vinyl {

/* x on a bar from bar_x, bar_w px wide -> 0..1 */
double seek_fraction(double x, double bar_x, double bar_w);

class SeekScrub {
public:
    /* the bar on the screen (the track's x and width; the touch area may be larger: x is clamped to the bar) */
    void set_bar(double x, double w) {
        bar_x_ = x;
        bar_w_ = w > 1 ? w : 1;
    }
    void press(double x);
    void move(double x);
    /* true with the fraction to seek to; false when no scrub was on */
    bool release(double x, double &frac);
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    double target() const { return target_; }
    /* a drag: the finger moved more than `slop` px from the press (a tap otherwise; both seek on release) */
    bool dragged(double slop = 4) const;
    /* the time the view shows while scrubbing: the target of a track `total_ms` long */
    uint64_t target_ms(uint64_t total_ms) const { return static_cast<uint64_t>(target_ * static_cast<double>(total_ms) + 0.5); }

private:
    double bar_x_ = 0, bar_w_ = 1;
    double press_x_ = 0, last_x_ = 0;
    double target_ = 0;
    bool active_ = false;
};

}  // namespace vinyl
