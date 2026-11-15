/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: A/V sync with the audio clock as the master (D12, brief 6.2). The audio capture gives a pair (CLOCK_MONOTONIC
 * ns, sample position) every 2 s; a straight line fitted over the pairs of the last 30 s maps any monotonic time onto
 * the audio timeline. t0 is the first video frame's timestamp (after the warm-up): its position on that line is the
 * scene's sample 0, audio before it is dropped, and every video frame's driver timestamp is mapped onto the same line,
 * so the video follows the audio clock (+0.046 % against monotonic on the CM969, P0-f). A dropped frame leaves a gap,
 * never a slide. Without audio: pts = ts - t0. Pure (unit-tested with synthetic clocks), used by the writer thread.
 * LVGL-free.
 */
#pragma once

#include <cstdint>
#include <deque>

namespace vhs {

class AvSync {
public:
    explicit AvSync(int rate = 0) : rate_(rate) {}
    bool has_audio() const { return rate_ > 0; }
    /* A clock pair from the audio capture (mono ns, the sample position then). */
    void pair(std::int64_t mono_ns, std::uint64_t position);
    /* A provisional pair (the first frame came before the audio clock's first pair: now and the samples received so
     * far); the next real pair replaces it. */
    void seed(std::int64_t mono_ns, std::uint64_t position);
    bool ready() const { return !has_audio() || !pairs_.empty(); }
    /* The scene starts at the first frame's timestamp (us). Audio: its position on the line is sample 0. */
    void start(std::int64_t t0_us);
    bool started() const { return started_; }
    /* A video frame's pts in ms from the scene's start. */
    std::int64_t video_ms(std::int64_t ts_us) const;
    /* The scene's sample index of the capture's sample `position` (negative: before t0, to drop). */
    std::int64_t audio_index(std::uint64_t position) const;
    /* The position (fractional samples) on the line at a monotonic time (ns). */
    double position_at(std::int64_t mono_ns) const;
    /* The fitted rate (samples per second of the monotonic clock); the nominal one with fewer than two pairs. */
    double fitted_rate() const;
    std::int64_t start_position() const { return start_pos_; }

private:
    struct Pair {
        std::int64_t ns;
        double pos;
        bool provisional;
    };
    int rate_;
    std::deque<Pair> pairs_;
    bool started_ = false;
    std::int64_t t0_us_ = 0;
    std::int64_t start_pos_ = 0;
    /* the line: pos = a + b * (ns - ref_ns) * 1e-9 */
    std::int64_t ref_ns_ = 0;
    double a_ = 0, b_ = 0;
    void fit();
};

} // namespace vhs
