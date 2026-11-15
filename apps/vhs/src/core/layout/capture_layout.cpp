/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Capture page's geometry (see capture_layout.hpp).
 */
#include "capture_layout.hpp"

#include <algorithm>
#include <cmath>

namespace vhs {

int floor16(int v)
{
    return v <= 0 ? 0 : (v / 16) * 16;
}

CaptureLayout capture_layout(const CaptureLayoutIn &in)
{
    CaptureLayout o;
    const int T = std::max(1, in.target);
    o.u = T / 44.0;
    /* canvas px in screen px, rounded down: at a TARGET a little over 44 px (6.3 to 6.7 px/mm) the canvas's 8 px
     * margins stay 8 and the picture box keeps the canvas's (and the spike's measured) 352x198 / 288x162 */
    auto R = [&o](double v) { return static_cast<int>(std::floor(v * o.u + 1e-9)); };
    o.column = in.w >= in.h;
    o.panel = Rect{R(8), in.body_y + R(4), in.w - 2 * R(8), 0};
    o.panel.h = std::max(0, in.h - o.panel.y - R(8));
    o.panel_radius = R(10);
    o.button_radius = R(8);
    o.ring = std::max(2, R(4));
    o.title_px = R(14);
    o.note_px = R(11);
    o.chip_px = R(11);
    o.button_px = R(13);
    o.toggle_px = R(11);
    o.sub_px = R(10);
    o.toast_px = R(12);
    o.toast_h = R(30);
    o.overlay_px = R(14);
    o.chip_x = R(8);
    o.chip_y = R(6);
    o.chip_pad_h = R(6);
    o.chip_pad_v = R(2);
    o.chip_radius = R(4);
    const int pad = R(8);
    const Rect inner{o.panel.x + pad, o.panel.y + pad, o.panel.w - 2 * pad, o.panel.h - 2 * pad};
    /* under the picture (the canvas): 8u, the mode word (20u), two note lines (28u), 6u to the panel's edge */
    const int text_budget = R(8) + R(20) + R(28) + R(6);

    if (o.column) {
        const int col_w = 2 * T;
        const int avail_w = inner.w - pad - col_w;
        const int avail_h = o.panel.h - pad - text_budget;
        const int pw = floor16(std::min(avail_w, avail_h * 16 / 9));
        o.picture = Rect{inner.x, inner.y, pw, pw * 9 / 16};
        const int col_x = o.panel.x + o.panel.w - pad - col_w;
        const int g1 = R(6), g = R(8);
        const int avail_col = inner.h;
        int d = col_w - 2 * R(8);
        int bh = T;
        if (d + g1 + 3 * bh + 2 * g > avail_col) {
            d = std::max(T, avail_col - (g1 + 3 * bh + 2 * g));
            if (d + g1 + 3 * bh + 2 * g > avail_col) bh = std::max(1, (avail_col - d - g1 - 2 * g) / 3);
        }
        o.shutter = Rect{col_x + (col_w - d) / 2, inner.y, d, d};
        o.toggle = Rect{col_x, o.shutter.bottom() + g1, col_w, bh};
        o.scenes = Rect{col_x, o.toggle.bottom() + g, col_w, bh};
        o.settings = Rect{col_x, o.scenes.bottom() + g, col_w, bh};
        const int tx = inner.x + R(4);
        const int tw = std::max(0, pw - R(12));
        o.title = Rect{tx, o.picture.bottom() + pad, tw, R(20)};
        o.note = Rect{tx, o.title.bottom(), tw, std::max(0, o.panel.bottom() - o.title.bottom())};
        o.toast = Rect{tx, o.title.y, tw, o.toast_h};
    } else {
        const int inset = R(4);
        const int side_w0 = 2 * T;
        const int gap_s = R(10);
        const int below = R(30) + R(84) + R(6) + T + R(14) + R(20) + R(28); /* the rows and the text under it */
        const int pw = floor16(std::min(inner.w, std::max(16, (inner.h - below) * 16 / 9)));
        o.picture = Rect{inner.x + (inner.w - pw) / 2, inner.y, pw, pw * 9 / 16};
        int side_w = side_w0;
        int d = inner.w - 2 * inset - 2 * side_w - 2 * gap_s;
        d = std::min(d, R(84));
        if (d < T) {
            d = T;
            side_w = std::max(1, (inner.w - 2 * inset - d - 2 * gap_s) / 2);
        }
        const int row_y = o.picture.bottom() + R(30);
        o.shutter = Rect{inner.x + (inner.w - d) / 2, row_y, d, d};
        const int side_h = std::max(T, R(56));
        const int side_y = o.shutter.y + d / 2 - side_h / 2;
        o.toggle = Rect{inner.x + inset, side_y, side_w, side_h};
        o.scenes = Rect{inner.x + inner.w - inset - side_w, side_y, side_w, side_h};
        const int row_bottom = std::max(o.shutter.bottom(), o.toggle.bottom());
        o.settings = Rect{inner.x + inset, row_bottom + R(6), inner.w - 2 * inset, T};
        const int tx = inner.x + inset, tw = inner.w - 2 * inset;
        o.title = Rect{tx, o.settings.bottom() + R(14), tw, R(20)};
        o.note = Rect{tx, o.title.bottom(), tw, std::max(0, o.panel.bottom() - o.title.bottom())};
        o.toast = Rect{tx, o.title.y, tw, o.toast_h};
    }

    /* NoCamera: title, text (two lines), [the hint (two lines)], Scenes; the block centred in the panel */
    o.nc_title_px = R(18);
    o.nc_text_px = R(13);
    o.nc_hint_px = R(11);
    o.nc_scenes_px = R(14);
    const int text_w = std::min(R(360), inner.w);
    const int title_h = R(22), text_h = R(36), hint_h = R(30);
    const int bw = std::min(inner.w, std::max(R(160), 2 * T)), bh = std::max(T, R(38));
    const int cx = o.panel.x + o.panel.w / 2;
    for (int with_hint = 0; with_hint < 2; ++with_hint) {
        /* the canvas: title, 10u, text (two 18u lines), 22u, Scenes; the hint adds 8u + two lines */
        const int block = title_h + R(10) + text_h + (with_hint ? R(8) + hint_h + R(14) : R(22)) + bh;
        const int top = o.panel.y + std::max(pad, (o.panel.h - block) / 2);
        Rect t{inner.x, top, inner.w, title_h};
        Rect x{cx - text_w / 2, t.bottom() + R(10), text_w, text_h};
        Rect hn{cx - text_w / 2, x.bottom() + R(8), text_w, hint_h};
        Rect b{cx - bw / 2, (with_hint ? hn.bottom() + R(14) : x.bottom() + R(22)), bw, bh};
        if (with_hint) {
            o.nch_title = t;
            o.nch_text = x;
            o.nc_hint = hn;
            o.nch_scenes = b;
        } else {
            o.nc_title = t;
            o.nc_text = x;
            o.nc_scenes = b;
        }
    }
    return o;
}

} // namespace vhs
