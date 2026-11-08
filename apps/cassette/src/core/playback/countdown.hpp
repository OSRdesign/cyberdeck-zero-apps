/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the record countdown (record timer Off, 3, 10 or 20 s). REC with a timer set starts it instead of the take;
 * at zero the take starts. Runs on the Clock seam (a fake clock in the tests); tick() is called from the UI timer.
 * No sound. LVGL-free.
 */
#pragma once

#include "fileops.hpp"

#include <cstdint>

namespace cassette {

/* The settings values. Off means no countdown: REC starts the take at once. */
enum class RecordTimer : unsigned { Off = 0, S3 = 3, S10 = 10, S20 = 20 };
inline unsigned timer_seconds(RecordTimer t) { return static_cast<unsigned>(t); }

class Countdown {
public:
    explicit Countdown(Clock &clock) : clock_(clock) {}

    /* Starts (or restarts) a countdown of `seconds`; 0 (Off) starts nothing and returns false. */
    bool start(unsigned seconds);
    void cancel();
    bool active() const { return active_; }
    unsigned total_seconds() const { return total_ms_ / 1000; }

    struct Remaining {
        unsigned seconds = 0;  /* the whole number shown: 3, 2, 1 (rounded up; 0 only when not active) */
        float fraction = 0.f;  /* the ring: 1.0 at the start down to 0.0 at zero */
    };
    Remaining remaining() const;

    enum class Tick { Idle, Running, Fired };
    /* Fired exactly once, on the first tick at or after zero; Idle when not counting. */
    Tick tick();

private:
    uint64_t left_ms() const;
    Clock &clock_;
    bool active_ = false;
    uint64_t start_ms_ = 0;
    uint32_t total_ms_ = 0;
};

} // namespace cassette
