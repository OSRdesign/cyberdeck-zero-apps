/*
 * SPDX-License-Identifier: MIT
 *
 * STAND-IN, deleted when task L1 lands: the declaration of cp0::ui::StatusStrip as L1 is expected to publish it in
 * cp0_lvgl (report 024 section 1, glue piece 3; guessed from the launcher's native_ui add_status_icons(),
 * status_strip_left() and add_title(), which L1 moves). Only _common/status_strip.cpp includes it.
 */
#pragma once

#include "lvgl/lvgl.h"

namespace cp0 {
namespace ui {

class StatusStrip {
public:
    /* The home grid's clock / Wi-Fi / Bluetooth strip at the top right of `parent` (a page header): a transparent
     * canvas drawn by the shared renderer (cp0_statusbar.h), refreshed by its own timer, freed with `parent`. */
    static void attach(lv_obj_t *parent);
    /* The screen x of the leftmost pixel the strip draws in its fullest state (the Bluetooth glyph). */
    static int left();
    /* The title label at the home grid's place, size and colour. */
    static lv_obj_t *add_title(lv_obj_t *parent, const char *text);
};

} // namespace ui
} // namespace cp0
