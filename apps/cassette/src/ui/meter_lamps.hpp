/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the lamps of the meter panel in the portrait and the square layouts (design px of each layout's own grid:
 * portrait 320 wide, square the 720x720 mockup). Pure numbers, no LVGL, so the tests can check them.
 *
 * Landscape shows LIM and LOW CUT in the info strip; these layouts have an info box instead, so the panel carries them:
 * one aligned list at the panel's right edge, top to bottom MON, LIM, LOW CUT (a lamp dot and a mono-caps label per row,
 * the dots in one column, the labels left aligned in another, the rows evenly spaced). PEAK sits at the bottom left,
 * where MON was.
 *
 * The page measures the label texts and hands the widths in; the test hands in estimates. "LOW CUT" is used when it fits
 * the width the list has, else "CUT".
 */
#pragma once

#include "sq_geom.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace cassette {
namespace ui {

struct DRectF {
    double x = 0, y = 0, w = 0, h = 0;
    double r() const { return x + w; }
    double b() const { return y + h; }
};

inline bool rects_overlap(const DRectF &a, const DRectF &c) { return a.x < c.r() && c.x < a.r() && a.y < c.b() && c.y < a.b(); }
inline bool rect_inside(const DRectF &in, const DRectF &out)
{
    return in.x >= out.x && in.y >= out.y && in.r() <= out.r() && in.b() <= out.b();
}

/* What the list is laid out in: the panel, the free strip at its right edge (clear of the bars), the lamp and the label size. */
struct MeterLampSpec {
    DRectF panel;
    DRectF area;
    double lamp_d = 6, px = 8, gap = 3;
};

constexpr int kLampRows = 3; /* MON, LIM, LOW CUT */

struct MeterLampGeo {
    DRectF lamp[kLampRows];
    DRectF label[kLampRows];
    bool short_cut = false; /* "CUT" instead of "LOW CUT" */
    const char *text(int row) const { return row == 0 ? "MON" : row == 1 ? "LIM" : short_cut ? "CUT" : "LOW CUT"; }
};

using WidthFn = std::function<double(const std::string &, double)>; /* design-px width of a label in the bold face at px */

/* The list: rows of equal height share the area's height; the block (lamp, gap, widest label) is right aligned to the area's
 * right edge, so the lamps are at one x and the labels at one x. */
inline MeterLampGeo meter_lamps_column(const MeterLampSpec &s, const WidthFn &width)
{
    MeterLampGeo g;
    g.short_cut = s.lamp_d + s.gap + width("LOW CUT", s.px) > s.area.w;
    double wmax = 0, lw[kLampRows];
    for (int i = 0; i < kLampRows; ++i) wmax = std::max(wmax, lw[i] = width(g.text(i), s.px));
    const double x0 = s.area.r() - (s.lamp_d + s.gap + wmax);
    const double row_h = s.area.h / kLampRows;
    for (int i = 0; i < kLampRows; ++i) {
        const double cy = s.area.y + row_h * (i + 0.5);
        g.lamp[i] = {x0, cy - s.lamp_d / 2, s.lamp_d, s.lamp_d};
        g.label[i] = {x0 + s.lamp_d + s.gap, cy - s.px * 0.6, lw[i], s.px * 1.2};
    }
    return g;
}

/* PEAK, at the bottom left: its lamp and its label (beside the lamp in portrait, under it in the square layout). */
struct PeakGeo {
    DRectF lamp, label;
};

/* ------------------------------------------------------------------------------------------------ portrait (320 wide) */

/* The panel is {16, 220, 288, 68} (3 px frame). The list takes the strip right of the bars (they end at x 253), from the
 * panel's top padding to its bottom: 3 rows of 20. */
inline MeterLampSpec portrait_lamp_spec()
{
    MeterLampSpec s;
    s.panel = {16, 220, 288, 68};
    s.area = {256, 224, 40, 60};
    s.lamp_d = 7;
    s.px = 9;
    s.gap = 3;
    return s;
}

/* PEAK where MON was: under IN, below the scale labels' row (they end at y 263). */
inline PeakGeo portrait_peak()
{
    return {{26, 269.5, 12, 12}, {42, 270, 36, 11}};
}

/* Every other item of the portrait meter panel (the boxes of what page_recorder.cpp draws; labels at their widest). */
inline std::vector<DRectF> portrait_meter_items()
{
    return {{26, 227, 24, 22},  /* IN / L R */
            {46, 227, 207, 20}, /* the bars: 15 steps of 11 + 3 */
            {50, 251, 24, 12},  /* -40 */
            {140, 251, 24, 12}, /* -20 */
            {184, 251, 24, 12}, /* -10 */
            {228, 251, 12, 12}, /* 0 */
            {158, 264, 54, 5},  /* the TARGET bracket */
            {158, 270, 54, 11}}; /* TARGET */
}

/* ------------------------------------------------------------------------------------------------ square (720 mockup) */

/* The panel is {24, meter_y, 672, 106}. Its contents are drawn for two channels (offset 0) or one (the rows at the top move
 * down, those at the bottom up, by kSqStereoGrow / 2: `mono`); the list and PEAK stand still in both. The list takes the
 * strip right of the bars (they end at x 624 with the tick) and left of the panel's frame. */
inline MeterLampSpec square_lamp_spec(const SqGeo &g)
{
    const double my = g.meter_y - 440;
    MeterLampSpec s;
    s.panel = {24, g.meter_y, 672, g.meter_h};
    s.area = {630, my + 446, 56, 90};
    s.lamp_d = 14;
    s.px = 14;
    s.gap = 5;
    return s;
}

/* PEAK: its 22 px lamp over its label, under IN (clear of the L / R rows above and of the -40 label right of it). */
inline PeakGeo square_peak(const SqGeo &g)
{
    const double my = g.meter_y - 440;
    return {{53, my + 496, 22, 22}, {33, my + 520, 62, 18}};
}

inline std::vector<DRectF> square_meter_items(const SqGeo &g, bool mono)
{
    const double my = g.meter_y - 440, up = mono ? kSqStereoGrow / 2 : 0;
    const double sy = my + kSqStereoGrow;
    return {{45, g.bar_y + up, 40, 28},                                    /* IN (L R sit in the same place) */
            {112, g.bar_y + up, 15 * 34 - 4 + 12, mono ? kSqBarH : kSqStereoBarH}, /* the bars, the tick's overshoot */
            {107, sy + 482 - up, 40, 22},                                  /* -40 */
            {345, sy + 482 - up, 40, 22},                                  /* -20 */
            {464, sy + 482 - up, 40, 22},                                  /* -10 */
            {583, sy + 482 - up, 40, 22},                                  /* 0 */
            {388, sy + 510 - up, 143, 6},                                  /* the TARGET bracket */
            {541, sy + 505 - up, 80, 16}};                                 /* TARGET */
}

} // namespace ui
} // namespace cassette
