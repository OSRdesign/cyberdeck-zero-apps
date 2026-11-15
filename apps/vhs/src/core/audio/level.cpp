/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Sound-on lamp and the silence detector (see level.hpp).
 */
#include "level.hpp"

#include <cmath>

namespace vhs {

void SoundLevel::reset(int rate, int channels)
{
    rate_ = rate > 0 ? rate : 16000;
    channels_ = channels > 0 ? channels : 1;
    win_target_ = static_cast<std::uint64_t>(rate_) * kWindowMs / 1000;
    win_frames_ = 0;
    win_sum_ = 0;
    below_frames_ = 0;
    zero_frames_ = 0;
    lamp_ = false;
    silence_ = false;
    silence_seen_ = false;
    last_dbfs_ = -120.0;
}

void SoundLevel::feed(const std::int16_t *f, std::size_t n)
{
    const std::uint64_t off_frames = static_cast<std::uint64_t>(rate_) * kOffMs / 1000;
    const std::uint64_t silence_frames = static_cast<std::uint64_t>(rate_) * kSilenceMs / 1000;
    for (std::size_t i = 0; i < n; ++i) {
        bool zero = true;
        for (int c = 0; c < channels_; ++c) {
            const int s = f[i * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)];
            win_sum_ += static_cast<double>(s) * s;
            zero = zero && s == 0;
        }
        /* the silence detector: exact zeros (a muted or broken microphone), not a quiet room */
        if (zero) {
            if (++zero_frames_ == silence_frames && !silence_seen_.load()) {
                silence_seen_ = true;
                silence_ = true;
            }
        } else {
            zero_frames_ = 0;
        }
        if (++win_frames_ < win_target_) continue;
        const double ms = win_sum_ / (static_cast<double>(win_frames_) * channels_);
        const double rms = std::sqrt(ms) / 32768.0;
        const double db = rms > 0 ? 20.0 * std::log10(rms) : -120.0;
        last_dbfs_ = db;
        if (db > kOnDbfs) {
            lamp_ = true;
            below_frames_ = 0;
        } else {
            below_frames_ += win_frames_;
            if (below_frames_ >= off_frames) lamp_ = false;
        }
        win_frames_ = 0;
        win_sum_ = 0;
    }
}

} // namespace vhs
