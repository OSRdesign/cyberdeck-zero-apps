/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: fonts, plain boxes and labels, and the drawing helpers the custom-drawn widgets share (keys, icons,
 * meter, counter, waveform). LVGL thread only.
 *
 * Fonts: the boards use web fonts the toolkit does not ship. The toolkit's own faces stand in (FreeType, from the
 * launcher's share/font, any px; the builtin Montserrat of the layout service when a file is missing):
 *   Face::Medium  Montserrat Medium   labels, values in words   (boards: Barlow Condensed 500)
 *   Face::Bold    Montserrat Bold     key captions, headings, the handwritten take name (boards: Barlow Condensed 600
 *                                     and Caveat: the toolkit has no condensed and no handwriting face)
 *   Face::Mono    JetBrains Mono      counter digits, dB and time values (boards: Share Tech Mono)
 * Montserrat is wider than Barlow Condensed: fit_px() picks the largest size up to the drawn one that fits the drawn
 * box (letter spacing goes first), so no text is cut.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

namespace cassette {
namespace ui {

enum class Face { Medium, Bold, Mono };

const lv_font_t *font(Face face, int px);
int text_width(const lv_font_t *f, const char *text, int letter_space);

/* The largest px in [min_px, px] at which text (letter spacing ls, scaled down with the size and dropped to 0 before
 * the size shrinks) fits max_w; *ls_out gets the spacing to use. min_px when nothing fits (the label then ends with
 * dots). */
int fit_px(Face face, int px, int ls, const std::string &text, int max_w, int min_px, int *ls_out);

/* A plain box: no theme style, not clickable, not scrollable. bg_opa 0 for a transparent one. */
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, std::uint32_t bg, int radius, lv_opa_t bg_opa = LV_OPA_COVER);
void border(lv_obj_t *o, int width, std::uint32_t color);
/* A one-line label (clipped, never wrapping) at x, y, w wide (w <= 0: as wide as its text). */
lv_obj_t *label(lv_obj_t *parent, Face face, int px, std::uint32_t color, int x, int y, int w,
                lv_text_align_t align = LV_TEXT_ALIGN_LEFT, int letter_space = 0);
/* Sets the text only when it differs (no needless redraw). */
void set_text(lv_obj_t *label, const std::string &text);
/* Text that must fit the label's width: the face's largest size up to px that fits (see fit_px), then the text. */
void set_fitted(lv_obj_t *label, Face face, int px, int ls, int min_px, const std::string &text);
void show(lv_obj_t *o, bool shown);
/* Strips a plain object to nothing (no style, not clickable, not scrollable). */
void plain(lv_obj_t *o);

/* ---------------------------------------------------------------- drawing inside a draw event (layer coordinates) */

void fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius, lv_opa_t opa = LV_OPA_COVER);
void fill_vgrad(lv_layer_t *layer, const lv_area_t &a, std::uint32_t top, std::uint32_t bottom, int radius,
                lv_opa_t opa = LV_OPA_COVER);
void frame(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int width, int radius, lv_opa_t opa = LV_OPA_COVER);
void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, std::uint32_t rgb, lv_opa_t opa = LV_OPA_COVER,
          bool round = false, int dash = 0, int gap = 0);
void triangle(lv_layer_t *layer, int x1, int y1, int x2, int y2, int x3, int y3, std::uint32_t rgb,
              lv_opa_t opa = LV_OPA_COVER);
void disc(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb, lv_opa_t opa = LV_OPA_COVER);
void ring(lv_layer_t *layer, int cx, int cy, int r, int width, int start_deg, int end_deg, std::uint32_t rgb,
          bool rounded, lv_opa_t opa = LV_OPA_COVER);
void text(lv_layer_t *layer, const lv_font_t *f, const char *t, const lv_area_t &a, std::uint32_t rgb,
          lv_text_align_t align = LV_TEXT_ALIGN_LEFT, int letter_space = 0, lv_opa_t opa = LV_OPA_COVER);
lv_area_t area(int x, int y, int w, int h);

/* The boards' small vector icons (SVG paths redrawn from primitives), fitted into a box. */
enum class Icon {
    None, Rew, Fwd, Play, Pause, Stop, Rec,  /* the five piano keys (viewBox 22 x 16, drawn 22 x 15) */
    SetIn, SetOut, Remove, Check,            /* trim keys */
    Pencil, Scissors, Delete, Load,          /* list keys (Load: a cassette above its slot) */
    Merge,                                   /* list key MERGE: two arrows joining into one */
    List, Gear,                              /* the Recordings and Settings buttons (viewBox 24, drawn 22) */
    Minus, Plus, Left, Right,                /* stepper buttons (the boards' 24 px "−" "+", 22 px "◀" "▶") */
    Chevron,                                 /* row chevron (viewBox 10 x 16) */
};
/* Draws icon centred on (cx, cy) at the board size times s. */
void icon(lv_layer_t *layer, Icon ic, int cx, int cy, double s, std::uint32_t rgb, lv_opa_t opa = LV_OPA_COVER);

} // namespace ui
} // namespace cassette
