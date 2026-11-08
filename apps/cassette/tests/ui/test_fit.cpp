/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: board coordinates to the screen (fit.hpp).
 */
#include "check.hpp"
#include "fit.hpp"

#include <cmath>

using namespace cassette::ui;

void test_fit()
{
    /* at the board's own size: the board's pixels */
    const Fit a = make_fit(480, 320, 40, 480, 320, 40, {179, 204, 251});
    CHECK(std::fabs(a.s - 1.0) < 1e-9);
    CHECK_EQ(a.x(16), 16);
    CHECK_EQ(a.y(252), 252);
    CHECK_EQ(a.w(16, 84), 84);
    CHECK_EQ(a.px(12), 12);
    const Fit p = make_fit(320, 480, 40, 320, 480, 40, {217, 291});
    CHECK_EQ(p.x(252), 252);
    CHECK_EQ(p.y(402), 402);

    /* the deck: 640x480 with a 56 px header: s = 4/3 from the width, 50.7 px left over: 16 px for each of three seams */
    const Fit d = make_fit(640, 480, 56, 480, 320, 40, {251, 179, 204});
    CHECK(std::fabs(d.s - 4.0 / 3.0) < 1e-9);
    CHECK_EQ(d.x(0), 0);
    CHECK_EQ(d.x(480), 640);
    CHECK_EQ(d.y(40), 56);
    CHECK_EQ(d.share, 16.0);
    CHECK_EQ(d.y(54), 56 + 19);                 /* above every seam: only scaled (18.7) */
    CHECK_EQ(d.y(182), 261);                    /* below the first seam: 56 + 189.3 + 16 */
    CHECK_EQ(d.y(252), 387);                    /* below all three: 56 + 282.7 + 48 */
    CHECK(d.y(302) <= 480);                     /* the keys' bottom stays on the screen */
    CHECK_EQ(d.px(12), 16);
    CHECK_EQ(d.px(9), 12);
    CHECK(d.px(44) >= 44);                      /* nothing smaller than drawn */

    /* a wide screen: the height decides, the content is centred */
    const Fit w = make_fit(800, 480, 56, 480, 320, 40, {});
    CHECK(std::fabs(w.s - 424.0 / 280.0) < 1e-9);
    CHECK(w.x(0) > 0);
    CHECK_EQ(w.x(0) + w.x(480), 800);
    CHECK_EQ(w.share, 0.0);

    /* portrait 480x800: s 1.5 from the width */
    const Fit t = make_fit(480, 800, 56, 320, 480, 40, {217, 291, 345, 399});
    CHECK(std::fabs(t.s - 1.5) < 1e-9);
    CHECK_EQ(t.x(320), 480);
    CHECK(t.y(458) <= 800);
    CHECK(t.share > 0);

    /* a size of 0 or less scales to 0, anything else to at least 1 */
    CHECK_EQ(d.px(0), 0);
    CHECK_EQ(a.px(0.4), 1);
}
