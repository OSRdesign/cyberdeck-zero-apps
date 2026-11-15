/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Scenes page and its options sheet in canvas units (canvas Scenes-480x320, Scenes-320x480,
 * SceneOptions-480x320; brief 12.2 for the undrawn classes: the same units scaled by TARGET / 44, landscape and square
 * take the 480x320 form, portrait the 320x480 form). Coordinates are the screen's. LVGL-free, unit-tested.
 *   head    "SCENES, 14": 16u in, a 30u line, 12u bold grey, in the panel's top left
 *   chips   All / Movies / Photos: 60u x 24u (56u portrait), 6u apart, at the panel's top right; each with a hit box at
 *           least ROW high (from the panel's top) so a finger can press it
 *   rows    52u (56u portrait) high, at least LIST_ROW; the thumbnail 64x36u at (12u, 8u) (56x32u at (10u, 10u)); the
 *           name 14u (13u) at its right, the meta 11u under it, the date 11u right-aligned 36u from the right (landscape
 *           only), the chevron 16u, 14u from the right
 *   sheet   the cream sheet 8u in, 30u under the panel's top (portrait: 34u), the header line, six 31u rows (at least
 *           LIST_ROW), the note under them
 */
#pragma once

#include "capture_layout.hpp"

namespace vhs {

struct ScenesLayoutIn {
    CaptureLayoutIn base;
    int row = 42;      /* the ROW token (px) */
    int list_row = 30; /* the LIST_ROW token (px) */
};

struct ScenesLayout {
    double u = 1;
    bool portrait = false;
    Rect panel;
    Rect head;                 /* the head line */
    int head_px = 12;
    Rect chip[3], chip_hit[3];
    int chip_px = 12, chip_radius = 12;
    Rect list;                 /* the rows' scrolling area */
    int row_h = 52;
    Rect thumb;                /* in a row */
    int name_x = 88, name_y = 6, name_w = 250, name_px = 14, name_line = 20;
    int meta_y = 28, meta_px = 11, meta_line = 16;
    int date_r = 36, date_y = 6, date_px = 11;   /* date_r 0: no date column */
    int chev_r = 14, chev_y = 18, chev = 16;
    int lock_px = 11;
    int empty_px = 13;
    Rect toast;
    int toast_px = 12;
    /* the sheet */
    Rect sheet;
    int sheet_radius = 10;
    Rect sheet_head;
    int sheet_head_px = 12;
    int sheet_row_y = 28, sheet_row_h = 31, sheet_label_x = 16, sheet_key_r = 16, sheet_label_px = 15, sheet_key_px = 13;
    int sheet_rows_h = 186;    /* the rows' box: all six, or what fits (it scrolls then) */
    Rect sheet_note;
    int sheet_note_px = 11;
};

ScenesLayout scenes_layout(const ScenesLayoutIn &in);

} // namespace vhs
