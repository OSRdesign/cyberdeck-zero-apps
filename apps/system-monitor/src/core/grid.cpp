/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the overview's grid geometry and focus moves (see grid.hpp).
 */
#include "grid.hpp"

#include <climits>
#include <cmath>
#include <cstdlib>

namespace sm {

GridLayout grid_layout(int w, int h, int gap, int footer_h, int footer_items, int tiles, int min_tile_h, int max_tile_w)
{
    GridLayout g;
    if (w <= 0 || h <= 0 || tiles <= 0) return g;
    const int foot = footer_items > 0 ? footer_h : 0;
    const int area_h = h - (foot > 0 ? foot + gap : 0);
    if (footer_items > 0) {
        const int iw = (w - (footer_items - 1) * gap) / footer_items;
        for (int i = 0; i < footer_items; ++i) {
            Rect r;
            r.x = i * (iw + gap);
            r.y = h - foot;
            r.w = i == footer_items - 1 ? w - r.x : iw; /* the last one takes the rounding */
            r.h = foot;
            g.footer.push_back(r);
        }
    }
    // a portrait area: one column of cards across the width, when one card that wide is not too wide and every card
    // keeps its height (else the rule below, as on a landscape screen)
    if (h > w && max_tile_w > 0 && w <= max_tile_w) {
        const int ch = (area_h - (tiles - 1) * gap) / tiles;
        if (ch >= min_tile_h && ch > 0) {
            g.cols = 1;
            g.rows = tiles;
            for (int i = 0; i < tiles; ++i) {
                Rect r;
                r.w = w;
                r.y = i * (ch + gap);
                r.h = ch;
                g.tiles.push_back(r);
            }
            return g;
        }
    }
    // the columns: the cells closest to a landscape card among those tall enough
    int best_cols = 0;
    double best_score = 1e9;
    int tallest_cols = 2, tallest_h = INT_MIN;
    for (int cols = 2; cols <= 3; ++cols) {
        const int rows = (tiles + cols - 1) / cols;
        const int cw = (w - (cols - 1) * gap) / cols;
        const int ch = (area_h - (rows - 1) * gap) / rows;
        if (ch > tallest_h) {
            tallest_h = ch;
            tallest_cols = cols;
        }
        if (ch < min_tile_h || ch <= 0 || cw <= 0) continue;
        const double score = std::fabs(std::log(static_cast<double>(cw) / ch / kTileAspect));
        if (score < best_score) {
            best_score = score;
            best_cols = cols;
        }
    }
    const int cols = best_cols ? best_cols : tallest_cols;
    const int cw = (w - (cols - 1) * gap) / cols;
    // the first and the last tile across the width when one row more still fits comfortably
    if (cols == 2 && tiles >= 4 && tiles % 2 == 0) {
        const int rows = tiles / 2 + 1;
        const int ch = (area_h - (rows - 1) * gap) / rows;
        if (ch >= min_tile_h && ch > 0 && cw <= 3 * ch) {
            g.cols = 2;
            g.rows = rows;
            g.wide = true;
            for (int i = 0; i < tiles; ++i) {
                Rect r;
                r.h = ch;
                if (i == 0 || i == tiles - 1) {
                    r.x = 0;
                    r.w = w;
                    r.y = i == 0 ? 0 : (rows - 1) * (ch + gap);
                } else {
                    const int k = i - 1;
                    r.x = (k % 2) * (cw + gap);
                    r.w = k % 2 ? w - r.x : cw;
                    r.y = (1 + k / 2) * (ch + gap);
                }
                g.tiles.push_back(r);
            }
            return g;
        }
    }
    const int rows = (tiles + cols - 1) / cols;
    const int ch = (area_h - (rows - 1) * gap) / rows;
    g.cols = cols;
    g.rows = rows;
    for (int i = 0; i < tiles; ++i) {
        Rect r;
        const int c = i % cols;
        r.x = c * (cw + gap);
        r.w = c == cols - 1 ? w - r.x : cw;
        r.y = (i / cols) * (ch + gap);
        r.h = ch;
        g.tiles.push_back(r);
    }
    return g;
}

int next_focus(const std::vector<Rect> &rects, int current, Dir dir)
{
    if (current < 0 || current >= static_cast<int>(rects.size())) return rects.empty() ? -1 : 0;
    const Rect &c = rects[static_cast<std::size_t>(current)];
    int best = -1;
    long best_score = LONG_MAX;
    for (int i = 0; i < static_cast<int>(rects.size()); ++i) {
        if (i == current) continue;
        const Rect &r = rects[static_cast<std::size_t>(i)];
        const bool horizontal = dir == Dir::Left || dir == Dir::Right;
        // how far along the move (centres, doubled) and how far across it
        long along = 0, across = 0;
        switch (dir) {
        case Dir::Right: along = r.cx2() - c.cx2(); break;
        case Dir::Left: along = c.cx2() - r.cx2(); break;
        case Dir::Down: along = r.cy2() - c.cy2(); break;
        case Dir::Up: along = c.cy2() - r.cy2(); break;
        }
        if (along <= 0) continue;
        across = horizontal ? std::labs(static_cast<long>(r.cy2() - c.cy2())) : std::labs(static_cast<long>(r.cx2() - c.cx2()));
        // only the ones in line: they overlap across the move (a footer item under a tile, the tile beside it); a
        // move that has none keeps the focus (no jump to another row)
        const bool in_line = horizontal ? (r.y < c.y + c.h && c.y < r.y + r.h) : (r.x < c.x + c.w && c.x < r.x + r.w);
        if (!in_line) continue;
        const long score = along * 4 + across;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

} // namespace sm
