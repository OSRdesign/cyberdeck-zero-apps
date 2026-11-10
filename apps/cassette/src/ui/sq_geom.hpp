/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the square layout's rows (design px of the 720x720 mockup; the recorder page scales them by width / 720).
 * Pure numbers, no LVGL, so the tests can check them. The meter panel has one height whatever the channel count (the
 * height two channels need), so the rows under it never depend on the sound input.
 */
#pragma once

#include <algorithm>

namespace cassette {
namespace ui {

constexpr double kSqMeterH = 92, kSqBarH = 24;
constexpr double kSqStereoBarH = 38; /* two rows of 17 and a gap of 4 */
constexpr double kSqStereoGrow = kSqStereoBarH - kSqBarH;

struct SqGeo {
    double win_h = 260, row_y = 356, row_h = 72, meter_y = 440, meter_h = kSqMeterH + kSqStereoGrow, bar_y = 452, step_y = 558,
           step_h = 58, key_y = 628, key_h = 72;
};

/* The mockup's rows from the top: window 84..344, counter row 356 (72), meter panel 440 (92 + the stereo growth), steppers,
 * keys, the body ends at 708. A control row is never shorter than the toolkit's touch target (`t`, design px: the panel's
 * density); what that needs more than the mockup gives is taken from the window, whose cassette then scales to fit (7 px
 * margin each side at least). The bottom keeps room for the keys' lip. */
inline SqGeo sq_geometry(double t)
{
    SqGeo g;
    g.row_h = std::max(72.0, t);
    g.step_h = std::max(58.0, t);
    g.key_h = std::max(72.0, t);
    g.meter_h = kSqMeterH + kSqStereoGrow;
    const double fixed = 84 + 12 + g.row_h + 12 + g.meter_h + 12 + g.step_h + 12 + g.key_h; /* all but the window */
    g.win_h = std::clamp(704 - 4 - fixed, 150.0, 260.0);
    g.row_y = 84 + g.win_h + 12;
    g.meter_y = g.row_y + g.row_h + 12;
    g.bar_y = g.meter_y + 12;
    g.step_y = g.meter_y + g.meter_h + 12;
    g.key_y = g.step_y + g.step_h + 12;
    return g;
}

} // namespace ui
} // namespace cassette
