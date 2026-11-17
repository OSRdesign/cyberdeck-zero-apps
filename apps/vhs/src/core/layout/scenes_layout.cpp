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
    /* the rows, or as many as fit above the note (the rows' box scrolls then): the note always shows */
    const int fixed = o.sheet_row_y + R(2) + note_h + R(4);
    o.sheet_rows_h = std::max(o.sheet_row_h, std::min(kSheetRows * o.sheet_row_h, ph - fixed));
    const int sh = fixed + o.sheet_rows_h;
    const int sy = std::max(py, std::min(py + top, py + ph - sh));
    o.sheet = Rect{sx, sy, sw, sh};
    o.sheet_radius = R(10);
    o.sheet_head = Rect{R(14), R(6), sw - 2 * R(14), R(20)};
    o.sheet_note = Rect{R(14), o.sheet_row_y + o.sheet_rows_h + R(2), sw - 2 * R(14), note_h};
    if (c.square) {
        /* the Square class (owner's canvas, 720x720 times k): chips 56 high at the top left, the count right, rows 92
         * (96 with the gap) with a 104x78 thumbnail, the hint line 40 high at the bottom; the sheet across the
         * panel's bottom: header 46, rows 72 (76 with the gap), the key labels darker */
        const double k = c.k;
        auto S = [k](double v) { return std::max(1, static_cast<int>(std::floor(v * k + 0.5))); };
        auto F = [k](double v) { return std::max(12, static_cast<int>(std::floor(v * k + 0.5))); };
        o.square = true;
        o.portrait = false;
        o.head_right = true;
        const int pad = S(8), g = S(8);
        const int chip_h = std::max(S(56), in.row);
        const int chip_w = S(152), chip_gap = S(10);
        const int cy2 = py + pad;
        int cx2 = px + pad;
        for (int i = 0; i < 3; ++i) {
            o.chip[i] = Rect{cx2, cy2, chip_w, chip_h};
            o.chip_hit[i] = o.chip[i];
            cx2 += chip_w + chip_gap;
        }
        o.chip_px = F(26);
        o.chip_radius = chip_h / 2;
        o.head_px = F(22);
        o.head = Rect{px + pw - pad - S(6) - S(150), cy2, S(150), chip_h};
        const int hint_h = S(40);
        o.hint = Rect{px + pad + S(6), py + ph - pad - hint_h, pw - 2 * pad - 2 * S(6), hint_h};
        o.hint_px = F(22);
        const int top2 = cy2 - py + chip_h + g;
        o.list = Rect{px + pad, py + top2, pw - 2 * pad, std::max(0, o.hint.y - g - (py + top2))};
        o.row_h = std::max(in.list_row, S(96));
        o.thumb = Rect{S(8), (o.row_h - S(78)) / 2, S(104), S(78)};
        o.name_x = S(8) + S(104) + S(16);
        o.name_px = F(26);
        o.name_line = S(32);
        o.meta_px = F(20);
        o.meta_line = S(24);
        o.name_y = std::max(0, (o.row_h - (o.name_line + S(4) + o.meta_line)) / 2);
        o.meta_y = o.name_y + o.name_line + S(4);
        o.date_px = F(20);
        o.date_r = S(12);
        o.date_y = o.name_y;
        o.chev_r = 0;
        o.chev = 0;
        o.chev_y = 0;
        o.lock_px = F(20);
        o.empty_px = F(24);
        o.name_w = std::max(1, o.list.w - o.name_x - S(170));
        o.toast_px = F(24);
        const int toast_h2 = S(56);
        o.toast = Rect{px + S(24), py + ph - toast_h2 - S(8), pw - 2 * S(24), toast_h2};
        /* the sheet */
        o.sheet_row_h = std::max(in.list_row, S(76));
        o.sheet_row_y = S(64);
        o.sheet_label_x = S(36);
        o.sheet_key_r = S(36);
        o.sheet_label_px = F(28);
        o.sheet_key_px = F(22);
        o.sheet_head_px = F(26);
        o.sheet_detail_px = F(20);
        o.sheet_note_px = F(22);
        o.sheet_radius = S(22);
        const int fixed2 = o.sheet_row_y + S(12);
        o.sheet_rows_h = std::max(o.sheet_row_h, std::min(kSheetRows * o.sheet_row_h, ph - fixed2));
        const int sh2 = fixed2 + o.sheet_rows_h;
        o.sheet = Rect{px, py + ph - sh2, pw, sh2};
        o.sheet_head = Rect{S(24), S(14), pw - 2 * S(24), S(46)};
        o.sheet_note = Rect{0, 0, 0, 0}; /* the canvas draws none */
    }
    return o;
}

} // namespace vhs
