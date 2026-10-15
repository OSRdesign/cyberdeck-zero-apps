/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the shared LVGL helpers (see style.hpp).
 */
#include "style.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

namespace nt {
namespace ui {

std::uint32_t tone_rgb(Tone tone, std::uint32_t normal)
{
    switch (tone) {
    case Tone::Ok: return kOk;
    case Tone::Warn: return kWarn;
    case Tone::Bad: return kBad;
    default: return normal;
    }
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, "");
    return l;
}

void set_text_if(lv_obj_t *label, const std::string &text)
{
    if (!label) return;
    const char *now = lv_label_get_text(label);
    if (!now || text != now) lv_label_set_text(label, text.c_str());
}

void set_color_if(lv_obj_t *label, std::uint32_t rgb)
{
    if (!label) return;
    const lv_color_t c = lv_color_hex(rgb);
    const lv_color_t now = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    if (now.red != c.red || now.green != c.green || now.blue != c.blue) lv_obj_set_style_text_color(label, c, 0);
}

static const lv_font_t *face(const char *name, int px)
{
    if (const lv_font_t *f = cp0_fonts().get(name, static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

const lv_font_t *font_medium(int px) { return face("Montserrat-Medium.ttf", px); }
const lv_font_t *font_bold(int px) { return face("Montserrat-Bold.ttf", px); }

int caption_px()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    return m ? m->text_px[CP0_TEXT_CAPTION] : 18;
}

const lv_font_t *caption_font() { return font_medium(caption_px()); }

int text_width(const char *text, const lv_font_t *font)
{
    if (!text || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

int line_height(const lv_font_t *font) { return font ? static_cast<int>(lv_font_get_line_height(font)) : 0; }

int count_objects(lv_obj_t *root)
{
    if (!root) return 0;
    int n = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) n += count_objects(lv_obj_get_child(root, static_cast<int32_t>(i)));
    return n;
}

int count_timers()
{
    int n = 0;
    for (lv_timer_t *t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) ++n;
    return n;
}

} // namespace ui
} // namespace nt
