/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl's look-ahead limiter (report 060 3e; owner request of 2026-11-13: a software gain boost of 0 to +4 dB "and
 * limit", because the Pi 3A+ / 4B jack at its real maximum is much quieter than a USB DAC):
 *   - linked stereo (one gain for both channels), 5 ms look-ahead, ceiling -1.0 dBFS sample peak (room for the DAC's
 *     inter-sample overshoot), release 150 ms;
 *   - the gain at a frame is the boxcar (the look-ahead long) average of a release-limited sliding minimum of the
 *     per-frame required gains, so it never exceeds the gain the frame needs: after the limiter no sample is above the
 *     ceiling, and the gain moves in ramps, not steps;
 *   - the output is the input delayed by `delay()` frames (zeros first), one frame out per frame in: the engine's frame
 *     counts do not change. flush() gives the last delay() frames at the end of a stream; reset() forgets everything.
 * The engine runs it only when the net digital gain is above 0 dB (it latches on per open stream: the delay line is
 * then in the signal until the stream closes). Float, interleaved stereo, LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vinyl {

/* the software gain boost: 0 to +4 dB in whole dB (owner, 2026-11-13) */
constexpr int kMaxBoostDb = 4;
constexpr double kLimiterCeilingDb = -1.0;
constexpr double kLimiterLookaheadMs = 5.0;
constexpr double kLimiterReleaseMs = 150.0;

class Limiter {
public:
    explicit Limiter(uint32_t rate = 44100) { init(rate); }
    void init(uint32_t rate);
    void reset();
    /* The frames of `io` (interleaved L R, in place): replaced by the limited frames delayed by delay(). */
    void process(float *io, size_t frames);
    /* The delay() frames still inside (the end of a stream), limited, into `out` (2 * delay() floats); resets. */
    void flush(float *out);
    size_t delay() const { return d_; }
    float ceiling() const { return ceil_; }
    /* the gain the last processed frame got (1 = untouched) */
    float last_gain() const { return last_gain_; }

private:
    size_t d_ = 0;          /* look-ahead frames */
    size_t w_ = 1;          /* d_ + 1: the window of the minimum and of the boxcar */
    float ceil_ = 0.8913f;
    float rel_inc_ = 0.0f;  /* the release: the most the gain may rise a frame */
    std::vector<float> xl_, xr_;            /* the delay line (ring of w_) */
    std::vector<float> qv_;                 /* the monotonic deque's values (ring) and indices */
    std::vector<uint64_t> qi_;
    size_t qh_ = 0, qn_ = 0;
    std::vector<float> rbuf_;               /* the last w_ release-limited gains (ring) */
    double rsum_ = 0;
    float rprev_ = 1.0f;
    float last_gain_ = 1.0f;
    uint64_t n_ = 0;                        /* frames in so far */
};

}  // namespace vinyl
