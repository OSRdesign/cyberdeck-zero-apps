/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: fonts, boxes, labels and drawing helpers (see paint.hpp).
 */
#include "paint.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

namespace cassette {
namespace ui {

const lv_font_t *font(Face face, int px)
{
    px = std::clamp(px, 6, 160);
    static std::map<std::pair<int, int>, const lv_font_t *> cache;
    const auto key = std::make_pair(static_cast<int>(face), px);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const char *file = face == Face::Mono ? "JetBrainsMono-Regular.ttf"
                       : face == Face::Bold ? "Montserrat-Bold.ttf"
                                            : "Montserrat-Medium.ttf";
    const lv_font_t *f = cp0_fonts().get(file, static_cast<uint16_t>(px));
    if (!f) f = cp0_ui_font_px(px); /* no FreeType face: the layout service's builtin Montserrat */
    cache[key] = f;
    return f;
}

int text_width(const lv_font_t *f, const char *text, int letter_space)
{
    if (!f || !text || !*text) return 0;
    lv_point_t size;
    lv_text_get_size(&size, text, f, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_RECOLOR); /* "#RRGGBB text#" counts as its text only */
    return size.x;
}

int fit_px(Face face, int px, int ls, const std::string &text, int max_w, int min_px, int *ls_out)
{
    if (ls_out) *ls_out = ls;
    if (text.empty() || max_w <= 0) return px;
    if (text_width(font(face, px), text.c_str(), ls) <= max_w) return px;
    if (ls > 0 && text_width(font(face, px), text.c_str(), 0) <= max_w) {
        int l = ls;
        while (l > 0 && text_width(font(face, px), text.c_str(), l) > max_w) --l;
        if (ls_out) *ls_out = l;
        return px;
    }
    if (ls_out) *ls_out = 0;
    for (int p = px - 1; p >= min_px; --p)
        if (text_width(font(face, p), text.c_str(), 0) <= max_w) return p;
    return min_px;
}

void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, std::uint32_t bg, int radius, lv_opa_t bg_opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, bg_opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

void border(lv_obj_t *o, int width, std::uint32_t color)
{
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}

lv_obj_t *label(lv_obj_t *parent, Face face, int px, std::uint32_t color, int x, int y, int w, lv_text_align_t align,
                int letter_space)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(l, font(face, px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, letter_space, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    else lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(l, "");
    return l;
}

void set_text(lv_obj_t *l, const std::string &t)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || t != cur) lv_label_set_text(l, t.c_str());
}

namespace {
struct FitMemo {
    int w, px, ls, min_px, ls_out;
    const lv_font_t *font;
    std::string text;
};
std::unordered_map<const lv_obj_t *, FitMemo> &fit_memo()
{
    static std::unordered_map<const lv_obj_t *, FitMemo> m;
    return m;
}
} // namespace

void set_fitted(lv_obj_t *l, Face face, int px, int ls, int min_px, const std::string &t)
{
    int w = lv_obj_get_style_width(l, LV_PART_MAIN);
    if (w <= 0 || w == LV_SIZE_CONTENT) w = 1 << 20;
    /* the same text in the same box, and the label still shows what was fitted: nothing to measure */
    auto &memo = fit_memo();
    if (auto it = memo.find(l); it != memo.end()) {
        const FitMemo &m = it->second;
        const char *cur = lv_label_get_text(l);
        if (m.w == w && m.px == px && m.ls == ls && m.min_px == min_px && m.text == t && cur && t == cur &&
            lv_obj_get_style_text_font(l, LV_PART_MAIN) == m.font &&
            lv_obj_get_style_text_letter_space(l, LV_PART_MAIN) == m.ls_out)
            return;
    }
    int ls_out = ls;
    const int p = fit_px(face, px, ls, t, w, min_px, &ls_out);
    const lv_font_t *f = font(face, p);
    if (lv_obj_get_style_text_font(l, LV_PART_MAIN) != f) lv_obj_set_style_text_font(l, f, 0);
    if (lv_obj_get_style_text_letter_space(l, LV_PART_MAIN) != ls_out) lv_obj_set_style_text_letter_space(l, ls_out, 0);
    set_text(l, t);
    memo[l] = FitMemo{w, px, ls, min_px, ls_out, f, t};
}

void show(lv_obj_t *o, bool shown)
{
    if (!o) return;
    if (shown == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------------------------------------ drawing */

lv_area_t area(int x, int y, int w, int h)
{
    lv_area_t a;
    a.x1 = x;
    a.y1 = y;
    a.x2 = x + w - 1;
    a.y2 = y + h - 1;
    return a;
}

void fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void fill_vgrad(lv_layer_t *layer, const lv_area_t &a, std::uint32_t top, std::uint32_t bottom, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = opa;
    d.radius = radius;
    d.bg_color = lv_color_hex(top);
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(top);
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(bottom);
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    lv_draw_rect(layer, &d, &a);
}

void frame(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int width, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = lv_color_hex(rgb);
    d.border_width = width;
    d.border_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, std::uint32_t rgb, lv_opa_t opa, bool round,
          int dash, int gap)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = std::max(1, width);
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.round_start = round;
    d.round_end = round;
    d.dash_width = dash;
    d.dash_gap = gap;
    lv_draw_line(layer, &d);
}

void triangle(lv_layer_t *layer, int x1, int y1, int x2, int y2, int x3, int y3, std::uint32_t rgb, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = x1;
    d.p[0].y = y1;
    d.p[1].x = x2;
    d.p[1].y = y2;
    d.p[2].x = x3;
    d.p[2].y = y3;
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    lv_draw_triangle(layer, &d);
}

void disc(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb, lv_opa_t opa)
{
    fill(layer, area(cx - r, cy - r, 2 * r + 1, 2 * r + 1), rgb, LV_RADIUS_CIRCLE, opa);
}

void ring(lv_layer_t *layer, int cx, int cy, int r, int width, int start_deg, int end_deg, std::uint32_t rgb, bool rounded,
          lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = static_cast<uint16_t>(std::max(1, r));
    d.width = std::max(1, width);
    d.start_angle = start_deg;
    d.end_angle = end_deg;
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.rounded = rounded;
    lv_draw_arc(layer, &d);
}

void text(lv_layer_t *layer, const lv_font_t *f, const char *t, const lv_area_t &a, std::uint32_t rgb,
          lv_text_align_t align, int letter_space, lv_opa_t opa)
{
    if (!t || !*t) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;
    d.font = f;
    d.color = lv_color_hex(rgb);
    d.align = align;
    d.letter_space = letter_space;
    d.opa = opa;
    d.flag = LV_TEXT_FLAG_NONE;
    lv_draw_label(layer, &d, &a);
}

/* ------------------------------------------------------------------------------------------------ icons */

namespace {

struct Map {
    double cx, cy, kx, ky, vw, vh;
    int x(double v) const { return static_cast<int>(std::lround(cx + (v - vw / 2) * kx)); }
    int y(double v) const { return static_cast<int>(std::lround(cy + (v - vh / 2) * ky)); }
};

int stroke(double v, double s)
{
    return std::max(1, static_cast<int>(std::lround(v * s)));
}

} // namespace

void icon(lv_layer_t *layer, Icon ic, int cx, int cy, double s, std::uint32_t c, lv_opa_t opa)
{
    /* the piano keys' viewBox 22 x 16 drawn 22 x 15 */
    const Map k{static_cast<double>(cx), static_cast<double>(cy), s, s * 15.0 / 16.0, 22, 16};
    /* the header buttons' viewBox 24 drawn 22 */
    const Map g{static_cast<double>(cx), static_cast<double>(cy), s * 22.0 / 24.0, s * 22.0 / 24.0, 24, 24};
    switch (ic) {
    case Icon::None: break;
    case Icon::Rew:
        triangle(layer, k.x(11), k.y(2), k.x(2), k.y(8), k.x(11), k.y(14), c, opa);
        triangle(layer, k.x(20), k.y(2), k.x(11), k.y(8), k.x(20), k.y(14), c, opa);
        break;
    case Icon::Fwd:
        triangle(layer, k.x(2), k.y(2), k.x(11), k.y(8), k.x(2), k.y(14), c, opa);
        triangle(layer, k.x(11), k.y(2), k.x(20), k.y(8), k.x(11), k.y(14), c, opa);
        break;
    case Icon::Play: triangle(layer, k.x(6), k.y(1), k.x(18), k.y(8), k.x(6), k.y(15), c, opa); break;
    case Icon::Pause:
        fill(layer, area(k.x(5), k.y(1), k.x(9.5) - k.x(5), k.y(15) - k.y(1)), c, 0, opa);
        fill(layer, area(k.x(12.5), k.y(1), k.x(17) - k.x(12.5), k.y(15) - k.y(1)), c, 0, opa);
        break;
    case Icon::Stop: fill(layer, area(k.x(5), k.y(2), k.x(17) - k.x(5), k.y(14) - k.y(2)), c, 0, opa); break;
    case Icon::Rec: disc(layer, k.x(11), k.y(8), stroke(6.5, s), c, opa); break;
    case Icon::SetIn: {
        const int w = stroke(2.6, s);
        line(layer, k.x(9), k.y(2), k.x(3.5), k.y(2), w, c, opa);
        line(layer, k.x(3.5), k.y(2) - w / 2, k.x(3.5), k.y(14) + w / 2, w, c, opa);
        line(layer, k.x(3.5), k.y(14), k.x(9), k.y(14), w, c, opa);
        break;
    }
    case Icon::SetOut: {
        const int w = stroke(2.6, s);
        line(layer, k.x(13), k.y(2), k.x(18.5), k.y(2), w, c, opa);
        line(layer, k.x(18.5), k.y(2) - w / 2, k.x(18.5), k.y(14) + w / 2, w, c, opa);
        line(layer, k.x(18.5), k.y(14), k.x(13), k.y(14), w, c, opa);
        break;
    }
    case Icon::Remove: {
        const int w = stroke(2.8, s);
        line(layer, k.x(5), k.y(3), k.x(17), k.y(13), w, c, opa);
        line(layer, k.x(17), k.y(3), k.x(5), k.y(13), w, c, opa);
        break;
    }
    case Icon::Delete: {
        const int w = stroke(2.6, s);
        line(layer, k.x(6), k.y(3), k.x(16), k.y(13), w, c, opa);
        line(layer, k.x(16), k.y(3), k.x(6), k.y(13), w, c, opa);
        break;
    }
    case Icon::Check: {
        const int w = stroke(3, s);
        line(layer, k.x(3), k.y(8.5), k.x(8.5), k.y(13.5), w, c, opa, true);
        line(layer, k.x(8.5), k.y(13.5), k.x(19), k.y(2.5), w, c, opa, true);
        break;
    }
    case Icon::Load: {
        /* a cassette (outline, two reels) going into its slot: the slot is a bar under it */
        frame(layer, area(k.x(4), k.y(0.5), k.x(18) - k.x(4), k.y(9.5) - k.y(0.5)), c, std::max(1, stroke(1.6, s)), stroke(1.5, s), opa);
        disc(layer, k.x(8.5), k.y(5), std::max(1, stroke(1.5, s)), c, opa);
        disc(layer, k.x(13.5), k.y(5), std::max(1, stroke(1.5, s)), c, opa);
        fill(layer, area(k.x(2), k.y(12), k.x(20) - k.x(2), std::max(2, k.y(14.4) - k.y(12))), c, stroke(1, s), opa);
        break;
    }
    case Icon::Pencil:
        triangle(layer, k.x(3), k.y(13), k.x(3), k.y(10), k.x(6), k.y(13), c, opa);
        triangle(layer, k.x(3), k.y(10), k.x(12), k.y(1), k.x(6), k.y(13), c, opa);
        triangle(layer, k.x(12), k.y(1), k.x(15), k.y(4), k.x(6), k.y(13), c, opa);
        break;
    case Icon::Scissors: {
        const int w = stroke(2, s);
        line(layer, k.x(3), k.y(2), k.x(19), k.y(14), w, c, opa);
        line(layer, k.x(3), k.y(14), k.x(19), k.y(2), w, c, opa);
        const int r = stroke(2.2, s) + stroke(0.8, s);
        ring(layer, k.x(4), k.y(3), r, stroke(1.6, s), 0, 360, c, false, opa);
        ring(layer, k.x(4), k.y(13), r, stroke(1.6, s), 0, 360, c, false, opa);
        break;
    }
    case Icon::List:
        for (int i = 0; i < 3; ++i) {
            const double yy = 6 + 6 * i;
            disc(layer, g.x(4), g.y(yy), stroke(1.8 * 22.0 / 24.0, s), c, opa);
            fill(layer, area(g.x(9), g.y(yy - 1.2), g.x(22) - g.x(9), std::max(2, g.y(yy + 1.2) - g.y(yy - 1.2))), c, 0,
                 opa);
        }
        break;
    case Icon::Gear: {
        const int w = stroke(3 * 22.0 / 24.0, s);
        const int r = stroke(6 * 22.0 / 24.0, s);
        ring(layer, g.x(12), g.y(12), r + stroke(1.3 * 22.0 / 24.0, s), stroke(2.6 * 22.0 / 24.0, s), 0, 360, c, false, opa);
        disc(layer, g.x(12), g.y(12), stroke(2 * 22.0 / 24.0, s), c, opa);
        const double seg[8][4] = {{12, 1.5, 12, 4.5},   {12, 19.5, 12, 22.5}, {1.5, 12, 4.5, 12},   {19.5, 12, 22.5, 12},
                                  {4.6, 4.6, 6.7, 6.7}, {17.3, 17.3, 19.4, 19.4}, {19.4, 4.6, 17.3, 6.7}, {6.7, 17.3, 4.6, 19.4}};
        for (const auto &l : seg) line(layer, g.x(l[0]), g.y(l[1]), g.x(l[2]), g.y(l[3]), w, c, opa);
        break;
    }
    case Icon::Minus: {
        const int hw = stroke(5.5, s), t = stroke(2.6, s);
        fill(layer, area(cx - hw, cy - t / 2, 2 * hw + 1, t), c, 0, opa);
        break;
    }
    case Icon::Plus: {
        const int hw = stroke(6, s), t = stroke(2.6, s);
        fill(layer, area(cx - hw, cy - t / 2, 2 * hw + 1, t), c, 0, opa);
        fill(layer, area(cx - t / 2, cy - hw, t, 2 * hw + 1), c, 0, opa);
        break;
    }
    case Icon::Left: { /* the board's 22 px "◀": about 14 x 16 */
        const int hw = stroke(7, s), hh = stroke(8, s);
        triangle(layer, cx + hw, cy - hh, cx - hw, cy, cx + hw, cy + hh, c, opa);
        break;
    }
    case Icon::Right: {
        const int hw = stroke(7, s), hh = stroke(8, s);
        triangle(layer, cx - hw, cy - hh, cx + hw, cy, cx - hw, cy + hh, c, opa);
        break;
    }
    case Icon::Chevron: {
        const Map v{static_cast<double>(cx), static_cast<double>(cy), s, s, 10, 16};
        const int w = stroke(2.4, s);
        line(layer, v.x(2), v.y(2), v.x(8), v.y(8), w, c, opa, true);
        line(layer, v.x(8), v.y(8), v.x(2), v.y(14), w, c, opa, true);
        break;
    }
    }
}

} // namespace ui
} // namespace cassette
