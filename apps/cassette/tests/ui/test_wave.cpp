/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the waveform geometry (wave_geom.hpp): the window, handles and flags inside or as edge chips, the
 * overview, resampling, the tap on a flag, the scrub and the ruler; the IN / OUT tags' place and rows, the draw order.
 */
#include "check.hpp"
#include "wave_geom.hpp"

#include <cmath>
#include <cstdio>

using namespace cassette::ui;

namespace {

bool near(double a, double b, double eps = 0.51) { return std::fabs(a - b) <= eps; }

/* the 480x320 board: a 60 s window at 5:44.2 over the 440 px from x 20 */
void window()
{
    const WaveWindow w = wave_window(344200, 60000, 20, 440);
    CHECK_EQ(w.start_ms, 314200);
    CHECK(near(w.x_of(344200), 240));      /* the marker in the middle (the board's red line at 240) */
    CHECK(near(w.x_of(314200), 20));
    CHECK(near(w.x_of(374200), 460));
    CHECK_EQ(w.t_of(240), 344200);
    CHECK_EQ(w.t_of(20), 314200);
    /* the board's two flags: at x 71 and 393 */
    const std::int64_t s1 = w.t_of(71), s2 = w.t_of(393);
    CHECK(near(w.x_of(s1), 71));
    CHECK(near(w.x_of(s2), 393));
    CHECK(wave_edge(w, s1) == WaveEdge::Inside);
    /* IN 02:58.7 and OUT 10:28.7 lie outside: chips at the left and the right edge */
    CHECK(wave_edge(w, 178700) == WaveEdge::Left);
    CHECK(wave_edge(w, 628700) == WaveEdge::Right);
    CHECK(wave_edge(w, 314200) == WaveEdge::Inside);
    CHECK(wave_edge(w, 374200) == WaveEdge::Inside);
    CHECK(wave_edge(w, 374201) == WaveEdge::Right);
    /* near the start the window reaches before 0 */
    const WaveWindow z = wave_window(1000, 60000, 0, 600);
    CHECK_EQ(z.start_ms, -29000);
    CHECK(near(z.x_of(0), 290));
    /* a zero span is treated as 1 ms */
    CHECK_EQ(wave_window(5, 0, 0, 10).span_ms, 1);
}

void overview_and_bars()
{
    CHECK(near(overview_x(0, 728000, 20, 440), 20));
    CHECK(near(overview_x(728000, 728000, 20, 440), 460));
    CHECK(near(overview_x(364000, 728000, 20, 440), 240));
    CHECK(near(overview_x(-5, 728000, 20, 440), 20));
    CHECK(near(overview_x(9999999, 728000, 20, 440), 460));
    CHECK_EQ(bar_count(440, 4), 110);
    CHECK_EQ(bar_count(272, 4), 68);
    CHECK_EQ(bar_count(3, 4), 0);
    CHECK_EQ(bar_count(0, 4), 0);

    const std::vector<float> in = {0.1f, 0.9f, 0.2f, 0.3f, 0.0f, 0.4f, 2.0f, -1.0f};
    const std::vector<float> r4 = resample_max(in, 4);
    CHECK_EQ(r4.size(), std::size_t(4));
    CHECK(near(r4[0], 0.9, 1e-6));
    CHECK(near(r4[1], 0.3, 1e-6));
    CHECK(near(r4[2], 0.4, 1e-6));
    CHECK(near(r4[3], 1.0, 1e-6)); /* clamped */
    const std::vector<float> up = resample_max({0.5f, 1.0f}, 4); /* fewer values than bars: each repeats */
    CHECK(near(up[0], 0.5, 1e-6) && near(up[1], 0.5, 1e-6) && near(up[2], 1.0, 1e-6) && near(up[3], 1.0, 1e-6));
    CHECK_EQ(resample_max({}, 3).size(), std::size_t(3));
    CHECK(resample_max({0.7f}, 0).empty());
}

void flags_scrub_ruler()
{
    const WaveWindow w = wave_window(344200, 60000, 20, 440);
    const std::vector<std::int64_t> splits = {w.t_of(71), w.t_of(393), 700000};
    CHECK_EQ(flag_at(w, splits, 75, 22), 0);
    CHECK_EQ(flag_at(w, splits, 380, 22), 1);
    CHECK_EQ(flag_at(w, splits, 240, 22), -1);
    CHECK_EQ(flag_at(w, splits, 460, 22), -1); /* the third flag is outside the window: not tappable there */
    CHECK_EQ(flag_at(w, {}, 75, 22), -1);

    /* dragging the take 110 px to the left (a quarter of the window) moves the marker 15 s later */
    CHECK_EQ(scrub_marker(w, 344200, -110, 728000), 359200);
    CHECK_EQ(scrub_marker(w, 344200, 110, 728000), 329200);
    CHECK_EQ(scrub_marker(w, 1000, 440, 728000), 0);
    CHECK_EQ(scrub_marker(w, 727000, -440, 728000), 728000);

    CHECK_EQ(nudge_step_ms(false), 100);
    CHECK_EQ(nudge_step_ms(true), 1000);

    const std::vector<RulerTick> t = ruler_ticks(w, 5);
    CHECK_EQ(t.size(), std::size_t(5));
    CHECK(near(t[0].x, 20) && t[0].t == 314200);
    CHECK(near(t[1].x, 130) && t[1].t == 329200);
    CHECK(near(t[2].x, 240) && t[2].t == 344200);
    CHECK(near(t[4].x, 460) && t[4].t == 374200);
    CHECK_EQ(ruler_ticks(w, 1).size(), std::size_t(2));
}

/* FIT: one gain for the take (its loudest peak at 90 %), none for a silent take, capped at x64; the chip's words */
void fit_rule()
{
    auto eq = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    /* a loud take (-1 dBFS, 0.89): the gain is just over 1; a full-scale take is trimmed to 90 % */
    CHECK(eq(fit_gain_for(0.89f), 0.9f / 0.89f));
    CHECK(eq(fit_gain_for(1.0f), 0.9f));
    CHECK(eq(fit_gain_for(1.7f), 0.9f)); /* a peak is never above full scale */
    /* a quiet take: 0.12 (-18 dBFS) gets x7.5, 0.05 gets x18 */
    CHECK(eq(fit_gain_for(0.12f), 7.5f));
    CHECK(eq(fit_gain_for(0.05f), 18.0f));
    /* the cap: x64 from a peak of 0.9 / 64 (-37 dBFS) down to the silence limit (0.001, -60 dBFS) */
    CHECK(eq(fit_gain_for(0.9f / 64.0f), 64.0f));
    CHECK(eq(fit_gain_for(0.005f), 64.0f));
    CHECK(eq(fit_gain_for(0.001f), 64.0f));
    CHECK(eq(fit_gain_for(0.02f), 45.0f));
    /* a silent take: no gain (0 = none), noise is not inflated */
    CHECK(fit_gain_for(0.0f) == 0.0f);
    CHECK(fit_gain_for(0.00099f) == 0.0f);
    CHECK(fit_gain_for(-1.0f) == 0.0f);
    CHECK(fit_gain_for(std::nanf("")) == 0.0f);
    /* the level: times the gain, clamped to 1; no gain (0) or 1 leaves it; never below 0 */
    CHECK(eq(fit_level(0.1f, 7.5f), 0.75f));
    CHECK(fit_level(0.5f, 7.5f) == 1.0f);
    CHECK(fit_level(1.0f, 0.9f) == 0.9f);
    CHECK(fit_level(0.3f, 0.0f) == 0.3f);
    CHECK(fit_level(0.3f, 1.0f) == 0.3f);
    CHECK(fit_level(-0.2f, 8.0f) == 0.0f);
    /* the loudest peak of the take fills 90 % whatever the take's level (above the cap's range) */
    for (float peak : {0.9f, 0.3f, 0.12f, 0.02f, 0.0142f}) CHECK(eq(fit_level(peak, fit_gain_for(peak)), 0.9f));
    /* the chip text: TRUE, FIT with the gain rounded to a whole number (x1 allowed), FIT alone when there is no gain */
    CHECK_EQ(wave_chip_text(false, 7.5f), std::string("TRUE"));
    CHECK_EQ(wave_chip_text(false, 0.0f), std::string("TRUE"));
    CHECK_EQ(wave_chip_text(true, 7.5f), std::string("FIT x8"));
    CHECK_EQ(wave_chip_text(true, 7.4f), std::string("FIT x7"));
    CHECK_EQ(wave_chip_text(true, 0.9f), std::string("FIT x1"));
    CHECK_EQ(wave_chip_text(true, 1.01f), std::string("FIT x1"));
    CHECK_EQ(wave_chip_text(true, 64.0f), std::string("FIT x64"));
    CHECK_EQ(wave_chip_text(true, 0.0f), std::string("FIT"));
}

/* the IN / OUT tags at the 480x320 board: the main waveform over [20, 460), the mode chip "FIT x8" at [404, 458), rows
 * one tag high, a 2 px gap, 3 rows fit in the panel */
int order_of(WaveLayer l)
{
    int i = 0;
    for (WaveLayer o : kWaveDrawOrder) {
        if (o == l) return i;
        ++i;
    }
    return -1;
}

void tags()
{
    const int L = 20, R = 460, inset = 4, gap = 2;
    const TagSpan chip{404, 458};
    /* OUT at the right edge (its line at 458, inside): the tag ends at its line, under the chip -> row 1 */
    const TagSpan out_edge = tag_span(WaveEdge::Inside, true, 458, 40, L, R, inset);
    CHECK_EQ(out_edge.x1, 418);
    CHECK_EQ(out_edge.x2, 458);
    const TagSpan in_far = tag_span(WaveEdge::Left, false, 0, 80, L, R, inset);
    CHECK_EQ(in_far.x1, 24);
    CHECK_EQ(in_far.x2, 104);
    TagRows r = tag_rows(in_far, out_edge, chip, gap, 3);
    CHECK_EQ(r.in, 0);
    CHECK_EQ(r.out, 1);
    /* OUT beyond the right edge: right-aligned at the edge, under the chip, inside the panel */
    const TagSpan out_past = tag_span(WaveEdge::Right, true, 9999, 90, L, R, inset);
    CHECK_EQ(out_past.x2, R);
    CHECK_EQ(out_past.x1, R - 90);
    CHECK_EQ(tag_rows(in_far, out_past, chip, gap, 3).out, 1);
    /* an OUT just inside the left part of the window: clamped into the panel, never before its left edge */
    const TagSpan out_left = tag_span(WaveEdge::Inside, true, 30, 40, L, R, inset);
    CHECK_EQ(out_left.x1, L);
    CHECK_EQ(out_left.x2, 60);
    /* IN at the left edge: inside at the window's first px the tag starts at its line; beyond the edge at left + inset */
    const TagSpan in_first = tag_span(WaveEdge::Inside, false, 20, 30, L, R, inset);
    CHECK_EQ(in_first.x1, 20);
    CHECK_EQ(in_first.x2, 50);
    CHECK_EQ(tag_rows(in_first, out_past, chip, gap, 3).in, 0);
    /* an IN near the right edge: kept inside the panel (ends at its right edge), touches the chip -> row 1 */
    const TagSpan in_right = tag_span(WaveEdge::Inside, false, 450, 30, L, R, inset);
    CHECK_EQ(in_right.x1, 430);
    CHECK_EQ(in_right.x2, R);
    /* the play marker near a tag: the rows do not depend on lines (the tag covers the line: drawn above it) */
    CHECK(order_of(WaveLayer::Tags) > order_of(WaveLayer::PlayMarker));
    CHECK(order_of(WaveLayer::Tags) > order_of(WaveLayer::HandleLines));
    CHECK(order_of(WaveLayer::Tags) > order_of(WaveLayer::SplitLines));
    CHECK(order_of(WaveLayer::Tags) > order_of(WaveLayer::Bars));
    CHECK(order_of(WaveLayer::ModeChip) > order_of(WaveLayer::PlayMarker));
    CHECK(order_of(WaveLayer::ModeChip) > order_of(WaveLayer::SplitLines));
    CHECK(order_of(WaveLayer::FlagBoxes) > order_of(WaveLayer::ModeChip));
    CHECK(order_of(WaveLayer::FlagBoxes) > order_of(WaveLayer::Tags));
    CHECK(order_of(WaveLayer::FlagBoxes) > order_of(WaveLayer::PlayMarker));
    const TagSpan in_mid = tag_span(WaveEdge::Inside, false, 230, 30, L, R, inset); /* the marker at 240 crosses it */
    r = tag_rows(in_mid, out_past, chip, gap, 3);
    CHECK_EQ(r.in, 0);
    CHECK_EQ(r.out, 1);
    /* IN and OUT close together: the IN tag right of its line, the OUT tag left of its own; they would touch -> OUT drops */
    const TagSpan in_c = tag_span(WaveEdge::Inside, false, 200, 30, L, R, inset);
    const TagSpan out_c = tag_span(WaveEdge::Inside, true, 215, 40, L, R, inset);
    CHECK(in_c.overlaps(out_c, gap));
    r = tag_rows(in_c, out_c, chip, gap, 3);
    CHECK_EQ(r.in, 0);
    CHECK_EQ(r.out, 1);
    /* ... and do not when apart by more than the gap */
    const TagSpan out_apart = tag_span(WaveEdge::Inside, true, 273, 40, L, R, inset);
    CHECK(!in_c.overlaps(out_apart, gap));
    CHECK_EQ(tag_rows(in_c, out_apart, chip, gap, 3).out, 0);
    CHECK(in_c.overlaps(tag_span(WaveEdge::Inside, true, 271, 40, L, R, inset), gap)); /* 1 px gap: too close */
    /* the chip: a tag clear of it stays on row 0; both under it -> IN row 1, OUT row 2; never below the last row */
    CHECK_EQ(tag_rows(in_c, tag_span(WaveEdge::Inside, true, 400, 40, L, R, inset), chip, gap, 3).out, 0);
    const TagSpan in_u = tag_span(WaveEdge::Inside, false, 410, 30, L, R, inset);
    const TagSpan out_u = tag_span(WaveEdge::Inside, true, 450, 40, L, R, inset);
    r = tag_rows(in_u, out_u, chip, gap, 3);
    CHECK_EQ(r.in, 1);
    CHECK_EQ(r.out, 2);
    r = tag_rows(in_u, out_u, chip, gap, 1);
    CHECK_EQ(r.in, 0);
    CHECK_EQ(r.out, 0);
    CHECK_EQ(tag_rows(in_u, out_u, chip, gap, 0).out, 0);
    /* a tag wider than the panel is cut to it */
    const TagSpan wide = tag_span(WaveEdge::Right, true, 0, 1000, L, R, inset);
    CHECK_EQ(wide.x1, L);
    CHECK_EQ(wide.x2, R);
}

/* The main bars on the take's grid: bar k always covers frames [k * fpb, (k + 1) * fpb), whatever the window, so a bar
 * keeps its height while the take scrolls; only its x moves, by the window's own mapping, the same for every bar. */
void bar_grid_rule()
{
    /* a 64 s window at 48 kHz, 220 bars across (the binding's default) */
    BarGrid g = bar_grid(0, 3072000, 220);
    CHECK_EQ(g.fpb, 13964);   /* 3072000 / 220 = 13963.6 */
    CHECK_EQ(g.first, -111);  /* the window starts at -1536000, in bar -110; one more before it */
    CHECK_EQ(g.count, 223);   /* ceil(3072000 / 13964) + 3 */
    /* the HackBerry: 3.3 s at 16 kHz, 109.75 bars across (the 720x720 trim page) */
    g = bar_grid(17600, 52800, 109.75);
    CHECK_EQ(g.fpb, 481);
    CHECK_EQ(g.first, -20);   /* the window starts at frame -8800, in bar -19 (frames -9139 .. -8659); one more */
    /* every marker: the window [m - vis / 2, m + vis / 2] lies inside the bars read, with a whole bar to spare at each
     * side, the count is the same, and the first bar moves only by whole bars */
    int bad = 0;
    const std::int64_t vis = 52800;
    const BarGrid g0 = bar_grid(100000, vis, 109.75);
    for (std::int64_t d : {1, 7, 160, 240, 480, 481, 482, 1000, 4811, 52801, -1, -481, -99999}) {
        const std::int64_t m = 100000 + d;
        const BarGrid gm = bar_grid(m, vis, 109.75);
        const std::int64_t a = m - vis / 2, b = a + vis;
        if (gm.fpb != g0.fpb || gm.count != g0.count) ++bad;
        if (gm.first * gm.fpb > a - gm.fpb) ++bad;
        if ((gm.first + gm.count) * gm.fpb < b + gm.fpb) ++bad;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(bar_grid(5, 0, 0).fpb, 1); /* a zero window or width: at least one frame per bar */
    CHECK(near(bars_across(437, 4), 109.25, 1e-9));
    CHECK(near(bars_across(0, 4), 1.0, 1e-9));
    CHECK(near(bars_across(100, 0), 1.0, 1e-9));

    /* the x of a grid bar: the window's mapping (ms) at sub-ms precision. 16 kHz, 481 frames per bar = 30.0625 ms; the
     * 720x720 window: 3300 ms over 657 px (from x 30) */
    const WaveWindow w1 = wave_window(1100, 3300, 30, 657), w2 = wave_window(1117, 3300, 30, 657);
    const double step = grid_bar_x(w1, 11, 481, 16000) - grid_bar_x(w1, 10, 481, 16000);
    CHECK(near(step, 30.0625 * 657 / 3300, 1e-9)); /* 5.985 px: the page's 6 px pitch within 0.3 % */
    CHECK(near(grid_bar_x(w1, 0, 481, 16000), w1.x_of(0), 1e-9));
    /* the marker 17 ms later: every bar 3.38 px further left, the same for all (the bars move together, as the take) */
    bad = 0;
    for (std::int64_t k = -2; k < 120; ++k) {
        const double shift = grid_bar_x(w1, k, 481, 16000) - grid_bar_x(w2, k, 481, 16000);
        if (!near(shift, 17.0 * 657 / 3300, 1e-6)) ++bad;
    }
    CHECK_EQ(bad, 0);
    CHECK(near(grid_bar_x(w1, 3, 100, 0), w1.x_of_ms(3 * 100 * 1000.0 / 48000), 1e-9)); /* no rate: 48 kHz */
    CHECK(near(w1.x_of_ms(1100.5), w1.x_of(1100) + 0.5 * 657 / 3300, 1e-9));

    /* a bar at the window's edge is cut to it, a bar outside it shows nothing */
    BarSpan sp = clip_bar(27, 5, 30, 687);
    CHECK(sp.x1 == 30 && sp.x2 == 32);
    sp = clip_bar(685, 5, 30, 687);
    CHECK(sp.x1 == 685 && sp.x2 == 687);
    sp = clip_bar(100, 5, 30, 687);
    CHECK(sp.x1 == 100 && sp.x2 == 105);
    sp = clip_bar(20, 5, 30, 687);
    CHECK(sp.x2 <= sp.x1);
    sp = clip_bar(700, 5, 30, 687);
    CHECK(sp.x2 <= sp.x1);
}

/* The ruler's row at the 480x320 board: the frame's inner edge at 191, a 12 px line, 4 px ticks */
void ruler_rule()
{
    const RulerRow r = ruler_row(191, 12, 4, 1, 2, 176);
    CHECK_EQ(r.label_y, 177); /* 191 - 2 - 12 */
    CHECK_EQ(r.label_h, 12);
    CHECK_EQ(r.tick_y, 172);  /* 177 - 1 - 4 */
    CHECK_EQ(r.tick_h, 4);
    CHECK_EQ(r.main_bottom, 171); /* the waveform gives 5 px back to the ruler */
    CHECK_EQ(ruler_row(300, 12, 4, 1, 2, 200).main_bottom, 200); /* room enough: the waveform keeps its place */

    /* the labels: the first starts at the window's left edge, the last ends at its right edge, the others centred */
    const WaveWindow w = wave_window(30000, 60000, 20, 437);
    const std::vector<RulerTick> t = ruler_ticks(w, 5);
    std::vector<RulerLabel> l = ruler_labels(t, {20, 20, 20, 20, 20}, 20, 457, 4);
    CHECK(l.size() == 5 && l[0].shown && l[4].shown);
    CHECK(l[0].x1 == 20 && l[0].x2 == 40);
    CHECK(l[4].x1 == 437 && l[4].x2 == 457);
    CHECK(l[2].shown && l[2].x1 == 229 && l[2].x2 == 249); /* centred on 239 (238.5 rounded) */
    /* a label near an edge is moved inward, never cut: the window starting at 0 (first label 0:00 at the left edge) */
    l = ruler_labels({{25, 0}, {130, 1}}, {30, 30}, 20, 457, 4);
    CHECK(l[0].x1 == 20 && l[1].x1 == 115);
    l = ruler_labels({{450, 0}}, {30}, 20, 457, 4);
    CHECK(l[0].x2 == 457);
    /* no label for a tick outside the take (width 0); its neighbours still placed */
    l = ruler_labels(t, {0, 20, 20, 20, 0}, 20, 457, 4);
    CHECK(!l[0].shown && l[1].shown && l[3].shown && !l[4].shown);
    /* a label that would touch the one before it (within the gap) is left out, its tick stays */
    l = ruler_labels({{20, 0}, {45, 1}, {120, 2}}, {30, 30, 30}, 20, 457, 4);
    CHECK(l[0].shown && !l[1].shown && l[2].shown);
    /* a label wider than the window: none */
    l = ruler_labels({{100, 0}}, {500}, 20, 457, 4);
    CHECK(!l[0].shown);
    CHECK(ruler_labels({}, {}, 0, 10, 4).empty());
}

/* The trim page's waveform panel at every layout the app meets (the screen sizes of the scenes, headers from 30 to 64 px,
 * touch targets 44 to 79 px): the ruler's labels and ticks lie inside the panel's frame, under the main waveform, and the
 * labels inside the window (a label of h:mm:ss too); the waveform keeps a usable height. */
void panel_every_layout()
{
    struct Size {
        int w, h;
    };
    const Size sizes[] = {{320, 480}, {480, 320}, {480, 480}, {480, 800}, {640, 480}, {720, 720}, {800, 480}};
    int bad = 0, cases = 0;
    for (const Size &z : sizes)
        for (int body : {30, 40, 48, 64})
            for (double target : {44.0, 47.0, 56.0, 60.0, 64.0, 70.0, 79.0}) {
                const bool portrait = z.h > z.w && !square_screen(z.w, z.h);
                const Fit f = trim_fit(z.w, z.h, body, portrait, target);
                const int lh = static_cast<int>(std::lround(1.3 * f.px(9))) + 1; /* above Montserrat's 1.22 em */
                const WavePanel g = wave_panel(f, portrait, lh);
                ++cases;
                const int in_top = g.y + g.border, in_bottom = g.y + g.h - g.border;
                const int in_left = g.x + g.border, in_right = g.x + g.w - g.border;
                bool ok = g.ruler.label_y + g.ruler.label_h <= in_bottom - 1 && g.ruler.label_y >= in_top;
                ok = ok && g.ruler.tick_y + g.ruler.tick_h <= g.ruler.label_y && g.ruler.tick_h >= 1;
                ok = ok && g.main_y + g.main_h <= g.ruler.tick_y && g.main_h >= 20;
                ok = ok && g.main_x >= in_left && g.main_x + g.bars_w <= in_right && g.bars >= 40;
                ok = ok && g.over_y >= in_top && g.over_y + g.over_h <= g.main_y;
                /* the labels at a window starting at 0 and one ending at the take's end, "0:00" .. "1:01:01" wide */
                const WaveWindow w = wave_window(30000, 60000, g.main_x, g.bars_w);
                auto tw = [&f](int digits, int colons) { /* Montserrat Medium: digits 0.62 em, a colon 0.3 em (above) */
                    return static_cast<int>(std::lround((0.62 * digits + 0.3 * colons) * f.px(9))) + 1;
                };
                const std::vector<RulerLabel> l = ruler_labels(ruler_ticks(w, 5), {tw(3, 1), tw(3, 1), tw(3, 1), tw(5, 2), tw(5, 2)},
                                                               g.main_x, g.main_x + g.bars_w, f.px(4));
                ok = ok && l.front().shown && l.back().shown;
                for (const RulerLabel &x : l)
                    if (x.shown && (x.x1 < g.main_x || x.x2 > g.main_x + g.bars_w)) ok = false;
                if (!ok) {
                    ++bad;
                    std::printf("  panel %dx%d body %d target %.0f: label %d+%d tick %d+%d main %d+%d (x %d+%d, %d bars) inner %d..%d "
                                "x %d..%d, overview %d+%d, labels %d %d\n",
                                z.w, z.h, body, target, g.ruler.label_y, g.ruler.label_h, g.ruler.tick_y, g.ruler.tick_h, g.main_y,
                                g.main_h, g.main_x, g.bars_w, g.bars, in_top, in_bottom, in_left, in_right, g.over_y, g.over_h,
                                l.front().shown, l.back().shown);
                }
            }
    CHECK_EQ(bad, 0);
    CHECK_EQ(cases, 7 * 4 * 7);
    /* the boards at their own size: 480x320 gives the waveform 57 px (was 62) for a 12 px ruler line; 320x480 has room */
    const WavePanel b = wave_panel(trim_fit(480, 320, 40, false, 44), false, 12);
    CHECK_EQ(b.main_y, 114);
    CHECK_EQ(b.main_h, 57);
    CHECK_EQ(b.ruler.label_y, 177);
    CHECK_EQ(b.bars, 110);
    CHECK_EQ(b.bars_w, 439);
    const WavePanel q = wave_panel(trim_fit(320, 480, 40, true, 44), true, 12);
    CHECK_EQ(q.main_y, 122);
    CHECK_EQ(q.main_h, 150);
    CHECK_EQ(q.ruler.label_y, 283);
    CHECK_EQ(q.bars, 68);
}

} // namespace

void test_wave()
{
    window();
    overview_and_bars();
    flags_scrub_ruler();
    fit_rule();
    tags();
    bar_grid_rule();
    ruler_rule();
    panel_every_layout();
}
