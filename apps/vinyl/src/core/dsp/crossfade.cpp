/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl's crossfade arithmetic (see crossfade.hpp).
 */
#include "crossfade.hpp"

#include <algorithm>
#include <cmath>

namespace vinyl {

namespace {
constexpr double kHalfPi = 1.57079632679489661923;
}  // namespace

std::string crossfade_word(int seconds) {
    const int s = crossfade_valid(seconds);
    return s == 0 ? std::string("Off") : std::to_string(s) + " s";
}

int crossfade_valid(int seconds) {
    for (int c : kCrossfadeChoices)
        if (c == seconds) return c;
    return 0;
}

uint64_t crossfade_frames(int seconds, uint32_t rate, uint64_t total_a, uint64_t total_b) {
    const int s = crossfade_valid(seconds);
    if (s == 0 || rate == 0) return 0;
    uint64_t len = static_cast<uint64_t>(s) * rate;
    if (total_a) len = std::min(len, total_a / 2);
    if (total_b) len = std::min(len, total_b / 2);
    if (len * 1000 < static_cast<uint64_t>(kCrossfadeMinMs) * rate) return 0;
    return len;
}

void crossfade_gains(uint64_t i, uint64_t len, double &g_out, double &g_in) {
    if (len == 0 || i >= len) {
        g_out = 0;
        g_in = 1;
        return;
    }
    const double a = kHalfPi * (static_cast<double>(i) + 0.5) / static_cast<double>(len);
    g_out = std::cos(a);
    g_in = std::sin(a);
}

void crossfade_mix(const float *out_track, const float *in_track, float *dst, size_t frames, uint64_t pos, uint64_t len) {
    if (frames == 0) return;
    /* the gains by a rotation from the block's first frame (two products a frame instead of a cos and a sin), exact
     * enough over a block (a few thousand frames, in double) */
    double c = 0, s = 1;
    crossfade_gains(pos, len, c, s);
    const double d = len ? kHalfPi / static_cast<double>(len) : 0;
    const double cd = std::cos(d), sd = std::sin(d);
    for (size_t f = 0; f < frames; ++f) {
        double go = c, gi = s;
        if (pos + f >= len) {
            go = 0;
            gi = 1;
        }
        const float o_l = out_track[2 * f], o_r = out_track[2 * f + 1];
        const float i_l = in_track[2 * f], i_r = in_track[2 * f + 1];
        dst[2 * f] = static_cast<float>(o_l * go + i_l * gi);
        dst[2 * f + 1] = static_cast<float>(o_r * go + i_r * gi);
        const double nc = c * cd - s * sd;
        s = s * cd + c * sd;
        c = nc;
    }
}

}  // namespace vinyl
