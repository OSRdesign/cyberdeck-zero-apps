/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the disk guard and the split rule (brief 6.4, 6.5). Pure (unit-tested); the writer thread asks every second.
 *   guard  stop when the free space is under min_free_mb (default 500) or under 2 minutes at the rate written over the
 *          last 10 s; before REC, refuse under the floor; the display's "size written, time left" comes from the free
 *          space and the same rate
 *   split  cut at the next video frame when part_bytes + next_packet + 16 MiB would pass split_mib (default 4096 MiB)
 * LVGL-free.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace vhs {

class DiskGuard {
public:
    enum class Verdict { Ok, Floor, TwoMinutes };
    explicit DiskGuard(std::uint64_t min_free_bytes = 500ull << 20) : min_free_(min_free_bytes) {}
    static bool refuse_at_rec(std::uint64_t free_bytes, std::uint64_t min_free_bytes) { return free_bytes < min_free_bytes; }

    /* One reading: the free space and the bytes written so far. */
    Verdict step(std::int64_t now_ms, std::uint64_t free_bytes, std::uint64_t written)
    {
        samples_.push_back(Sample{now_ms, written});
        while (samples_.size() > 2 && now_ms - samples_.front().ms > 10000) samples_.pop_front();
        free_ = free_bytes;
        if (free_bytes < min_free_) return Verdict::Floor;
        const double r = rate();
        if (r > 0 && static_cast<double>(free_bytes) < r * 120.0) return Verdict::TwoMinutes;
        return Verdict::Ok;
    }
    /* Bytes per second over the last 10 s (0 until 2 s of readings). */
    double rate() const
    {
        if (samples_.size() < 2) return 0;
        const Sample &a = samples_.front(), &b = samples_.back();
        if (b.ms - a.ms < 2000 || b.written < a.written) return 0;
        return static_cast<double>(b.written - a.written) * 1000.0 / static_cast<double>(b.ms - a.ms);
    }
    /* Seconds left at that rate before the floor (-1 while the rate is unknown). */
    std::int64_t seconds_left() const
    {
        const double r = rate();
        if (r <= 0) return -1;
        return free_ > min_free_ ? static_cast<std::int64_t>(static_cast<double>(free_ - min_free_) / r) : 0;
    }
    std::uint64_t free_bytes() const { return free_; }

private:
    struct Sample {
        std::int64_t ms;
        std::uint64_t written;
    };
    std::uint64_t min_free_;
    std::uint64_t free_ = 0;
    std::deque<Sample> samples_;
};

/* The split rule (brief 6.5); split_bytes 0 = Split Off. */
inline bool split_due(std::uint64_t part_bytes, std::uint64_t next_packet, std::uint64_t split_bytes,
                      std::uint64_t margin = 16ull << 20)
{
    return split_bytes > 0 && part_bytes > 0 && part_bytes + next_packet + margin > split_bytes;
}

/* "1.4 GB", "62 MB", "820 KB" (canvas: "1.4 GB, 3 h 20 min left") */
std::string bytes_text(std::uint64_t bytes);
/* "3 h 20 min left", "12 min left", "40 s left"; "" when unknown (< 0) */
std::string time_left_text(std::int64_t seconds);
/* "00:12:34" */
std::string clock_text(std::int64_t ms);

} // namespace vhs
