/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: how the three boards fit the screen (see view_layout.hpp).
 */
#include "view_layout.hpp"

#include <algorithm>
#include <cmath>

namespace minefield {

int max_cell_px(double ppmm)
{
    return std::max(1, static_cast<int>(std::floor(kMaxCellMm * ppmm + 1e-9)));
}

BoardPlan fit_board(int vw, int vh, double ppmm, int cols, int rows, int mines)
{
    BoardPlan p;
    p.dims = Dims{cols, rows, mines};
    if (vw <= 0 || vh <= 0 || cols <= 0 || rows <= 0 || ppmm <= 0) return p;
    p.cell_px = std::max(1, std::min(std::min(vw / cols, vh / rows), max_cell_px(ppmm)));
    p.board_w = p.cell_px * cols;
    p.board_h = p.cell_px * rows;
    p.origin_x = (vw - p.board_w) / 2;
    p.origin_y = (vh - p.board_h) / 2;
    p.cell_mm = p.cell_px / ppmm;
    p.fits = p.board_w <= vw && p.board_h <= vh;
    p.valid = true;
    return p;
}

Dims expert_grid(int vw, int vh, double ppmm)
{
    const double min_px = std::max(1.0, kMinCellMm * ppmm);
    Dims d;
    d.w = std::clamp(static_cast<int>(std::floor(vw / min_px + 1e-9)), kMinGrid, kMaxDim);
    d.h = std::clamp(static_cast<int>(std::floor(vh / min_px + 1e-9)), kMinGrid, kMaxDim);
    d.mines = 0;
    return d;
}

int mines_for(int cols, int rows, Preset p)
{
    const double density = p == Preset::Expert ? kExpertDensity : (p == Preset::Intermediate ? kIntermediateDensity : kBeginnerDensity);
    const int cells = cols * rows;
    return std::clamp(static_cast<int>(std::lround(cells * density)), 1, std::max(1, max_mines(cols, rows)));
}

/* a share of the level above, rounded, at least 6, at most the level above */
static Dims scaled(const Dims &above, double share)
{
    Dims d;
    d.w = std::clamp(static_cast<int>(std::lround(above.w * share)), std::min(kMinGrid, above.w), above.w);
    d.h = std::clamp(static_cast<int>(std::lround(above.h * share)), std::min(kMinGrid, above.h), above.h);
    /* a level must have fewer cells than the one above; on a tiny screen it gives up a row or a column, down to 5 */
    while (d.w * d.h >= above.w * above.h) {
        if (d.w >= d.h && d.w > kMinDim) --d.w;
        else if (d.h > kMinDim) --d.h;
        else break;
    }
    return d;
}

static std::array<Dims, kRecordedPresets> grids(int vw, int vh, double ppmm)
{
    std::array<Dims, kRecordedPresets> g;
    g[2] = expert_grid(vw, vh, ppmm);
    g[1] = scaled(g[2], kIntermediateShare);
    /* Beginner is about 55 % of Expert, not of Intermediate */
    Dims b;
    b.w = std::clamp(static_cast<int>(std::lround(g[2].w * kBeginnerShare)), std::min(kMinGrid, g[1].w), g[1].w);
    b.h = std::clamp(static_cast<int>(std::lround(g[2].h * kBeginnerShare)), std::min(kMinGrid, g[1].h), g[1].h);
    while (b.w * b.h >= g[1].w * g[1].h) {
        if (b.w >= b.h && b.w > kMinDim) --b.w;
        else if (b.h > kMinDim) --b.h;
        else break;
    }
    g[0] = b;
    return g;
}

BoardPlan derive_board(int vw, int vh, double ppmm, Preset p)
{
    const std::array<Dims, kRecordedPresets> g = grids(vw, vh, ppmm);
    const Dims &d = g[static_cast<std::size_t>(p)];
    return fit_board(vw, vh, ppmm, d.w, d.h, mines_for(d.w, d.h, p));
}

std::array<Dims, kRecordedPresets> derive_boards(int vw, int vh, double ppmm)
{
    std::array<Dims, kRecordedPresets> out;
    for (int i = 0; i < kRecordedPresets; ++i) out[static_cast<std::size_t>(i)] = derive_board(vw, vh, ppmm, static_cast<Preset>(i)).dims;
    return out;
}

int hit_cell(int px, int py, int origin_x, int origin_y, int cell_px, int cols, int rows)
{
    if (cell_px <= 0) return -1;
    const int rx = px - origin_x, ry = py - origin_y;
    if (rx < 0 || ry < 0) return -1;
    const int x = rx / cell_px, y = ry / cell_px;
    if (x >= cols || y >= rows) return -1;
    return y * cols + x;
}

} // namespace minefield
