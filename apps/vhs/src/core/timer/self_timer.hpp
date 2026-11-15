/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the self-timer (D30, brief 6.7): Off / 3 / 5 / 10 s (settings self_timer_s). States Idle, Countdown(n), Fire:
 * start() begins the countdown, shown() is the big number over the picture (seconds left, rounded up), fire() is true
 * once at zero (then Idle), cancel() goes back to Idle and nothing is taken. The controller does the rest: the stream
 * switch at the start (Light: the Photo size, so zero acts at once), the photo and its flash at zero (a video's start
 * comes with M3). Pure (the time is passed in), unit-tested. LVGL-free.
 */
#pragma once

#include <cstdint>

namespace vhs {

enum class TimerKind { Photo, Video };

class SelfTimer {
public:
    void start(int seconds, std::int64_t now_ms, TimerKind kind = TimerKind::Photo)
    {
        total_ = seconds;
        end_ms_ = now_ms + static_cast<std::int64_t>(seconds) * 1000;
        kind_ = kind;
        active_ = seconds > 0;
    }
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    int total() const { return total_; }
    TimerKind kind() const { return kind_; }
    /* The number shown at now_ms: the seconds left rounded up (3, 2, 1); 0 when idle. */
    int shown(std::int64_t now_ms) const
    {
        if (!active_) return 0;
        const std::int64_t left = end_ms_ - now_ms;
        if (left <= 0) return 0;
        return static_cast<int>((left + 999) / 1000);
    }
    /* True once, when the countdown reaches zero (it is Idle afterwards). */
    bool fire(std::int64_t now_ms)
    {
        if (!active_ || now_ms < end_ms_) return false;
        active_ = false;
        return true;
    }

private:
    bool active_ = false;
    int total_ = 0;
    std::int64_t end_ms_ = 0;
    TimerKind kind_ = TimerKind::Photo;
};

} // namespace vhs
