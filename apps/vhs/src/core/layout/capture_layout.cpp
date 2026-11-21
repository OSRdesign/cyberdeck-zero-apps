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

double square_scale(int w, int h)
{
    return std::max(0.4, std::min(2.5, std::min(w, h) / 720.0));
}

CaptureLayout capture_layout(const CaptureLayoutIn &in)
{
    CaptureLayout o;
    const int T = std::max(1, in.target);
    o.u = T / 44.0;
    if (in.square) {
        /* the Square class: the canvas drawn at 720x720, scaled by k; u = 2k (see capture_layout.hpp) */
        o.k = square_scale(in.w, in.h);
        o.u = 2 * o.k;
    }
    const double k = o.k;
    /* a canvas-720 length in px: rounded, at least 1 */
    auto S = [k](double v) { return std::max(1, static_cast<int>(std::floor(v * k + 0.5))); };
    /* a canvas-720 type size: rounded, never under 12 px */
    auto F = [k](double v) { return std::max(12, static_cast<int>(std::floor(v * k + 0.5))); };
    /* canvas px in screen px, rounded down: at a TARGET a little over 44 px (6.3 to 6.7 px/mm) the canvas's 8 px
     * margins stay 8 and the picture box keeps the canvas's (and the spike's measured) 352x198 / 288x162 */
    auto R = [&o](double v) { return static_cast<int>(std::floor(v * o.u + 1e-9)); };
    o.square = in.square;
    o.column = in.w >= in.h && !in.square;
    o.panel = Rect{R(8), in.body_y + R(4), in.w - 2 * R(8), 0};
    o.panel.h = std::max(0, in.h - o.panel.y - R(8));
    if (o.square) o.panel = Rect{S(8), in.body_y, in.w - 2 * S(8), std::max(0, in.h - in.body_y - S(8))};
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
    const int pad = o.square ? S(8) : R(8);
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
    } else if (o.square) {
        /* the Square class (owner's canvas, 720x720 times k): the picture across the panel, one control row 120 high
         * (Photo / Video stacked 56 high each, the shutter 120, Scenes / Settings stacked), a 40 high status line */
        const int g = S(8);
        const int bh = std::max(T, S(56));
        const int side_h = 2 * bh + g;
        const int row_h = std::max(S(120), side_h);
        const int status_h = S(40);
        const int avail = inner.h - row_h - status_h - 2 * g;
        const int pic_h = std::max(16, std::min(S(440), avail));
        o.picture = Rect{inner.x, inner.y, inner.w, pic_h};
        const int row_y = o.picture.bottom() + g;
        const int d = std::min(row_h, std::max(S(120), T));
        o.shutter = Rect{inner.x + (inner.w - d) / 2, row_y + (row_h - d) / 2, d, d};
        const int colw = std::max(1, (inner.w - d) / 2 - S(14));
        const int side_y = row_y + (row_h - side_h) / 2;
        const int rx = inner.right() - colw;
        o.photo_btn = Rect{inner.x, side_y, colw, bh};
        o.video_btn = Rect{inner.x, o.photo_btn.bottom() + g, colw, bh};
        o.toggle = Rect{inner.x, row_y, colw, row_h};   /* "Photo now" while recording */
        o.scenes = Rect{rx, side_y, colw, bh};
        o.settings = Rect{rx, o.scenes.bottom() + g, colw, bh};
        const int sx = inner.x + S(6);
        o.status = Rect{sx, row_y + row_h + g, inner.w - 2 * S(6), status_h};
        o.title = o.status;
        o.note = o.status;
        o.toast_h = S(56);
        o.toast = Rect{inner.x + S(12), o.picture.bottom() - S(72) - S(12), inner.w - 2 * S(12), o.toast_h};
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
    /* the self-timer's countdown (canvas Countdown-480x320): a 96u ring (6u, red) with the 52u number, centred on the
     * picture; smaller when the picture is lower than the ring would need */
    o.cd_d = std::max(16, std::min(R(96), o.picture.h - 2 * R(8)));
    o.cd_border = std::max(2, R(6) * o.cd_d / std::max(1, R(96)));
    o.cd_px = std::max(12, R(52) * o.cd_d / std::max(1, R(96)));
    /* the recording's tags (canvas Recording-480x320) */
    o.tag_h = R(22);
    o.tag_pad = R(8);
    o.tag_radius = R(11);
    o.tag_dot = R(10);
    o.tag_gap = R(6);
    o.tag_px = R(12);
    o.photo_now_px = R(12);
    o.rec_face = std::max(4, o.shutter.w * 34 / 100);
    o.rec_face_radius = R(6);
    o.lamp_y = std::max(o.chip_y + o.tag_h + R(4), o.picture.h - R(28));
    if (o.square) {
        /* the canvas's own sizes (720x720 px times k); what is not listed keeps the u = 2k value (1 u = 2 canvas px) */
        o.panel_radius = S(14);
        o.button_radius = S(10);
        o.ring = std::max(2, S(6));
        o.title_px = F(28);
        o.note_px = F(22);
        o.status_px = F(22);
        o.chip_px = F(20);
        o.button_px = F(26);
        o.mode_px = F(26);
        o.toggle_px = F(26);
        o.sub_px = F(20);
        o.photo_now_px = F(28);
        o.toast_px = F(24);
        o.overlay_px = F(28);
        o.chip_x = S(12);
        o.chip_y = S(12);
        o.chip_pad_h = S(12);
        o.chip_pad_v = S(6);
        o.chip_radius = S(8);
        /* the Sound-on lamp (22 px, 16 px dot, 16 px padding) at the picture's top right; the REC tag (34 px, 26 px dot) */
        o.tag_h = S(44);
        o.tag_pad = S(16);
        o.tag_radius = o.tag_h / 2;
        o.tag_dot = S(16);
        o.tag_gap = S(10);
        o.tag_px = F(22);
        o.rec_tag_h = S(58);
        o.rec_tag_pad_l = S(14);
        o.rec_tag_pad_r = S(18);
        o.rec_tag_gap = S(12);
        o.rec_tag_dot = S(26);
        o.rec_tag_px = F(34);
        o.lamp_inset = S(12);
        o.lamp_y = o.chip_y;
        o.rec_face = std::max(4, o.shutter.w * 47 / 100);
        o.rec_face_radius = S(10);
        o.cd_d = std::max(16, std::min(S(192), o.picture.h - 2 * S(8)));
        o.cd_border = std::max(2, S(10) * o.cd_d / std::max(1, S(192)));
        o.cd_px = std::max(12, S(104) * o.cd_d / std::max(1, S(192)));
    }
    /* the camera chip: at the picture's top right, mirroring the format chip's inset at the top left */
    {
        const int ch = o.square ? std::max(T, S(52)) : T;
        const int cw = std::max(2 * T, o.picture.w * (o.square ? 45 : 50) / 100);
        const int mx = o.chip_x, my = o.chip_y;
        const int w = std::min(cw, std::max(1, o.picture.w - 2 * mx));
        o.cam_chip = Rect{o.picture.x + o.picture.w - mx - w, o.picture.y + my, w, std::min(ch, std::max(1, o.picture.h - 2 * my))};
        o.cam_chip_px = o.square ? o.button_px : std::max(10, o.button_px);
    }
    return o;
}

ReviewLayout review_layout(const CaptureLayoutIn &in)
{
    /* the capture page's frame (panel, picture, text), with the review's buttons (canvas PhotoReview-480x320): in the
     * column Previous, Next, [Send to Notes], Delete at the canvas's slots (8u between, 16u before Send to Notes);
     * portrait and square: one row of them under the picture */
    const CaptureLayout c = capture_layout(in);
    ReviewLayout o;
    o.u = c.u;
    o.column = c.column;
    o.panel = c.panel;
    o.title_px = c.title_px;
    o.note_px = c.note_px;
    o.chip_px = c.chip_px;
    o.button_px = static_cast<int>(std::floor(12 * c.u + 1e-9));
    o.notes_px = static_cast<int>(std::floor(11 * c.u + 1e-9));
    const int T = std::max(1, in.target);
    o.square = c.square;
    if (c.square) {
        /* the Square class (undrawn: the capture page's scale): the picture as the live view's, one row of Previous /
         * Next / Delete 80 high under it, then the file name (28 px) and the note (22 px) */
        const double k = c.k;
        auto S = [k](double v) { return std::max(1, static_cast<int>(std::floor(v * k + 0.5))); };
        const int g = S(8);
        const Rect in_r{c.panel.x + S(8), c.panel.y + S(8), c.panel.w - 2 * S(8), c.panel.h - 2 * S(8)};
        const int bh = std::max(T, S(80));
        const int title_h = S(40);
        const int avail = in_r.h - bh - title_h - 2 * g - S(8);
        const int pic_h = std::max(16, std::min(S(440), avail));
        o.picture = Rect{in_r.x, in_r.y, in_r.w, pic_h};
        const int y = o.picture.bottom() + g;
        const int bw = std::max(1, (in_r.w - 2 * g) / 3);
        o.prev = Rect{in_r.x, y, bw, bh};
        o.next = Rect{o.prev.right() + g, y, bw, bh};
        o.del = Rect{o.next.right() + g, y, in_r.right() - (o.next.right() + g), bh};
        o.notes = Rect{o.del.x, y, 0, 0};
        const int tx = in_r.x + S(6), tw = in_r.w - 2 * S(6);
        o.title = Rect{tx, o.prev.bottom() + g, tw, title_h};
        o.note = Rect{tx, o.title.bottom(), tw, std::max(0, c.panel.bottom() - o.title.bottom() - S(8))};
        o.button_px = std::max(12, S(26));
        o.notes_px = o.button_px;
        o.chip_x = c.chip_x;
        o.chip_y = c.chip_y;
        o.chip_pad_h = c.chip_pad_h;
        o.chip_pad_v = c.chip_pad_v;
        o.chip_radius = c.chip_radius;
        return o;
    }
    auto R = [&c](double v) { return static_cast<int>(std::floor(v * c.u + 1e-9)); };
    const int pad = R(8), g = R(8);
    const Rect inner{c.panel.x + pad, c.panel.y + pad, c.panel.w - 2 * pad, c.panel.h - 2 * pad};
    if (o.column) {
        o.picture = c.picture;
        const int col_w = 2 * T, col_x = c.toggle.x;
        int bh = T, g2 = R(16);
        if (4 * bh + 2 * g + g2 > inner.h) {
            g2 = g;
            bh = std::max(1, std::min(T, (inner.h - 3 * g) / 4));
        }
        o.prev = Rect{col_x, inner.y, col_w, bh};
        o.next = Rect{col_x, o.prev.bottom() + g, col_w, bh};
        o.notes = Rect{col_x, o.next.bottom() + g2, col_w, bh};
        o.del = Rect{col_x, o.notes.bottom() + g, col_w, bh};
        o.title = c.title;
        o.note = c.note;
    } else {
        /* one row: Previous, Next, Delete (Send to Notes, optional, M6, would take a fourth slot) */
        const int below = g + T + g + R(20) + R(28) + R(6);
        const int pw = floor16(std::min(inner.w, std::max(16, (c.panel.h - pad - below) * 16 / 9)));
        o.picture = Rect{inner.x + (inner.w - pw) / 2, inner.y, pw, pw * 9 / 16};
        const int y = o.picture.bottom() + g;
        const int bw = std::max(1, (inner.w - 2 * g) / 3);
        o.prev = Rect{inner.x, y, bw, T};
        o.next = Rect{o.prev.right() + g, y, bw, T};
        o.del = Rect{o.next.right() + g, y, inner.right() - (o.next.right() + g), T};
        o.notes = Rect{o.del.x, y, 0, 0};
        const int tx = inner.x + R(4), tw = inner.w - 2 * R(4);
        o.title = Rect{tx, o.prev.bottom() + g, tw, R(20)};
        o.note = Rect{tx, o.title.bottom(), tw, std::max(0, c.panel.bottom() - o.title.bottom())};
    }
    o.chip_x = c.chip_x;
    o.chip_y = c.chip_y;
    o.chip_pad_h = c.chip_pad_h;
    o.chip_pad_v = c.chip_pad_v;
    o.chip_radius = c.chip_radius;
    return o;
}

} // namespace vhs
