/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the touch areas (core/layout grow_hit, split_pair): two neighbours' grown areas meet in the middle of the
 * gap and each keeps the minimum size. The mini player's case of the design QA (play 44 and next 40 design px, 4 apart,
 * grown to 79 px at 640x480) is one of them.
 */
#include "check.hpp"

#include "layout/layout.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace vinyl::layout;

namespace {

bool contains(const R &outer, const R &inner) {
    return inner.x >= outer.x - 1e-9 && inner.y >= outer.y - 1e-9 && inner.right() <= outer.right() + 1e-9 &&
           inner.bottom() <= outer.bottom() + 1e-9;
}

HitArea grown(const R &d, double target) { return HitArea{d, grow_hit(d, target, target)}; }

bool none_overlap(const NowHitSet &h) {
    std::vector<const HitArea *> all;
    for (const std::vector<HitArea> *v : {&h.chips, &h.seek, &h.keys, &h.bottom})
        for (const HitArea &a : *v) all.push_back(&a);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            if (overlaps(all[i]->area, all[j]->area)) return false;
    return true;
}

/* The small class at 480x320 (NP-480x320, owner 2026-11-13): the output button, the volume group under it, the seek
 * row and the transport keys share the info column's height. The target (44) does not fit all four: the button keeps
 * 44 (it grows up over the album, nothing tappable there), the group and the seek row get 42 each (their boundary
 * moved off the middle of the gap so neither has less), the keys their drawn 48; no area overlaps another, each covers
 * what it draws (owner rule R-4: under the 7 mm, listed in the QA notes). */
void test_row_hits() {
    const double t = 44;
    NowHitSet h;
    h.chips = {grown(R{236, 146, 224, 28}, t)};
    h.bottom = {grown(R{236, 178, 56, 34}, t), grown(R{380, 178, 80, 34}, t)};
    h.seek = {HitArea{R{63, 225, 354, 18}, grow_hit(R{63, 225, 354, 18}, 0, t)}};
    const double kx[6] = {20, 90, 160, 262, 332, 402}, kw[6] = {56, 56, 88, 56, 56, 58};
    for (int i = 0; i < 6; ++i) h.keys.push_back(grown(R{kx[i], 258, kw[i], 48}, t));
    place_now_hits(h, t, 40, 480, 320);
    CHECK(none_overlap(h));
    CHECK_NEAR(h.chips[0].area.y, 130.0, 1e-9);
    CHECK_NEAR(h.chips[0].area.h, 44.0, 1e-9);
    for (const HitArea &b : h.bottom) {
        CHECK_NEAR(b.area.y, 174.0, 1e-9);
        CHECK_NEAR(b.area.h, 42.0, 1e-9);
    }
    CHECK_NEAR(h.seek[0].area.y, 216.0, 1e-9);
    CHECK_NEAR(h.seek[0].area.h, 42.0, 1e-9);
    for (const HitArea &k : h.keys) {
        CHECK_NEAR(k.area.y, 258.0, 1e-9);
        CHECK(k.area.h >= 48 - 1e-9);
        CHECK(k.area.bottom() <= 320 + 1e-9);
    }
    for (const std::vector<HitArea> *v : {&h.chips, &h.seek, &h.keys, &h.bottom})
        for (const HitArea &a : *v) CHECK(contains(a.area, a.drawn));
    CHECK_NEAR(now_rows_min_height(h), 42.0, 1e-9);
    /* keys wider than the target keep their drawn width; narrower neighbours meet in the middle of their gap */
    CHECK_NEAR(h.keys[0].area.w, 56.0, 1e-9);
    NowHitSet row;
    row.keys = {grown(R{100, 100, 30, 44}, 44), grown(R{134, 100, 30, 44}, 44)};
    place_now_hits(row, 44, 40, 480, 320);
    CHECK(none_overlap(row));
    CHECK_NEAR(row.keys[0].area.right(), 132.0, 1e-9);
    CHECK_NEAR(row.keys[1].area.x, 132.0, 1e-9);
    CHECK_NEAR(row.keys[0].area.x, 93.0, 1e-9); /* the free side as grown */
    /* with room (a taller screen), every row the target high, centred where it can be */
    NowHitSet tall;
    tall.chips = {grown(R{100, 100, 200, 30}, 50)};
    tall.keys = {grown(R{100, 300, 60, 40}, 50)};
    place_now_hits(tall, 50, 40, 480, 480);
    CHECK_NEAR(tall.chips[0].area.h, 50.0, 1e-9);
    CHECK_NEAR(tall.keys[0].area.h, 50.0, 1e-9);
    CHECK_NEAR(tall.keys[0].area.y, 295.0, 1e-9);
}

/* the board screenshot of 2026-11-13: "-3..." beside the seek bar on a one-hour mixtape */
void test_time_and_chips() {
    CHECK_EQ(track_time_text(74000, false, true), std::string("01:14"));
    CHECK_EQ(track_time_text(231000, false, false), std::string("3:51"));
    CHECK_EQ(track_time_text(2190000, false, false), std::string("36:30"));
    CHECK_EQ(track_time_text(74000, true, true), std::string("0:01:14")); /* a track of an hour or more: h:mm:ss */
    CHECK_EQ(track_time_text(3676000, true, false), std::string("1:01:16"));
    CHECK_EQ(track_time_text(0, true, false), std::string("0:00:00"));
    /* the widest text of each format holds every value of it (mono digits: the length decides) */
    CHECK(std::string(track_time_widest(false, true)).size() >= std::string("-") .size() + track_time_text(3599000, false, false).size());
    CHECK(std::string(track_time_widest(true, true)).size() >= 1 + track_time_text(99 * 3600000ull - 1000, true, false).size());
    CHECK(std::string(track_time_widest(true, false)).size() >= track_time_text(36000000, true, true).size());

}

/* Design QA run 3 (D-33): every touch rectangle of now playing, of the queue page and of the browser's mini player, at
 * every QA size with the TARGET Vinyl uses there, and no two of them overlap. `row` is the harness's ROW at the scenes'
 * density (its size check): every area at least that high; row 0: a board's own density where the approved layout has
 * less room than the 7 mm (owner rule R-4: the heights are printed for the QA notes, qa-info.txt). */
struct QaSize {
    int w, h, body, target, row;
    const char *what;
};
const QaSize kQaSizes[] = {
    {640, 480, 56, 79, 0, "the deck (11.3 px/mm)"},
    {640, 480, 56, 63, 63, "640x480 scenes (ppmm 901)"},
    {800, 480, 56, 64, 64, "800x480 (9.2 px/mm)"},
    {480, 480, 56, 80, 79, "480x480 (11.3 px/mm)"},
    {720, 720, 56, 70, 70, "720x720 (10 px/mm)"},
    {480, 800, 56, 60, 60, "480x800 (8.51 px/mm)"},
    {480, 640, 56, 63, 63, "480x640 (9.01 px/mm)"},
    {480, 320, 40, 44, 43, "480x320 scenes (ppmm 608, 610)"},
    {480, 320, 40, 46, 0, "a 3.5-inch 480x320 panel (6.5 px/mm)"},
    {320, 480, 40, 44, 43, "320x480 scenes (ppmm 610)"},
    {800, 600, 56, 64, 64, "800x600 (two panes, 9.2 px/mm)"},
    {1024, 768, 56, 79, 79, "1024x768 (two panes, 11.3 px/mm)"},
};

bool hits(const R &a, const R &b) {
    const double e = 1e-6;
    return a.x < b.right() - e && b.x < a.right() - e && a.y < b.bottom() - e && b.y < a.bottom() - e;
}

void test_all_touch_areas() {
    for (const QaSize &q : kQaSizes) {
        const NowGeom g = now_geom(size_class(q.w, q.h));
        const vinyl::ui::Fit f = now_fit(g, q.w, q.h, q.body);
        auto P = [&](double v) { return std::max(1.0, std::round(v * f.s)); };
        /* the output button (every class but the large one) or the large class's output chip (a typical name) */
        std::vector<R> chips;
        if (g.out_button_on) {
            chips.push_back(R{double(f.x(g.out_button.x)), double(f.y(g.out_button.y)), double(f.w(g.out_button.x, g.out_button.w)),
                              double(f.h(g.out_button.y, g.out_button.h))});
        } else {
            const double x0 = f.x(g.chip0.x), row_w = f.x(g.title.box.right()) - x0;
            for (size_t r = 0; r < g.chip_layout.size(); ++r)
                for (int id : g.chip_layout[r])
                    if (id == 2) chips.push_back(R{x0, double(f.y(g.chip0.y + static_cast<double>(r) * g.chip_step)), 0.7 * row_w, P(g.chip0.h)});
        }
        const int dy = now_keys_shift(g, f, q.target, chips, q.body, q.w, q.h);
        CHECK(dy >= 0 && dy <= P(4));
        NowHitSet set = now_hit_set(g, f, q.target, dy);
        for (const R &c : chips) set.chips.push_back({c, grow_hit(c, q.target, q.target)});
        place_now_hits(set, q.target, q.body, q.w, q.h);
        struct Named {
            const char *kind;
            const HitArea *a;
        };
        std::vector<Named> all;
        for (const HitArea &a : set.keys) all.push_back({"key", &a});
        for (const HitArea &a : set.bottom) all.push_back({"bottom", &a});
        for (const HitArea &a : set.chips) all.push_back({"output", &a});
        for (const HitArea &a : set.seek) all.push_back({"seek", &a});
        int bad = 0;
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j) {
                if (!hits(all[i].a->area, all[j].a->area)) continue;
                ++bad;
                std::fprintf(stderr, "  overlap %dx%d (%s): %s / %s\n", q.w, q.h, q.what, all[i].kind, all[j].kind);
            }
        CHECK_EQ(bad, 0);
        for (const Named &n : all) {
            CHECK(n.a->area.y >= q.body - 1e-6);       /* nothing under the header */
            CHECK(n.a->area.bottom() <= q.h + 1e-6);   /* nor past the screen */
            CHECK(n.a->area.x >= -1e-6 && n.a->area.right() <= q.w + 1e-6);
            CHECK(contains(n.a->area, n.a->drawn));     /* a tap on a drawn control is that control's */
        }
        const double minh = now_rows_min_height(set);
        if (q.row > 0) CHECK(std::floor(minh + 1e-6) >= q.row);
        /* the heights under the target, for the QA notes */
        std::string under;
        for (const Named &n : all)
            if (n.a->area.h < q.target - 1e-6) {
                char b[64];
                std::snprintf(b, sizeof b, " %s %.0f", n.kind, std::floor(n.a->area.h + 1e-6));
                if (under.find(b) == std::string::npos) under += b;
            }
        std::fprintf(stderr, "  touch heights %dx%d, %s, target %d: least %.0f px%s%s%s\n", q.w, q.h, q.what, q.target,
                     std::floor(minh + 1e-6), under.empty() ? "" : ", under the target:", under.c_str(),
                     dy ? (", the keys " + std::to_string(dy) + " px lower").c_str() : "");
        /* the queue page's buttons, and the list above them */
        const ListGeom lq = list_geom(ListPage::Queue, q.w, q.h, q.body);
        double list_bottom = 0;
        const std::vector<R> qa = queue_touch_areas(lq, q.h, q.target, list_bottom);
        CHECK_EQ(qa.size(), static_cast<size_t>(4));
        for (size_t i = 0; i < qa.size(); ++i) {
            CHECK(qa[i].h >= q.target - 1e-6);
            CHECK(qa[i].y >= list_bottom - 1e-6);
            CHECK(qa[i].bottom() <= q.h + 1e-6);
            CHECK(contains(qa[i], lq.queue_buttons[i]));
            for (size_t j = i + 1; j < qa.size(); ++j) CHECK(!hits(qa[i], qa[j]));
        }
        /* the mini player */
        const ListGeom lb = list_geom(ListPage::Browser, q.w, q.h, q.body);
        const MiniBoxes m = mini_touch_areas(lb, q.w, q.h, q.target);
        CHECK(!hits(m.open_area, m.play_area));
        CHECK(!hits(m.play_area, m.next_area));
        CHECK(!hits(m.open_area, m.next_area));
        for (const R &a : {m.open_area, m.play_area, m.next_area}) {
            CHECK(a.y >= m.list_bottom - 1e-6);
            CHECK(a.bottom() <= q.h + 1e-6);
            CHECK(a.h >= std::min(double(q.target), double(q.h)) - 1e-6);
        }
        CHECK(m.play_area.w >= q.target - 1e-6 && m.next_area.w >= q.target - 1e-6);
    }
}

/* The record (the owner's deck report, 2026-11-13, and the approved NP-640x480 / NP-480x320 drafts): 94 % of the
 * sleeve's height, centred on it, never above or below it; the label 40 % of the record with about a third of it out of
 * the sleeve; never into the info column; the two drafts' positions within 2 px. */
void test_disc_in_sleeve() {
    for (SizeClass c : {SizeClass::Small, SizeClass::Medium, SizeClass::Portrait, SizeClass::Square, SizeClass::Large}) {
        const NowGeom g = now_geom(c);
        CHECK(g.disc.y > g.sleeve.y && g.disc.bottom() < g.sleeve.bottom());
        CHECK_NEAR(g.disc.w, std::round(g.sleeve.h * 0.94), 1e-9);
        CHECK_NEAR(g.disc.y + g.disc.h / 2, g.sleeve.y + g.sleeve.h / 2, 0.51); /* centred on the sleeve */
        CHECK(g.disc.right() > g.sleeve.right());                                /* it slides out to the right */
        CHECK_NEAR(g.label_d, std::round(g.disc.w * 0.40), 1e-9);
        CHECK(g.hole_d >= 3 && g.hole_d < g.label_d / 4);
        const double cx = g.disc.x + g.disc.w / 2;
        const double out = (cx + g.label_d / 2 - g.sleeve.right()) / g.label_d; /* the label's share out of the sleeve */
        if (g.title.box.x > g.sleeve.right()) CHECK(g.disc.right() <= g.title.box.x - 2 + 1e-9);
        if (c == SizeClass::Large) CHECK(out > 0.15 && out <= 0.36); /* held back by the info column */
        else CHECK(out >= 0.33 - 1e-9 && out <= 0.35 + 1e-9);        /* about a third */
        CHECK(g.disc.right() <= g.panel.right());
    }
    CHECK_NEAR(now_geom(SizeClass::Medium).disc.x, 131.0, 2.0);
    CHECK_NEAR(now_geom(SizeClass::Medium).disc.w, 210.0, 1.0);
    CHECK_NEAR(now_geom(SizeClass::Small).disc.x, 89.0, 2.0);
    CHECK_NEAR(now_geom(SizeClass::Small).disc.w, 139.0, 1.0);
    for (const QaSize &q : kQaSizes) {
        const NowGeom g = now_geom(size_class(q.w, q.h));
        const vinyl::ui::Fit f = now_fit(g, q.w, q.h, q.body);
        /* the view: the sleeve and the disc square images of their fitted width, at their fitted y */
        const int sy = f.y(g.sleeve.y), ss = f.w(g.sleeve.x, g.sleeve.w);
        const int dyy = f.y(g.disc.y), ds = f.w(g.disc.x, g.disc.w);
        CHECK(dyy >= sy);
        CHECK(dyy + ds <= sy + ss);
    }
}

}  // namespace

void test_hits() {
    /* grow_hit: around the centre, never smaller than drawn */
    const R g = grow_hit(R{10, 10, 20, 10}, 40, 40);
    CHECK_NEAR(g.x, 0.0, 1e-9);
    CHECK_NEAR(g.y, -5.0, 1e-9);
    CHECK_NEAR(g.w, 40.0, 1e-9);
    CHECK_NEAR(g.h, 40.0, 1e-9);
    const R big = grow_hit(R{0, 0, 100, 50}, 40, 40);
    CHECK_NEAR(big.w, 100.0, 1e-9);
    CHECK_NEAR(big.h, 50.0, 1e-9);

    /* the mini player at 640x480 (s = 4/3): play 44 and next 40 design px wide, 4 apart, grown to 79 px: they overlapped
     * by about 19 px (design QA D-33); split, each keeps 79 px */
    const double s = 4.0 / 3.0;
    const R play{400, 405, 44 * s, 36 * s}, next{400 + 48 * s, 405, 40 * s, 36 * s};
    HitArea hp{play, grow_hit(play, 79, 79)}, hn{next, grow_hit(next, 79, 79)};
    CHECK(overlaps(hp.area, hn.area));
    split_pair(hp, hn, 79);
    CHECK(!overlaps(hp.area, hn.area));
    const double mid = (play.right() + next.x) / 2;
    CHECK_NEAR(hp.area.right(), mid, 1e-9);
    CHECK_NEAR(hn.area.x, mid, 1e-9);
    CHECK(hp.area.w >= 79 - 1e-9 && hn.area.w >= 79 - 1e-9); /* the size check still holds */
    CHECK_NEAR(hp.area.h, 79.0, 1e-9);                       /* only the shared axis changes */
    CHECK(contains(hp.area, play) && contains(hn.area, next));

    /* the same call in the other order gives the same areas */
    HitArea hp2{play, grow_hit(play, 79, 79)}, hn2{next, grow_hit(next, 79, 79)};
    split_pair(hn2, hp2, 79);
    CHECK_NEAR(hp2.area.x, hp.area.x, 1e-9);
    CHECK_NEAR(hn2.area.right(), hn.area.right(), 1e-9);

    /* stacked chips (480x320: 26 px high, 6 apart, TARGET 44): they meet in the middle of the gap, each 44 high */
    const R top{228, 136, 60, 26}, bottom{228, 168, 120, 26};
    HitArea ht{top, grow_hit(top, 44, 44)}, hb{bottom, grow_hit(bottom, 44, 44)};
    split_pair(ht, hb, 44);
    CHECK(!overlaps(ht.area, hb.area));
    CHECK_NEAR(ht.area.bottom(), 165.0, 1e-9);
    CHECK_NEAR(hb.area.y, 165.0, 1e-9);
    CHECK_NEAR(ht.area.h, 44.0, 1e-9);
    CHECK_NEAR(hb.area.h, 44.0, 1e-9);
    CHECK(contains(ht.area, top) && contains(hb.area, bottom));

    /* areas that do not overlap are left alone; overlapping drawings too */
    const R a{0, 0, 10, 10}, b{100, 0, 10, 10}, c{5, 5, 10, 10};
    HitArea ha{a, grow_hit(a, 20, 20)}, hb2{b, grow_hit(b, 20, 20)}, hc{c, c};
    const R a0 = ha.area, b0 = hb2.area;
    split_pair(ha, hb2, 20);
    CHECK_NEAR(hb2.area.x, b0.x, 1e-9);
    CHECK_NEAR(ha.area.w, a0.w, 1e-9);
    split_pair(ha, hc, 20);
    CHECK_NEAR(ha.area.w, a0.w, 1e-9);

    test_row_hits();
    test_time_and_chips();
    test_all_touch_areas();
    test_disc_in_sleeve();
}
