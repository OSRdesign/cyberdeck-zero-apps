/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS chip on the map: a satellite dish and three reception bars on a small plate under the engine's top
 * row, drawn with LVGL primitives (no image). Which dish, which bars and which colour is decided in core/gps_chip.cpp
 * (the five states: no receiver, receiver and no fix, 2D or weak 3D, good 3D, stale); this file only draws it and takes
 * the touch. A tap, or the S key, calls open_gps_status() (a stub until the GPS status page exists).
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace atlasui {

namespace {

constexpr std::uint32_t kPlateRgb = 0x2B2B2B;   /* the engine's plates (pal::kBar) */
constexpr std::uint32_t kRed = 0xFF4D4D;        /* receiver connected, no fix */
constexpr std::uint32_t kOrange = 0xFF9A1F;     /* 2D fix or weak 3D geometry */
constexpr std::uint32_t kGreen = 0x3FD166;      /* good 3D fix */

lv_color_t rgb(std::uint32_t c) { return lv_color_hex(c); }

void fill_rect(lv_layer_t *layer, const lv_area_t &a, std::uint32_t c, lv_opa_t opa, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = rgb(c);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void outline(lv_layer_t *layer, const lv_area_t &a, std::uint32_t c, lv_opa_t opa, int width, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = rgb(c);
    d.border_opa = opa;
    d.border_width = width;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void stroke(lv_layer_t *layer, double x0, double y0, double x1, double y1, std::uint32_t c, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = static_cast<lv_value_precise_t>(std::lround(x0));
    d.p1.y = static_cast<lv_value_precise_t>(std::lround(y0));
    d.p2.x = static_cast<lv_value_precise_t>(std::lround(x1));
    d.p2.y = static_cast<lv_value_precise_t>(std::lround(y1));
    d.color = rgb(c);
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

/* The dish in the s x s square at (sx, sy): a bowl (an arc of the circle's lower left half, opening to the upper right),
 * the feed arm and its horn, a post and a base. */
void draw_dish(lv_layer_t *layer, int sx, int sy, int s, std::uint32_t c, bool crossed)
{
    const int w = std::max(s / 7, 2);
    const double r = s * 0.36, cx = sx + s * 0.50, cy = sy + s * 0.40;
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.color = rgb(c);
    a.opa = LV_OPA_COVER;
    a.width = w;
    a.center.x = static_cast<int32_t>(std::lround(cx));
    a.center.y = static_cast<int32_t>(std::lround(cy));
    a.radius = static_cast<uint16_t>(std::lround(r));
    a.start_angle = 45;   /* clockwise from three o'clock: south-east, south, west, to north-west */
    a.end_angle = 225;
    a.rounded = 1;
    lv_draw_arc(layer, &a);
    const double k = 0.7071;
    const double fx = cx + 0.95 * r * k, fy = cy - 0.95 * r * k; /* the horn, on the bowl's axis */
    const int lw = std::max(s / 10, 1);
    stroke(layer, cx, cy, fx, fy, c, lw);
    const int hr = std::max(s / 11, 1);
    fill_rect(layer, lv_area_t{static_cast<int>(std::lround(fx)) - hr, static_cast<int>(std::lround(fy)) - hr,
                               static_cast<int>(std::lround(fx)) + hr, static_cast<int>(std::lround(fy)) + hr},
              c, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    const double by = sy + s - 1;
    stroke(layer, cx, cy + r, cx, by, c, lw);                     /* the post */
    stroke(layer, cx - s * 0.28, by, cx + s * 0.28, by, c, lw);   /* the base */
    if (crossed) {
        /* a slash with a gap of plate colour on both sides, so it reads over the dish */
        stroke(layer, sx + s * 0.06, sy + s * 0.06, sx + s * 0.94, sy + s * 0.94, kPlateRgb, w + 3);
        stroke(layer, sx + s * 0.06, sy + s * 0.06, sx + s * 0.94, sy + s * 0.94, c, std::max(w - 1, 2));
    }
}

} // namespace

void AtlasApp::build_gps_chip()
{
    gps_chip_ = lv_obj_create(map_box_);
    plain(gps_chip_);
    lv_obj_add_flag(gps_chip_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(gps_chip_, target_, target_);
    lv_obj_add_event_cb(gps_chip_, chip_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(gps_chip_, chip_draw_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(gps_chip_, chip_draw_cb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(gps_chip_, chip_draw_cb, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(gps_chip_, chip_click_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(gps_chip_, LV_OBJ_FLAG_HIDDEN);
}

/* The chip's plate: about 1.5 zoom-button squares wide, the old text chip's height, in the row under the engine's top row
 * (the "overzoom" badge's row). The object is the touch square (TARGET, at least as wide as the plate), the plate is
 * drawn inside it. */
void AtlasApp::gps_layout_chip()
{
    if (!gps_chip_) return;
    const atlas::MapMetrics mm = atlas::map_metrics(overlay_input());
    chip_w_ = mm.plate * 3 / 2;
    chip_h_ = mm.row_h;
    const int vx = mm.margin, vy = mm.margin + mm.row_h + mm.margin / 2;
    const int tw = std::max(chip_w_, target_), th = std::max(chip_h_, target_);
    const int ox = std::max(0, vx + chip_w_ / 2 - tw / 2), oy = std::max(0, vy + chip_h_ / 2 - th / 2);
    chip_dx_ = vx - ox;
    chip_dy_ = vy - oy;
    lv_obj_set_size(gps_chip_, tw, th);
    lv_obj_set_pos(gps_chip_, ox, oy);
    lv_obj_invalidate(gps_chip_);
}

void AtlasApp::chip_draw_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) {
        lv_area_t a;
        lv_obj_get_coords(self->gps_chip_, &a);
        self->draw_gps_chip(lv_event_get_layer(e), a);
    } else {
        lv_obj_invalidate(self->gps_chip_); /* pressed, released: the plate changes colour */
    }
}

void AtlasApp::chip_click_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    self->post([self] { self->open_gps_status(); });
}

/* The stub of the GPS status page: the chip's tap and the S key land here. The page itself comes with the next change; until
 * then this only records the request (gps_status_requested_) and writes a log line. */
void AtlasApp::open_gps_status()
{
    if (!settings_.gps) return;
    gps_status_requested_ = true;
    log_line("gps: status page requested (%s)", atlas::chip_description(gps_chip_state_).c_str());
}

void AtlasApp::draw_gps_chip(lv_layer_t *layer, const lv_area_t &o)
{
    const atlas::ChipState cs = gps_chip_state_;
    if (cs.level == atlas::ChipLevel::Hidden || chip_w_ <= 0) return;
    const atlas::MapMetrics mm = atlas::map_metrics(overlay_input());
    const int radius = cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S];
    const lv_area_t a = {o.x1 + chip_dx_, o.y1 + chip_dy_, o.x1 + chip_dx_ + chip_w_ - 1, o.y1 + chip_dy_ + chip_h_ - 1};
    const bool pressed = lv_obj_has_state(gps_chip_, LV_STATE_PRESSED);
    fill_rect(layer, a, pressed ? pal::kAccent : kPlateRgb, LV_OPA_80, radius);
    outline(layer, a, pal::kText, LV_OPA_30, 1, radius);

    /* the contents: dish, bars, and a slot for the stale "!" (always reserved, so the chip does not shift between states) */
    const int vp = std::max(mm.pad / 2, 2), hp = std::max(mm.pad / 2, 2);
    const int H = std::max(chip_h_ - 2 * vp, 8);
    const int bw = std::max(H / 5, 3), bgap = std::max(H / 10, 1), g = std::max(H / 4, 3);
    const int bars_w = 3 * bw + 2 * bgap, ex_w = std::max(bw * 2 / 3, 2);
    int s = H;
    const int avail = chip_w_ - 2 * hp;
    if (s + g + bars_w + g + ex_w > avail) s = std::max(avail - (g + bars_w + g + ex_w), 8);
    const int group = s + g + bars_w + g + ex_w;
    const int gx = a.x1 + (chip_w_ - group) / 2, gy = a.y1 + (chip_h_ - H) / 2;
    const bool none = cs.level == atlas::ChipLevel::NoReceiver;
    draw_dish(layer, gx, gy + (H - s) / 2, s, none ? pal::kMuted : pal::kText, cs.crossed);

    std::uint32_t colour = pal::kMuted;
    switch (cs.level) {
    case atlas::ChipLevel::Searching: colour = kRed; break;
    case atlas::ChipLevel::Weak: colour = kOrange; break;
    case atlas::ChipLevel::Good: colour = kGreen; break;
    default: break;
    }
    const int bx0 = gx + s + g;
    for (int i = 0; i < 3; ++i) {
        const int bh = std::max(H * (i + 1) / 3, 2), x = bx0 + i * (bw + bgap), y1 = gy + H - 1;
        const lv_area_t bar = {x, y1 - bh + 1, x + bw - 1, y1};
        if (i < cs.bars) fill_rect(layer, bar, colour, cs.stale ? LV_OPA_40 : LV_OPA_COVER, 0);
        else outline(layer, bar, pal::kMuted, LV_OPA_80, 1, 0);
    }
    if (cs.stale) { /* the "!": a bar and a dot */
        const int ex = bx0 + bars_w + g, top = gy + H / 8, bottom = gy + H - 1 - H / 8, dot = std::max(ex_w, 2);
        fill_rect(layer, lv_area_t{ex, top, ex + ex_w - 1, bottom - dot - 1}, pal::kWarn, LV_OPA_COVER, 0);
        fill_rect(layer, lv_area_t{ex, bottom - dot + 1, ex + ex_w - 1, bottom}, pal::kWarn, LV_OPA_COVER, 0);
    }
}

} // namespace atlasui
