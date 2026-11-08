/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: board coordinates to screen coordinates (see fit.hpp).
 */
#include "fit.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

int Fit::x(double dx) const
{
    return static_cast<int>(std::lround(ox + dx * s));
}

int Fit::y(double dy) const
{
    double extra = 0;
    for (double at : seams)
        if (dy > at) extra += share;
    return static_cast<int>(std::lround(oy + (dy - dbody) * s + extra));
}

int Fit::px(double v) const
{
    if (v <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(v * s)));
}

Fit make_fit(int screen_w, int screen_h, int body_y, int design_w, int design_h, int design_body,
             std::vector<double> seams)
{
    Fit f;
    f.sw = screen_w;
    f.sh = screen_h;
    f.dbody = design_body;
    const double body_h = static_cast<double>(screen_h - body_y);
    const double dbody_h = static_cast<double>(design_h - design_body);
    const double sx = static_cast<double>(screen_w) / design_w;
    const double sy = dbody_h > 0 ? body_h / dbody_h : sx;
    f.s = std::min(sx, sy);
    /* a hair under 1 from rounding (a header one px taller) is not a reason to shrink every font */
    if (f.s > 0.985 && f.s < 1.0) f.s = 1.0;
    f.ox = (screen_w - design_w * f.s) / 2.0;
    f.oy = body_y;
    std::sort(seams.begin(), seams.end());
    f.seams = seams;
    const double left = body_h - dbody_h * f.s;
    f.share = (!seams.empty() && left > 0) ? std::floor(left / static_cast<double>(seams.size())) : 0;
    return f;
}

} // namespace ui
} // namespace cassette
