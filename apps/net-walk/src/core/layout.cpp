/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the layout arithmetic (see layout.hpp).
 */
#include "layout.hpp"

#include <algorithm>

namespace netwalk {

int Layout::tile_at(int px, int py) const
{
    int lx = 0, ly = 0;
    if (!ok || !to_logical(px, py, &lx, &ly) || !board.contains(lx, ly)) return -1;
    return ((ly - board.y) / tile) * n + (lx - board.x) / tile;
}

Rect Layout::tile_rect(int index) const
{
    Rect r;
    r.x = board.x + (index % n) * tile;
    r.y = board.y + (index / n) * tile;
    r.w = r.h = tile;
    return r;
}

bool Layout::to_logical(int px, int py, int *lx, int *ly) const
{
    const int x = px - ox, y = py - oy;
    if (x < 0 || y < 0 || x >= lw * scale || y >= lh * scale) return false;
    *lx = x / scale;
    *ly = y / scale;
    return true;
}

namespace {

int pick_scale(int sw, int sh, int min_w, int min_h)
{
    for (int s = std::max(1, std::min(sw / min_w, sh / min_h)); s >= 1; --s)
        if (sw / s >= min_w && sh / s >= min_h) return s;
    return 1;
}

} // namespace

Layout compute_layout_for(int sw, int sh, int n, bool portrait)
{
    Layout L;
    L.portrait = portrait;
    L.n = n;
    L.scale = portrait ? pick_scale(sw, sh, kMinPortraitW, kMinPortraitH) : pick_scale(sw, sh, kMinLandscapeW, kMinLandscapeH);
    L.lw = sw / L.scale;
    L.lh = sh / L.scale;
    L.ox = (sw - L.lw * L.scale) / 2;
    L.oy = (sh - L.lh * L.scale) / 2;
    const int body_top = kMargin;
    if (!portrait) {
        const int hc = L.lh - 2 * kMargin;
        const int t_h = (hc - 2 * kBevel) / n;
        const int t_w = (L.lw - 3 * kMargin - 2 * kBevel - kPanelMinW) / n;
        L.tile = std::max(4, std::min(t_h, t_w));
        const int fs = L.tile * n + 2 * kBevel;
        L.frame = {kMargin, body_top + (hc - fs) / 2, fs, fs};
        L.board = {L.frame.x + kBevel, L.frame.y + kBevel, L.tile * n, L.tile * n};
        const int px = L.frame.x + fs + kMargin;
        L.panel = {px, body_top, L.lw - px - kMargin, hc};
        for (int i = 0; i < 4; ++i) L.line[i] = {L.panel.x, L.panel.y + i * kLineH, L.panel.w, kLineH};
        const int bh = kButtonH * 3 + kButtonGap * 2;
        const int by = L.frame.y + fs - bh; /* the buttons end where the board ends */
        for (int i = 0; i < 3; ++i) L.button[i] = {L.panel.x, by + i * (kButtonH + kButtonGap), L.panel.w, kButtonH};
    } else {
        const int panel_h = kPortraitPanelH;
        const int t_w = (L.lw - 2 * kMargin - 2 * kBevel) / n;
        const int t_h = (L.lh - 3 * kMargin - panel_h - 2 * kBevel) / n;
        L.tile = std::max(4, std::min(t_h, t_w));
        const int fs = L.tile * n + 2 * kBevel;
        L.panel = {kMargin, L.lh - kMargin - panel_h, L.lw - 2 * kMargin, panel_h};
        const int room = L.panel.y - kMargin - body_top; /* between the top margin and the panel */
        L.frame = {(L.lw - fs) / 2, body_top + std::max(0, (room - fs) / 2), fs, fs};
        L.board = {L.frame.x + kBevel, L.frame.y + kBevel, L.tile * n, L.tile * n};
        /* 2 x 2 cells: Moves | Time over caption | Best; the left column is the wider (it holds "Expert wrap") */
        const int left = L.panel.w / 2 + 6;
        L.line[1] = {L.panel.x, L.panel.y, left - 8, kLineH}; /* a gap before the Time column */
        L.line[2] = {L.panel.x + left, L.panel.y, L.panel.w - left, kLineH};
        L.line[0] = {L.panel.x, L.panel.y + kLineH, left - 1, kLineH};
        L.line[3] = {L.panel.x + left, L.panel.y + kLineH, L.panel.w - left, kLineH};
        const int bw = (L.panel.w - 2 * kButtonGap) / 3;
        const int by = L.panel.y + L.panel.h - kButtonH;
        for (int i = 0; i < 3; ++i) L.button[i] = {L.panel.x + i * (bw + kButtonGap), by, i == 2 ? L.panel.w - 2 * (bw + kButtonGap) : bw, kButtonH};
    }
    L.short_new = L.button[0].w - 8 < 8 * 7; /* "New game" in the bold font is 8 * 7 wide */
    L.ok = L.lw >= (portrait ? kMinPortraitW : kMinLandscapeW) && L.lh >= (portrait ? kMinPortraitH : kMinLandscapeH);
    return L;
}

Layout compute_layout(int sw, int sh, int n)
{
    /* a body that is clearly wider or taller keeps its orientation; a near-square one (the screens of 720x720, once the
     * header is taken off: 720x656) takes whichever gives the larger tiles */
    if (std::max(sw, sh) * 4 > std::min(sw, sh) * 5) return compute_layout_for(sw, sh, n, sw < sh);
    const Layout a = compute_layout_for(sw, sh, n, sw < sh);
    const Layout b = compute_layout_for(sw, sh, n, sw >= sh);
    return (b.ok && (!a.ok || b.tile * b.scale > a.tile * a.scale)) ? b : a;
}

} // namespace netwalk
