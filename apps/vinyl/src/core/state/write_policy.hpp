/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl write policy (report 060 finding 3, P2a): when a state file is written, so a board's SD card is not rewritten
 * every few seconds while music plays (0.1.0 wrote the whole queue to state.json every 5 s: up to 450 kB a write).
 *
 *   an event       a track change, pause, stop, exit, a settings or queue change, a folder opened: written at the next
 *                  check (the controller checks every 100 ms), and at once at exit
 *   progress       the position moved while playing: written at most once a minute (kProgressIntervalMs)
 *
 * The same policy for state.json and for positions.json (core/state/positions.hpp). Pure (the clock is the caller's
 * monotonic ms): tests/test_settings_state.cpp runs it with a fake clock and a fake file that counts writes. LVGL-free.
 */
#pragma once

#include <cstdint>

namespace vinyl {

class WritePolicy {
public:
    static constexpr uint64_t kProgressIntervalMs = 60000;

    /* something the file must hold now */
    void event() { event_ = true; }
    /* the position moved (while playing) */
    void progress() { progress_ = true; }
    /* a write is due at `now` */
    bool due(uint64_t now_ms) const {
        if (event_) return true;
        return progress_ && (!wrote_once_ || now_ms - last_ms_ >= kProgressIntervalMs);
    }
    /* the file was written at `now` (whatever made it due) */
    void wrote(uint64_t now_ms) {
        event_ = progress_ = false;
        last_ms_ = now_ms;
        wrote_once_ = true;
    }
    bool pending() const { return event_ || progress_; }

private:
    bool event_ = false, progress_ = false, wrote_once_ = false;
    uint64_t last_ms_ = 0;
};

}  // namespace vinyl
