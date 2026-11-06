/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the drawing of the Radar, Table and Graph pages (see radar_app.hpp). Everything is drawn by LVGL's
 * draw descriptors in the view's draw event: no widget per point, no allocation while drawing (texts go to a fixed
 * pool that lives until the next frame).
 */
#include "radar_app.hpp"

#include "palette.hpp"
#include "units.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace mmwui {

namespace {

const char *direction_text(mmw::Direction d)
{
    switch (d) {
    case mmw::Direction::Approaching: return "Approaching";
    case mmw::Direction::Receding: return "Receding";
    default: return "Still";
    }
}

} // namespace

/* ------------------------------------------------------------------------------------------------ primitives */

const char *RadarApp::fmt(const char *format, ...)
{
    char *buf = texts_[text_n_ < kTextSlots ? text_n_++ : kTextSlots - 1];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(texts_[0]), format, ap);
    va_end(ap);
    return buf;
}

int RadarApp::text_w(const char *s, int px) const
{
    lv_point_t size;
    lv_text_get_size(&size, s, cp0_ui_font_px(px), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

void RadarApp::text(lv_layer_t *layer, const char *s, int x, int y, int px, std::uint32_t rgb, lv_opa_t opa)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = cp0_ui_font_px(px);
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.text = s; /* from the pool (or a literal): valid until the next frame */
    const int w = text_w(s, px);
    const lv_area_t a{x, y, x + w, y + lv_font_get_line_height(d.font)};
    lv_draw_label(layer, &d, &a);
}

void RadarApp::line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int w, std::uint32_t rgb, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = w;
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(layer, &d);
}

void RadarApp::disc(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb, lv_opa_t opa)
{
    rect(layer, cx - r, cy - r, 2 * r + 1, 2 * r + 1, rgb, opa, LV_RADIUS_CIRCLE);
}

void RadarApp::rect(lv_layer_t *layer, int x, int y, int w, int h, std::uint32_t rgb, lv_opa_t opa, int radius)
{
    if (w <= 0 || h <= 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa = opa;
    d.radius = radius;
    d.border_width = 0;
    const lv_area_t a{x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &d, &a);
}

void RadarApp::draw_cb(lv_event_t *e)
{
    auto *self = static_cast<RadarApp *>(lv_event_get_user_data(e));
    self->draw(lv_event_get_layer(e));
}

void RadarApp::draw(lv_layer_t *layer)
{
    if (!link_) return;
    text_n_ = 0;
    lv_area_t a;
    lv_obj_get_coords(view_, &a);
    switch (tab_) {
    case Tab::Radar: draw_radar(layer, a); break;
    case Tab::Table: draw_table(layer, a); break;
    case Tab::Graph: draw_graph(layer, a); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ Radar */

mmw::PlotGeom RadarApp::plot_geometry(bool local) const
{
    lv_area_t a;
    lv_obj_get_coords(view_, &a);
    const int pad = std::max(sz_.space_m, sz_.dot);
    /* classic: the whole view; tall: the rectangle of the layout (the sensor at the bottom centre of it) */
    const int y = (local ? 0 : a.y1) + lay_.plot_y;
    const int h = lay_.tall ? lay_.plot_h : lv_area_get_height(&a);
    return mmw::plot_geom(local ? 0 : a.x1, y, lv_area_get_width(&a), h, effective_range(), settings_.orientation, pad,
                          std::max(sz_.space_m, sz_.dot / 2 + 4));
}

/* The static part of the plot (the wedge, the rings and their labels, the axis, the sensor, the range) is drawn once
 * into the background canvas under the view, again only when the range, the orientation, the units or the size
 * change; each frame then only copies it where targets moved and draws the targets on top. */
void RadarApp::render_background()
{
    const int range = effective_range();
    const int key = range * 100 + static_cast<int>(settings_.orientation) * 10 + static_cast<int>(settings_.units) +
                    (settings_.range_m ? 1000 : 0);
    if (key == bg_key_ || !bg_) return;
    bg_key_ = key;
    text_n_ = 0;
    lv_canvas_fill_bg(bg_, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(bg_, &layer);
    const mmw::PlotGeom g = plot_geometry(true);
    const int cap = sz_.caption_px;
    const bool down = g.ysign > 0; /* Rotate180: the front is down */
    const int front = down ? 90 : 270; /* LVGL angles: 0 = 3 o'clock, clockwise */
    const int R = static_cast<int>(std::lround(range * 1000.0 * g.px_per_mm));
    {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center.x = g.sx;
        d.center.y = g.sy;
        d.radius = static_cast<uint16_t>(std::max(1, R));
        d.width = std::max(1, R);
        d.start_angle = front - static_cast<int>(mmw::kFovDeg);
        d.end_angle = front + static_cast<int>(mmw::kFovDeg);
        d.color = lv_color_hex(pal::kWedge);
        d.opa = LV_OPA_10;
        lv_draw_arc(&layer, &d);
    }
    for (int side = -1; side <= 1; side += 2) {
        const double ang = (front + side * mmw::kFovDeg) * M_PI / 180.0;
        line(&layer, g.sx, g.sy, g.sx + static_cast<int>(std::lround(R * std::cos(ang))),
             g.sy + static_cast<int>(std::lround(R * std::sin(ang))), 1, pal::kWedge, LV_OPA_40);
    }
    /* range rings every metre over the half circle in front, brighter inside the field of view; their labels along the
     * wedge's left edge, just inside each ring (off the centre line, where a target's label would cover them) on a
     * dark backing */
    const double edge = (down ? front + mmw::kFovDeg : front - mmw::kFovDeg) * M_PI / 180.0; /* the screen-left edge */
    const double ex = std::cos(edge), ey = std::sin(edge);
    const double fx = std::cos(front * M_PI / 180.0), fy = std::sin(front * M_PI / 180.0);
    const double fe = fx * ex + fy * ey;
    double nx = fx - fe * ex, ny = fy - fe * ey; /* the normal from the edge into the wedge */
    const double nl = std::max(1e-6, std::hypot(nx, ny));
    nx /= nl;
    ny /= nl;
    const int lh_cap = lv_font_get_line_height(cp0_ui_font_px(cap));
    for (int m = 1; m <= range; ++m) {
        const int r = static_cast<int>(std::lround(m * 1000.0 * g.px_per_mm));
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center.x = g.sx;
        d.center.y = g.sy;
        d.radius = static_cast<uint16_t>(std::max(1, r));
        d.width = 1;
        d.start_angle = front - 90;
        d.end_angle = front + 90;
        d.color = lv_color_hex(pal::kRing);
        d.opa = LV_OPA_COVER;
        lv_draw_arc(&layer, &d);
        d.start_angle = front - static_cast<int>(mmw::kFovDeg);
        d.end_angle = front + static_cast<int>(mmw::kFovDeg);
        d.color = lv_color_hex(pal::kRingFov);
        lv_draw_arc(&layer, &d);
        const char *label = settings_.units == mmw::UnitMode::MmCms ? fmt("%d mm", m * 1000) : fmt("%d m", m);
        const int lw = text_w(label, cap);
        const int px = g.sx + static_cast<int>(std::lround(ex * (r - 4) + nx * 3));
        const int py = g.sy + static_cast<int>(std::lround(ey * (r - 4) + ny * 3));
        const int ly = ny < 0 ? py - lh_cap : py; /* the text grows away from the edge, into the wedge */
        rect(&layer, px - 1, ly, lw + 3, lh_cap, 0x000000, LV_OPA_60, 3);
        text(&layer, label, px, ly, cap, pal::kMuted);
    }
    line(&layer, g.sx, g.sy, g.sx, down ? g.sy + R : g.sy - R, 1, pal::kRing, LV_OPA_COVER);
    /* the sensor: a small bar across its position */
    const int sb = std::max(6, sz_.dot / 2);
    rect(&layer, g.sx - sb, g.sy - 2, 2 * sb, 5, pal::kMuted, LV_OPA_COVER, 2);
    /* the range in the far corner */
    const char *rl = settings_.range_m ? fmt("%d m", range) : fmt("Auto %d m", range);
    const int rw = text_w(rl, cap);
    /* (tall: in the chips' row, over the plot's rectangle) */
    text(&layer, rl, g.x + g.w - rw - sz_.space_s, down ? g.y + g.h - cap - sz_.space_s - 4 : sz_.space_s, cap,
         pal::kMuted);
    lv_canvas_finish_layer(bg_, &layer);
    lv_obj_invalidate(bg_);
}

bool RadarApp::target_area(int i, const mmw::PlotGeom &g, lv_area_t &out) const
{
    const mmw::SlotState &s = slot(i);
    if (!mmw::slot_shown(s, settings_.hide_held)) return false;
    int px, py;
    mmw::plot_point(g, s.last.x_mm, s.last.y_mm, px, py);
    const int reach = sz_.dot / 2 + 4 * sz_.dot + sz_.dot; /* circle, the longest arrow, its head */
    const int lh = lv_font_get_line_height(cp0_ui_font_px(sz_.caption_px)) + 2;
    int x1 = px - std::max(reach, sz_.dot + label_w_max_), x2 = px + std::max(reach, sz_.dot + label_w_max_);
    int y1 = py - std::max(reach, 3 * lh), y2 = py + std::max(reach, 3 * lh);
    if (settings_.trails)
        for (const mmw::TrailPoint &p : s.trail) {
            int tx, ty;
            mmw::plot_point(g, p.x_mm, p.y_mm, tx, ty);
            x1 = std::min(x1, tx - sz_.dot);
            x2 = std::max(x2, tx + sz_.dot);
            y1 = std::min(y1, ty - sz_.dot);
            y2 = std::max(y2, ty + sz_.dot);
        }
    out.x1 = std::max(x1, g.x);
    out.y1 = std::max(y1, g.y);
    out.x2 = std::min(x2, g.x + g.w - 1);
    out.y2 = std::min(y2, g.y + g.h - 1);
    return out.x1 <= out.x2 && out.y1 <= out.y2;
}

/* Only what moved is drawn again: the areas the targets covered in the last frame and cover in this one (the rings
 * and the wedge under them are redrawn there too, clipped). A change of range, orientation, page or state redraws all. */
void RadarApp::invalidate_view()
{
    if (tab_ != Tab::Radar || full_redraw_) {
        full_redraw_ = false;
        chart_dirty_ = false;
        lv_obj_invalidate(view_);
        if (tab_ == Tab::Radar) {
            const mmw::PlotGeom g = plot_geometry(false);
            for (int i = 0; i < 3; ++i) have_box_[i] = target_area(i, g, last_box_[i]);
        }
        return;
    }
    const mmw::PlotGeom g = plot_geometry(false);
    for (int i = 0; i < 3; ++i) {
        lv_area_t now_box;
        const bool have = target_area(i, g, now_box);
        if (have_box_[i]) lv_obj_invalidate_area(view_, &last_box_[i]);
        if (have) lv_obj_invalidate_area(view_, &now_box);
        last_box_[i] = now_box;
        have_box_[i] = have;
    }
    if (lay_.tall) { /* the list with every frame, the strip chart when the history moved */
        lv_area_t v;
        lv_obj_get_coords(view_, &v);
        lv_area_t r{v.x1, v.y1 + lay_.list_y - list_gap_, v.x2, v.y1 + lay_.list_y + lay_.list_h};
        lv_obj_invalidate_area(view_, &r);
        if (lay_.chart_h > 0 && chart_dirty_) {
            r = {v.x1, v.y1 + lay_.chart_y, v.x2, v.y1 + lay_.chart_y + lay_.chart_h - 1};
            lv_obj_invalidate_area(view_, &r);
        }
    }
    chart_dirty_ = false;
}

void RadarApp::draw_radar(lv_layer_t *layer, const lv_area_t &a)
{
    (void)a;
    const int cap = sz_.caption_px;
    const mmw::PlotGeom g = plot_geometry(false);

    /* the targets: trail, speed arrow, circle; labels last (on top) */
    const std::int64_t t_now = view_time();
    const int dot_r = sz_.dot / 2;
    const int trail_w = std::max(2, sz_.dot / 4);
    mmw::Box taken[3];
    int n_taken = 0;
    struct Pending {
        int px, py, slot;
        bool live;
    } pending[3];
    int n_pending = 0;
    for (int i = 0; i < 3; ++i) {
        const mmw::SlotState &s = slot(i);
        if (!mmw::slot_shown(s, settings_.hide_held)) continue;
        const std::uint32_t col = pal::kTarget[i];
        /* a held (stale) target: dimmed, no trail, no arrow */
        if (settings_.trails && !s.held && s.trail.size() > 1) {
            /* one segment per 150 ms at most (about 13 for the 2 s trail; a line is the costly primitive on a
             * Zero 2 W), the newest point always, segments under 2 px merged into the next */
            int px0 = 0, py0 = 0;
            std::int64_t t0 = 0;
            bool have = false;
            const std::int64_t newest = s.trail.back().t_ms;
            for (const mmw::TrailPoint &p : s.trail) {
                if (have && p.t_ms - t0 < 150 && p.t_ms != newest) continue;
                int px, py;
                mmw::plot_point(g, p.x_mm, p.y_mm, px, py);
                if (have && std::abs(px - px0) + std::abs(py - py0) < 2 && p.t_ms != newest) continue;
                const std::int64_t age = std::max<std::int64_t>(0, t_now - p.t_ms);
                if (have && age < 2000 && mmw::plot_contains(g, px, py) && mmw::plot_contains(g, px0, py0)) {
                    const int opa = static_cast<int>(200.0 * (1.0 - static_cast<double>(age) / 2000.0));
                    if (opa > 8) line(layer, px0, py0, px, py, trail_w, col, static_cast<lv_opa_t>(opa));
                }
                px0 = px;
                py0 = py;
                t0 = p.t_ms;
                have = true;
            }
        }
        int px, py;
        mmw::plot_point(g, s.last.x_mm, s.last.y_mm, px, py);
        if (!mmw::plot_contains(g, px, py)) continue; /* beyond a fixed range: not drawn outside the plot */
        /* the arrow along the movement: the trail's last 0.4 s, else radial by the speed's sign */
        const int len = s.held ? 0 : mmw::arrow_length(s.speed_smooth_cm_s, sz_.dot);
        if (len > 0) {
            double dx = 0, dy = 0;
            if (s.trail.size() > 1) {
                const mmw::TrailPoint &last = s.trail.back();
                const mmw::TrailPoint *from = &s.trail.front();
                for (auto it = s.trail.rbegin(); it != s.trail.rend(); ++it)
                    if (last.t_ms - it->t_ms >= 400) {
                        from = &*it;
                        break;
                    }
                dx = last.x_mm - from->x_mm;
                dy = last.y_mm - from->y_mm;
            }
            if (std::hypot(dx, dy) < 40.0) { /* hardly moved: along the line to the sensor */
                const double d = std::max(1.0, std::hypot(s.last.x_mm, s.last.y_mm));
                const double sign = s.speed_smooth_cm_s >= 0 ? 1.0 : -1.0;
                dx = sign * s.last.x_mm / d;
                dy = sign * s.last.y_mm / d;
            }
            const double n = std::max(1e-6, std::hypot(dx, dy));
            const double ux = g.xsign * dx / n, uy = g.ysign * dy / n; /* screen direction */
            const int ex = px + static_cast<int>(std::lround(ux * (dot_r + len)));
            const int ey = py + static_cast<int>(std::lround(uy * (dot_r + len)));
            const int sx0 = px + static_cast<int>(std::lround(ux * dot_r));
            const int sy0 = py + static_cast<int>(std::lround(uy * dot_r));
            line(layer, sx0, sy0, ex, ey, std::max(2, sz_.dot / 5), col, LV_OPA_COVER);
            const int head = std::max(5, sz_.dot / 2);
            lv_draw_triangle_dsc_t t;
            lv_draw_triangle_dsc_init(&t);
            t.p[0].x = ex + static_cast<int>(std::lround(ux * head * 0.6));
            t.p[0].y = ey + static_cast<int>(std::lround(uy * head * 0.6));
            t.p[1].x = ex - static_cast<int>(std::lround(ux * head * 0.6 - uy * head * 0.6));
            t.p[1].y = ey - static_cast<int>(std::lround(uy * head * 0.6 + ux * head * 0.6));
            t.p[2].x = ex - static_cast<int>(std::lround(ux * head * 0.6 + uy * head * 0.6));
            t.p[2].y = ey - static_cast<int>(std::lround(uy * head * 0.6 - ux * head * 0.6));
            t.color = lv_color_hex(col);
            t.opa = LV_OPA_COVER;
            lv_draw_triangle(layer, &t);
        }
        disc(layer, px, py, dot_r, col, s.held ? LV_OPA_40 : s.present_now ? LV_OPA_COVER : LV_OPA_50);
        pending[n_pending++] = {px, py, i, s.present_now};
    }
    /* the chips' corner is kept free of labels */
    lv_area_t chip_a;
    lv_obj_get_coords(chip_, &chip_a);
    taken[n_taken++] = {chip_a.x1, chip_a.y1, lv_area_get_width(&chip_a) + sz_.space_s,
                        lv_area_get_height(&chip_a) + sz_.space_s};
    mmw::Box placed[3];
    for (int k = 0; k < n_pending; ++k) {
        const Pending &p = pending[k];
        const mmw::SlotState &s = slot(p.slot);
        const int speed = static_cast<int>(std::lround(std::fabs(s.speed_smooth_cm_s)));
        /* the units helper's formats (units.hpp), written here without a std::string per frame */
        const char *label = settings_.units == mmw::UnitMode::MmCms
                                ? fmt("T%d %d mm  %d cm/s", p.slot + 1, s.last.distance_mm, speed)
                                : fmt("T%d %.2f m  %.1f km/h", p.slot + 1, mmw::mm_to_m(s.last.distance_mm),
                                      mmw::cm_s_to_km_h(speed));
        const bool held = s.held;
        const int held_w = held ? text_w(" held", cap) : 0;
        const int lw = text_w(label, cap) + held_w + 6, lh = lv_font_get_line_height(cp0_ui_font_px(cap)) + 2;
        mmw::Box all[6];
        int n = 0;
        for (int j = 0; j < n_taken; ++j) all[n++] = taken[j];
        for (int j = 0; j < k; ++j) all[n++] = placed[j];
        const mmw::Box b = mmw::place_label(g, p.px, p.py, dot_r, lw, lh, all, n);
        placed[k] = b;
        rect(layer, b.x, b.y, b.w, b.h, 0x000000, LV_OPA_60, 3);
        if (held) { /* the readout dimmed, then "held" in the muted colour */
            text(layer, label, b.x + 3, b.y + 1, cap, pal::kTarget[p.slot], LV_OPA_60);
            text(layer, " held", b.x + 3 + text_w(label, cap), b.y + 1, cap, pal::kMuted);
        } else {
            text(layer, label, b.x + 3, b.y + 1, cap, pal::kTarget[p.slot], p.live ? LV_OPA_COVER : LV_OPA_70);
        }
    }
    if (lay_.tall) {
        draw_radar_list(layer, a);
        if (lay_.chart_h > 0) draw_radar_chart(layer, a);
    }
}

/* The tall Radar page's list: the three slots in the Table's two-line block, in the units chosen (Metres: m, km/h;
 * Millimetres: mm, cm/s): distance and angle, then speed and direction; an empty slot is a dimmed "-". */
void RadarApp::draw_radar_list(lv_layer_t *layer, const lv_area_t &a)
{
    const int w = lv_area_get_width(&a);
    const int pad = sz_.space_m;
    const int x0 = a.x1 + pad + sz_.dot + pad; /* the Table's: the dot column, "T1" under the dot */
    const int col = (w - 2 * pad - sz_.dot - pad) / 2;
    const int xs[2] = {x0, x0 + col};
    const bool raw = settings_.units == mmw::UnitMode::MmCms;
    for (int i = 0; i < 3; ++i) {
        const mmw::SlotState &s = slot(i);
        const char *v1[2] = {"-", "-"}, *v2[2] = {"-", "-"};
        if (mmw::slot_shown(s, settings_.hide_held)) {
            const double speed = std::fabs(s.speed_smooth_cm_s);
            v1[0] = raw ? fmt("%d mm", s.last.distance_mm) : fmt("%.2f m", mmw::mm_to_m(s.last.distance_mm));
            v1[1] = fmt("%+.0f deg", s.last.angle_deg);
            v2[0] = s.held ? "held" : raw ? fmt("%.0f cm/s", speed) : fmt("%.1f km/h", mmw::cm_s_to_km_h(speed));
            v2[1] = direction_text(s.last.direction);
        }
        const int top = a.y1 + lay_.list_y + i * (list_block_h_ + list_gap_);
        draw_block(layer, i, a.x1, w, top + pad / 4, list_block_h_, BlockLine{2, xs, nullptr, v1},
                   BlockLine{2, xs, nullptr, v2}, 0);
    }
}

/* The tall Radar page's strip chart: the Graph's distance chart of the last 30 s, smaller, with "-30 s" and "now". */
void RadarApp::draw_radar_chart(lv_layer_t *layer, const lv_area_t &a)
{
    const int cap = sz_.caption_px;
    const int lh = lv_font_get_line_height(cp0_ui_font_px(cap));
    const int pad = sz_.space_m;
    const ChartScale s = chart_scale();
    const int x0 = a.x1 + pad + s.axis_w, x1 = a.x2 - pad;
    const int top = a.y1 + lay_.chart_y + lh + 2;
    const int chart_h = lay_.chart_h - 2 * (lh + 2);
    if (chart_h < 20 || x1 - x0 < 40) return;
    text(layer, s.raw ? "Distance (mm), last 30 s" : "Distance (m), last 30 s", x0, top - lh - 2, cap, pal::kMuted);
    draw_chart(layer, s, 0, x0, x1, top, chart_h);
    text(layer, "-30 s", x0, top + chart_h + 2, cap, pal::kMuted);
    text(layer, "now", x1 - text_w("now", cap), top + chart_h + 2, cap, pal::kMuted);
}

/* ------------------------------------------------------------------------------------------------ Table */

void RadarApp::draw_table(lv_layer_t *layer, const lv_area_t &a)
{
    const int w = lv_area_get_width(&a);
    const int body = sz_.body_px, cap = sz_.caption_px;
    const int lh_body = lv_font_get_line_height(cp0_ui_font_px(body));
    const int lh_cap = lv_font_get_line_height(cp0_ui_font_px(cap));
    const int pad = sz_.space_m;
    static const char *const kHead[7] = {"x mm", "y mm", "dist mm", "angle deg", "speed cm/s", "direction", "res mm"};
    static const char *const kSample[7] = {"-8000", "8000", "8000", "-60", "-1000", "Approaching", "2000"};
    const int dot_col = sz_.dot + pad;                               /* two-line layouts: "T1" under the dot */
    const int wide_col = sz_.dot + 4 + text_w("T3", cap) + pad;      /* one line: "T1" right of the dot */
    int col_w[7], need = wide_col + pad;
    for (int c = 0; c < 7; ++c) {
        col_w[c] = std::max(text_w(kHead[c], cap), text_w(kSample[c], body)) + pad;
        need += col_w[c];
    }
    int y = a.y1 + pad;
    if (paused_) /* the bottom right corner, under the rows */
        text(layer, "PAUSED", a.x2 - text_w("PAUSED", cap) - pad, a.y2 - lh_cap - pad / 2, cap, pal::kWarn);
    auto values = [&](const mmw::SlotState &s, const char *out[7]) {
        const mmw::Target &t = s.last;
        out[0] = fmt("%d", t.x_mm);
        out[1] = fmt("%d", t.y_mm);
        out[2] = fmt("%d", t.distance_mm);
        out[3] = fmt("%+.0f", t.angle_deg);
        out[4] = s.held ? "held" : fmt("%d", t.speed_cm_s);
        out[5] = direction_text(t.direction);
        out[6] = fmt("%d", t.resolution_mm);
    };
    if (need <= w) { /* one line per target under a header row */
        int x = a.x1 + pad + wide_col;
        for (int c = 0; c < 7; ++c) {
            text(layer, kHead[c], x, y, cap, pal::kMuted);
            x += col_w[c];
        }
        y += lh_cap + pad;
        for (int i = 0; i < 3; ++i) {
            const mmw::SlotState &s = slot(i);
            const int row_h = lh_body + pad;
            const bool shown = mmw::slot_shown(s, settings_.hide_held);
            rect(layer, a.x1 + pad / 2, y - pad / 4, w - pad, row_h, pal::kPanel, i % 2 ? LV_OPA_TRANSP : LV_OPA_COVER, 4);
            disc(layer, a.x1 + pad + sz_.dot / 2, y + lh_body / 2, sz_.dot / 2, pal::kTarget[i],
                 !shown ? LV_OPA_30 : s.held ? LV_OPA_40 : LV_OPA_COVER);
            text(layer, fmt("T%d", i + 1), a.x1 + pad + sz_.dot + 4, y, cap, shown ? pal::kTarget[i] : pal::kDim);
            const char *v[7] = {"-", "-", "-", "-", "-", "-", "-"};
            if (shown) values(s, v);
            x = a.x1 + pad + wide_col;
            for (int c = 0; c < 7; ++c) {
                text(layer, v[c], x, y, body, !shown ? pal::kDim : s.held ? pal::kMuted : pal::kText);
                x += col_w[c];
            }
            y += row_h + pad / 2;
        }
        text(layer, "Raw sensor units: mm, cm/s; a negative speed comes closer.", a.x1 + pad, y + pad, cap, pal::kMuted);
        return;
    }
    /* narrower: each target in two lines (x y dist angle / speed direction res), the label above the value: in every
     * cell when the height allows (portrait), else once in a header over the three targets (480x320) */
    const int x0 = a.x1 + pad + dot_col;
    const int cell_w = (w - 2 * pad - dot_col) / 4;
    int xs2[3] = {x0, x0 + std::max(cell_w, text_w(kHead[4], cap) + pad), x0 + 3 * cell_w};
    xs2[2] = std::max(xs2[2], xs2[1] + std::max(text_w(kHead[5], cap), text_w(kSample[5], body)) + pad);
    const int cell_h = lh_cap + lh_body + 2;
    const int h = lv_area_get_height(&a);
    const bool per_cell = 3 * (2 * cell_h + pad + pad / 2) + pad <= h;
    const int xs1[4] = {x0, x0 + cell_w, x0 + 2 * cell_w, x0 + 3 * cell_w};
    if (!per_cell) {
        for (int c = 0; c < 4; ++c) text(layer, kHead[c], xs1[c], y, cap, pal::kMuted);
        for (int k = 0; k < 3; ++k) text(layer, kHead[4 + k], xs2[k], y + lh_cap, cap, pal::kMuted);
        y += 2 * lh_cap + pad / 2;
    }
    for (int i = 0; i < 3; ++i) {
        const mmw::SlotState &s = slot(i);
        const int block_h = per_cell ? 2 * cell_h + pad : 2 * lh_body + pad / 2 + 2;
        const char *v[7] = {"-", "-", "-", "-", "-", "-", "-"};
        if (mmw::slot_shown(s, settings_.hide_held)) values(s, v);
        draw_block(layer, i, a.x1, w, y, block_h, BlockLine{4, xs1, per_cell ? kHead : nullptr, v},
                   BlockLine{3, xs2, per_cell ? kHead + 4 : nullptr, v + 4}, per_cell ? 2 : 0);
        y += block_h + pad / 2;
    }
}

void RadarApp::draw_block(lv_layer_t *layer, int i, int x1, int w, int y, int block_h, const BlockLine &l1,
                          const BlockLine &l2, int line_gap)
{
    const int body = sz_.body_px, cap = sz_.caption_px;
    const int lh_body = lv_font_get_line_height(cp0_ui_font_px(body));
    const int lh_cap = lv_font_get_line_height(cp0_ui_font_px(cap));
    const int pad = sz_.space_m;
    const bool active = mmw::slot_shown(slot(i), settings_.hide_held);
    const bool held = active && slot(i).held;
    rect(layer, x1 + pad / 2, y - pad / 4, w - pad, block_h, pal::kPanel, LV_OPA_COVER, 4);
    disc(layer, x1 + pad + sz_.dot / 2, y + lh_body / 2, sz_.dot / 2, pal::kTarget[i],
         !active ? LV_OPA_30 : held ? LV_OPA_40 : LV_OPA_COVER);
    text(layer, fmt("T%d", i + 1), x1 + pad, y + lh_body, cap, active ? pal::kTarget[i] : pal::kDim);
    const std::uint32_t vc = !active ? pal::kDim : held ? pal::kMuted : pal::kText;
    int yv = y;
    for (const BlockLine *l : {&l1, &l2}) {
        const int hy = l->heads ? lh_cap : 0;
        for (int c = 0; c < l->n; ++c) {
            if (l->heads) text(layer, l->heads[c], l->xs[c], yv, cap, pal::kMuted);
            text(layer, l->values[c], l->xs[c], yv + hy, body, vc);
        }
        yv += hy + lh_body + line_gap;
    }
}

/* ------------------------------------------------------------------------------------------------ Graph */

RadarApp::ChartScale RadarApp::chart_scale()
{
    const int cap = sz_.caption_px;
    const std::int64_t span = 30000, t_end = view_time(), t_start = t_end - span;
    const bool raw = settings_.units == mmw::UnitMode::MmCms;

    /* the scales from what is in view */
    double max_d = effective_range() * 1000.0, max_v = 50.0;
    for (int i = 0; i < 3; ++i)
        for (const mmw::HistorySample &s : slot(i).history)
            if (s.valid && s.t_ms >= t_start) {
                max_d = std::max(max_d, static_cast<double>(s.distance_mm));
                max_v = std::max(max_v, std::fabs(s.speed_cm_s));
            }
    /* in the units shown */
    /* the plot's range, or a round number above the farthest point when one went beyond it */
    const double range_mm = effective_range() * 1000.0;
    const double d_top = max_d <= range_mm ? range_mm
                         : raw            ? mmw::nice_ceiling(max_d)
                                          : mmw::nice_ceiling(max_d / 1000.0) * 1000.0;
    const double v_scale = raw ? 1.0 : 0.036; /* cm/s -> km/h */
    const double v_top_u = mmw::nice_ceiling(max_v * v_scale);
    const double v_top = v_top_u / v_scale;

    const char *l_top = raw ? fmt("%.0f", d_top) : fmt("%.0f", d_top / 1000.0);
    const char *l_vtop = fmt("+%g", v_top_u);
    const char *l_vbot = fmt("-%g", v_top_u);
    const int axis_w = std::max({text_w(l_top, cap), text_w(l_vtop, cap), text_w(l_vbot, cap), text_w("0", cap)}) + 6;
    ChartScale cs;
    cs.span = span;
    cs.t_end = t_end;
    cs.t_start = t_start;
    cs.raw = raw;
    cs.d_top = d_top;
    cs.v_top = v_top;
    cs.l_top = l_top;
    cs.l_vtop = l_vtop;
    cs.l_vbot = l_vbot;
    cs.axis_w = axis_w;
    return cs;
}

void RadarApp::draw_chart(lv_layer_t *layer, const ChartScale &cs, int chart, int x0, int x1, int top, int chart_h)
{
    const int cap = sz_.caption_px;
    const int lh = lv_font_get_line_height(cp0_ui_font_px(cap));
    auto tx = [&](std::int64_t t) { return x0 + static_cast<int>((t - cs.t_start) * (x1 - x0) / cs.span); };
    const int bottom = top + chart_h;
    rect(layer, x0, top, x1 - x0, chart_h, pal::kPanel, LV_OPA_COVER);
    /* grid: every 10 s, and the half / zero lines */
    for (int k = 1; k < 3; ++k)
        line(layer, tx(cs.t_start + k * 10000), top, tx(cs.t_start + k * 10000), bottom, 1, pal::kRing, LV_OPA_COVER);
    const int mid = (top + bottom) / 2;
    line(layer, x0, mid, x1, mid, 1, chart == 1 ? pal::kAxis : pal::kRing, LV_OPA_COVER);
    if (chart == 0) {
        text(layer, cs.l_top, x0 - text_w(cs.l_top, cap) - 4, top - lh / 2, cap, pal::kMuted);
        text(layer, "0", x0 - text_w("0", cap) - 4, bottom - lh / 2, cap, pal::kMuted);
    } else {
        text(layer, cs.l_vtop, x0 - text_w(cs.l_vtop, cap) - 4, top - lh / 2, cap, pal::kMuted);
        text(layer, "0", x0 - text_w("0", cap) - 4, mid - lh / 2, cap, pal::kMuted);
        text(layer, cs.l_vbot, x0 - text_w(cs.l_vbot, cap) - 4, bottom - lh / 2, cap, pal::kMuted);
    }
    auto ty = [&](const mmw::HistorySample &s) {
        double f;
        if (chart == 0) f = std::clamp(s.distance_mm / cs.d_top, 0.0, 1.0);
        else f = std::clamp(0.5 + 0.5 * s.speed_cm_s / cs.v_top, 0.0, 1.0);
        return bottom - static_cast<int>(std::lround(f * chart_h));
    };
    for (int i = 0; i < 3; ++i) {
        const mmw::SlotState &st = slot(i);
        if (settings_.hide_held && st.held) continue; /* a hidden held target is not in the charts either */
        const mmw::HistorySample *prev = nullptr;
        for (const mmw::HistorySample &s : st.history) {
            if (s.t_ms < cs.t_start || s.t_ms > cs.t_end || !s.valid) {
                prev = nullptr;
                continue;
            }
            if (prev && s.t_ms - prev->t_ms <= 1000)
                line(layer, tx(prev->t_ms), ty(*prev), tx(s.t_ms), ty(s), 2, pal::kTarget[i], LV_OPA_COVER);
            else
                disc(layer, tx(s.t_ms), ty(s), 1, pal::kTarget[i], LV_OPA_COVER);
            prev = &s;
        }
    }
}

void RadarApp::draw_graph(lv_layer_t *layer, const lv_area_t &a)
{
    const int h = lv_area_get_height(&a);
    const int cap = sz_.caption_px;
    const int lh = lv_font_get_line_height(cp0_ui_font_px(cap));
    const int pad = sz_.space_m;
    const ChartScale cs = chart_scale();
    const bool raw = cs.raw;
    const int x0 = a.x1 + pad + cs.axis_w, x1 = a.x2 - pad;
    const int chart_h = (h - 2 * (lh + 2) - lh - 3 * pad) / 2;
    if (chart_h < 20 || x1 - x0 < 40) return;

    for (int chart = 0; chart < 2; ++chart) {
        const int top = a.y1 + pad + chart * (chart_h + lh + 2 + pad) + lh + 2;
        const char *title = chart == 0 ? (raw ? "Distance (mm)" : "Distance (m)")
                                       : (raw ? "Speed (cm/s, minus = closer)" : "Speed (km/h, minus = closer)");
        text(layer, title, x0, top - lh - 2, cap, pal::kMuted);
        if (chart == 0 && paused_) text(layer, "PAUSED", x1 - text_w("PAUSED", cap), top - lh - 2, cap, pal::kWarn);
        draw_chart(layer, cs, chart, x0, x1, top, chart_h);
    }
    /* the time axis under the second chart, and the legend */
    const int ty0 = a.y1 + pad + 2 * (chart_h + lh + 2) + pad + 2;
    text(layer, "-30 s", x0, ty0, cap, pal::kMuted);
    text(layer, "now", x1 - text_w("now", cap), ty0, cap, pal::kMuted);
    int lx = x0 + text_w("-30 s", cap) + 2 * pad;
    for (int i = 0; i < 3; ++i) {
        const char *t = fmt("T%d", i + 1);
        if (lx + text_w(t, cap) + sz_.dot > x1 - text_w("now", cap) - pad) break;
        disc(layer, lx + sz_.dot / 3, ty0 + lh / 2, sz_.dot / 3, pal::kTarget[i], LV_OPA_COVER);
        text(layer, t, lx + sz_.dot, ty0, cap, pal::kTarget[i]);
        lx += sz_.dot + text_w(t, cap) + pad;
    }
}

} // namespace mmwui
