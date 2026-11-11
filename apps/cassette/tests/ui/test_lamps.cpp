/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the meter panel's lamps in the portrait and square layouts, on the screen (meter_lamps.hpp, what the
 * page draws): the list MON / LIM / LOW CUT at the right edge and PEAK at the bottom left. At 320x480, 480x800, 480x480 and
 * 720x720, with one channel and with two: no lamp dot or label touches the bar (its whole object: the 15 steps, the
 * peak-hold tick at full scale above, below and after them), IN / L R, the scale labels, the TARGET bracket or caption, PEAK
 * or each other; the dots keep at least 6 design px after the last step; the labels end inside the panel's frame and keep
 * the size they had (10, 15, 9, 14 px). The bar keeps its 15 steps and one pitch, the scale and the bracket follow it.
 * The regression check: ce9c9e7's geometry (the board's bar and the list at its design place) fails the same check at
 * 480x800 (the rounded steps ran into the MON dot).
 */
#include "check.hpp"
#include "fit.hpp"
#include "meter_lamps.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

using namespace cassette::ui;

namespace {

/* An upper bound of the faces' widths: caps at 0.76 em, the widest (W, M) at 1.0, narrow ones less, plus 0.5 px a letter
 * and the letter spacing between them. */
double est_width(const std::string &t, bool, int px, int ls)
{
    double w = 0;
    for (char c : t) w += (c == ' ' ? 0.3 : c == 'I' || c == '1' ? 0.36 : c == 'W' || c == 'M' ? 1.0 : 0.76) * px + 0.5;
    return std::ceil(w + ls * (double(t.size()) - 1));
}

/* Narrow texts (half the estimate): the list right aligned leaves the bar more room; the same rules must hold. */
double narrow_width(const std::string &t, bool b, int px, int ls) { return std::ceil(est_width(t, b, px, ls) / 2); }

struct Screen {
    const char *name;
    int w, h, body_y;
    bool square;
    int label_px; /* the list's label size before this change (ce9c9e7): it stays */
};
/* the toolkit header's height at these sizes in the scenes (ppmm 608): 40, 56, 64, 64 */
const Screen kScreens[] = {{"320x480", 320, 480, 40, false, 10},
                           {"480x800", 480, 800, 56, false, 15},
                           {"480x480", 480, 480, 64, true, 9},
                           {"720x720", 720, 720, 64, true, 14}};

Fit screen_fit(const Screen &sc) { return sc.square ? square_fit(sc.w, sc.h, sc.body_y) : make_fit(sc.w, sc.h, sc.body_y, 320, 480, 40, {}); }

/* Every rule of the list against the rest of the panel, with one channel and with two; `why` names the first broken one. */
bool panel_clear(const Fit &f, const MeterSpec &s, const MeterLayout &m, const TextWidthFn &width, std::string *why)
{
    auto fail = [&](const char *w) {
        if (why) *why = w;
        return false;
    };
    const double fr = f.px(3); /* the panel's frame */
    const DRectF panel{double(f.x(s.panel.x)), double(f.y(s.panel.y)), double(f.w(s.panel.x, s.panel.w)), double(f.h(s.panel.y, s.panel.h))};
    const DRectF inner{panel.x + fr, panel.y + fr, panel.w - 2 * fr, panel.h - 2 * fr};
    if (m.lamp_x - m.bar.last_r() < f.px(kLampGapSeg)) return fail("dot nearer the last step than 6 design px");
    if (m.lamp_x < m.bar.tick_r() + f.px(kLampGapTick)) return fail("dot nearer the full-scale tick than 3 design px");
    if (m.bar.tick_r() > m.bar.right()) return fail("the tick outside the bar object");
    for (bool mono : {false, true}) {
        const std::vector<DRectF> items = meter_items_px(f, m, s, mono, width);
        for (int i = 0; i < kLampRows; ++i) {
            if (!rect_inside(m.lamp[i], inner) || !rect_inside(m.label[i], inner)) return fail("lamp or label outside the frame");
            if (rects_overlap(m.lamp[i], m.label[i])) return fail("dot over its label");
            for (const DRectF &o : items)
                if (rects_overlap(m.lamp[i], o) || rects_overlap(m.label[i], o)) return fail("lamp or label over a meter item");
            for (int j = i + 1; j < kLampRows; ++j)
                if (rects_overlap(m.lamp[i], m.lamp[j]) || rects_overlap(m.label[i], m.label[j]) ||
                    rects_overlap(m.lamp[i], m.label[j]) || rects_overlap(m.label[i], m.lamp[j]))
                    return fail("rows overlap");
        }
        /* the meter's own items keep inside the frame */
        for (const DRectF &o : items)
            if (!rect_inside(o, inner)) return fail("meter item outside the frame");
    }
    return true;
}

void check_layout(const Screen &sc, const Fit &f, const MeterSpec &s, const TextWidthFn &width, bool print)
{
    const MeterLayout m = meter_layout(f, s, width);
    std::string why;
    const bool ok = panel_clear(f, s, m, width, &why);
    if (!ok) std::fprintf(stderr, "%s: %s\n", sc.name, why.c_str());
    CHECK(ok);
    /* the label size and the dot size stay; one aligned list */
    CHECK_EQ(m.label_px, f.px(s.lamps.px));
    if (print) CHECK_EQ(m.label_px, sc.label_px);
    CHECK_EQ(m.lamp_d, f.px(s.lamps.lamp_d));
    for (int i = 0; i < kLampRows; ++i) {
        CHECK_EQ(m.lamp[i].x, m.lamp_x);
        CHECK_EQ(m.label[i].x, m.label_x);
        CHECK_EQ(m.lamp[i].w, double(m.lamp_d));
        CHECK_EQ(m.lamp[i].h, double(m.lamp_d));
        CHECK_EQ(m.label[i].h, m.label[0].h);
        CHECK(m.label[i].x >= m.lamp[i].r());
    }
    CHECK(std::fabs((m.lamp[1].y - m.lamp[0].y) - (m.lamp[2].y - m.lamp[1].y)) <= 1); /* one pitch (whole px) */
    CHECK(m.lamp[1].y > m.lamp[0].y);
    /* the bar: never wider than the board's, never narrower than needed for one px gaps, LEDs at least 60 % with the board's gap */
    CHECK(m.bar.x0 == m.board.x0 && m.bar.tick_w == m.board.tick_w);
    CHECK(m.bar.led_w <= m.board.led_w && m.bar.gap <= m.board.gap && m.bar.gap >= 1);
    if (m.bar.gap == m.board.gap) CHECK(m.bar.led_w >= std::ceil(m.board.led_w * 0.6));
    /* the scale labels, the bracket and TARGET follow the steps: their centres in steps from the first LED are the board's */
    auto steps_at = [&](double screen_x) { return (screen_x - m.bar.x0) / m.bar.pitch(); };
    for (const MeterText &t : s.scale) {
        const DRectF r = meter_text_px(f, m, t, width);
        const double board_c = (t.x - s.x0 + (t.w > 0 ? f.w(t.x, t.w) : width(t.text, t.bold, f.px(t.px), meter_ls(t.ls, f.s))) / f.s / 2) /
                               (s.led_w + s.gap);
        CHECK(std::fabs(steps_at(r.x + r.w / 2) - board_c) * m.bar.pitch() <= 1);
    }
    const DRectF br = meter_bracket_px(f, m, s);
    CHECK(std::fabs(steps_at(br.x) - (s.bracket.x - s.x0) / (s.led_w + s.gap)) * m.bar.pitch() <= 1);
    CHECK(std::fabs(steps_at(br.r()) - (s.bracket.r() - s.x0) / (s.led_w + s.gap)) * m.bar.pitch() <= 1);
    if (print)
        std::printf("lamps %s: bar x0 %d led %d gap %d (board %d + %d) last step ends %d, object %d; dot x %.0f (%d px), label x %.0f "
                    "(%d px, %s), gap to the last step %.0f px, to the full-scale tick %.0f px, label right %.0f, frame inner right %d\n",
                    sc.name, m.bar.x0, m.bar.led_w, m.bar.gap, m.board.led_w, m.board.gap, m.bar.last_r(), m.bar.right(), m.lamp_x,
                    m.lamp_d, m.label_x, m.label_px, m.text(2), m.lamp_x - m.bar.last_r(), m.lamp_x - m.bar.tick_r(),
                    std::max({m.label[0].r(), m.label[1].r(), m.label[2].r()}), f.x(s.panel.r()) - f.px(3));
}

/* ce9c9e7's geometry on the screen: the board's bar (every length rounded on its own) and the list right aligned in its
 * design strip (portrait 256..296: x0 = 296 - (7 + 3 + the widest label), in design px). */
MeterLayout old_portrait_layout(const Fit &f, const MeterSpec &s, const TextWidthFn &width)
{
    MeterLayout m = meter_layout(f, s, width);
    m.bar = m.board;
    const int ls = meter_ls(s.lamps.ls, f.s);
    const double wmax = std::max({width("MON", true, m.label_px, ls), width("LIM", true, m.label_px, ls), width("CUT", true, m.label_px, ls)}) / f.s;
    const double x0 = s.lamps.area.r() - (s.lamps.lamp_d + s.lamps.gap + wmax);
    m.short_cut = true;
    m.lamp_x = f.x(x0);
    m.label_x = f.x(x0 + s.lamps.lamp_d + s.lamps.gap);
    for (int i = 0; i < kLampRows; ++i) {
        m.lamp[i].x = m.lamp_x;
        m.label[i].x = m.label_x;
    }
    return m;
}

} // namespace

void test_lamps()
{
    /* every layout, both channel counts, two label width models; the square at the touch targets that shrink its window */
    for (const Screen &sc : kScreens) {
        const Fit f = screen_fit(sc);
        if (sc.square) {
            for (double t : {0.0, 58.0, 72.0, 80.0}) {
                check_layout(sc, f, square_meter_spec(sq_geometry(t)), est_width, t == 0.0);
                for (int body : {40, 48, 56, 72}) { /* other header heights: other scales and centring offsets */
                    Screen o = sc;
                    o.body_y = body;
                    check_layout(o, screen_fit(o), square_meter_spec(sq_geometry(t)), est_width, false);
                    check_layout(o, screen_fit(o), square_meter_spec(sq_geometry(t)), narrow_width, false);
                }
                check_layout(sc, f, square_meter_spec(sq_geometry(t)), narrow_width, false);
                CHECK_EQ(square_meter_spec(sq_geometry(t)).panel.h, 106.0); /* fixed in every state */
            }
        } else {
            check_layout(sc, f, portrait_meter_spec(), est_width, true);
            check_layout(sc, f, portrait_meter_spec(), narrow_width, false);
            CHECK_EQ(portrait_meter_spec().panel.h, 68.0);
        }
    }
    /* the regression: ce9c9e7 at 480x800 (stereo and mono share the bar's width): the rounded steps (17 + 5 px) end at x 394,
     * right of the MON dot's left, and the same check fails; at 320x480 (no rounding) it held */
    {
        const Fit f = screen_fit(kScreens[1]);
        const MeterSpec s = portrait_meter_spec();
        const MeterLayout old = old_portrait_layout(f, s, est_width);
        CHECK_EQ(old.board.last_r(), 394);
        CHECK(old.lamp_x < old.bar.last_r());
        std::string why;
        CHECK(!panel_clear(f, s, old, est_width, &why));
        const MeterLayout now = meter_layout(f, s, est_width);
        CHECK(panel_clear(f, s, now, est_width, nullptr));
        const Fit f320 = screen_fit(kScreens[0]);
        CHECK(panel_clear(f320, s, old_portrait_layout(f320, s, est_width), est_width, nullptr));
    }
    /* where nothing ran into the list the bar is the board's, pixel for pixel (320x480, 720x720) */
    CHECK(meter_layout(screen_fit(kScreens[0]), portrait_meter_spec(), est_width).bar.led_w == 11);
    {
        const MeterLayout m = meter_layout(screen_fit(kScreens[3]), square_meter_spec(sq_geometry(0)), est_width);
        CHECK(m.bar.led_w == m.board.led_w && m.bar.gap == m.board.gap);
    }
    /* the label: LOW CUT where the board's bar leaves room for it, CUT where it does not */
    {
        const Fit f = screen_fit(kScreens[0]);
        auto tiny = [](const std::string &t, bool, int, int) { return 1.0 * t.size(); };
        CHECK(!meter_layout(f, portrait_meter_spec(), tiny).short_cut);
        CHECK(meter_layout(f, portrait_meter_spec(), est_width).short_cut);
        CHECK_EQ(std::string(meter_layout(f, portrait_meter_spec(), tiny).text(2)), std::string("LOW CUT"));
        CHECK_EQ(std::string(meter_layout(f, portrait_meter_spec(), est_width).text(2)), std::string("CUT"));
        CHECK_EQ(std::string(meter_layout(f, portrait_meter_spec(), est_width).text(0)), std::string("MON"));
        CHECK_EQ(std::string(meter_layout(f, portrait_meter_spec(), est_width).text(1)), std::string("LIM"));
    }
    /* PEAK where ce9c9e7 had it */
    {
        const PeakGeo p = portrait_meter_spec().peak;
        CHECK(p.lamp.x == 26 && p.lamp.y == 269.5 && p.lamp.w == 12 && p.label.x == 42 && p.label.y == 270);
        const SqGeo g = sq_geometry(60);
        const PeakGeo q = square_meter_spec(g).peak;
        CHECK(q.lamp.x == 53 && q.lamp.y == g.meter_y - 440 + 496 && q.lamp.w == 22 && q.label.x == 33);
    }
    /* the off look of MON, LIM and LOW CUT: a neutral dark grey dot (not a dim green) and a dimmed label (about 55 %) */
    {
        const unsigned r = (col::kListLampOff >> 16) & 0xFF, gr = (col::kListLampOff >> 8) & 0xFF, b = col::kListLampOff & 0xFF;
        CHECK(r < 0x50 && gr < 0x50 && b < 0x50);                              /* dark */
        CHECK(std::max({r, gr, b}) - std::min({r, gr, b}) <= 8);               /* neutral: no channel stands out */
        CHECK(col::kListLampOff != col::kMonOff && col::kListLampOff != col::kLampGreen);
        CHECK(col::kListLabelOffOpa >= 128 && col::kListLabelOffOpa <= 150);   /* 50-59 % */
    }
}
