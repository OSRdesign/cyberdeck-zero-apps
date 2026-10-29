/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the table's geometry (core/view_layout.hpp) at every size the UI is tested at, and of the card art
 * (core/cardart.hpp). No LVGL: the toolkit's tokens are given as numbers (the harness's densities, 7 mm buttons with the
 * 44 px floor, a 56 px header).
 */
#include "test_util.hpp"

#include "cardart.hpp"
#include "view_layout.hpp"

#include <algorithm>
#include <cmath>

using namespace klondike;

namespace {

struct Size {
    int w, h;
    double ppmm;
};

const Size kSizes[] = {{640, 480, 11.3}, {800, 480, 9.2}, {720, 720, 10.0}, {1280, 800, 9.0},
                       {480, 800, 8.5},  {800, 1280, 8.6}, {480, 640, 9.0}};

LayoutIn input(const Size &s, bool left, bool draw3)
{
    LayoutIn in;
    in.w = s.w;
    in.h = s.h;
    in.body_y = 56;
    in.target = std::max(44, static_cast<int>(7.0 * s.ppmm));
    in.pad = std::max(4, static_cast<int>(0.9 * s.ppmm));
    in.hud_h = 26;
    in.ppmm_x100 = static_cast<int>(s.ppmm * 100);
    in.left_handed = left;
    in.draw3 = draw3;
    return in;
}

void check_size(const Size &s, bool left, bool draw3)
{
    const Layout l = compute_layout(input(s, left, draw3));
    CHECK(l.valid);
    if (!l.valid) return;
    const int target = input(s, left, draw3).target;
    CHECK(l.card_w >= 40);
    CHECK(l.card_h == static_cast<int>(std::lround(l.card_w * kCardAspect)));
    /* seven columns inside the board, spread, not overlapping */
    CHECK(l.col_x[0] >= l.board.x && l.col_x[6] + l.card_w <= l.board.right());
    for (int c = 1; c < kColumns; ++c) CHECK(l.col_x[c] >= l.col_x[c - 1] + l.card_w + 2);
    /* the top row and the tableau are inside the board */
    CHECK(l.top_y >= l.board.y && l.tab_y >= l.top_y + l.card_h);
    /* the worst column of every height ends inside the board; the steps never go under the readable minimum */
    for (int nd = 0; nd <= 6; ++nd)
        for (int nu = 1; nu <= 13; ++nu) {
            CHECK(l.tab_y + column_extent(l, nd, nu) <= l.board.bottom());
            const Fan f = fan_for(l, nd, nu);
            /* nothing clips: every covered card shows at least its whole index (rank and suit with their margins), face
             * up or down, in pixels as drawn (rounded positions), also for the worst-case column (6 down, K..A up) */
            CHECK(f.up >= l.fan_up_min && f.down >= l.fan_down_min);
            for (int i = 1; i < nd + nu; ++i) {
                const int step = fan_y(f, nd, i) - fan_y(f, nd, i - 1);
                CHECK(step >= (i <= nd ? l.fan_down_min : l.fan_up_min));
            }
        }
    /* the strip a covered card shows holds the index of the font the UI draws it in */
    CHECK(l.fan_up_min >= index_strip_px(l.card_h) && l.index_h == l.fan_up_min);
    CHECK(l.fan_up_min >= static_cast<int>(kFanUpMin * l.card_h));
    CHECK(l.fan_up_min + kIndexMargin <= 3 * l.fan_up_min);                           /* (sanity) */
    CHECK(kIndexMargin + static_cast<int>(std::ceil(index_font_px(l.card_h) * kIndexCapFrac)) + 1 <= l.fan_up_min);
    /* the normal fan is kept while it fits (no compression for a short column) */
    {
        const Fan f = fan_for(l, 2, 4);
        CHECK(std::fabs(f.up - l.fan_up0) < 0.001 && std::fabs(f.down - l.fan_down0) < 0.001);
    }
    /* the waste fan stays on the board, towards the middle */
    {
        const int x = l.pile_x(kWaste), span = l.waste_step * 2;
        const int lo = l.waste_dir > 0 ? x : x - span, hi = l.waste_dir > 0 ? x + span + l.card_w : x + l.card_w;
        CHECK(lo >= l.board.x && hi <= l.board.right());
    }
    /* the buttons: inside the dock, at least a finger target, apart from each other and from the board */
    for (int i = 0; i < kDockButtons; ++i) {
        const Rect &b = l.button[i];
        CHECK(b.x >= l.dock.x && b.y >= l.dock.y && b.right() <= l.dock.right() && b.bottom() <= l.dock.bottom());
        CHECK(b.w >= target && b.h >= target);
        for (int j = i + 1; j < kDockButtons; ++j) {
            const Rect &c = l.button[j];
            CHECK(b.right() <= c.x || c.right() <= b.x || b.bottom() <= c.y || c.bottom() <= b.y);
        }
    }
    const bool overlap = !(l.dock.right() <= l.board.x || l.board.right() <= l.dock.x || l.dock.bottom() <= l.board.y ||
                           l.board.bottom() <= l.dock.y);
    CHECK(!overlap);
    CHECK(l.hud.bottom() <= l.board.y && l.hud.w == l.board.w);
    CHECK(l.dock.right() <= s.w && l.dock.bottom() <= s.h && l.board.bottom() <= s.h && l.board.right() <= s.w);
    /* the top row mirrors */
    if (left) {
        CHECK(l.pile_col[kStock] == 6 && l.pile_col[kWaste] == 5 && l.pile_col[foundation_pile(0)] == 3 && l.pile_col[foundation_pile(3)] == 0);
        CHECK(l.waste_dir == -1);
        if (l.side_dock) CHECK(l.dock.x == 0);
    } else {
        CHECK(l.pile_col[kStock] == 0 && l.pile_col[kWaste] == 1 && l.pile_col[foundation_pile(0)] == 3 && l.pile_col[foundation_pile(3)] == 6);
        if (l.side_dock) CHECK(l.dock.right() == s.w);
    }
    if (!left && !draw3) {
        std::printf("  layout %dx%d: card %dx%d (%.1f x %.1f mm), %s dock, fan up %.0f..%.0f px, button %dx%d\n", s.w, s.h, l.card_w,
                    l.card_h, l.card_w / s.ppmm, l.card_h / s.ppmm, l.side_dock ? "side" : "bottom", fan_for(l, 6, 13).up, l.fan_up0,
                    l.button[0].w, l.button[0].h);
    }
}

void test_layout()
{
    for (const Size &s : kSizes)
        for (int left = 0; left < 2; ++left)
            for (int d3 = 0; d3 < 2; ++d3) check_size(s, left != 0, d3 != 0);
    /* the dock follows the shape: portrait canvases get the row at the bottom, the deck the column at the side */
    CHECK(!compute_layout(input(kSizes[4], false, false)).side_dock);
    CHECK(!compute_layout(input(kSizes[6], false, false)).side_dock);
    CHECK(!compute_layout(input(kSizes[5], false, false)).side_dock);
    CHECK(compute_layout(input(kSizes[0], false, false)).side_dock);
    CHECK(compute_layout(input(kSizes[1], false, false)).side_dock);
    /* too small a canvas is refused, not drawn wrongly */
    LayoutIn tiny;
    tiny.w = 200;
    tiny.h = 120;
    tiny.body_y = 40;
    CHECK(!compute_layout(tiny).valid);
}

/* No dock label wider than its button, at every size and several panel densities (140, 187 and 250 dpi, and the harness's own),
 * for label widths of a small to a large font. The density only changes TARGET and the padding (the fonts do not follow it),
 * which is how a 5" 800x480 board clipped "Undo" while every harness size looked fine. */
void test_dock_labels()
{
    const double dpis[] = {140, 187, 250};
    const int label_ws[] = {44, 58, 72, 96};
    for (const Size &base : kSizes)
        for (double dpi : dpis)
            for (int lw : label_ws)
                for (int left = 0; left < 2; ++left) {
                    Size s = base;
                    s.ppmm = dpi / 25.4;
                    LayoutIn in = input(s, left != 0, false);
                    in.label_w = lw;
                    const Layout l = compute_layout(in);
                    CHECK(l.valid);
                    if (!l.valid) continue;
                    const int need = lw + 2 * label_air(in.pad);
                    for (int i = 0; i < kDockButtons; ++i) {
                        /* the side column is always wide enough; the bottom row's buttons are the canvas width / 4 (the UI drops to the
                         * caption font only if even that is too narrow, which no size here needs) */
                        if (l.side_dock) CHECK(l.button[i].w >= need);
                        else CHECK(l.button[i].w >= need || base.w < 4 * need + 5 * in.pad);
                        CHECK(l.button[i].w >= std::min(in.target, l.button[i].w));
                        CHECK(l.button[i].right() <= l.dock.right() && l.button[i].x >= l.dock.x);
                    }
                    /* the cards still fit seven columns and are not tiny */
                    CHECK(l.card_w >= 40);
                    CHECK(l.col_x[0] >= l.board.x && l.col_x[6] + l.card_w <= l.board.right());
                    if (l.side_dock) CHECK(l.board.w + l.dock.w == s.w);
                }
}

void test_art()
{
    for (int suit = 0; suit < 4; ++suit) {
        const art::Mask m = art::render_suit(suit, 24);
        CHECK(m.w == 24 && m.h == 24 && static_cast<int>(m.a.size()) == 576);
        int sum = 0, partial = 0;
        for (std::uint8_t v : m.a) {
            sum += v;
            if (v > 0 && v < 255) ++partial;
        }
        CHECK(sum > 255 * 24 * 24 / 6 && sum < 255 * 24 * 24 * 9 / 10); /* a real shape, not a block */
        CHECK(partial > 10);                                          /* anti-aliased */
    }
    /* the diamond is symmetric, the heart is not upside down */
    const art::Mask d = art::render_suit(Diamonds, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 10; ++x) CHECK(std::abs(int(d.at(x, y)) - int(d.at(19 - x, y))) <= 1);
    const art::Mask h = art::render_suit(Hearts, 20);
    int top = 0, bottom = 0;
    for (int x = 0; x < 20; ++x) {
        top += h.at(x, 4);
        bottom += h.at(x, 17);
    }
    CHECK(top > bottom); /* the lobes are on top, the point below */
    for (int k = 0; k < 3; ++k) {
        const art::Mask e = art::render_emblem(k, 30, 20);
        int sum = 0;
        for (std::uint8_t v : e.a) sum += v;
        CHECK(sum > 255 * 30 * 20 / 8);
    }
    for (int design = 0; design < art::kBacks; ++design) {
        const std::vector<std::uint32_t> b = art::render_back(design, 60, 84);
        CHECK(b.size() == 60u * 84u);
        CHECK((b[0] >> 24) < 128);                     /* the corner is cut round */
        CHECK((b[42 * 60 + 30] >> 24) == 255);        /* the middle is solid */
        CHECK((b[10 * 60 + 30] >> 24) == 255);
    }
    art::Pip p[10];
    for (int r = 2; r <= 10; ++r) CHECK(art::pip_layout(r, p) == r);
    CHECK(art::pip_layout(1, p) == 0 && art::pip_layout(11, p) == 0);
    CHECK(art::suit_rgb(Diamonds, false) == art::suit_rgb(Hearts, false));
    CHECK(art::suit_rgb(Diamonds, true) != art::suit_rgb(Hearts, true));
    CHECK(art::suit_rgb(Clubs, true) != art::suit_rgb(Spades, true));
    CHECK(art::suit_rgb(Clubs, false) == art::suit_rgb(Spades, false));
}

} // namespace

void test_layout_group()
{
    std::printf("layout sizes (7 mm button, 56 px header, test densities):\n");
    test_layout();
    test_dock_labels();
    test_art();
}
