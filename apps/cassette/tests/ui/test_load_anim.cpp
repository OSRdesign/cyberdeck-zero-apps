/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the cassette LOAD animation's timeline (load_anim.hpp): positions at 0, 70, 190, 420, 600, 750 and
 * 900 ms, the empty gap, the bounce, the label reveal, the name hidden until the landing, the first load without a
 * drop-out, the input gate and Esc finishing the animation; and the handwriting font's size ladder.
 */
#include "check.hpp"
#include "load_anim.hpp"

#include <algorithm>
#include <cmath>

using namespace cassette::ui;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

void t_ladder()
{
    CHECK_EQ(hand_rung(22), 22);
    CHECK_EQ(hand_rung(31), 31);
    CHECK_EQ(hand_rung(30), 29);
    CHECK_EQ(hand_rung(28), 22);
    CHECK_EQ(hand_rung(21), 15);
    CHECK_EQ(hand_rung(9), 15);   /* under the smallest: the smallest */
    CHECK_EQ(hand_rung(80), 31);  /* over the largest: the largest */
    CHECK_EQ(hand_rung_below(31), 29);
    CHECK_EQ(hand_rung_below(22), 15);
    CHECK_EQ(hand_rung_below(15), 0);
}

void t_bezier()
{
    CHECK(near(bezier_at(0.42, 0, 1, 1, 0), 0));
    CHECK(near(bezier_at(0.42, 0, 1, 1, 1), 1));
    CHECK(bezier_at(0.42, 0, 1, 1, 0.5) < 0.5);       /* ease in: slow at first */
    CHECK(bezier_at(0.2, 0.7, 0.3, 1, 0.5) > 0.5);    /* the fall-in: fast at first */
    CHECK(near(bezier_at(0.42, 0, 0.58, 1, 0.5), 0.5, 1e-3));
    double last = -1;
    bool mono = true;
    for (int i = 0; i <= 100; ++i) { /* monotonic */
        const double v = bezier_at(0.2, 0.7, 0.3, 1, i / 100.0);
        mono = mono && v >= last;
        last = v;
    }
    CHECK(mono);
}

void t_totals()
{
    CHECK_EQ(load_total_ms(true), 900);
    CHECK_EQ(load_landing_ms(true), 600);
    CHECK_EQ(load_total_ms(false), 660);
    CHECK_EQ(load_landing_ms(false), 360);
}

void t_over_old()
{
    using Ph = LoadPose::Phase;
    /* t = 0: the old cassette still in its place, nothing of the new one */
    LoadPose p = load_pose(0, true);
    CHECK(p.phase == Ph::Out);
    CHECK(p.old_shown && !p.new_shown);
    CHECK(near(p.old_y, 0));
    CHECK(near(p.name, 0) && !p.landed);
    /* t = 70: the old cassette on its way down, ease in: less than half way, more than a start */
    p = load_pose(70, true);
    CHECK(p.phase == Ph::Out);
    CHECK(p.old_shown && !p.new_shown);
    CHECK(p.old_y > 5 && p.old_y < kLoadFar / 2);
    CHECK(near(p.old_y, kLoadFar * bezier_at(0.42, 0, 1, 1, 0.5)));
    /* just before 140 ms it is nearly out of the window, at 140 ms the empty gap starts */
    CHECK(load_pose(139, true).old_y > 0.95 * kLoadFar && load_pose(139, true).old_shown);
    /* t = 190: the empty window: nothing in view */
    p = load_pose(190, true);
    CHECK(p.phase == Ph::Gap);
    CHECK(!p.old_shown && !p.new_shown);
    CHECK(load_pose(140, true).phase == Ph::Gap);
    CHECK(load_pose(239, true).phase == Ph::Gap);
    /* t = 240: the new cassette starts above the window */
    p = load_pose(240, true);
    CHECK(p.phase == Ph::In);
    CHECK(p.new_shown && !p.old_shown);
    CHECK(near(p.new_y, -kLoadFar));
    /* t = 420: on its way down (180 ms into the fall: 60 % of the 300 ms, ease out) */
    p = load_pose(420, true);
    CHECK(p.phase == Ph::In);
    CHECK(p.new_shown);
    CHECK(p.new_y > -kLoadFar && p.new_y < kLoadBounce);
    CHECK(near(p.new_y, -kLoadFar + (kLoadFar + kLoadBounce) * bezier_at(0.2, 0.7, 0.3, 1, 0.6)));
    CHECK(p.new_y > -kLoadFar / 2); /* fast at first: past the middle at 60 % of the time */
    CHECK(near(p.name, 0) && !p.landed); /* no name yet */
    /* the bounce: +4 units at 540 ms (83 % of the way), back to 0 at 600 ms; never above 4, never above the window */
    CHECK(near(load_pose(540, true).new_y, kLoadBounce, 1e-6));
    double top = -1000;
    for (int t = 240; t <= 600; ++t) top = std::max(top, load_pose(t, true).new_y);
    CHECK(near(top, kLoadBounce, 1e-6));
    CHECK(load_pose(570, true).new_y > 0 && load_pose(570, true).new_y < kLoadBounce);
    CHECK(near(load_pose(599, true).new_y, 0, 0.1));
    /* before the landing the label shows no name */
    bool hidden = true, unlanded = true;
    for (int t = 0; t < 600; ++t) {
        hidden = hidden && near(load_pose(t, true).name, 0);
        unlanded = unlanded && !load_pose(t, true).landed;
    }
    CHECK(hidden);
    CHECK(unlanded);
    /* t = 600: landed, in place, the name starts to be written */
    p = load_pose(600, true);
    CHECK(p.phase == Ph::Write);
    CHECK(p.landed && p.new_shown);
    CHECK(near(p.new_y, 0));
    CHECK(near(p.name, 0));
    /* t = 750: half the name */
    p = load_pose(750, true);
    CHECK(p.phase == Ph::Write);
    CHECK(near(p.name, 0.5));
    CHECK(near(p.new_y, 0));
    /* the reveal never goes backwards */
    double last = -1;
    bool grows = true;
    for (int t = 600; t <= 900; ++t) {
        const double n = load_pose(t, true).name;
        grows = grows && n >= last;
        last = n;
    }
    CHECK(grows);
    /* t = 900: done, the whole name */
    p = load_pose(900, true);
    CHECK(p.phase == Ph::Done);
    CHECK(near(p.name, 1) && p.landed && near(p.new_y, 0) && p.new_shown);
    CHECK(near(load_pose(5000, true).name, 1));
}

void t_first_load()
{
    using Ph = LoadPose::Phase;
    /* nothing in the deck: no drop-out, no gap: the new cassette falls in at t = 0 (360 ms), the name follows (300 ms) */
    LoadPose p = load_pose(0, false);
    CHECK(p.phase == Ph::In);
    CHECK(!p.old_shown && p.new_shown);
    CHECK(near(p.new_y, -kLoadFar));
    CHECK(near(p.name, 0));
    bool no_old = true;
    for (int t = 0; t <= 660; ++t) no_old = no_old && !load_pose(t, false).old_shown;
    CHECK(no_old); /* no drop-out, no empty gap */
    CHECK(near(load_pose(300, false).new_y, kLoadBounce, 1e-6)); /* the bounce at 300 ms */
    p = load_pose(359, false);
    CHECK(p.phase == Ph::In && !p.landed);
    p = load_pose(360, false);
    CHECK(p.phase == Ph::Write && p.landed && near(p.new_y, 0) && near(p.name, 0));
    CHECK(near(load_pose(510, false).name, 0.5));
    p = load_pose(660, false);
    CHECK(p.phase == Ph::Done && near(p.name, 1));
    bool hidden = true;
    for (int t = 0; t < 360; ++t) hidden = hidden && near(load_pose(t, false).name, 0);
    CHECK(hidden); /* no name before the landing */
    bool no_gap = true;
    for (int t = 0; t <= 360; ++t) no_gap = no_gap && load_pose(t, false).new_shown;
    CHECK(no_gap);
}

void t_gate()
{
    LoadAnim a;
    CHECK(!a.active());
    CHECK(a.accepts(false) && a.accepts(true)); /* nothing runs: everything is let through */
    a.start(true);
    CHECK(a.active());
    CHECK(!a.accepts(false)); /* keys and touches are ignored for the 900 ms */
    CHECK(a.accepts(true));   /* ... but Esc */
    a.advance(450);
    CHECK(a.active() && a.t_ms() == 450 && !a.accepts(false));
    a.advance(449);
    CHECK(a.active() && !a.accepts(false));
    a.advance(1);
    CHECK(!a.active() && a.accepts(false)); /* 900 ms: over */
    CHECK(a.pose().phase == LoadPose::Phase::Done);

    /* Esc: the animation finishes at once (the cassette in place, the whole name), then Esc is handled as usual */
    a.start(true);
    a.advance(100);
    CHECK(!a.accepts(false));
    a.finish();
    CHECK(!a.active());
    CHECK(a.accepts(false));
    LoadPose p = a.pose();
    CHECK(p.phase == LoadPose::Phase::Done && p.landed && near(p.new_y, 0) && near(p.name, 1) && p.new_shown && !p.old_shown);
    a.advance(100); /* no-ops */
    CHECK(!a.active());

    /* the first load runs 660 ms */
    a.start(false);
    a.advance(659);
    CHECK(a.active());
    a.advance(1);
    CHECK(!a.active());
    /* a negative step moves nothing */
    a.start(true);
    a.advance(-5);
    CHECK_EQ(a.t_ms(), 0);
}

} // namespace

void test_load_anim()
{
    t_ladder();
    t_bezier();
    t_totals();
    t_over_old();
    t_first_load();
    t_gate();
}
