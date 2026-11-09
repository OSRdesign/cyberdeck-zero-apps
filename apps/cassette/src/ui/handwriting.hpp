/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the handwriting face of the cassette label, Caveat SemiBold (SIL OFL 1.1, ui/fonts/OFL-Caveat.txt) as LVGL
 * bitmap fonts compiled into the app (4 bpp, ASCII and Latin-1; one font per rung of kHandRungs, load_anim.hpp). A
 * character the font does not have falls back to the Bold face of the same size (the font the label used before), so a
 * name in another script still draws. LVGL thread only.
 */
#pragma once

#include "lvgl/lvgl.h"

namespace cassette {
namespace ui {

/* The font of a rung (15, 22, 29 or 31: kHandRungs; any other value gives the nearest rung at most that size) with the
 * Bold fallback set. Stable for the life of the program. */
const lv_font_t *handwriting_font(int rung);

} // namespace ui
} // namespace cassette
