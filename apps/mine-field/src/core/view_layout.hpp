/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: how the three boards fit the screen (pure functions, no LVGL; tests/test_view.cpp). The board ALWAYS
 * fits the playable area: nothing is panned, zoomed or scrolled. The UI gives the viewport (the canvas minus the page
 * header and the dock, in px) and the pixel density; it gets the boards.
 *
 *   Expert        the largest grid of 5 mm cells that fits: cols = floor(viewport_w / 5 mm), rows = floor(viewport_h / 5 mm),
 *                 each at least 6 and at most 30. The grid is NOT forced square.
 *   Intermediate  about 78 % of Expert's columns and rows, Beginner about 55 % (each rounded, at least 6, never larger than
 *                 the level above). On a screen so small that rounding would leave two levels the same size, the lower one
 *                 loses a row or column (down to 5, the core's minimum) so that Beginner < Intermediate < Expert in cells.
 *   Mines         12 % (Beginner), 16 % (Intermediate), 20.5 % (Expert) of the cells, rounded, 1 .. cells - 9.
 *   Cell          square: min(viewport_w / cols, viewport_h / rows), never over 12 mm; the grid is centred in the viewport,
 *                 so easier levels get bigger cells.
 *   hit test      hit_cell() answers the cell index under a point, or -1.
 */
#pragma once

#include "board.hpp"

#include <array>

namespace minefield {

constexpr double kMinCellMm = 5.0;
constexpr double kMaxCellMm = 12.0;
constexpr int kMinGrid = 6;                 /* the least columns and rows of a derived board (5 only to keep the levels apart) */
constexpr double kExpertDensity = 0.205;
constexpr double kIntermediateDensity = 0.16;
constexpr double kBeginnerDensity = 0.12;
constexpr double kIntermediateShare = 0.78;
constexpr double kBeginnerShare = 0.55;

struct BoardPlan {
    Dims dims;                       /* columns, rows (w, h) and mines */
    int cell_px = 0;
    int origin_x = 0, origin_y = 0;  /* the grid's top left, relative to the viewport's */
    int board_w = 0, board_h = 0;    /* cols * cell_px, rows * cell_px */
    double cell_mm = 0;
    bool fits = false;               /* the whole grid is inside the viewport (always true when valid) */
    bool valid = false;
};

int max_cell_px(double ppmm);        /* floor(12 mm) */

/* The grid of the given size in the viewport: cell size, centred origin. mines is copied into dims. */
BoardPlan fit_board(int viewport_w, int viewport_h, double ppmm, int cols, int rows, int mines);

/* Columns and rows of the Expert grid for a viewport (mines not set). */
Dims expert_grid(int viewport_w, int viewport_h, double ppmm);

/* Mines for a grid at a difficulty. */
int mines_for(int cols, int rows, Preset p);

/* The board of a difficulty on this screen. */
BoardPlan derive_board(int viewport_w, int viewport_h, double ppmm, Preset p);

/* The three boards' dims (Beginner, Intermediate, Expert). */
std::array<Dims, kRecordedPresets> derive_boards(int viewport_w, int viewport_h, double ppmm);

/* The cell under a point given in viewport coordinates, or -1 (outside the grid). Index = y * cols + x. */
int hit_cell(int px, int py, int origin_x, int origin_y, int cell_px, int cols, int rows);

} // namespace minefield
