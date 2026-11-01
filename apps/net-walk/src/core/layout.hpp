/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: where everything goes (pure arithmetic, no LVGL, unit tested).
 *
 * The body (the screen under the launcher's shared page header: the header's height is the layout service's, in real
 * pixels, and is not this module's business) is drawn from a LOGICAL canvas (the Palm OS look) scaled by a whole number,
 * so every edge is crisp. The scale is the largest whole number at which the logical canvas still holds the minimum of
 * the body (landscape 240x136, portrait 160x200); the canvas then takes the whole body (at most scale-1 spare pixels per
 * side stay as the grey letterbox) and the board's tile size is the largest whole number of logical pixels that fits.
 * All coordinates are relative to the body's top left corner.
 *
 * Landscape: the board at the left, the panel (readouts, three buttons) at the right.
 * Portrait: the board, the panel under it (the readouts in two lines, the three buttons side by side).
 * A near-square body (within 5:4) takes whichever of the two gives the larger tiles.
 */
#pragma once

namespace netwalk {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

constexpr int kMargin = 3;
constexpr int kBevel = 2;        /* the frame round the board */
constexpr int kButtonH = 20;
constexpr int kButtonGap = 3;
constexpr int kLineH = 11;       /* a readout line */
constexpr int kPanelMinW = 84;   /* landscape: the narrowest panel (Time 01:37 in the bold font) */
constexpr int kPortraitPanelH = 52;

constexpr int kMinLandscapeW = 240, kMinLandscapeH = 136; /* the body: 480x280 is scale 2 */
constexpr int kMinPortraitW = 160, kMinPortraitH = 200;

struct Layout {
    bool ok = false;
    bool portrait = false;
    int scale = 1;           /* screen pixels per logical pixel */
    int lw = 0, lh = 0;      /* the logical canvas */
    int ox = 0, oy = 0;      /* where its top left corner lies in the body (pixels) */
    int tile = 0;            /* logical pixels per tile */
    int n = 0;               /* tiles per side */
    Rect frame;              /* the board with its bevel */
    Rect board;              /* the tiles: n * tile square */
    Rect panel;
    Rect line[4];            /* caption (the level), Moves, Time, Best (landscape); portrait uses them as 2 x 2 cells */
    Rect button[3];          /* New game, Size, Scores */
    bool short_new = false;  /* the first button says "New" */

    /* the tile under a screen pixel, or -1 */
    int tile_at(int px, int py) const;
    /* the tile's rectangle in logical pixels */
    Rect tile_rect(int index) const;
    /* body pixel to logical (false outside the canvas) */
    bool to_logical(int px, int py, int *lx, int *ly) const;
};

/* the layout of an n x n board on a screen_w x screen_h body (the screen under the page header) */
Layout compute_layout(int screen_w, int screen_h, int n);
/* the same for a forced orientation (tests) */
Layout compute_layout_for(int screen_w, int screen_h, int n, bool portrait);

} // namespace netwalk
