/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the win cascade's physics (core/win_sim.hpp): bounce, exit, determinism, the time bound, the order, the
 * stamps, at every size the UI is tested at. No LVGL.
 */
#include "test_util.hpp"

#include "view_layout.hpp"
#include "win_sim.hpp"

#include <algorithm>
#include <cstring>

using namespace klondike;

namespace {

struct Size {
    int w, h;
};
const Size kSizes[] = {{640, 480}, {800, 480}, {720, 720}, {1280, 800}, {480, 800}, {800, 1280}, {480, 640}};

/* The foundations' places and the launch order of the real UI: kings first, the four piles in turn. */
WinParams params(const Size &s, bool left, bool reduced, std::uint32_t seed)
{
    LayoutIn in;
    in.w = s.w;
    in.h = s.h;
    in.body_y = 56;
    in.hud_h = 26;
    in.left_handed = left;
    const Layout l = compute_layout(in);
    WinParams p;
    p.x0 = l.board.x;
    p.x1 = l.board.right();
    p.top = l.hud.y;
    p.floor = l.board.bottom();
    p.card_w = l.card_w;
    p.card_h = l.card_h;
    p.seed = seed;
    p.reduced = reduced;
    int n = 0;
    for (int rank = 13; rank >= 1; --rank)
        for (int f = 0; f < kFoundations; ++f) {
            p.launch[n].card = make_card(f, rank);
            p.launch[n].x = l.pile_x(foundation_pile(f));
            p.launch[n].y = l.pile_y(foundation_pile(f));
            ++n;
        }
    p.count = n;
    return p;
}

struct Trace {
    int steps = 0;
    std::uint64_t hash = 14695981039346656037ull;
    int max_active = 0;
    int stamps = 0, flights = 0;
    bool in_bounds = true, order_ok = true, once = true;
    int bounces = 0;
    int first_bounce_card = -1;
};

Trace run(const WinParams &p)
{
    WinSim sim;
    sim.init(p);
    Trace t;
    int seen_launch = 0;
    int left[kCards] = {};
    int last_y[kCards];
    int last_vy[kCards];
    std::memset(last_vy, 0, sizeof last_vy);
    for (int i = 0; i < kCards; ++i) last_y[i] = -100000;
    const int ymax = p.floor - p.card_h;
    while (!sim.done() && t.steps < 5000) {
        sim.step();
        ++t.steps;
        t.max_active = std::max(t.max_active, sim.active());
        for (int i = 0; i < sim.active(); ++i) {
            const Flyer &f = sim.flyer(i);
            if (f.fresh) {
                if (seen_launch >= p.count || f.card != p.launch[seen_launch].card) t.order_ok = false;
                ++seen_launch;
            }
            if (f.y > ymax || f.y < p.top || f.x + p.card_w <= p.x0 || f.x >= p.x1) t.in_bounds = false;
            t.stamps += f.stamp;
            ++t.flights;
            for (std::uint64_t v : {static_cast<std::uint64_t>(f.card), static_cast<std::uint64_t>(f.x + 5000),
                                    static_cast<std::uint64_t>(f.y + 5000), static_cast<std::uint64_t>(f.stamp)})
                t.hash = (t.hash ^ v) * 1099511628211ull;
            /* a bounce: the card was moving down and now moves up */
            const int vy = f.y - last_y[f.card];
            if (last_y[f.card] != -100000 && last_vy[f.card] > 0 && vy < 0) {
                ++t.bounces;
                if (t.first_bounce_card < 0) t.first_bounce_card = f.card;
            }
            last_vy[f.card] = vy;
            last_y[f.card] = f.y;
        }
        for (int i = 0; i < sim.left_count(); ++i) ++left[sim.left_card(i)];
    }
    for (int c = 0; c < kCards; ++c)
        if (left[c] != 1) t.once = false;
    t.once = t.once && sim.exited() == p.count && seen_launch == p.count;
    return t;
}

void test_all_sizes()
{
    std::printf("win cascade run time per size, normal (33 ms steps) / reduced:\n");
    for (const Size &s : kSizes) {
        for (bool left : {false, true}) {
            const Trace a = run(params(s, left, false, 4242));
            const Trace r = run(params(s, left, true, 4242));
            if (!left) std::printf("  %4dx%-4d %4d steps (%4.1f s)  /  %4d steps of 66 ms (%4.1f s)   bounces %d, max in flight %d / %d\n", s.w, s.h, a.steps,
                                   a.steps * kWinStepMs / 1000.0, r.steps, r.steps * 2 * kWinStepMs / 1000.0, a.bounces, a.max_active, r.max_active);
            CHECK(a.once && r.once);                    /* all 52 launched once and gone once */
            CHECK(a.order_ok && r.order_ok);            /* in the order the caller gave */
            CHECK(a.in_bounds && r.in_bounds);          /* never under the floor, never reported outside the area */
            CHECK(a.steps > 300 && a.steps <= 900);     /* bounded: well under the hard stop */
            CHECK(r.steps * 2 < a.steps);               /* reduced is shorter on the clock ... */
            CHECK(r.steps * 2 > a.steps / 3);           /* ... but not absurdly */
            CHECK(a.max_active <= kWinMaxActive && r.max_active <= kReducedActive);
            CHECK(a.bounces >= 52 * 3 / 2);             /* the cards bounce, a few times each */
            CHECK(a.stamps == a.flights);               /* a stamp every step */
            CHECK(r.stamps * 2 <= r.flights + 60 && r.stamps * 3 > r.flights); /* about every 2nd step */
        }
    }
}

void test_determinism()
{
    for (const Size &s : kSizes) {
        const Trace a = run(params(s, false, false, 77));
        const Trace b = run(params(s, false, false, 77));
        const Trace c = run(params(s, false, false, 78));
        CHECK(a.hash == b.hash && a.steps == b.steps);
        CHECK(a.hash != c.hash);
    }
}

/* A single card on a flat floor: the heights of successive bounce peaks shrink by about the restitution squared. */
void test_bounce_energy()
{
    WinParams p;
    p.x0 = 0;
    p.x1 = 200;
    p.top = 0;
    p.floor = 300;
    p.card_w = 20;
    p.card_h = 30;
    p.count = 1;
    p.launch[0] = {make_card(0, 13), 0, 20}; /* at the left edge: a throw to the right crosses the whole width */
    const int ymax = p.floor - p.card_h;
    int peak[40] = {};
    int peaks = 0;
    for (std::uint32_t seed = 1; seed < 400 && peaks < 4; ++seed) { /* the first seed that throws slowly to the right */
        p.seed = seed;
        WinSim sim;
        sim.init(p);
        int prev_y = 20, dir = 0, n = 0;
        peaks = 0;
        for (int i = 0; i < 600 && !sim.done(); ++i) {
            sim.step();
            if (!sim.active()) break;
            const int y = sim.flyer(0).y;
            CHECK(y <= ymax);
            const int d = y - prev_y;
            if (dir <= 0 && d > 0 && n < 40) peak[n++] = ymax - prev_y; /* an apex: height above the floor */
            if (d != 0) dir = d;
            prev_y = y;
        }
        peaks = n;
    }
    CHECK(peaks >= 4);
    for (int i = 2; i < peaks; ++i) {
        if (peak[i - 1] < 12) break;                                             /* pixel noise below this */
        CHECK(peak[i] < peak[i - 1]);                                            /* each bounce lower than the last */
        const double ratio = static_cast<double>(peak[i]) / std::max(1, peak[i - 1]);
        CHECK(ratio > 0.45 && ratio < 0.78);                                     /* 0.76^2 .. 0.84^2 = 0.58 .. 0.71 */
    }
}

void test_exit_condition()
{
    for (int dir = 0; dir < 2; ++dir) {
        WinParams p;
        p.x0 = 100;
        p.x1 = 500;
        p.top = 0;
        p.floor = 400;
        p.card_w = 40;
        p.card_h = 56;
        p.count = 1;
        p.seed = 9 + dir;
        p.launch[0] = {make_card(1, 5), dir ? 440 : 120, 10};
        WinSim sim;
        sim.init(p);
        int steps = 0;
        int lastx = 0;
        while (!sim.done() && steps < 2000) {
            sim.step();
            ++steps;
            if (sim.active()) lastx = sim.flyer(0).x;
        }
        CHECK(sim.done() && sim.left_count() == 1);
        /* it left through a side: its last visible position was close to that edge, never above the area */
        CHECK(lastx < p.x1 && lastx + p.card_w > p.x0);
        CHECK(!sim.step());                         /* nothing moves after the end */
        CHECK(sim.exited() == 1);
    }
}

void test_hard_stop_and_empty()
{
    WinParams p;
    WinSim sim;
    sim.init(p); /* no launches */
    CHECK(sim.done() && !sim.step());
}

} // namespace

void test_winsim_group()
{
    test_all_sizes();
    test_determinism();
    test_bounce_energy();
    test_exit_condition();
    test_hard_stop_and_empty();
}
