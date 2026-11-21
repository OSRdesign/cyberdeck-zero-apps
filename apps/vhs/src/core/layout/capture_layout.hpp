/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Capture page's geometry, pure (unit-tested), from the layout service's tokens only (brief 12.2).
 *
 * The canvas (canvas-vhs-p1/gen_vhs.py: Capture-480x320, Capture-320x480, NoCamera-480x320, CameraTurned-480x320,
 * Notices-480x320) draws 480x320 and 320x480 with a 44 px touch target. Its sizes are kept in canvas units
 * u = TARGET / 44, so at a 44 px TARGET they are the canvas's pixels and on a denser panel they grow with the touch
 * target. The other classes are derived by the spike's rule:
 *   - landscape: the picture box is the largest 16:9 box whose width is a multiple of 16 (352x198 at 480x320, the
 *     canvas), with a column 2 x TARGET wide at the panel's right edge: the shutter (72u, the column minus 16u), then
 *     Photo | Video, Scenes and Settings; the mode word ("Photo") and the note under the picture;
 *   - square (the layout service's Square class; G1 review, 2026-11-15): the picture across the panel and one row of
 *     TARGET-high buttons under it (Photo | Video, the shutter, Scenes, Settings), then the mode word and the note;
 *   - portrait: the picture across the panel (288x162 at 320x480), then a row of Photo | Video, the shutter and Scenes
 *     (the canvas's 88 x 56 side buttons and 84 px shutter), Settings across, the mode word and the note.
 * The Square class takes its own scale (owner's review of 2026-11-17, canvas vhs-square-layout, drawn at 720x720: the
 * 480x320 units are 11 to 14 px there, 1.6 to 2.1 mm at 6.7 px/mm, and the lower 40 percent of the panel was empty):
 * k = min(w, h) / 720 and every size is the canvas's 720x720 px times k (the type 20 px minimum, 24 body, 26 buttons,
 * 28 to 34 names and the REC time, below 720 proportional and never under 12 px; targets 56 px, at least TARGET), and
 * u = 2k so the pages that still scale by u (the dialogs, the toast, the review) take the same type. The picture box
 * is the panel's width (688x440 at 720), one control row under it (Photo / Video stacked, the 120 px shutter, Scenes
 * / Settings stacked), then a 40 px status line (the camera and its format left, the key hints right).
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
    bool square = false; /* the layout service's Square class (cp0_ui_classify: 0.8 <= w/h <= 1.25, h >= 400) */
};

struct CaptureLayout {
    double u = 1;      /* canvas units: TARGET / 44 */
    bool column = true;/* landscape (not square): the buttons in a column right of the picture */
    bool square = false; /* Square class: the picture across the panel, one row of buttons under it (G1 review) */
    Rect panel, picture;
    Rect shutter, toggle, scenes, settings;
    Rect title, note, toast;
    /* Square class only: Photo / Video as two stacked buttons (toggle is then the 270x120 "Photo now" while recording),
     * the status line under the control row (title and note are its two halves' places), the type unit k */
    double k = 1;
    Rect photo_btn, video_btn, status;
    int mode_px = 13, status_px = 12;
    int rec_tag_h = 22, rec_tag_pad_l = 8, rec_tag_pad_r = 8, rec_tag_gap = 6, rec_tag_dot = 10, rec_tag_px = 12;
    int lamp_inset = -1;                     /* >= 0: the Sound-on lamp at the picture's top right, this far in */
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
    /* the self-timer's countdown ring, centred on the picture (canvas Countdown-480x320); Cancel takes Scenes' place */
    int cd_d = 96, cd_border = 6, cd_px = 52;
    /* recording (canvas Recording-480x320): the REC tag in the chip's place and the Sound-on tag at the picture's bottom
     * left (22u high, 8u padding, an 11u radius, a 10u dot, 6u to the text, 12u bold) */
    int tag_h = 22, tag_pad = 8, tag_radius = 11, tag_dot = 10, tag_gap = 6, tag_px = 12;
    int photo_now_px = 12;                   /* "Photo now" in the toggle's place (12u, the toggle's own is 11u) */
    int rec_face = 24, rec_face_radius = 6;  /* the red shutter's rounded square: 0.34 of the shutter, a 6u radius */
    int lamp_y = 170;                        /* in the picture: its height - 28u */
    /* 0.1.2: the camera chip (a tappable button over the picture's top right, shown with two or more cameras while the live
     * view runs; hidden while recording or counting down): the largest area it may take (the button is as wide as its text
     * and sits at this rectangle's right edge), TARGET high at least, and its type size */
    Rect cam_chip;
    int cam_chip_px = 13;
};

/* The photo review page (canvas PhotoReview-480x320): the capture page's panel, picture and text places; Previous,
 * Next, Send to Notes (optional, M6: its slot kept, not drawn in P1 yet) and Delete. */
struct ReviewLayout {
    double u = 1;
    bool column = true;
    bool square = false;
    Rect panel, picture, prev, next, notes, del, title, note;
    int chip_x = 0, chip_y = 0;
    int chip_pad_h = 0, chip_pad_v = 0, chip_radius = 0;
    int title_px = 14, note_px = 11, chip_px = 11, button_px = 12, notes_px = 11;
};

int floor16(int v);
/* The Square class's scale: min(w, h) / 720, kept between 0.4 and 2.5. */
double square_scale(int w, int h);
CaptureLayout capture_layout(const CaptureLayoutIn &in);
ReviewLayout review_layout(const CaptureLayoutIn &in);

} // namespace vhs
