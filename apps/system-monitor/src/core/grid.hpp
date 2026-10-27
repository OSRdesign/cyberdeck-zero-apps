/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the overview's tile grid as pure geometry (no LVGL; tests/test_core.cpp). The screen side gives the
 * area under the header, the gap and the footer height (layout service tokens) and the smallest tile height its fonts
 * need; this decides the columns and places every tile and footer item; next_focus() moves the keyboard focus by
 * position.
 *
 * The rule (no screen size is named anywhere):
 *   - the footer is one row across the bottom, its items side by side;
 *   - the tiles take the rest in 2 or 3 columns: the count whose cells come closest to a landscape card (width about
 *     2.1 x height) among those that keep a cell at least min_tile_h high; with none, the tallest cells;
 *   - with 2 columns and an even count, when the height allows one row more without squashing a tile (still >= min_tile_h
 *     and at most 3 x as wide as high), the first tile (Power) and the last one (Network) span the whole width;
 *   - a portrait area (taller than wide, PDA mode): the tiles stack in one column across the whole width, when that
 *     width is at most max_tile_w (a card wider than that is better split in two columns) and each tile keeps at least
 *     min_tile_h; otherwise the rules above. A landscape or square screen's area is always wider than high: unchanged.
 * 640x480: 2 x 3. 800x480, 1024x600, 1280x720: 3 x 2. 720x720: Power and Network across, 2 x 2 between them.
 * Portrait: 480x800 one column of six; 480x640 (too low for six) and 800x1280 (too wide for one card) Power and Network
 * across, 2 x 2 between them.
 */
#pragma once

#include <vector>

namespace sm {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int cx2() const { return 2 * x + w; } /* twice the centre (integers) */
    int cy2() const { return 2 * y + h; }
};

struct GridLayout {
    int cols = 0, rows = 0;
    bool wide = false;         /* the first and the last tile span the width */
    std::vector<Rect> tiles;   /* in order */
    std::vector<Rect> footer;  /* in order */
};

constexpr double kTileAspect = 2.1;

/* The area is (0, 0, w, h); tiles >= 1, footer_items >= 0 (0: no footer row). max_tile_w: the widest a card of the
 * portrait column may be (0: no portrait column). */
GridLayout grid_layout(int w, int h, int gap, int footer_h, int footer_items, int tiles, int min_tile_h,
                       int max_tile_w = 0);

enum class Dir { Left, Right, Up, Down };

/* The rectangle to focus when `dir` is pressed on rects[current]: the nearest one in line (overlapping it across the
 * move) whose centre lies that way; -1 when there is none (the focus stays). */
int next_focus(const std::vector<Rect> &rects, int current, Dir dir);

} // namespace sm
