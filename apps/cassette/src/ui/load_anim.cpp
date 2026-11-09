/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the load animation's timeline (see load_anim.hpp).
 */
#include "load_anim.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

int hand_rung(int px)
{
    int best = kHandRungs[0];
    for (int r : kHandRungs)
        if (r <= px) best = r;
    return best;
}

int hand_rung_below(int rung)
{
    int below = 0;
    for (int r : kHandRungs)
        if (r < rung) below = r;
    return below;
}

double bezier_at(double x1, double y1, double x2, double y2, double x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    auto bx = [&](double t) { return 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t; };
    auto by = [&](double t) { return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t; };
    /* solve bx(t) = x by bisection (monotonic for x1, x2 in 0..1): 40 steps are exact to 1e-12 */
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2;
        if (bx(mid) < x) lo = mid;
        else hi = mid;
    }
    return by((lo + hi) / 2);
}

int load_total_ms(bool had_old)
{
    return load_landing_ms(had_old) + kLoadWriteMs;
}

int load_landing_ms(bool had_old)
{
    return (had_old ? kLoadOutMs + kLoadGapMs : 0) + kLoadInMs;
}

LoadPose load_pose(int t, bool had_old)
{
    LoadPose p;
    t = std::max(0, t);
    const int in_start = had_old ? kLoadOutMs + kLoadGapMs : 0;
    const int land = in_start + kLoadInMs;
    if (had_old && t < kLoadOutMs) {
        p.phase = LoadPose::Phase::Out;
        p.old_shown = true;
        p.old_y = kLoadFar * bezier_at(0.42, 0, 1, 1, static_cast<double>(t) / kLoadOutMs);
        p.new_y = -kLoadFar;
        return p;
    }
    if (t < in_start) {
        p.phase = LoadPose::Phase::Gap;
        p.new_y = -kLoadFar;
        return p;
    }
    p.new_shown = true;
    if (t < land) {
        p.phase = LoadPose::Phase::In;
        const int ti = t - in_start;
        const int fall = kLoadInMs - kLoadSettleMs; /* 300 ms down to +4 */
        if (ti < fall) {
            p.new_y = -kLoadFar + (kLoadFar + kLoadBounce) * bezier_at(0.2, 0.7, 0.3, 1, static_cast<double>(ti) / fall);
        } else {
            p.new_y = kLoadBounce * (1 - bezier_at(0.42, 0, 0.58, 1, static_cast<double>(ti - fall) / kLoadSettleMs));
        }
        return p;
    }
    p.landed = true;
    p.new_y = 0;
    const int tw = t - land;
    if (tw < kLoadWriteMs) {
        p.phase = LoadPose::Phase::Write;
        p.name = static_cast<double>(tw) / kLoadWriteMs;
    } else {
        p.phase = LoadPose::Phase::Done;
        p.name = 1;
    }
    return p;
}

} // namespace ui
} // namespace cassette
