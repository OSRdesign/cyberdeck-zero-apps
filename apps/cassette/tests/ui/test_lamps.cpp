/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the meter panel's lamps in the portrait and square layouts: the list MON / LIM / LOW CUT at the right
 * edge and PEAK at the bottom left. Every lamp and label lies inside the panel's frame and clear of the bars, IN / L R,
 * the scale labels, the TARGET bracket and caption and of each other, with one channel and with two; at the layouts' real
 * screen sizes too (after the board-to-screen rounding).
 */
#include "check.hpp"
#include "fit.hpp"
#include "meter_lamps.hpp"

#include <cmath>

using namespace cassette::ui;

namespace {

/* An upper bound of the bold face's width: caps at 0.76 em, the widest (W, M) at 1.0, narrow ones less, plus 0.5 px spacing. */
double est_width(const std::string &t, double px)
{
    double w = 0;
    for (char c : t) w += (c == ' ' ? 0.3 : c == 'I' ? 0.36 : c == 'W' || c == 'M' ? 1.0 : 0.76) * px + 0.5;
    return w;
}

/* Everything the panel holds, from the list, PEAK and the other items: nothing overlaps, everything is inside the frame. */
void check_panel(const MeterLampSpec &s, const MeterLampGeo &g, const PeakGeo &pk, const std::vector<DRectF> &others)
{
    const DRectF inner{s.panel.x + 4, s.panel.y + 4, s.panel.w - 8, s.panel.h - 8}; /* the 3 px frame and a px */
    CHECK(rect_inside(s.area, inner));
    std::vector<DRectF> all = others;
    all.push_back(pk.lamp);
    all.push_back(pk.label);
    CHECK(rect_inside(pk.lamp, inner));
    CHECK(rect_inside(pk.label, inner));
    CHECK(!rects_overlap(pk.lamp, pk.label));
    for (int i = 0; i < kLampRows; ++i) {
        CHECK(rect_inside(g.lamp[i], s.area));
        CHECK(rect_inside(g.label[i], s.area));
        CHECK(g.label[i].x >= g.lamp[i].r()); /* the label follows its lamp */
        CHECK(!rects_overlap(g.lamp[i], g.label[i]));
        for (const DRectF &o : all) {
            CHECK(!rects_overlap(g.lamp[i], o));
            CHECK(!rects_overlap(g.label[i], o));
        }
    }
    for (int i = 0; i < kLampRows; ++i)
        for (int j = i + 1; j < kLampRows; ++j) {
            CHECK(!rects_overlap(g.lamp[i], g.lamp[j]));
            CHECK(!rects_overlap(g.label[i], g.label[j]));
            CHECK(!rects_overlap(g.lamp[i], g.label[j]));
            CHECK(!rects_overlap(g.label[i], g.lamp[j]));
        }
    /* one aligned list: lamps at one x, labels at one x, one size, one pitch, top to bottom MON, LIM, LOW CUT */
    for (int i = 1; i < kLampRows; ++i) {
        CHECK_EQ(g.lamp[i].x, g.lamp[0].x);
        CHECK_EQ(g.label[i].x, g.label[0].x);
        CHECK_EQ(g.lamp[i].w, g.lamp[0].w);
        CHECK_EQ(g.label[i].h, g.label[0].h);
        CHECK(g.lamp[i].y > g.lamp[i - 1].y);
    }
    CHECK(std::fabs((g.lamp[1].y - g.lamp[0].y) - (g.lamp[2].y - g.lamp[1].y)) < 1e-9);
    CHECK(std::fabs((g.label[1].y - g.label[0].y) - (g.label[2].y - g.label[1].y)) < 1e-9);
    /* PEAK is left of the list and in the panel's left half */
    CHECK(pk.label.r() < g.lamp[0].x);
}

} // namespace

void test_lamps()
{
    /* portrait: the panel is {16, 220, 288, 68}; the same numbers at 320x480 and 480x800 (design px) */
    {
        const MeterLampSpec s = portrait_lamp_spec();
        const MeterLampGeo g = meter_lamps_column(s, est_width);
        check_panel(s, g, portrait_peak(), portrait_meter_items());
        CHECK_EQ(s.panel.h, 68.0); /* the panel does not grow */
        CHECK(g.short_cut);        /* LOW CUT does not fit 40 px at 9 px type */
    }
    /* square: with two channels and with one, the lamps never move and never touch what moves around them */
    for (double t : {0.0, 58.0, 72.0, 80.0}) {
        const SqGeo sg = sq_geometry(t);
        const MeterLampSpec s = square_lamp_spec(sg);
        const MeterLampGeo g = meter_lamps_column(s, est_width);
        CHECK_EQ(s.panel.h, 106.0); /* fixed in every state */
        check_panel(s, g, square_peak(sg), square_meter_items(sg, false));
        check_panel(s, g, square_peak(sg), square_meter_items(sg, true));
    }
    /* the label: LOW CUT where it fits the list, CUT where it does not */
    {
        const MeterLampSpec s = portrait_lamp_spec();
        auto narrow = [](const std::string &t, double) { return 2.0 * t.size(); };
        auto wide = [](const std::string &t, double) { return 20.0 * t.size(); };
        CHECK(!meter_lamps_column(s, narrow).short_cut);
        CHECK(meter_lamps_column(s, wide).short_cut);
        CHECK_EQ(std::string(meter_lamps_column(s, wide).text(2)), std::string("CUT"));
        CHECK_EQ(std::string(meter_lamps_column(s, narrow).text(2)), std::string("LOW CUT"));
        CHECK_EQ(std::string(meter_lamps_column(s, narrow).text(0)), std::string("MON"));
        CHECK_EQ(std::string(meter_lamps_column(s, narrow).text(1)), std::string("LIM"));
    }
    /* at the real screens, in pixels: the rounded boxes keep clear of each other (the fit's rounding) */
    for (int w : {720, 480}) {
        const Fit f = square_fit(w, w, w == 720 ? 64 : 42);
        const SqGeo sg = sq_geometry(60);
        const MeterLampSpec s = square_lamp_spec(sg);
        const MeterLampGeo g = meter_lamps_column(s, est_width);
        auto px = [&](const DRectF &r) {
            return DRectF{double(f.x(r.x)), double(f.y(r.y)), double(f.w(r.x, r.w)), double(f.h(r.y, r.h))};
        };
        const PeakGeo pk = square_peak(sg);
        for (bool mono : {false, true}) {
            for (const DRectF &o : square_meter_items(sg, mono)) {
                CHECK(!rects_overlap(px(pk.lamp), px(o)));
                CHECK(!rects_overlap(px(pk.label), px(o)));
                for (int i = 0; i < kLampRows; ++i) {
                    CHECK(!rects_overlap(px(g.lamp[i]), px(o)));
                    CHECK(!rects_overlap(px(g.label[i]), px(o)));
                }
            }
        }
        const DRectF panel = px(s.panel);
        for (int i = 0; i < kLampRows; ++i) CHECK(rect_inside(px(g.lamp[i]), panel));
        CHECK(rect_inside(px(pk.lamp), panel));
        CHECK(rect_inside(px(pk.label), panel));
    }
}
