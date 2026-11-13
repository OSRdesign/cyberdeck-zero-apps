/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: fonts, plain objects, icons (see gfx.hpp).
 */
#include "gfx.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vinyl {
namespace ui {

namespace {

std::map<std::pair<int, int>, const lv_font_t *> &font_cache() {
    static std::map<std::pair<int, int>, const lv_font_t *> m;
    return m;
}
std::vector<lv_font_t *> &owned() {
    static std::vector<lv_font_t *> v;
    return v;
}
std::map<int, lv_font_t *> &chains() {
    static std::map<int, lv_font_t *> m;
    return m;
}

std::string font_file(const char *name) {
    const std::string rel = std::string("share/font/") + name;
    if (const char *own = cp0_file_path_c(rel.c_str()); own && own[0] && ::access(own, R_OK) == 0) return own;
    const std::string installed = std::string("/usr/share/APPLaunch/") + rel;
    if (::access(installed.c_str(), R_OK) == 0) return installed;
    return "";
}

lv_font_t *create(const char *name, int px) {
    const std::string path = font_file(name);
    if (path.empty()) return nullptr;
    lv_font_t *f = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, static_cast<uint32_t>(px),
                                           LV_FREETYPE_FONT_STYLE_NORMAL);
    if (f) owned().push_back(f);
    return f;
}

lv_font_t *chain(int px) {
    auto &c = chains();
    if (auto it = c.find(px); it != c.end()) return it->second;
    lv_font_t *dejavu = create("DejaVuSans.ttf", px);
    lv_font_t *cjk = create("NotoSansCJK-Regular.ttc", px);
    if (dejavu) dejavu->fallback = cjk;
    lv_font_t *head = dejavu ? dejavu : cjk;
    c[px] = head;
    return head;
}

}  // namespace

const lv_font_t *font(Face face, int px) {
    px = std::clamp(px, 6, 160);
    auto &cache = font_cache();
    const auto key = std::make_pair(static_cast<int>(face), px);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const char *file = face == Face::Mono ? "JetBrainsMono-Regular.ttf"
                       : face == Face::Bold ? "Montserrat-Bold.ttf"
                                            : "Montserrat-Medium.ttf";
    const lv_font_t *result = nullptr;
    if (lv_font_t *f = create(file, px)) {
        if (face != Face::Mono) f->fallback = chain(px);
        result = f;
    } else {
        result = cp0_ui_font_px(px);
    }
    cache[key] = result;
    return result;
}

void release_fonts() {
    for (lv_font_t *f : owned()) lv_freetype_font_delete(f);
    owned().clear();
    font_cache().clear();
    chains().clear();
}

int text_width(const lv_font_t *f, const std::string &text, int letter_space) {
    if (!f || text.empty()) return 0;
    lv_point_t size;
    lv_text_get_size(&size, text.c_str(), f, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

int fit_px(Face face, int px, const std::string &text, int max_w, int min_px) {
    if (text.empty() || max_w <= 0) return px;
    for (int p = px; p > min_px; --p)
        if (text_width(font(face, p), text) <= max_w) return p;
    return min_px;
}

void plain(lv_obj_t *o) {
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, std::uint32_t bg, int radius, lv_opa_t opa) {
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, std::max(1, w), std::max(1, h));
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

void set_bg(lv_obj_t *o, std::uint32_t color) {
    if (!o) return;
    const lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(o, c, 0);
}

lv_obj_t *label(lv_obj_t *parent, Face face, int px, std::uint32_t color, int x, int y, int w, lv_text_align_t align,
                int letter_space) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(l, font(face, px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, letter_space, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_obj_set_height(l, lv_font_get_line_height(font(face, px))); /* one line: dots, never a wrap */
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    } else {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    }
    lv_label_set_text(l, "");
    return l;
}

void set_text(lv_obj_t *l, const std::string &t) {
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (!cur || t != cur) lv_label_set_text(l, t.c_str());
}

void show(lv_obj_t *o, bool shown) {
    if (!o) return;
    if (shown == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_area_t area(int x, int y, int w, int h) {
    lv_area_t a;
    a.x1 = x;
    a.y1 = y;
    a.x2 = x + w - 1;
    a.y2 = y + h - 1;
    return a;
}

void fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius, lv_opa_t opa) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

/* ------------------------------------------------------------------------------------------------ icons */

namespace {

struct Pen {
    lv_layer_t *layer;
    double ox, oy, k; /* screen = o + k * (svg - 0) */
    std::uint32_t rgb;
    lv_opa_t opa;
    double width;

    double X(double x) const { return ox + k * x; }
    double Y(double y) const { return oy + k * y; }
    void seg(double x1, double y1, double x2, double y2) const {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1.x = static_cast<lv_value_precise_t>(X(x1));
        d.p1.y = static_cast<lv_value_precise_t>(Y(y1));
        d.p2.x = static_cast<lv_value_precise_t>(X(x2));
        d.p2.y = static_cast<lv_value_precise_t>(Y(y2));
        d.width = std::max(1, static_cast<int>(std::lround(width * k)));
        d.color = lv_color_hex(rgb);
        d.opa = opa;
        d.round_start = 1;
        d.round_end = 1;
        lv_draw_line(layer, &d);
    }
    void poly(std::initializer_list<std::pair<double, double>> pts, bool close = false) const {
        const std::vector<std::pair<double, double>> v(pts);
        for (size_t i = 0; i + 1 < v.size(); ++i) seg(v[i].first, v[i].second, v[i + 1].first, v[i + 1].second);
        if (close && v.size() > 2) seg(v.back().first, v.back().second, v.front().first, v.front().second);
    }
    void bezier(double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) const {
        const int n = 10;
        double px = x0, py = y0;
        for (int i = 1; i <= n; ++i) {
            const double t = static_cast<double>(i) / n, u = 1 - t;
            const double x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
            const double y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
            seg(px, py, x, y);
            px = x;
            py = y;
        }
    }
    void arc(double cx, double cy, double r, double a0, double a1) const { /* degrees, clockwise on screen */
        const int n = std::max(4, static_cast<int>(std::fabs(a1 - a0) / 12));
        double px = cx + r * std::cos(a0 * M_PI / 180), py = cy + r * std::sin(a0 * M_PI / 180);
        for (int i = 1; i <= n; ++i) {
            const double a = (a0 + (a1 - a0) * i / n) * M_PI / 180;
            const double x = cx + r * std::cos(a), y = cy + r * std::sin(a);
            seg(px, py, x, y);
            px = x;
            py = y;
        }
    }
    void rect(double x, double y, double w, double h) const {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(rgb);
        d.bg_opa = opa;
        lv_area_t a;
        a.x1 = static_cast<int32_t>(std::lround(X(x)));
        a.y1 = static_cast<int32_t>(std::lround(Y(y)));
        a.x2 = static_cast<int32_t>(std::lround(X(x + w))) - 1;
        a.y2 = static_cast<int32_t>(std::lround(Y(y + h))) - 1;
        lv_draw_rect(layer, &d, &a);
    }
    void tri(double x1, double y1, double x2, double y2, double x3, double y3) const {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        d.p[0].x = static_cast<lv_value_precise_t>(X(x1));
        d.p[0].y = static_cast<lv_value_precise_t>(Y(y1));
        d.p[1].x = static_cast<lv_value_precise_t>(X(x2));
        d.p[1].y = static_cast<lv_value_precise_t>(Y(y2));
        d.p[2].x = static_cast<lv_value_precise_t>(X(x3));
        d.p[2].y = static_cast<lv_value_precise_t>(Y(y3));
        d.color = lv_color_hex(rgb);
        d.opa = opa;
        lv_draw_triangle(layer, &d);
    }
};

struct IconData {
    Icon ic;
    std::uint32_t rgb;
    lv_opa_t opa;
    int px;
};

void icon_draw_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    auto *d = static_cast<IconData *>(lv_obj_get_user_data(o));
    if (!d || d->ic == Icon::None) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    draw_icon(lv_event_get_layer(e), d->ic, (a.x1 + a.x2 + 1) / 2, (a.y1 + a.y2 + 1) / 2, d->px, d->rgb, d->opa);
}

void icon_delete_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    delete static_cast<IconData *>(lv_obj_get_user_data(o));
    lv_obj_set_user_data(o, nullptr);
}

}  // namespace

void draw_icon(lv_layer_t *layer, Icon ic, int cx, int cy, int px, std::uint32_t rgb, lv_opa_t opa) {
    const double k = px / 24.0;
    Pen p{layer, cx - 12 * k, cy - 12 * k, k, rgb, opa, 2.0};
    switch (ic) {
    case Icon::Bluetooth:
        p.width = 2.4;
        p.poly({{7, 7}, {17, 17}, {12, 22}, {12, 2}, {17, 7}, {7, 17}});
        break;
    case Icon::Shuffle:
        p.seg(3, 7, 6, 7);
        p.bezier(6, 7, 10, 7, 11, 17, 15, 17);
        p.seg(15, 17, 20, 17);
        p.seg(3, 17, 6, 17);
        p.bezier(6, 17, 7.5, 17, 8.5, 15.5, 9.5, 14);
        p.bezier(13.5, 9.5, 14.5, 8, 15, 7, 17, 7);
        p.seg(17, 7, 20, 7);
        p.poly({{17, 4}, {20, 7}, {17, 10}});
        p.poly({{17, 14}, {20, 17}, {17, 20}});
        break;
    case Icon::Prev:
        p.rect(5, 5, 2.5, 14);
        p.tri(19, 5, 19, 19, 9, 12);
        break;
    case Icon::Play: p.tri(7, 5, 7, 19, 19, 12); break;
    case Icon::Pause:
        p.rect(6, 5, 4, 14);
        p.rect(14, 5, 4, 14);
        break;
    case Icon::Next:
        p.tri(5, 5, 5, 19, 15, 12);
        p.rect(16.5, 5, 2.5, 14);
        break;
    case Icon::Repeat:
        p.poly({{17, 2}, {21, 6}, {17, 10}});
        p.seg(3, 11, 3, 9);
        p.arc(6, 9, 3, 180, 270);
        p.seg(6, 6, 21, 6);
        p.poly({{7, 22}, {3, 18}, {7, 14}});
        p.seg(21, 13, 21, 15);
        p.arc(18, 15, 3, 0, 90);
        p.seg(18, 18, 3, 18);
        break;
    case Icon::Queue:
        p.seg(4, 6, 20, 6);
        p.seg(4, 12, 20, 12);
        p.seg(4, 18, 13, 18);
        break;
    case Icon::Output:
        p.poly({{4, 9}, {4, 15}, {8, 15}, {13, 19}, {13, 5}, {8, 9}}, true);
        p.arc(13.35, 12, 4, -48.6, 48.6);
        break;
    case Icon::Muted: /* the speaker with a cross: the muted level (the owner's deck report, 2026-11-13) */
        p.poly({{3, 9}, {3, 15}, {7, 15}, {12, 19}, {12, 5}, {7, 9}}, true);
        p.seg(15, 9, 21, 15);
        p.seg(21, 9, 15, 15);
        break;
    case Icon::Browse: p.poly({{3, 7}, {10, 7}, {12, 9}, {21, 9}, {21, 19}, {3, 19}}, true); break;
    case Icon::Sleep:
        p.arc(12, 13, 8, 0, 360);
        p.poly({{12, 9}, {12, 13}, {15, 15}});
        p.seg(9, 2, 15, 2);
        break;
    case Icon::Equalizer: p.poly({{4, 12}, {7, 12}, {10, 5}, {14, 19}, {17, 12}, {20, 12}}); break;
    case Icon::Info:
        p.width = 2.2;
        p.arc(12, 12, 9, 0, 360);
        p.seg(12, 11, 12, 17);
        p.seg(12, 7.5, 12, 8);
        break;
    case Icon::Phone:
        p.width = 2.2;
        p.poly({{5, 4}, {9, 4}, {11, 9}, {8.5, 10.5}});
        p.bezier(8.5, 10.5, 9.5, 12.5, 11.5, 14.5, 13.5, 15.5);
        p.poly({{13.5, 15.5}, {15, 13}, {20, 15}, {20, 19}});
        p.arc(18, 19, 2, 0, 90);
        p.bezier(18, 21, 10, 21, 3, 14, 3, 6);
        p.arc(5, 6, 2, 180, 270);
        break;
    case Icon::Warning:
        p.width = 2.4;
        p.poly({{12, 3.2}, {2.6, 19.8}, {21.4, 19.8}}, true);
        p.seg(12, 8, 12, 13);
        p.seg(12, 16.5, 12, 17);
        break;
    case Icon::Chevron:
        p.width = 2.4;
        p.poly({{9, 6}, {15, 12}, {9, 18}});
        break;
    case Icon::Up:
        p.width = 2.4;
        p.poly({{6, 15}, {12, 9}, {18, 15}});
        break;
    case Icon::Down:
        p.width = 2.4;
        p.poly({{6, 9}, {12, 15}, {18, 9}});
        break;
    case Icon::Minus:
        p.width = 2.4;
        p.seg(5, 12, 19, 12);
        break;
    case Icon::Plus: /* the volume strip's "+" (the Minus with its upright) */
        p.width = 2.4;
        p.seg(5, 12, 19, 12);
        p.seg(12, 5, 12, 19);
        break;
    case Icon::Trash:
        p.width = 2.2;
        p.seg(5, 7, 19, 7);
        p.poly({{9, 7}, {9, 4}, {15, 4}, {15, 7}});
        p.poly({{7, 7}, {8, 20}, {16, 20}, {17, 7}});
        break;
    default: break;
    }
}

lv_obj_t *icon_obj(lv_obj_t *parent, Icon ic, int x, int y, int w, int h, int px, std::uint32_t rgb) {
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, std::max(1, w), std::max(1, h));
    lv_obj_set_user_data(o, new IconData{ic, rgb, LV_OPA_COVER, px});
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(o, icon_delete_cb, LV_EVENT_DELETE, nullptr);
    return o;
}

void icon_set(lv_obj_t *o, Icon ic, std::uint32_t rgb, lv_opa_t opa) {
    auto *d = static_cast<IconData *>(lv_obj_get_user_data(o));
    if (!d) return;
    if (d->ic == ic && d->rgb == rgb && d->opa == opa) return;
    d->ic = ic;
    d->rgb = rgb;
    d->opa = opa;
    lv_obj_invalidate(o);
}

void icon_resize(lv_obj_t *o, int px) {
    auto *d = static_cast<IconData *>(lv_obj_get_user_data(o));
    if (!d) return;
    px = std::max(1, px);
    if (d->px == px && lv_obj_get_width(o) == px && lv_obj_get_height(o) == px) return;
    d->px = px;
    lv_obj_set_size(o, px, px);
    lv_obj_invalidate(o);
}

} // namespace ui
} // namespace vinyl
