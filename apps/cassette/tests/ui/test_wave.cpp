/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the waveform geometry (wave_geom.hpp): the window, handles and flags inside or as edge chips, the
 * overview, resampling, the tap on a flag, the scrub and the ruler; the IN / OUT tags' place and rows, the draw order.
 */
#include "check.hpp"
#include "wave_geom.hpp"

#include <cmath>

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

} // namespace

void test_wave()
{
    window();
    overview_and_bars();
    flags_scrub_ruler();
    fit_rule();
    tags();
}
