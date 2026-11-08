/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: from the drawn boards to any screen (pure, no LVGL; tests/ui/test_fit.cpp).
 *
 * The pages are drawn at 480x320 (landscape) and 320x480 (portrait, PDA mode), each with the toolkit's 40 px header on
 * top. Every element keeps its board coordinates ("design px"); Fit maps them to the screen:
 *   - one uniform scale s = min(screen w / design w, body h / design body h), never under 1 on a screen at least as
 *     large as the board (fonts, paddings and touch targets scale with s, nothing gets smaller than drawn);
 *   - the design body's top (y 40) sits on the toolkit header's bottom (body_y); the content is centred horizontally
 *     in the width left over;
 *   - the height left over is shared equally by the page's seams: design y lines in the gaps between rows of the
 *     composition. An element above a seam keeps its place, one below moves down by the seam's share, one that spans
 *     it grows by it (a scrolling list, the waveform). Nothing new appears and no element changes its proportions
 *     except the spanning ones.
 * At exactly 480x320 / 320x480 (header 40) s = 1 and nothing is left over: the board's pixels.
 */
#pragma once

#include <vector>

namespace cassette {
namespace ui {

struct Fit {
    double s = 1.0;      /* uniform scale */
    double ox = 0;       /* screen x of design x 0 */
    double oy = 0;       /* screen y of design y = dbody (the header's bottom) */
    double dbody = 40;   /* the design header's height */
    std::vector<double> seams;  /* design y of each seam, ascending */
    double share = 0;    /* px each seam adds */
    int sw = 0, sh = 0;  /* the screen */

    int x(double dx) const;
    int y(double dy) const;
    int w(double dx, double dw) const { return x(dx + dw) - x(dx); }
    int h(double dy, double dh) const { return y(dy + dh) - y(dy); }
    /* A size (font px, stroke, radius, padding): round(v * s), at least 1 for v > 0. */
    int px(double v) const;
};

/* screen_w x screen_h with the toolkit header ending at body_y; the board is design_w x design_h with its header
 * ending at design_body; seams in design y (any order). */
Fit make_fit(int screen_w, int screen_h, int body_y, int design_w, int design_h, int design_body,
             std::vector<double> seams);

} // namespace ui
} // namespace cassette
