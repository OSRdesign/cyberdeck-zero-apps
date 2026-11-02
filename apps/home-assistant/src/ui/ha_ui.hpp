/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app: small drawing helpers shared by the pages (ha_app.cpp) and the stored dashboard's cards
 * (ha_dash.cpp). LVGL thread only.
 */
#pragma once

#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

namespace haui {

constexpr int kMuted = 0x9A9A9A;
constexpr std::uint32_t kHighlight = CP0_UI_LIST_BAR_RGB; /* the launcher's highlight: a focused tile's ground */
constexpr std::uint32_t kAccent = 0x2D9CDB;  /* the launcher's selection blue: the focus outline, the slider's fill */
constexpr std::uint32_t kTrack = 0x3A3A3A;   /* the toolkit's slider track */
constexpr std::uint32_t kKnob = 0xFFFFFF;
constexpr int kSwipeMmX10 = 60;              /* a sideways swipe this long switches views (a convenience) */

inline lv_obj_t *plain_label(lv_obj_t *parent, const lv_font_t *font, int rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(static_cast<uint32_t>(rgb)), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* A plain box: no theme style, not clickable, not scrollable. */
inline lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* A one-line label cut with dots. */
inline lv_obj_t *line_label(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (w > 0) {
        /* one line high: the dots cut what does not fit (an auto height would wrap instead) */
        lv_obj_set_size(l, w, font ? lv_font_get_line_height(font) : LV_SIZE_CONTENT);
    }
    return l;
}

/* The keyboard focus: the launcher's selection blue as an outline inside the object (no layout change), the home
 * grid's selected-tile width. */
inline void set_focus_ring(lv_obj_t *o, bool on)
{
    const int w = cp0_ui_metrics_get()->shell.tile_border_sel;
    lv_obj_set_style_outline_color(o, lv_color_hex(kAccent), 0);
    lv_obj_set_style_outline_width(o, on ? w : 0, 0);
    lv_obj_set_style_outline_pad(o, on ? -w : 0, 0);
    lv_obj_set_style_outline_opa(o, LV_OPA_COVER, 0);
}

inline int line_h(const lv_font_t *f)
{
    return f ? lv_font_get_line_height(f) : 0;
}

inline int text_w(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t p{};
    lv_text_get_size(&p, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(p.x);
}

/* The first character of a name (the glyph's stand-in when the icon font is missing). */
inline std::string initial(const std::string &name)
{
    if (name.empty()) return "?";
    std::size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    std::string s = name.substr(0, n);
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

} // namespace haui
