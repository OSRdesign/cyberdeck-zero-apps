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

    /* the deck (640x480, 11.3 px/mm): the keys and the steppers get the toolkit's 79 px, the window row what the meter
     * panel needs with 79 px buttons, the margins give back the rest */
    {
        const double s = 4.0 / 3.0, t = 79;
        const std::vector<Band> bands = {{40, 54, 0, 0, true}, {54, 176, 5 * s + t + 73 * s, 0, false},
                                         {206, 250, t, 0, false}, {252, 302, t, 0, false}, {302, 320, 0, 0, true}};
        const Fit k = make_fit(640, 480, 56, 480, 320, 40, {179, 204, 251}, bands);
        CHECK(!k.mins_dropped);
        CHECK_EQ(k.h(206, 44), 79);
        CHECK_EQ(k.h(252, 50), 79);
        CHECK(k.h(54, 122) >= 182);
        CHECK(k.yd(320) <= 480.5);
        CHECK(k.y(54) > 56);
        /* the same bands on the small panel (44 px): the board's pixels */
        const std::vector<Band> small = {{40, 54, 0, 0, true}, {54, 176, 122, 0, false}, {206, 250, 44, 0, false},
                                         {252, 302, 44, 0, false}, {302, 320, 0, 0, true}};
        const Fit z = make_fit(480, 320, 40, 480, 320, 40, {179, 204, 251}, small);
        CHECK(!z.mins_dropped);
        CHECK_EQ(z.y(252), 252);
        CHECK_EQ(z.h(206, 44), 44);
        CHECK_EQ(z.y(320), 320);
        /* 79 px controls on 480x320 do not fit: the minimums are dropped, the board is scaled */
        const Fit n = make_fit(480, 320, 40, 480, 320, 40, {179, 204, 251}, bands);
        CHECK(n.mins_dropped);
        CHECK_EQ(n.y(252), 252);
        /* a box laid alone: the button band at its minimum, nothing added */
        double share = -1;
        const std::vector<Fit::Seg> seg = fit_segments(28, 289, 1.0, {{157, 209, 79, 0, false}}, 1e6, &share);
        double total = 0;
        for (const Fit::Seg &g : seg) total += g.h;
        CHECK(std::fabs(total - (261 + 27)) < 1e-6);
        CHECK_EQ(share, 0.0);
    }

    /* the layout chosen by the screen's shape: square from 0.9 to 1.1 (both included), else the four boards as before */
    CHECK(square_screen(720, 720));
    CHECK(square_screen(480, 480));
    CHECK(square_screen(900, 1000));   /* 0.90 */
    CHECK(!square_screen(899, 1000));
    CHECK(square_screen(1100, 1000));  /* 1.10 */
    CHECK(!square_screen(1101, 1000));
    CHECK(square_screen(720, 800) && square_screen(720, 660));
    CHECK(!square_screen(720, 801) && !square_screen(720, 654));
    CHECK(!square_screen(480, 320));   /* landscape */
    CHECK(!square_screen(640, 480));   /* the deck */
    CHECK(!square_screen(320, 480));   /* portrait */
    CHECK(!square_screen(480, 800));
    CHECK(!square_screen(800, 480) && !square_screen(1280, 720) && !square_screen(0, 0));

    /* the square fit: width / 720, linear in y, the design header bottom (64) on the toolkit's, content centred */
    {
        const Fit q = square_fit(720, 720, 64);
        CHECK(std::fabs(q.s - 1.0) < 1e-9);
        CHECK_EQ(q.x(24), 24);
        CHECK_EQ(q.x(696), 696);
        CHECK_EQ(q.y(72), 72);
        CHECK_EQ(q.y(614), 614);
        CHECK_EQ(q.w(24, 672) + 2 * q.x(24), 720);  /* equal left and right margins */
        const Fit h = square_fit(480, 480, 42);
        CHECK(std::fabs(h.s - 2.0 / 3.0) < 1e-9);
        CHECK_EQ(h.x(24), 16);
        CHECK_EQ(h.x(696), 464);
        CHECK_EQ(h.y(64), 42);
        CHECK_EQ(h.y(72), 42 + 5);       /* 8 * 2/3 = 5.3 */
        CHECK_EQ(h.px(12), 8);
        /* a header taller than the mockup's gives back height: the width no longer decides */
        const Fit t = square_fit(720, 720, 100);
        CHECK(t.s < 1.0);
        CHECK(t.y(720) <= 720);
        CHECK(t.x(0) > 0);
    }

    /* a size of 0 or less scales to 0, anything else to at least 1 */
    CHECK_EQ(d.px(0), 0);
    CHECK_EQ(a.px(0.4), 1);
}
