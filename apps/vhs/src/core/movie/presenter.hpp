/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the presenter and the playback clock (brief 10).
 *   Presenter  2-3 timed pictures from the movie thread; at the display refresh the UI takes the newest whose pts is at
 *              or before the clock (older ones go, counted as dropped: the screen was slower than the video). A full
 *              presenter makes the movie thread wait (it decodes ahead, never far).
 *   PlayClock  the monotonic clock of a movie without sound (or while the output failed): it runs while playing and
 *              stops on pause; set() for a seek. The time is passed in (unit-tested).
 *   DropWindow the drop rule's notice (brief 10: "after 2 s of drops", "This board drops frames at this size."): drops
 *              that keep coming (no gap over 600 ms) for 2 s make it fire, once per movie.
 * LVGL-free.
 */
#pragma once

#include "live/pipeline.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace vhs {

struct TimedPicture {
    std::shared_ptr<Picture> pic;
    std::int64_t pts_ms = 0;
};

class Presenter {
public:
    explicit Presenter(std::size_t cap = 3) : cap_(cap) {}
    bool full() const;
    std::size_t size() const;
    void push(TimedPicture p);
    /* The newest picture due at clock_ms (nullptr: none due); the older due ones are dropped. */
    std::shared_ptr<Picture> pick(std::int64_t clock_ms, std::int64_t *pts_ms = nullptr);
    /* The next picture's pts (-1: none queued). */
    std::int64_t next_pts() const;
    void clear();
    std::uint64_t dropped() const;

private:
    mutable std::mutex m_;
    std::deque<TimedPicture> q_;
    std::size_t cap_;
    std::uint64_t dropped_ = 0;
};

class PlayClock {
public:
    void set(std::int64_t pos_ms, std::int64_t now_ms) { base_ = pos_ms; since_ = now_ms; }
    void pause(std::int64_t now_ms)
    {
        if (!running_) return;
        base_ = at(now_ms);
        running_ = false;
    }
    void resume(std::int64_t now_ms)
    {
        if (running_) return;
        since_ = now_ms;
        running_ = true;
    }
    std::int64_t at(std::int64_t now_ms) const { return running_ ? base_ + (now_ms - since_) : base_; }
    bool running() const { return running_; }

private:
    std::int64_t base_ = 0, since_ = 0;
    bool running_ = false;
};

class DropWindow {
public:
    static constexpr std::int64_t kSpanMs = 2000, kGapMs = 600;
    /* A drop at now_ms: true once, when drops have kept coming for 2 s. */
    bool drop(std::int64_t now_ms)
    {
        if (fired_) return false;
        if (start_ < 0 || now_ms - last_ > kGapMs) start_ = now_ms;
        last_ = now_ms;
        if (now_ms - start_ >= kSpanMs) {
            fired_ = true;
            return true;
        }
        return false;
    }
    void reset()
    {
        start_ = last_ = -1;
        fired_ = false;
    }
    bool fired() const { return fired_; }

private:
    std::int64_t start_ = -1, last_ = -1;
    bool fired_ = false;
};

} // namespace vhs
