/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Capture page's geometry, pure (unit-tested), from the layout service's tokens only (brief 12.2).
 *
 * The canvas (canvas-vhs-p1/gen_vhs.py: Capture-480x320, Capture-320x480, NoCamera-480x320, CameraTurned-480x320,
 * Notices-480x320) draws 480x320 and 320x480 with a 44 px touch target. Its sizes are kept in canvas units
 * u = TARGET / 44, so at a 44 px TARGET they are the canvas's pixels and on a denser panel they grow with the touch
 * target. The other classes are derived by the spike's rule:
 *   - landscape and square: the picture box is the largest 16:9 box whose width is a multiple of 16 (352x198 at 480x320,
 *     the canvas), with a column 2 x TARGET wide at the panel's right edge: the shutter (72u, the column minus 16u),
 *     then Photo | Video, Scenes and Settings; the mode word ("Photo") and the note under the picture;
 *   - portrait: the picture across the panel (288x162 at 320x480), then a row of Photo | Video, the shutter and Scenes
 *     (the canvas's 88 x 56 side buttons and 84 px shutter), Settings across, the mode word and the note.
 * Buttons are at least TARGET high (the canvas draws 34 and 40 px buttons at 480x320, under its 44 px target; brief
 * 12.4 item 7, reported at G1). The NoCamera block (title, text, the replug hint, Scenes) is centred in the panel. The
 * toast takes the place of the mode word and the note (Cassette: the notice replaces the info strip).
 */
#pragma once

#include "live/convert.hpp"

namespace vhs {

struct CaptureLayoutIn {
    int w = 0, h = 0;  /* the canvas */
    int body_y = 0;    /* under the header */
    int target = 44;   /* TARGET */
};

struct CaptureLayout {
    double u = 1;      /* canvas units: TARGET / 44 */
    bool column = true;/* landscape / square: the buttons in a column right of the picture */
    Rect panel, picture;
    Rect shutter, toggle, scenes, settings;
    Rect title, note, toast;
    int chip_x = 0, chip_y = 0;              /* in the picture */
    int chip_pad_h = 0, chip_pad_v = 0, chip_radius = 0;
    int title_px = 14, note_px = 11, chip_px = 11, button_px = 13, toggle_px = 11, sub_px = 10;
    int toast_px = 12, toast_h = 30;
    int overlay_px = 14;                     /* the Turned / Lost overlay text */
    int panel_radius = 10, button_radius = 8, ring = 4;
    /* NoCamera: without the hint line (the canvas), and with it (nch_*, nc_hint) */
    Rect nc_title, nc_text, nc_scenes;
    Rect nch_title, nch_text, nc_hint, nch_scenes;
    int nc_title_px = 18, nc_text_px = 13, nc_hint_px = 11, nc_scenes_px = 14;
};

int floor16(int v);
CaptureLayout capture_layout(const CaptureLayoutIn &in);

} // namespace vhs
