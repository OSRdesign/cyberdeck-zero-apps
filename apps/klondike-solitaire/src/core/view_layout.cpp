/*
 * SPDX-License-Identifier: MIT
 */
#include "view_layout.hpp"

#include <algorithm>
#include <cmath>

namespace klondike {

int index_font_px(int card_h)
{
    int n = static_cast<int>(std::lround(card_h * kIndexPxFrac));
    n &= ~1;
    return std::min(40, std::max(10, n));
}

int index_strip_px(int card_h)
{
    return kIndexMargin + static_cast<int>(std::ceil(index_font_px(card_h) * kIndexCapFrac)) + 1;
}

namespace {

int fan_up_min_for(int card_h)
{
    return std::max(static_cast<int>(std::ceil(kFanUpMin * card_h)), index_strip_px(card_h));
}

int fan_down_min_for(int card_h)
{
    return static_cast<int>(std::ceil(kFanDownMin * card_h));
}

Layout attempt(const LayoutIn &in, bool side)
{
    Layout l;
    l.side_dock = side;
    /* the button: TARGET, and wide enough for the widest label (side dock; the bottom row's buttons share the width) */
    const int btn_w = std::max(in.target, in.label_w + 2 * label_air(in.pad));
    const int thick = (side ? btn_w : in.target) + 2 * in.pad;
    Rect area; /* body minus the dock */
    area.y = in.body_y;
    area.h = in.h - in.body_y;
    if (side) {
        l.dock = Rect{in.left_handed ? 0 : in.w - thick, in.body_y, thick, in.h - in.body_y};
        area.x = in.left_handed ? thick : 0;
        area.w = in.w - thick;
    } else {
        l.dock = Rect{0, in.h - thick, in.w, thick};
        area.x = 0;
        area.w = in.w;
        area.h -= thick;
    }
    if (area.w <= 0 || area.h <= in.hud_h) return l;
    l.hud = Rect{area.x, area.y, area.w, in.hud_h};
    Rect cards{area.x, area.y + in.hud_h, area.w, area.h - in.hud_h};
    l.board = cards;

    const int mx = std::max(4, cards.w / 80);
    const int my = std::max(4, cards.h / 80);
    const int gap = std::max(4, cards.w / 100);
    const int by_w = (cards.w - 2 * mx - 6 * gap) / 7;
    /* the worst case column: 6 face down, King .. Ace face up, plus the top row and the gap between */
    const double total = 1.0 + 0.10 + (1.0 + 6 * kFanDownMin + 12 * kFanUpMin);
    const int ch_max = static_cast<int>((cards.h - 2 * my) / total);
    const int by_h = static_cast<int>(ch_max / kCardAspect);
    /* the worst column must fit with the integer minimum steps (the estimate above ignores their rounding up and the
     * index strip): shrink the card until it does */
    l.card_w = std::min(by_w, by_h);
    while (l.card_w >= kMinCardW) {
        const int h = static_cast<int>(std::lround(l.card_w * kCardAspect));
        const int need = h + std::max(6, h / 10) + h + 6 * fan_down_min_for(h) + 12 * fan_up_min_for(h);
        if (need <= cards.h - 2 * my) break;
        --l.card_w;
    }
    if (l.card_w < kMinCardW) {
        l.card_w = 0;
        return l;
    }
    l.card_h = static_cast<int>(std::lround(l.card_w * kCardAspect));
    l.radius = std::max(3, l.card_w / 11);

    const int used = 7 * l.card_w;
    const int spare = cards.w - 2 * mx - used;
    l.gap_x = std::min(spare / 6, static_cast<int>(l.card_w * 0.5));
    const int span = used + 6 * l.gap_x;
    const int x0 = cards.x + (cards.w - span) / 2;
    for (int c = 0; c < kColumns; ++c) l.col_x[c] = x0 + c * (l.card_w + l.gap_x);

    const int vgap = std::max(6, l.card_h / 10);
    l.top_y = cards.y + my;
    l.tab_y = l.top_y + l.card_h + vgap;
    l.fan_avail = std::max(0, (cards.y + cards.h - my) - l.tab_y - l.card_h);
    l.fan_up0 = kFanUp0 * l.card_h;
    l.fan_down0 = kFanDown0 * l.card_h;
    l.fan_up_min = fan_up_min_for(l.card_h);
    l.fan_down_min = fan_down_min_for(l.card_h);
    l.index_h = l.fan_up_min;

    for (int p = 0; p < kPiles; ++p) l.pile_col[p] = 0;
    l.pile_col[kStock] = 0;
    l.pile_col[kWaste] = 1;
    for (int f = 0; f < kFoundations; ++f) l.pile_col[foundation_pile(f)] = 3 + f;
    if (in.left_handed)
        for (int p = 0; p < kTableau0; ++p) l.pile_col[p] = 6 - l.pile_col[p];
    l.waste_dir = in.left_handed ? -1 : 1;
    l.waste_step = in.draw3 ? std::max(6, static_cast<int>(l.card_w * 0.24)) : 0;

    /* buttons */
    if (side) {
        const int bw = btn_w;
        int y = l.dock.y + in.pad;
        const int avail = l.dock.h - 2 * in.pad;
        const int step = std::min(in.target + in.pad, (avail + in.pad) / kDockButtons);
        const int bh = std::max(1, step - in.pad);
        for (int i = 0; i < kDockButtons; ++i) {
            l.button[i] = Rect{l.dock.x + in.pad, y, bw, bh};
            y += step;
        }
    } else {
        const int bh = in.target;
        const int bw = (in.w - 2 * in.pad - (kDockButtons - 1) * in.pad) / kDockButtons;
        for (int i = 0; i < kDockButtons; ++i)
            l.button[i] = Rect{in.pad + i * (bw + in.pad), l.dock.y + in.pad, bw, bh};
    }
    l.valid = true;
    return l;
}

} // namespace

Layout compute_layout(const LayoutIn &in)
{
    Layout a = attempt(in, true);
    Layout b = attempt(in, false);
    if (!a.valid) return b;
    if (!b.valid) return a;
    if (a.card_w != b.card_w) return a.card_w > b.card_w ? a : b;
    return in.h > in.w ? b : a;
}

Fan fan_for(const Layout &l, int ndown, int nup)
{
    Fan f{l.fan_down0, l.fan_up0};
    const double need = ndown * f.down + (nup > 0 ? (nup - 1) * f.up : 0);
    if (need > l.fan_avail && need > 0) {
        /* both steps give way together, each towards its minimum, neither below it */
        const double lo = ndown * static_cast<double>(l.fan_down_min) + (nup > 0 ? (nup - 1) * static_cast<double>(l.fan_up_min) : 0);
        if (l.fan_avail <= lo || need <= lo) {
            f.down = l.fan_down_min;
            f.up = l.fan_up_min;
        } else {
            const double t = (l.fan_avail - lo) / (need - lo);
            f.down = l.fan_down_min + t * (l.fan_down0 - l.fan_down_min);
            f.up = l.fan_up_min + t * (l.fan_up0 - l.fan_up_min);
        }
    }
    return f;
}

int fan_y(const Fan &f, int ndown, int i)
{
    if (i <= ndown) return static_cast<int>(std::lround(i * f.down));
    return static_cast<int>(std::lround(ndown * f.down + (i - ndown) * f.up));
}

int column_extent(const Layout &l, int ndown, int nup)
{
    const int n = ndown + nup;
    if (n <= 0) return l.card_h;
    return fan_y(fan_for(l, ndown, nup), ndown, n - 1) + l.card_h;
}

} // namespace klondike
