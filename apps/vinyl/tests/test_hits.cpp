/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the touch areas (core/layout grow_hit, split_pair): two neighbours' grown areas meet in the middle of the
 * gap and each keeps the minimum size. The mini player's case of the design QA (play 44 and next 40 design px, 4 apart,
 * grown to 79 px at 640x480) is one of them.
 */
#include "check.hpp"

#include "layout/layout.hpp"

#include <vector>

using namespace vinyl::layout;

namespace {

bool contains(const R &outer, const R &inner) {
    return inner.x >= outer.x - 1e-9 && inner.y >= outer.y - 1e-9 && inner.right() <= outer.right() + 1e-9 &&
           inner.bottom() <= outer.bottom() + 1e-9;
}

/* every strip area against every other area of now playing; the keys against the bottom row */
bool strip_apart(const NowHitSet &h) {
    for (const HitArea &st : h.strip) {
        for (const HitArea &o : h.chips)
            if (overlaps(st.area, o.area)) return false;
        for (const HitArea &o : h.keys)
            if (overlaps(st.area, o.area)) return false;
        for (const HitArea &o : h.bottom)
            if (overlaps(st.area, o.area)) return false;
        for (const HitArea &o : h.strip)
            if (&o != &st && overlaps(st.area, o.area)) return false;
    }
    for (const HitArea &k : h.keys)
        for (const HitArea &b : h.bottom)
            if (overlaps(k.area, b.area)) return false;
    for (size_t i = 0; i < h.chips.size(); ++i)
        for (size_t j = i + 1; j < h.chips.size(); ++j)
            if (overlaps(h.chips[i].area, h.chips[j].area)) return false;
    return true;
}

HitArea grown(const R &d, double target) { return HitArea{d, grow_hit(d, target, target)}; }

/* now playing with the volume strip open, screen px of the renders (design QA run 2, D-33) */
void test_strip_hits() {
    /* 480x480 (TARGET 80): the output and volume chips side by side, the strip under them, six transport keys, the
     * word row; the QA saw the keys' tops act as "-" / "+" and "+" cover the volume chip's bottom edge */
    {
        const double t = 80;
        NowHitSet h;
        h.chips = {grown(R{295, 205, 100, 25}, t), grown(R{403, 205, 53, 25}, t)};
        split_pair(h.chips[0], h.chips[1], t);
        h.strip = {grown(R{28, 262, 54, 41}, t), grown(R{398, 262, 54, 41}, t)};
        const double kx[6] = {28, 95, 163, 261, 329, 396}, kw[6] = {54, 54, 85, 54, 54, 56};
        for (int i = 0; i < 6; ++i) h.keys.push_back(grown(R{kx[i], 310, kw[i], 46}, t));
        for (int i = 0; i < 3; ++i) h.bottom.push_back(grown(R{28 + i * 144.5, 422, 135, 40}, t));
        bool before = false;
        for (const HitArea &st : h.strip)
            for (const HitArea &k : h.keys) before = before || overlaps(st.area, k.area);
        CHECK(before); /* the overlap the QA measured */
        place_strip_hits(h, t, 56, 480);
        CHECK(strip_apart(h));
        for (const HitArea &st : h.strip) {
            CHECK_NEAR(st.area.y, 230.0, 1e-9); /* the band: the chips' bottom to the keys' top, 80 px */
            CHECK_NEAR(st.area.h, 80.0, 1e-9);
            CHECK(contains(st.area, st.drawn));
        }
        for (const HitArea &c : h.chips) {
            CHECK(c.area.h >= t - 1e-9);
            CHECK(c.area.y >= 56);
            CHECK(contains(c.area, c.drawn));
        }
        for (const HitArea &k : h.keys) {
            CHECK(k.area.h >= t - 1e-9);
            CHECK(contains(k.area, k.drawn));
        }
        for (const HitArea &b : h.bottom) CHECK(contains(b.area, b.drawn));
    }
    /* 800x480 (TARGET 64): the keys pushed down meet the word row, which moves down in turn */
    {
        const double t = 64;
        NowHitSet h;
        h.chips = {grown(R{570, 245, 90, 25}, t), grown(R{668, 245, 52, 25}, t)};
        split_pair(h.chips[0], h.chips[1], t);
        h.strip = {grown(R{325, 277, 54, 34}, t), grown(R{712, 277, 54, 34}, t)};
        for (int i = 0; i < 6; ++i) h.keys.push_back(grown(R{325 + i * 67.5, 318, i == 2 ? 85.0 : 54.0, 46}, t));
        for (int i = 0; i < 2; ++i) h.bottom.push_back(grown(R{325 + i * 144.5, 387.5, 135, 40}, t));
        place_strip_hits(h, t, 56, 480);
        CHECK(strip_apart(h));
        for (const HitArea &k : h.keys) CHECK(k.area.h >= t - 1e-9);
        for (const HitArea &b : h.bottom) {
            CHECK(b.area.h >= t - 1e-9);
            CHECK(b.area.bottom() <= 480);
        }
    }
    /* 480x320 (TARGET 44): the volume chip on the first row, the output chip under it, the strip lower: a band wider
     * than the target leaves the strip areas the target high, centred on the buttons */
    {
        const double t = 44;
        NowHitSet h;
        h.chips = {grown(R{330, 136, 60, 26}, t), grown(R{228, 168, 230, 26}, t)};
        split_pair(h.chips[0], h.chips[1], t);
        h.strip = {grown(R{20, 210, 56, 44}, t), grown(R{404, 210, 56, 44}, t)};
        for (int i = 0; i < 6; ++i) h.keys.push_back(grown(R{20 + i * 70.0, 258, 56, 48}, t));
        place_strip_hits(h, t, 40, 320);
        CHECK(strip_apart(h));
        for (const HitArea &st : h.strip) CHECK_NEAR(st.area.h, 44.0, 1e-9);
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

    test_strip_hits();
}
