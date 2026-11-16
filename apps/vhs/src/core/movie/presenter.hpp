/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the presenter and the playback clock (brief 10).
 *   Presenter  2-3 timed pictures from the movie thread; at the display refresh the UI takes the oldest whose pts is
 *              at or before the clock, so a picture a little late is still shown (G6); a due picture more than
 *              late_ms (two frame periods) behind the clock goes when a newer one is due too (counted as dropped: the
 *              screen was slower than the video). A full presenter makes the movie thread wait (it decodes ahead,
 *              never far).
 *   PlayClock  the picture's clock: monotonic, it runs while playing and stops on pause; set() for a seek. With sound
 *              it is steered toward the sound's heard position (steer(), G6): a big difference (a seek, the sound's
 *              join) moves it at once, a small one is taken out slowly (at most 5 % faster or slower), so the picture
 *              moves smoothly although the engine's position comes in steps. The time is passed in (unit-tested).
 *   DropWindow the drop rule's notice (brief 10: "after 2 s of drops", "This board drops frames at this size."): drops
 *              that keep coming (no gap over 600 ms) for 2 s make it fire; the toast then shows its 4 s, and only a new
 *              2 s streak after that fires again.
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
    /* how late a due picture may be and still be shown when a newer one is due too (the movie: two frame periods) */
    void set_late_ms(std::int64_t ms) { late_ms_ = ms; }
    bool full() const;
    std::size_t size() const;
    void push(TimedPicture p);
    /* The oldest picture due at clock_ms (nullptr: none due); due ones more than late_ms behind go first while a newer
     * one is due too. */
    std::shared_ptr<Picture> pick(std::int64_t clock_ms, std::int64_t *pts_ms = nullptr);
    /* The next picture's pts (-1: none queued). */
    std::int64_t next_pts() const;
    void clear();
    std::uint64_t dropped() const;

private:
    mutable std::mutex m_;
    std::deque<TimedPicture> q_;
    std::size_t cap_;
    std::int64_t late_ms_ = 67;
    std::uint64_t dropped_ = 0;
};

class PlayClock {
public:
    static constexpr std::int64_t kJumpMs = 300;     /* a bigger difference to the sound moves the clock at once */
    static constexpr double kHorizonMs = 1500;       /* a smaller one is taken out over about this long ... */
    static constexpr double kMaxSlew = 0.05;         /* ... at most 5 % faster or slower */
    void set(std::int64_t pos_ms, std::int64_t now_ms)
    {
        base_ = static_cast<double>(pos_ms);
        since_ = now_ms;
        rate_ = 1.0;
    }
    void pause(std::int64_t now_ms)
    {
        if (!running_) return;
        base_ = exact(now_ms);
        running_ = false;
    }
    void resume(std::int64_t now_ms)
    {
        if (running_) return;
        since_ = now_ms;
        running_ = true;
    }
    std::int64_t at(std::int64_t now_ms) const { return static_cast<std::int64_t>(exact(now_ms) + 0.5); }
    bool running() const { return running_; }
    double rate() const { return rate_; }
    /* Steers toward ref_ms (the sound's heard position at now_ms) while running: returns true when it jumped. */
    bool steer(std::int64_t ref_ms, std::int64_t now_ms)
    {
        if (!running_) return false;
        const double cur = exact(now_ms);
        const double err = static_cast<double>(ref_ms) - cur;
        since_ = now_ms;
        if (err > kJumpMs || err < -kJumpMs) {
            base_ = static_cast<double>(ref_ms);
            rate_ = 1.0;
            return true;
        }
        base_ = cur;
        double adj = err / kHorizonMs;
        if (adj > kMaxSlew) adj = kMaxSlew;
        if (adj < -kMaxSlew) adj = -kMaxSlew;
        rate_ = 1.0 + adj;
        return false;
    }

private:
    double exact(std::int64_t now_ms) const
    {
        return running_ ? base_ + static_cast<double>(now_ms - since_) * rate_ : base_;
    }
    double base_ = 0;
    std::int64_t since_ = 0;
    double rate_ = 1.0;
    bool running_ = false;
};

class DropWindow {
public:
    /* kQuietMs: the toast's lifetime (NoticeQueue, 4 s): drops while it shows do not count, and only a new 2 s streak
     * after it hid fires again (G6) */
    static constexpr std::int64_t kSpanMs = 2000, kGapMs = 600, kQuietMs = 4000;
    /* A drop at now_ms: true when drops have kept coming for 2 s (and the last notice is over). */
    bool drop(std::int64_t now_ms)
    {
        if (fired_at_ >= 0 && now_ms - fired_at_ < kQuietMs) {
            start_ = -1;
            return false;
        }
        if (start_ < 0 || now_ms - last_ > kGapMs) start_ = now_ms;
        last_ = now_ms;
        if (now_ms - start_ >= kSpanMs) {
            fired_at_ = now_ms;
            start_ = -1;
            ++fires_;
            return true;
        }
        return false;
    }
    void reset()
    {
        start_ = last_ = fired_at_ = -1;
        fires_ = 0;
    }
    bool fired() const { return fires_ > 0; }
    int fires() const { return fires_; }

private:
    std::int64_t start_ = -1, last_ = -1, fired_at_ = -1;
    int fires_ = 0;
};

} // namespace vhs
