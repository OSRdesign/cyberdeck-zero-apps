/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the palette and the small LVGL helpers the pages share (LVGL thread only). Colours are the launcher's
 * (docs/APP-GUIDELINES.md): green, amber and red mean a status and never stand alone (every state also has a word or a
 * shape). Sizes come from the layout service: nothing here is a pixel constant of a screen.
 */
#pragma once

#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

namespace nt {
namespace ui {

constexpr std::uint32_t kText = 0xE0E0E0;   /* secondary text */
constexpr std::uint32_t kLabel = 0x9A9A9A;  /* labels, "not yet" */
constexpr std::uint32_t kOk = 0x66CC88;     /* status: OK */
constexpr std::uint32_t kWarn = 0xF0B400;   /* status: amber */
constexpr std::uint32_t kBad = 0xFF6666;    /* status: red */
constexpr std::uint32_t kBlue = 0x2D9CDB;   /* the jitter band, the selection */
constexpr std::uint32_t kTrack = 0x2B2B2B;  /* tracks, grid lines */
constexpr std::uint32_t kBorder = 0x3A3A3A; /* panel borders */
constexpr std::uint32_t kInk = 0x14181C;    /* a mark inside a coloured disc */
constexpr std::uint32_t kBlack = 0x000000;

/* the colour of a status tone; normal = the colour to use when the tone says nothing */
std::uint32_t tone_rgb(Tone tone, std::uint32_t normal = kText);

/* a plain, not clickable, not scrollable object */
lv_obj_t *plain(lv_obj_t *parent);
/* a label with this font and colour, empty */
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb);
/* the text only when it changed */
void set_text_if(lv_obj_t *label, const std::string &text);
void set_color_if(lv_obj_t *label, std::uint32_t rgb);

/* the launcher's Montserrat (Medium / Bold) at a size, else the builtin one */
const lv_font_t *font_medium(int px);
const lv_font_t *font_bold(int px);
/* the CAPTION text size of the layout service */
const lv_font_t *caption_font();
int caption_px();
int text_width(const char *text, const lv_font_t *font);
int line_height(const lv_font_t *font);
/* the height of the text wrapped to width px */
int wrapped_text_height(const std::string &text, const lv_font_t *font, int width);

/* PDA mode (0.5.0, X-Portrait): the canvas is taller than wide. The blocks that adapt to a tall narrow screen ask this; the
 * landscape layouts never look at it, so every landscape size draws as before. */
bool portrait();

/* the number of LVGL objects below `root` (the page leak check) and of LVGL timers */
int count_objects(lv_obj_t *root);
int count_timers();

} // namespace ui
} // namespace nt
