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
