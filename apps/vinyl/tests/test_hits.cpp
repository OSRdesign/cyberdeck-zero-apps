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
}
