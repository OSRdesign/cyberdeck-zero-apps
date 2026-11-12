/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the meter panel of the portrait and the square layouts (design px of each layout's own grid: portrait 320
 * wide, square the 720x720 mockup), and where its LED bar, scale and lamps land on the screen. Pure numbers, no LVGL, so
 * the tests can check exactly what the page draws (the page builds its objects from these functions).
 *
 * Landscape shows LIM and LOW CUT in the info strip; these layouts have an info box instead, so the panel carries them:
 * one aligned list at the panel's right edge, top to bottom MON, LIM, LOW CUT (a lamp dot and a mono-caps label per row,
 * the dots in one column, the labels left aligned in another, the rows evenly spaced; every row the same dot diameter and
 * the same label size). PEAK sits at the bottom left.
 *
 * The list keeps its label size and stays right aligned in its strip; the LED bar gives way to it (owner, 2026-11-11):
 * on the screen the bar's 15 steps are whole pixels (LedPx: the board's LED width and gap each rounded at the scale), so at
 * some scales the rounded steps add up to more than the board's bar and ran into the dots (480x800: the last step ended
 * 6 px right of the MON dot's left). meter_layout keeps the board's bar where it leaves the dots at least 6 design px
 * after the last step (and clear of the peak-hold tick after it), else narrows the LEDs (down to 60 %), then the gap,
 * until it does; the pitch stays uniform. The scale labels, the TARGET bracket and its caption follow the steps (their
 * board places, in steps from the first LED). "LOW CUT" is used only where the board's bar leaves room for it, else "CUT".
 */
#pragma once

#include "fit.hpp"
#include "sq_geom.hpp"

#include <algorithm>
#include <cmath>
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

/* ------------------------------------------------------------------------------------------------ the LED bar on screen */

constexpr int kLedSteps = 15;

/* The bar's steps in screen px, as LedBar draws them: step i at x0 + i * pitch, led_w wide; the white peak-hold tick in
 * the gap after the held step; the object ends one gap after the last step (the tick's room at full scale). */
struct LedPx {
    int x0 = 0, led_w = 0, gap = 0, tick_w = 0;
    int pitch() const { return led_w + gap; }
    int seg_x(int i) const { return x0 + i * pitch(); }
    int last_r() const { return seg_x(kLedSteps - 1) + led_w; }
    int right() const { return x0 + kLedSteps * pitch(); }
    int tick_x(int hold) const { return x0 + hold * pitch() - (gap + tick_w) / 2; }
    int tick_r() const { return tick_x(kLedSteps) + tick_w; } /* the tick's right edge at full scale */
};

/* The board's bar at the screen's scale (every length rounded on its own: what landscape draws). */
inline LedPx led_px(const Fit &f, double x0, double led_w, double gap, double tick_w)
{
    return {f.x(x0), f.px(led_w), f.px(gap), f.px(tick_w)};
}

/* ------------------------------------------------------------------------------------------------ the panel's design */

/* What the list is laid out in: the strip at the panel's right edge (its right edge and its rows; the dots are right
 * aligned with the widest label to area.r()), the dot and label size, the dot-to-label gap, the labels' spacing. */
struct MeterLampSpec {
    DRectF area;
    double lamp_d = 6, px = 8, gap = 3, ls = 0;
};

constexpr int kLampRows = 3; /* MON, LIM, LOW CUT */

/* A text of the panel: its board place (w 0: as wide as its text, else a box of that width with the text centred). */
struct MeterText {
    const char *text;
    bool bold;
    double px, ls, x, y, w, h;
};

/* PEAK, at the bottom left: its lamp and its label (beside the lamp in portrait, under it in the square layout). */
struct PeakGeo {
    DRectF lamp, label;
};

/* Landscape: the one-bar label left of the bar (whose first step starts at board x 286): IN, or BT for a Bluetooth input. BT
 * is wider than IN, so it is one font step smaller (12 instead of 13) and 2 px further left: it ends at least kLandInLabelGap
 * board px before the bar (owner, 2026-11-11). Portrait and square keep IN's place and size for BT (they have the room). */
struct InLabelGeo {
    double x, px, ls;
    const char *text;
};
constexpr double kLandBarX = 286, kLandInLabelGap = 2;
inline InLabelGeo landscape_in_label(bool bt) { return bt ? InLabelGeo{266, 12, 1, "BT"} : InLabelGeo{268, 13, 1, "IN"}; }

/* The whole panel as the board draws it with two channels. One channel: the bar is mono_h high and starts mono_dy lower,
 * IN moves down by mono_dy and the scale, the bracket and TARGET move up by it (square; portrait has no such change). */
struct MeterSpec {
    DRectF panel;
    DRectF in_box, in_box_mono; /* L / R (two channels), IN (one) */
    double x0 = 0, y = 0, led_w = 0, gap = 0, led_h = 0, mono_h = 0, mono_dy = 0;
    double over = 3, tick_w = 2, radius = 1; /* LedStyle */
    MeterText scale[4];
    DRectF bracket;
    MeterText target;
    MeterLampSpec lamps;
    PeakGeo peak;
};

/* Portrait: the panel is {16, 220, 288, 68} (3 px frame); bars of 15 steps of 11 + 3 from x 46; the list in the strip
 * right of them up to x 296, from the panel's top padding to its bottom: 3 rows of 20; 7 px dots, 10 px labels. */
inline MeterSpec portrait_meter_spec()
{
    MeterSpec s;
    s.panel = {16, 220, 288, 68};
    s.in_box = s.in_box_mono = {26, 227, 24, 22};
    s.x0 = 46;
    s.y = 227;
    s.led_w = 11;
    s.gap = 3;
    s.led_h = s.mono_h = 20;
    const double sx[4] = {4, 94, 138, 182};
    const char *const st[4] = {"-40", "-20", "-10", "0"};
    for (int i = 0; i < 4; ++i) s.scale[i] = {st[i], true, 10, 0, 46 + sx[i], 251, 0, 12};
    s.bracket = {158, 264, 54, 5};
    s.target = {"TARGET", true, 9, 1.2, 158, 270, 54, 11};
    s.lamps = {{256, 224, 40, 60}, 7, 10, 3, 0.5};
    s.peak = {{26, 269.5, 12, 12}, {42, 270, 36, 11}}; /* where MON was: under IN, below the scale labels */
    return s;
}

/* Square: the panel is {24, meter_y, 672, 106}, as tall as two channels need; bars of 15 steps of 30 + 4 from x 112; the
 * list in the strip up to x 686 (6 px inside the frame); 14 px dots and labels. PEAK: its 22 px lamp over its label, under
 * IN. The list and PEAK stand still with one channel and with two. */
inline MeterSpec square_meter_spec(const SqGeo &g)
{
    const double my = g.meter_y - 440, sy = my + kSqStereoGrow;
    MeterSpec s;
    s.panel = {24, g.meter_y, 672, g.meter_h};
    s.in_box = {45, g.bar_y, 40, 38};                       /* L over R */
    s.in_box_mono = {45, g.bar_y + kSqStereoGrow / 2, 40, 28}; /* IN, moved with the single row */
    s.x0 = 112;
    s.y = g.bar_y;
    s.led_w = 30;
    s.gap = 4;
    s.led_h = kSqStereoBarH;
    s.mono_h = kSqBarH;
    s.mono_dy = kSqStereoGrow / 2;
    s.over = 6;
    s.tick_w = 3;
    s.radius = 3;
    const double sx[4] = {107, 345, 464, 583};
    const char *const st[4] = {"-40", "-20", "-10", "0"};
    for (int i = 0; i < 4; ++i) s.scale[i] = {st[i], false, 22, 0, sx[i], sy + 482, 40, 22};
    s.bracket = {388, sy + 510, 143, 6};
    s.target = {"TARGET", true, 16, 2, 541, sy + 505, 0, 16};
    s.lamps = {{628, my + 446, 58, 90}, 14, 14, 5, 1};
    s.peak = {{53, my + 496, 22, 22}, {33, my + 520, 62, 18}};
    return s;
}

/* ------------------------------------------------------------------------------------------------ on the screen */

/* Screen-px width of a text at a screen font size and letter spacing: the page measures the real face, the tests estimate. */
using TextWidthFn = std::function<double(const std::string &text, bool bold, int px, int ls)>;

/* The letter spacing in screen px of a board spacing (widgets.cpp ls_screen: truncated). */
inline int meter_ls(double ls, double s) { return static_cast<int>(ls * s); }

struct MeterLayout {
    LedPx board;     /* the bar at the board's step size */
    LedPx bar;       /* the bar drawn */
    bool short_cut = false;
    int lamp_d = 0;  /* screen px */
    int label_px = 0;
    double lamp_x = 0, label_x = 0;
    DRectF lamp[kLampRows], label[kLampRows];
    double label_dy[kLampRows] = {}; /* a label's line top in design y (its line is lamps.px * 1.2 high) */
    double x0d = 0, pitch_d = 0;
    const char *text(int row) const { return row == 0 ? "MON" : row == 1 ? "LIM" : short_cut ? "CUT" : "LOW CUT"; }
    /* a board x in the bar's frame (steps from the first LED's left) on the bar drawn */
    double map_x(double dx) const { return bar.x0 + (dx - x0d) / pitch_d * bar.pitch(); }
};

/* The least room between the last step and the dots (design px), and between the full-scale tick and the dots. */
constexpr double kLampGapSeg = 6, kLampGapTick = 3;

inline MeterLayout meter_layout(const Fit &f, const MeterSpec &s, const TextWidthFn &width)
{
    MeterLayout m;
    m.x0d = s.x0;
    m.pitch_d = s.led_w + s.gap;
    m.board = led_px(f, s.x0, s.led_w, s.gap, s.tick_w);
    m.lamp_d = f.px(s.lamps.lamp_d);
    m.label_px = f.px(s.lamps.px);
    const int ls = meter_ls(s.lamps.ls, f.s), lgap = f.px(s.lamps.gap);
    const int area_r = f.x(s.lamps.area.r());
    const int min_seg = f.px(kLampGapSeg), min_tick = f.px(kLampGapTick);
    auto w = [&](const char *t) { return width(t, true, m.label_px, ls); };
    auto block = [&](bool sc) { return m.lamp_d + lgap + std::max({w("MON"), w("LIM"), w(sc ? "CUT" : "LOW CUT")}); };
    auto fits = [&](const LedPx &b, double lx) { return b.last_r() + min_seg <= lx && b.tick_r() + min_tick <= lx; };
    m.short_cut = !fits(m.board, std::floor(area_r - block(false)));
    m.lamp_x = std::floor(area_r - block(m.short_cut));
    m.label_x = m.lamp_x + m.lamp_d + lgap;
    /* the widest bar that leaves the room: narrower LEDs first (to 60 % of the board's), then a narrower gap */
    m.bar = m.board;
    bool done = fits(m.bar, m.lamp_x);
    for (int gap = m.board.gap; !done && gap >= 1; --gap) {
        const int lo = gap == m.board.gap ? std::max(1, static_cast<int>(std::ceil(m.board.led_w * 0.6))) : 1;
        for (int led = m.board.led_w; !done && led >= lo; --led) {
            LedPx b = m.board;
            b.led_w = led;
            b.gap = gap;
            if (fits(b, m.lamp_x)) {
                m.bar = b;
                done = true;
            }
        }
    }
    const double row_h = s.lamps.area.h / kLampRows;
    for (int i = 0; i < kLampRows; ++i) {
        const double cy = s.lamps.area.y + row_h * (i + 0.5);
        const int top = f.y(cy - s.lamps.lamp_d / 2);
        m.lamp[i] = {m.lamp_x, double(top), double(m.lamp_d), double(m.lamp_d)};
        m.label_dy[i] = cy - s.lamps.px * 0.6;
        m.label[i] = {m.label_x, double(f.y(m.label_dy[i])), w(m.text(i)), double(f.px(s.lamps.px * 1.2))};
    }
    return m;
}

/* A text of the panel on the screen: the box (x, w) follows the bar's steps by its centre; y as the board (+ dy). */
inline DRectF meter_text_px(const Fit &f, const MeterLayout &m, const MeterText &t, const TextWidthFn &width, double dy = 0)
{
    const double wpx = t.w > 0 ? f.w(t.x, t.w) : width(t.text, t.bold, f.px(t.px), meter_ls(t.ls, f.s));
    const double cx = m.map_x(t.x + wpx / f.s / 2);
    return {std::round(cx - wpx / 2), double(f.y(t.y + dy)), wpx, double(f.px(t.h))};
}

/* The TARGET bracket on the screen: both ends follow the steps. */
inline DRectF meter_bracket_px(const Fit &f, const MeterLayout &m, const MeterSpec &s, double dy = 0)
{
    const double x = std::round(m.map_x(s.bracket.x)), r = std::round(m.map_x(s.bracket.r()));
    return {x, double(f.y(s.bracket.y + dy)), r - x, double(f.h(s.bracket.y + dy, s.bracket.h))};
}

/* Everything of the panel but the list, on the screen, with one channel or two: IN / L R, the bar (its whole object: the
 * steps and the tick's reach above, below and after them), the scale labels, the bracket, TARGET, PEAK's lamp and label. */
inline std::vector<DRectF> meter_items_px(const Fit &f, const MeterLayout &m, const MeterSpec &s, bool mono,
                                          const TextWidthFn &width)
{
    const double down = mono ? s.mono_dy : 0;
    auto px = [&](const DRectF &r) { return DRectF{double(f.x(r.x)), double(f.y(r.y)), double(f.w(r.x, r.w)), double(f.h(r.y, r.h))}; };
    std::vector<DRectF> out;
    out.push_back(px(mono ? s.in_box_mono : s.in_box));
    const double by = s.y + down, bh = mono ? s.mono_h : s.led_h;
    const int over = f.px(s.over);
    out.push_back({double(m.bar.x0), double(f.y(by) - over), double(m.bar.right() - m.bar.x0), double(f.h(by, bh) + 2 * over)});
    for (const MeterText &t : s.scale) out.push_back(meter_text_px(f, m, t, width, -down));
    out.push_back(meter_bracket_px(f, m, s, -down));
    out.push_back(meter_text_px(f, m, s.target, width, -down));
    out.push_back(px(s.peak.lamp));
    out.push_back(px(s.peak.label));
    return out;
}

} // namespace ui
} // namespace cassette
