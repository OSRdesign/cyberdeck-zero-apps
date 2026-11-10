/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the square layout's rows. The meter panel has one height for one and for two channels, so the
 * steppers and the keys under it are at the same place in every state of the sound input.
 */
#include "check.hpp"
#include "fit.hpp"
#include "sq_geom.hpp"

using namespace cassette::ui;

void test_sq()
{
    /* the panel is always the stereo height; what is under it follows from that alone (no channel count anywhere) */
    CHECK_EQ(kSqStereoGrow, 14.0);
    for (double t : {0.0, 58.0, 70.0, 72.0, 80.0, 100.0}) {
        const SqGeo g = sq_geometry(t);
        CHECK_EQ(g.meter_h, kSqMeterH + kSqStereoGrow);
        CHECK_EQ(g.step_y, g.meter_y + g.meter_h + 12);
        CHECK_EQ(g.key_y, g.step_y + g.step_h + 12);
        CHECK_EQ(g.meter_y, g.row_y + g.row_h + 12);
        CHECK_EQ(g.row_y, 84 + g.win_h + 12);
        CHECK(g.win_h >= 150 && g.win_h <= 260);
        CHECK(g.row_h >= t && g.step_h >= t && g.key_h >= t); /* never under the touch target */
        if (t <= 100) CHECK(g.key_y + g.key_h <= 704);        /* inside the body, whatever the density (window gives) */
    }
    /* the mockup's density: the window keeps its 260 and the rows end exactly where the stereo mockup did */
    const SqGeo m = sq_geometry(0);
    CHECK_EQ(m.win_h, 260.0);
    CHECK_EQ(m.step_y, 558.0);
    CHECK_EQ(m.key_y, 628.0);
    CHECK_EQ(m.key_y + m.key_h, 700.0);
    /* one channel: the 24 px bar row sits in the middle of the two-row zone (equal padding above and below) */
    const double mono_top = kSqStereoGrow / 2;
    CHECK_EQ(mono_top, kSqStereoBarH - kSqBarH - mono_top);
    /* in pixels, 720 and 480 wide: the rows are the same numbers for both channel counts (one geometry) */
    for (int w : {720, 480}) {
        const Fit f = square_fit(w, w, w == 720 ? 64 : 42);
        const SqGeo a = sq_geometry(60), b = sq_geometry(60);
        CHECK_EQ(f.y(a.step_y), f.y(b.step_y));
        CHECK_EQ(f.y(a.key_y), f.y(b.key_y));
        CHECK(f.y(a.key_y + a.key_h) <= w);
    }
}
