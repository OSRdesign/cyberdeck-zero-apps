/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike UI: the geometry of the table (pure, no LVGL; tests/test_layout.cpp). The UI gives it the canvas, where the
 * page body starts and the toolkit's tokens; it answers where everything goes. Nothing here knows a screen by name:
 * both docks are tried and the one that allows the larger cards wins, so 640x480 and 800x480 get the buttons in a column
 * at the side, a portrait canvas gets them in a row at the bottom, and a square or large canvas gets whichever is better.
 *
 *   canvas = header (the page's) / body. The body holds
 *     hud     one line over the cards (score, time, moves, deal; the UI draws the text)
 *     board   the cards: 7 columns, the top row (stock, waste, 4 foundations) over the tableau
 *     dock    the buttons (Undo, Redo, Hint or Auto, Menu), a column at the side or a row at the bottom. A button is at
 *             least TARGET wide and at least as wide as the widest label (the UI measures it) plus label_air on each
 *             side; the side dock takes that width out of the playfield BEFORE the card size is computed, so a denser
 *             or sparser panel (px per mm) changes the dock, never clips a label; when the column would cost too much
 *             card, the bottom row wins (both are tried).
 *   Card size: the largest that makes 7 columns fit the width AND the worst-case column (6 face down + King .. Ace) fit
 *   the height with every covered face-up card showing its whole index: the strip each shows is at least fan_up_min =
 *   max(15 % of the card, index_strip_px: the rank, the suit and their margins), a covered face-down card shows at least
 *   fan_down_min. The columns are spread over the spare width.
 *   A fanned column keeps its normal overlap until it would run off the board, then both its steps shrink (fan_for),
 *   each towards its minimum and never below it: the last card always ends inside the board, no index is ever cut.
 *   Left-handed mirrors the top row (stock and waste at the right, foundations at the left, the waste fanning leftwards)
 *   and puts the side dock at the left.
 */
#pragma once

#include "board.hpp"

namespace klondike {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

constexpr int kDockButtons = 4;   /* Undo, Redo, Hint (Auto when everything is face up), Menu */

/* A button holds its label with this much air on each side (border included). */
inline int label_air(int pad) { return (pad > 8 ? pad : 8) / 2 + 1; }

struct LayoutIn {
    int w = 0, h = 0;              /* canvas */
    int body_y = 0;                /* where the page body starts (under the header) */
    int target = 44;               /* a button's smallest side: the TARGET token */
    int pad = 6;                   /* the base padding: SPACE_M */
    int hud_h = 20;                /* height of the HUD line */
    int label_w = 0;               /* the widest dock label (Undo, Redo, Hint, Auto, Menu) measured in the font it is drawn in */
    int ppmm_x100 = 1130;          /* px per mm x 100 (only reported) */
    bool left_handed = false;
    bool draw3 = false;            /* the waste fans three cards */
};

struct Layout {
    bool valid = false;
    bool side_dock = false;
    Rect hud, board, dock;
    Rect button[kDockButtons];
    int card_w = 0, card_h = 0, radius = 0;
    int col_x[kColumns] = {};      /* x of each visual column, left to right */
    int top_y = 0, tab_y = 0;      /* top of the top row / of the tableau */
    int gap_x = 0;
    int pile_col[kPiles] = {};     /* visual column of the top-row piles (stock, waste, foundations); tableau c = c */
    int waste_step = 0;            /* x offset between the cards of a fanned (draw 3) waste */
    int waste_dir = 1;             /* +1 fans to the right, -1 (left-handed) to the left */
    int fan_avail = 0;             /* the tableau's room for steps: from tab_y to the board's bottom minus one card */
    double fan_down0 = 0, fan_up0 = 0;      /* the normal steps */
    int index_h = 0;               /* height of a card's index strip (what a covered card shows) = fan_up_min */
    int fan_up_min = 0, fan_down_min = 0; /* the least a covered face-up / face-down card shows, px */

    int pile_x(int pile) const { return col_x[is_tableau(pile) ? pile - kTableau0 : pile_col[pile]]; }
    int pile_y(int pile) const { return is_tableau(pile) ? tab_y : top_y; }
    Rect pile_rect(int pile) const { return Rect{pile_x(pile), pile_y(pile), card_w, card_h}; }
};

/* Card shape (width : height = 5 : 7) and the worst case the board must hold (a fraction of the card height). */
constexpr double kCardAspect = 1.4;
constexpr double kFanUp0 = 0.30;       /* a face-up card's strip when there is room */
constexpr double kFanDown0 = 0.11;     /* a face-down card's */
constexpr double kFanUpMin = 0.15;     /* the guarantee for the worst case: never less than this (index stays readable) */
constexpr double kFanDownMin = 0.05;
constexpr int kMinCardW = 26;

/* The index (rank and suit, top left of a card) must be whole in the strip a covered card shows. The UI draws the rank
 * in a font of index_font_px(card_h) with the top of the glyphs kIndexMargin px under the card's top edge; the glyphs of
 * the fonts in use are at most kIndexCapFrac of their size high (the UI measures the real font and shrinks it if a font
 * is ever taller). index_strip_px is the strip that holds margin + glyph height + one px of air. */
constexpr double kIndexPxFrac = 0.165;
constexpr double kIndexCapFrac = 0.76;
constexpr int kIndexMargin = 2;
int index_font_px(int card_h);
int index_strip_px(int card_h);

Layout compute_layout(const LayoutIn &in);

struct Fan {
    double down = 0, up = 0;
};
/* The steps of a column of `ndown` face-down and `nup` face-up cards (normal, or shrunk to fit fan_avail). */
Fan fan_for(const Layout &l, int ndown, int nup);
/* y of card i of that column relative to tab_y. */
int fan_y(const Fan &f, int ndown, int i);
/* Bottom of the column relative to tab_y (its last card included). */
int column_extent(const Layout &l, int ndown, int nup);

} // namespace klondike
