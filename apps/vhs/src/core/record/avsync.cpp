/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: A/V sync with the audio clock as the master (see avsync.hpp).
 */
#include "avsync.hpp"

#include <cmath>

namespace vhs {

void AvSync::pair(std::int64_t mono_ns, std::uint64_t position)
{
    if (!has_audio()) return;
    if (!pairs_.empty() && pairs_.back().provisional) pairs_.pop_back();
    if (!pairs_.empty() && mono_ns <= pairs_.back().ns) return;
    pairs_.push_back(Pair{mono_ns, static_cast<double>(position), false});
    while (pairs_.size() > 2 && mono_ns - pairs_.front().ns > 30LL * 1000000000LL) pairs_.pop_front(); /* the last 30 s */
    fit();
}

void AvSync::seed(std::int64_t mono_ns, std::uint64_t position)
{
    if (!has_audio() || !pairs_.empty()) return;
    pairs_.push_back(Pair{mono_ns, static_cast<double>(position), true});
    fit();
}

void AvSync::fit()
{
    ref_ns_ = pairs_.back().ns;
    if (pairs_.size() < 2 || pairs_.back().ns - pairs_.front().ns < 500000000LL) {
        /* one pair (or too close together): the nominal rate through the newest */
        a_ = pairs_.back().pos;
        b_ = rate_;
        return;
    }
    /* least squares of pos against t (s, relative to the newest pair) */
    double st = 0, sp = 0, stt = 0, stp = 0;
    const double n = static_cast<double>(pairs_.size());
    for (const Pair &p : pairs_) {
        const double t = static_cast<double>(p.ns - ref_ns_) * 1e-9;
        st += t;
        sp += p.pos;
        stt += t * t;
        stp += t * p.pos;
    }
    const double den = n * stt - st * st;
    if (std::fabs(den) < 1e-12) {
        a_ = pairs_.back().pos;
        b_ = rate_;
        return;
    }
    b_ = (n * stp - st * sp) / den;
    a_ = (sp - b_ * st) / n;
    /* a clock that is off by more than 1 % is not believed: the nominal rate */
    if (b_ < rate_ * 0.99 || b_ > rate_ * 1.01) {
        a_ = pairs_.back().pos;
        b_ = rate_;
    }
}

double AvSync::position_at(std::int64_t mono_ns) const
{
    return a_ + b_ * static_cast<double>(mono_ns - ref_ns_) * 1e-9;
}

double AvSync::fitted_rate() const
{
    return pairs_.empty() ? rate_ : b_;
}

void AvSync::start(std::int64_t t0_us)
{
    started_ = true;
    t0_us_ = t0_us;
    if (has_audio() && !pairs_.empty()) start_pos_ = static_cast<std::int64_t>(std::llround(position_at(t0_us * 1000)));
}

std::int64_t AvSync::video_ms(std::int64_t ts_us) const
{
    if (!has_audio() || pairs_.empty()) return (ts_us - t0_us_ + 500) / 1000;
    const double samples = position_at(ts_us * 1000) - static_cast<double>(start_pos_);
    return static_cast<std::int64_t>(std::llround(samples * 1000.0 / rate_));
}

std::int64_t AvSync::audio_index(std::uint64_t position) const
{
    return static_cast<std::int64_t>(position) - start_pos_;
}

} // namespace vhs
