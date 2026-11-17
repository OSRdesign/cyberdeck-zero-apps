/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI (P2d): the Equalizer page, drawn as the full-UX canvas draws it (EQ-480x320, EQ-320x480) in the Menu pages'
 * dark panel (core/layout eq_geom: every position, the boards' numbers at the class's scale):
 *
 *   the band line   "Band 4 kHz:" and the selected band's gain on a cream chip in teal ("+3 dB")
 *   the preset      "PRESET: BASS BOOST" (grey capitals; at the band line's right, under it in portrait)
 *   ten bars        a dark track, the middle line at 0 dB, the gain in teal from it (up to the track's top at +12 dB, down
 *                   to its bottom at -12 dB), the label under it; the selected band outlined in cream, its label cream
 *   the buttons     "-" and "+" (the selected band 1 dB down / up), Preset (its pick list), Reset (Flat), On (teal; "Off"
 *                   on the button colour when the EQ is off)
 *
 * Touch (owner, 2026-11-13: "equalizer should also work by dragging the bars"): a press on a band's column selects it; a
 * drag up or down past the toolkit's tap slop sets its gain where the finger is (1 dB steps, live), a tap only selects.
 * The keys are the controller's (D30: Left / Right a band, Up / Down its gain). Draws only: every action goes to the
 * callbacks. LVGL thread only.
 */
#pragma once

#include "layout/layout.hpp"
#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>

namespace vinyl {
namespace ui {

class EqPage {
public:
    /* make_root: a page root of the View (full screen under the header, hidden at first); target / row: the toolkit's
     * TARGET and ROW (the buttons' touch height); on_band(band, phase, db), on_button(i): as Actions::eq_band / eq_button */
    EqPage(std::function<lv_obj_t *()> make_root, int screen_w, int screen_h, int body_y, int target, int row,
           std::function<void(int, int, int)> on_band, std::function<void(int)> on_button);
    ~EqPage();
    EqPage(const EqPage &) = delete;
    EqPage &operator=(const EqPage &) = delete;

    lv_obj_t *root() const;
    void update(const EqView &v);
    /* where a notice may sit: over the buttons at the panel's bottom (screen px) */
    layout::R notice_room() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace ui
}  // namespace vinyl
