/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the windowed Playback page in canvas units (canvas PlaybackWindowed-480x320, without its Full screen button:
 * P2). The picture box is the Capture page's. Landscape (the canvas): Pause / Play 88x56u in the column's top, "-" and
 * "+" 42x40u under it, "Volume 72%" 12u under them; under the picture the seek bar (6u, a 14u knob) with the times 11u
 * under it, then the name 12u. Portrait and square (undrawn, brief 12.4 item 7): one row under the picture (Pause / Play
 * half the width, "-" and "+" a quarter each), then the volume line, the seek bar, the times and the name. The seek bar's
 * hit box is at least ROW high. The Square class has its own form (see the struct). LVGL-free, unit-tested.
 */
#pragma once

#include "capture_layout.hpp"

namespace vhs {

struct PlaybackLayout {
    double u = 1;
    bool column = true;
    /* Square class only (canvas vhs-square-layout, 720x720 times k): the picture, the name line (name left, `info`, the
     * codec, right), the seek row (time, bar, time), then Pause and the volume buttons 80 high */
    bool square = false;
    Rect info;
    int info_px = 11;
    Rect panel, picture;
    Rect pause, minus, plus, volume;
    Rect bar, bar_hit;        /* the seek bar's track and its touch box */
    int knob = 14;
    Rect time_left, time_right, name, toast;
    int pause_px = 15, pm_px = 18, volume_px = 12, time_px = 11, name_px = 12, toast_px = 12;
    int radius = 8;
};

PlaybackLayout playback_layout(const CaptureLayoutIn &in, int row_token);

/* A position on the seek bar (screen x) as a time: clamped to the bar, 0..total_ms. */
std::int64_t seek_ms_at(const PlaybackLayout &l, int x, std::int64_t total_ms);
/* The knob's centre x for a time. */
int seek_x_of(const PlaybackLayout &l, std::int64_t ms, std::int64_t total_ms);

} // namespace vhs
