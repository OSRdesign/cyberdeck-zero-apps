/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: fonts, plain objects and the drafts' vector icons. LVGL thread only.
 *
 * Fonts: the drafts use Montserrat (500 / 600 / 700) and JetBrains Mono 500, the launcher's own faces, read at run time
 * from /usr/share/APPLaunch/share/font through FreeType (none is shipped). Montserrat Medium stands for 500, Montserrat
 * Bold for 600 and 700. Every Montserrat size gets the fallback chain the other apps use (Markdown Notes): DejaVu Sans
 * (accents, symbols), then Noto Sans CJK (Chinese, Japanese, Korean titles), both the launcher's.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

namespace vinyl {
namespace ui {

enum class Face { Medium, Bold, Mono };

const lv_font_t *font(Face face, int px);
int text_width(const lv_font_t *f, const std::string &text, int letter_space = 0);
/* The largest px in [min_px, px] at which `text` fits max_w. */
int fit_px(Face face, int px, const std::string &text, int max_w, int min_px);
/* Frees the FreeType fonts (at exit). */
void release_fonts();

/* A plain object: no theme style, not clickable, not scrollable. */
void plain(lv_obj_t *o);
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, std::uint32_t bg, int radius, lv_opa_t opa = LV_OPA_COVER);
/* A label at x, y; w > 0 clips (dots) at that width. */
lv_obj_t *label(lv_obj_t *parent, Face face, int px, std::uint32_t color, int x, int y, int w,
                lv_text_align_t align = LV_TEXT_ALIGN_LEFT, int letter_space = 0);
void set_text(lv_obj_t *l, const std::string &t);
void show(lv_obj_t *o, bool shown);
void set_bg(lv_obj_t *o, std::uint32_t color);

/* The drafts' icons (SVG, viewBox 24), redrawn from lines, triangles and rectangles. */
enum class Icon {
    None, Bluetooth, Shuffle, Prev, Play, Pause, Next, Repeat, Queue, Output, Browse, Sleep, Equalizer, Info, Phone,
    Warning, Chevron, Up, Down, Minus, Plus, Trash, Muted, Menu,
};
/* Draws `ic` px big, centred on (cx, cy), in a draw event's layer. */
void draw_icon(lv_layer_t *layer, Icon ic, int cx, int cy, int px, std::uint32_t rgb, lv_opa_t opa = LV_OPA_COVER);
/* A small object that draws one icon centred in itself (not clickable). */
lv_obj_t *icon_obj(lv_obj_t *parent, Icon ic, int x, int y, int w, int h, int px, std::uint32_t rgb);
void icon_set(lv_obj_t *o, Icon ic, std::uint32_t rgb, lv_opa_t opa = LV_OPA_COVER);
/* Its size: the object px square and the icon drawn px big. */
void icon_resize(lv_obj_t *o, int px);

lv_area_t area(int x, int y, int w, int h);
void fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius, lv_opa_t opa = LV_OPA_COVER);

} // namespace ui
} // namespace vinyl
