/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: from the drawn boards to any screen (pure, no LVGL; tests/ui/test_fit.cpp).
 *
 * The pages are drawn at 480x320 (landscape) and 320x480 (portrait, PDA mode), each with the toolkit's 40 px header on
 * top. Every element keeps its board coordinates ("design px"); Fit maps them to the screen:
 *   - one uniform scale s = min(screen w / design w, body h / design body h) for x and for every size (fonts,
 *     paddings, radii); never under 1 on a screen at least as large as the board;
 *   - the design body's top (y 40) sits on the toolkit header's bottom (body_y); the content is centred horizontally
 *     in the width left over;
 *   - vertically the board is cut into bands (design y ranges). A band is scaled by s, but
 *       min_px   a band of controls (the keys, the steppers) is at least this high: the toolkit's touch target where
 *                the panel's density needs more than the board's 44 px times s (the deck: 7 mm = 79 px);
 *       weight   a seam (a zero-high band in a gap between rows) or a band that takes the height left over;
 *       shrink   a band that gives height back when the min_px bands took more than the screen has (the margins);
 *     what is left over after the bands goes to the weighted ones, a shortage is taken from the shrinking ones (never
 *     under 40 % of their scaled height). When even that cannot fit the bands' minimums, make_fit drops them and
 *     scales the board (a panel that dense at that size does not exist).
 *   Inside a band, design y maps linearly; positions above a band keep their place, below it move by its change.
 * At exactly 480x320 / 320x480 (header 40, small panels) s = 1, nothing is left over and no minimum applies: the
 * board's pixels.
 */
#pragma once

#include <vector>

namespace cassette {
namespace ui {

struct Band {
    double y0 = 0, y1 = 0;  /* design y; y0 == y1 is a seam */
    double min_px = 0;      /* at least this many screen px high (0: none) */
    double weight = 0;      /* share of the height left over (seams: 1) */
    bool shrink = false;    /* may give height back */
};

struct Fit {
    struct Seg {
        double y0, y1, h; /* a design range and its screen height */
    };
    double s = 1.0;      /* uniform scale */
    double ox = 0;       /* screen x of design x 0 */
    double oy = 0;       /* screen y of the first band's top (design y = dbody) */
    double dbody = 40;   /* the design header's height */
    std::vector<Seg> segs; /* contiguous from dbody, ascending; seams are zero-long segments */
    double share = 0;    /* what each unit of weight got (a seam's extra px) */
    bool mins_dropped = false; /* the minimums did not fit and were dropped */
    int sw = 0, sh = 0;  /* the screen */

    double yd(double dy) const; /* unrounded */
    int x(double dx) const;
    int y(double dy) const;
    int w(double dx, double dw) const { return x(dx + dw) - x(dx); }
    int h(double dy, double dh) const { return y(dy + dh) - y(dy); }
    /* A size (font px, stroke, radius, padding): round(v * s), at least 1 for v > 0. */
    int px(double v) const;
};

/* screen_w x screen_h with the toolkit header ending at body_y; the board is design_w x design_h with its header
 * ending at design_body; seams in design y (weight 1 each); bands as above (not overlapping, no seam inside a band
 * with a minimum). */
Fit make_fit(int screen_w, int screen_h, int body_y, int design_w, int design_h, int design_body,
             std::vector<double> seams, std::vector<Band> bands = {});

/* The square layout of the recorder page (HackBerry 720x720): chosen when width / height is within 0.9 .. 1.1 (both ends
 * included). Its design is the 720x720 screen of the mockup; everything is scaled by screen width / 720. */
bool square_screen(int screen_w, int screen_h);

/* The plain linear Fit of the square layout: design x 0..720 on the screen's width (centred if the height is the
 * limit), design y `kSquareBody` (the toolkit header's bottom in the mockup) on body_y; no bands, nothing stretched. */
constexpr double kSquareDesign = 720, kSquareBody = 64;
Fit square_fit(int screen_w, int screen_h, int body_y);

/* The bands of one design range [d0, d1] laid into avail_px (no weighted band: nothing is added): the segments, as
 * Fit::segs. For a box of its own (the save box) whose height follows its content. */
std::vector<Fit::Seg> fit_segments(double d0, double d1, double s, const std::vector<Band> &bands, double avail_px,
                                   double *share = nullptr, bool *mins_dropped = nullptr);

} // namespace ui
} // namespace cassette
