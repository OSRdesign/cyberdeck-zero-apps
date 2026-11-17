/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Playback page geometry (see playback_layout.hpp).
 */
#include "playback_layout.hpp"

#include <algorithm>
#include <cmath>

namespace vhs {

PlaybackLayout playback_layout(const CaptureLayoutIn &in, int row_token)
{
    const CaptureLayout c = capture_layout(in);
    PlaybackLayout o;
    o.u = c.u;
    auto R = [&o](double v) { return static_cast<int>(std::floor(v * o.u + 1e-9)); };
    o.column = c.column;
    o.panel = c.panel;
    o.picture = c.picture;
    o.radius = c.button_radius;
    o.pause_px = R(15);
    o.pm_px = R(18);
    o.volume_px = R(12);
    o.time_px = R(11);
    o.name_px = R(12);
    o.toast_px = c.toast_px;
    o.knob = std::max(8, R(14));
    const int bar_h = std::max(3, R(6));
    int y;
    if (o.column) {
        /* the canvas: the column of the Capture page (88u wide, at the picture's top) */
        const int cx = c.toggle.x, cw = c.toggle.w;
        o.pause = Rect{cx, o.picture.y, cw, R(56)};
        const int half = (cw - R(4)) / 2;
        const int pm_h = std::max(R(40), in.target); /* the canvas's 40u, at least TARGET (a button) */
        o.minus = Rect{cx, o.picture.y + R(64), half, pm_h};
        o.plus = Rect{cx + cw - half, o.picture.y + R(64), half, pm_h};
        o.volume = Rect{cx, o.minus.y + pm_h + R(2), cw, R(16)};
        y = o.picture.y + o.picture.h + R(16);
    } else {
        const int pad = R(8), gap = R(8);
        const int x0 = o.panel.x + pad, w = o.panel.w - 2 * pad;
        const int rh = std::max(c.toggle.h, R(40));
        const int ry = o.picture.y + o.picture.h + R(12);
        const int pw = (w - 2 * gap) / 2, qw = (w - pw - 2 * gap) / 2;
        o.pause = Rect{x0, ry, pw, rh};
        o.minus = Rect{x0 + pw + gap, ry, qw, rh};
        o.plus = Rect{x0 + w - qw, ry, qw, rh};
        o.volume = Rect{x0, ry + rh + R(6), w, R(16)};
        y = o.volume.y + o.volume.h + R(14);
    }
    o.bar = Rect{o.picture.x + R(4), y, o.picture.w - R(8), bar_h};
    const int hit_h = std::max(row_token, R(24));
    o.bar_hit = Rect{o.bar.x - o.knob / 2, y + bar_h / 2 - hit_h / 2, o.bar.w + o.knob, hit_h};
    o.time_left = Rect{o.bar.x, y + R(12), o.bar.w / 2, R(16)};
    o.time_right = Rect{o.bar.x + o.bar.w / 2, y + R(12), o.bar.w - o.bar.w / 2, R(16)};
    o.name = Rect{o.bar.x, y + R(24) + (o.column ? 0 : R(4)), o.bar.w, R(18)};
    if (o.column) o.name.y = y + R(24);
    /* the toast over the picture's bottom (provisional): the seek bar, the times and the name stay readable */
    o.toast = Rect{o.picture.x + R(8), o.picture.y + o.picture.h - R(30) - R(8), o.picture.w - 2 * R(8), R(30)};
    if (c.square) {
        const double k = c.k;
        auto S = [k](double v) { return std::max(1, static_cast<int>(std::floor(v * k + 0.5))); };
        auto F = [k](double v) { return std::max(12, static_cast<int>(std::floor(v * k + 0.5))); };
        o.square = true;
        o.column = false;
        const int g = S(8);
        const Rect in_r{c.panel.x + g, c.panel.y + g, c.panel.w - 2 * g, c.panel.h - 2 * g};
        const int seek_h = std::max(S(56), row_token);
        const int ctl_h = std::max(S(80), in.target);
        const int name_h = S(40);
        const int avail = in_r.h - name_h - seek_h - ctl_h - 3 * g;
        const int pic_h = std::max(16, std::min(S(400), avail));
        o.picture = Rect{in_r.x, in_r.y, in_r.w, pic_h};
        o.radius = S(12);
        o.pause_px = F(30);
        o.pm_px = F(40);
        o.volume_px = F(24);
        o.time_px = F(24);
        o.name_px = F(28);
        o.info_px = F(22);
        o.toast_px = F(24);
        o.knob = S(32);
        const int in_pad = S(6);
        int y2 = o.picture.bottom() + g;
        const int info_w = S(220);
        o.name = Rect{in_r.x + in_pad, y2, in_r.w - 2 * in_pad - info_w - S(16), name_h};
        o.info = Rect{in_r.right() - in_pad - info_w, y2, info_w, name_h};
        y2 += name_h + g;
        const int tl_w = S(72), tr_w = S(84), tgap = S(14), th = S(30);
        o.time_left = Rect{in_r.x + in_pad, y2 + (seek_h - th) / 2, tl_w, th};
        o.time_right = Rect{in_r.right() - in_pad - tr_w, y2 + (seek_h - th) / 2, tr_w, th};
        const int bar_h2 = std::max(3, S(10));
        const int bx = o.time_left.right() + tgap;
        o.bar = Rect{bx, y2 + (seek_h - bar_h2) / 2, std::max(1, o.time_right.x - tgap - bx), bar_h2};
        o.bar_hit = Rect{o.bar.x - o.knob / 2, y2, o.bar.w + o.knob, seek_h};
        y2 += seek_h + g;
        const int pm = std::max(S(80), in.target), vgap = S(10), pause_w = std::max(S(230), 2 * in.target);
        const int vol_w = std::max(1, std::min(S(190), in_r.w - pause_w - 2 * pm - 2 * vgap - g));
        o.pause = Rect{in_r.x, y2, pause_w, ctl_h};
        o.plus = Rect{in_r.right() - pm, y2, pm, ctl_h};
        o.volume = Rect{o.plus.x - vgap - vol_w, y2 + (ctl_h - th) / 2, vol_w, th};
        o.minus = Rect{o.volume.x - vgap - pm, y2, pm, ctl_h};
        o.toast = Rect{o.picture.x + S(12), o.picture.bottom() - S(56) - S(12), o.picture.w - 2 * S(12), S(56)};
    }
    return o;
}

std::int64_t seek_ms_at(const PlaybackLayout &l, int x, std::int64_t total_ms)
{
    if (total_ms <= 0 || l.bar.w <= 0) return 0;
    const int d = std::max(0, std::min(l.bar.w, x - l.bar.x));
    return static_cast<std::int64_t>(static_cast<double>(d) * static_cast<double>(total_ms) / l.bar.w + 0.5);
}

int seek_x_of(const PlaybackLayout &l, std::int64_t ms, std::int64_t total_ms)
{
    if (total_ms <= 0) return l.bar.x;
    const double f = std::max(0.0, std::min(1.0, static_cast<double>(ms) / static_cast<double>(total_ms)));
    return l.bar.x + static_cast<int>(std::lround(f * l.bar.w));
}

} // namespace vhs
