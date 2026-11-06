/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: screen geometry (see view.hpp).
 */
#include "view.hpp"

#include <algorithm>
#include <cmath>

namespace mmw {

int auto_range(int current_m, int max_dist_mm, bool any, int max_range_m)
{
    /* the allowed steps: kAutoSteps up to max_range_m, never fewer than the first three */
    int top = kRangeSteps[2];
    for (int i = 0; i < kAutoStepCount; ++i)
        if (kAutoSteps[i] <= max_range_m) top = kAutoSteps[i];
    if (current_m > top) current_m = top;
    if (!any) return current_m;
    const int need = max_dist_mm + kAutoMarginMm;
    int fit = top;
    for (int i = 0; i < kAutoStepCount && kAutoSteps[i] <= top; ++i)
        if (kAutoSteps[i] * 1000 >= need) {
            fit = kAutoSteps[i];
            break;
        }
    if (fit >= current_m) return fit; /* grow at once (or stay) */
    /* shrink: one step, and only well inside it */
    int smaller = current_m;
    for (int i = 0; i < kAutoStepCount; ++i)
        if (kAutoSteps[i] < current_m) smaller = kAutoSteps[i];
    if (smaller < current_m && need <= smaller * 1000 - kAutoShrinkMm) return smaller;
    return current_m;
}

int ring_step_m(int range_m)
{
    if (range_m <= 6) return 1;
    if (range_m <= 10) return 2;
    if (range_m <= 20) return 5;
    if (range_m <= 50) return 10;
    return 20;
}

int step_range(int setting_m, int effective_m, int dir)
{
    const int base = setting_m == 0 ? effective_m : setting_m;
    int idx = -1;
    for (int i = 0; i < 3; ++i)
        if (kRangeSteps[i] == base) idx = i;
    if (idx < 0) idx = base > kRangeSteps[2] ? 2 : 1; /* an Auto range of 10 m or more: next to the 6 m step */
    if (setting_m == 0) { /* from Auto: the neighbour of what is shown, clamped */
        const int j = std::clamp(idx + (dir > 0 ? 1 : -1), 0, 2);
        return kRangeSteps[j];
    }
    const int j = idx + (dir > 0 ? 1 : -1);
    if (j < 0 || j > 2) return 0;
    return kRangeSteps[j];
}

PlotGeom plot_geom(int x, int y, int w, int h, int range_m, Orientation o, int pad, int near_pad)
{
    PlotGeom g;
    g.x = x;
    g.y = y;
    g.w = w;
    g.h = h;
    g.range_m = range_m > 0 ? range_m : 4;
    const double range_mm = g.range_m * 1000.0;
    const double half_w = range_mm * std::sin(kFovDeg * M_PI / 180.0); /* the wedge's half width at full range */
    const double avail_w = std::max(1, w - 2 * pad) / 2.0;
    const double avail_h = std::max(1, h - pad - near_pad);
    g.px_per_mm = std::min(avail_w / half_w, avail_h / range_mm);
    g.sx = x + w / 2;
    if (o == Orientation::Rotate180) {
        g.sy = y + near_pad;
        g.ysign = 1;
        g.xsign = -1;
    } else {
        g.sy = y + h - 1 - near_pad;
        g.ysign = -1;
        g.xsign = o == Orientation::Mirror ? -1 : 1;
    }
    return g;
}

int plot_full_width_h(int w, int pad, int near_pad)
{
    /* plot_geom: px_per_mm = min(avail_w / half_w, avail_h / range); width-limited when avail_h >= avail_w / sin(fov) */
    const double avail_w = std::max(1, w - 2 * pad) / 2.0;
    return static_cast<int>(std::ceil(avail_w / std::sin(kFovDeg * M_PI / 180.0))) + pad + near_pad;
}

RadarLayout radar_layout(int w, int h, int top_band, int pad, int near_pad, int list_block_h, int list_gap,
                         int chart_min_h)
{
    RadarLayout l;
    l.plot_y = 0;
    l.plot_h = h;
    if (w <= 0 || h <= w) return l; /* landscape and square: classic */
    const int plot_h = plot_full_width_h(w, pad, near_pad);
    const int list_h = 3 * list_block_h + 2 * list_gap;
    const int free_h = h - top_band - plot_h;
    if (free_h < list_gap + list_h) return l;
    l.tall = true;
    l.plot_y = top_band;
    l.plot_h = plot_h;
    l.list_y = l.plot_y + plot_h + list_gap;
    l.list_h = list_h;
    const int rest = h - (l.list_y + list_h);
    if (rest >= chart_min_h && rest > 2 * list_gap) {
        l.chart_y = l.list_y + list_h + list_gap;
        l.chart_h = h - list_gap - l.chart_y;
    }
    return l;
}

void plot_point(const PlotGeom &g, int x_mm, int y_mm, int &px, int &py)
{
    px = g.sx + static_cast<int>(std::lround(g.xsign * x_mm * g.px_per_mm));
    py = g.sy + static_cast<int>(std::lround(g.ysign * y_mm * g.px_per_mm));
}

bool plot_contains(const PlotGeom &g, int px, int py)
{
    return px >= g.x && px < g.x + g.w && py >= g.y && py < g.y + g.h;
}

int arrow_length(double speed_cm_s, int dot_px)
{
    const double v = std::fabs(speed_cm_s);
    if (v < 5.0) return 0;
    const double t = std::min(v, 150.0) / 150.0;
    return static_cast<int>(std::lround(dot_px * (1.5 + 2.5 * t)));
}

Box place_label(const PlotGeom &g, int px, int py, int r, int lw, int lh, const Box *taken, int n_taken)
{
    const int gap = std::max(2, r / 2);
    Box b;
    b.w = lw;
    b.h = lh;
    b.x = px + r + gap;
    if (b.x + lw > g.x + g.w) b.x = px - r - gap - lw;
    b.y = py - lh / 2;
    auto clamp_in = [&](Box &c) {
        c.x = std::clamp(c.x, g.x, std::max(g.x, g.x + g.w - c.w));
        c.y = std::clamp(c.y, g.y, std::max(g.y, g.y + g.h - c.h));
    };
    clamp_in(b);
    for (int pass = 0; pass < 2; ++pass) {
        bool moved = false;
        for (int i = 0; i < n_taken; ++i) {
            if (!b.overlaps(taken[i])) continue;
            Box below = b;
            below.y = taken[i].y + taken[i].h + 1;
            clamp_in(below);
            Box above = b;
            above.y = taken[i].y - lh - 1;
            clamp_in(above);
            b = !below.overlaps(taken[i]) ? below : above;
            moved = true;
        }
        if (!moved) break;
    }
    return b;
}

bool slot_shown(const SlotState &s, bool hide_held) { return s.active && !(hide_held && s.held); }

int held_count(const SlotState *slots, int n)
{
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (slots[i].active && slots[i].present_now && slots[i].held) ++c;
    return c;
}

double nice_ceiling(double v)
{
    if (!(v > 0.0)) return 1.0;
    const double p = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * p >= v - 1e-9) return m * p;
    return 10.0 * p;
}

} // namespace mmw
