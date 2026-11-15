/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Scenes page geometry (see scenes_layout.hpp).
 */
#include "scenes_layout.hpp"

#include <algorithm>
#include <cmath>

namespace vhs {

ScenesLayout scenes_layout(const ScenesLayoutIn &in)
{
    const CaptureLayout c = capture_layout(in.base);
    ScenesLayout o;
    o.u = c.u;
    auto R = [&o](double v) { return static_cast<int>(std::floor(v * o.u + 1e-9)); };
    o.panel = c.panel;
    /* the 320x480 form: portrait, and a landscape panel too narrow for the date column (a dense square) */
    o.portrait = in.base.h > in.base.w || o.panel.w < R(400);
    const int px = o.panel.x, py = o.panel.y, pw = o.panel.w, ph = o.panel.h;
    o.head = Rect{px + R(16), py, pw / 2, R(30)};
    o.head_px = R(12);

    /* the chips: the canvas's x (250 of 464, 112 of 304) counted from the right edge */
    const int cw = o.portrait ? R(56) : R(60), ch = R(24), gap = R(6), right = o.portrait ? R(12) : R(22);
    const int cy = py + (o.portrait ? R(4) : R(3));
    int x = px + pw - right - 3 * cw - 2 * gap;
    const int hit_h = std::max(in.row, cy - py + ch);
    for (int i = 0; i < 3; ++i) {
        o.chip[i] = Rect{x, cy, cw, ch};
        o.chip_hit[i] = Rect{x - gap / 2, py, cw + gap, hit_h};
        x += cw + gap;
    }
    o.chip_px = R(12);
    o.chip_radius = ch / 2;

    const int top = o.portrait ? R(34) : R(30);
    o.list = Rect{px, py + top, pw, std::max(0, ph - top - R(4))};
    o.row_h = std::max(in.list_row, o.portrait ? R(56) : R(52));
    if (o.portrait) {
        o.thumb = Rect{R(10), R(10), R(56), R(32)};
        o.name_x = R(76);
        o.name_y = R(7);
        o.name_px = R(13);
        o.meta_y = R(29);
        o.date_r = 0;
        o.chev_y = R(20);
        o.name_w = pw - o.name_x - R(32);
    } else {
        o.thumb = Rect{R(12), R(8), R(64), R(36)};
        o.name_x = o.thumb.w + R(24);
        o.name_y = R(6);
        o.name_px = R(14);
        o.meta_y = R(28);
        o.date_r = R(36);
        o.date_y = R(6);
        o.chev_y = R(18);
        o.name_w = pw - o.thumb.w - R(150);
    }
    o.name_line = R(20);
    o.meta_px = R(11);
    o.meta_line = R(16);
    o.date_px = R(11);
    o.chev_r = R(14);
    o.chev = R(16);
    o.lock_px = R(11);
    o.empty_px = R(13);
    o.toast_px = R(12);
    const int toast_h = R(30);
    o.toast = Rect{px + R(24), py + ph - toast_h - R(8), pw - 2 * R(24), toast_h};

    /* the sheet (canvas SceneOptions-480x320; portrait the same parts across the panel) */
    o.sheet_row_h = std::max(in.list_row, R(31));
    o.sheet_row_y = R(28);
    o.sheet_label_x = R(16);
    o.sheet_key_r = R(16);
    o.sheet_label_px = R(15);
    o.sheet_key_px = R(13);
    o.sheet_head_px = R(12);
    o.sheet_note_px = R(11);
    const int sx = px + R(8), sw = pw - 2 * R(8);
    const int note_lines = o.portrait ? 2 : 1;
    const int note_h = note_lines * R(14);
    /* the six rows, or as many as fit above the note (the rows' box scrolls then): the note always shows */
    const int fixed = o.sheet_row_y + R(2) + note_h + R(4);
    o.sheet_rows_h = std::max(o.sheet_row_h, std::min(6 * o.sheet_row_h, ph - fixed));
    const int sh = fixed + o.sheet_rows_h;
    const int sy = std::max(py, std::min(py + top, py + ph - sh));
    o.sheet = Rect{sx, sy, sw, sh};
    o.sheet_radius = R(10);
    o.sheet_head = Rect{R(14), R(6), sw - 2 * R(14), R(20)};
    o.sheet_note = Rect{R(14), o.sheet_row_y + o.sheet_rows_h + R(2), sw - 2 * R(14), note_h};
    return o;
}

} // namespace vhs
