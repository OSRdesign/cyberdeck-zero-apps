/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS on the screen (see atlas_app.hpp): the reader started from the settings (a fake port in
 * demonstration mode), polled from the app's 100 ms timer; the position dot over the map (a layer of our own: the
 * engine's own dot hides after 10 s instead of going hollow after 5, and its locate button would sit above the zoom
 * buttons), the status chip, the follow button below the zoom buttons, follow mode, and position.state. The position
 * never leaves the deck: it is drawn, and written to position.state in the launcher's state folder, nothing else.
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "format.hpp"
#include "gps_demo.hpp"
#include "gps_posix.hpp"

#include "cp0_map_widget.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <linux/input-event-codes.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace atlasui {

namespace {

constexpr std::uint32_t kPlateRgb = 0x2B2B2B;      /* the engine's plates (pal::kBar) */

lv_color_t rgb(std::uint32_t c) { return lv_color_hex(c); }

void fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t c, lv_opa_t opa, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = rgb(c);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void ring(lv_layer_t *layer, const lv_area_t &a, std::uint32_t c, lv_opa_t opa, int width, int radius)
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

void line(lv_layer_t *layer, double x0, double y0, double x1, double y1, std::uint32_t c, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = static_cast<lv_value_precise_t>(x0);
    d.p1.y = static_cast<lv_value_precise_t>(y0);
    d.p2.x = static_cast<lv_value_precise_t>(x1);
    d.p2.y = static_cast<lv_value_precise_t>(y1);
    d.color = rgb(c);
    d.opa = LV_OPA_COVER;
    d.width = width;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

lv_area_t square(double cx, double cy, int r)
{
    const int x = static_cast<int>(std::lround(cx)), y = static_cast<int>(std::lround(cy));
    return lv_area_t{x - r, y - r, x + r, y + r};
}

/* metres on the ground per screen pixel at a latitude, zoom and display scale (256 px tiles) */
double metres_per_px(double lat, int zoom, double scale)
{
    return 40075016.686 * std::cos(lat * M_PI / 180.0) / (256.0 * std::pow(2.0, zoom) * scale);
}

} // namespace

/* ------------------------------------------------------------------------------------------------ start and stop */

atlas::AttrInput AtlasApp::overlay_input() const
{
    atlas::AttrInput in;
    in.w = w_;
    in.h = view_h_;
    in.target = target_;
    in.space = space_m_;
    in.text_h = lv_font_get_line_height(cp0_ui_font(CP0_TEXT_CAPTION));
    return in;
}

void AtlasApp::gps_start()
{
    /* the reader: the real serial ports, or in demonstration mode a feed of synthetic sentences (never a device) */
    if (demo_) {
        std::string feed;
        if (const char *f = std::getenv("DECKATLAS_GPS_FEED"); f && *f) {
            feed = f;
            if (feed == "other") feed = "@other"; /* an unknown serial port, no receiver */
            else if (feed.find('/') == std::string::npos) {
#ifdef CDK_APP_DIR
                feed = std::string(CDK_APP_DIR) + "/tests/gps-feeds/" + feed + ".nmea";
#endif
            }
        }
        gps_ports_ = std::make_unique<atlas::DemoGpsPorts>(feed);
    } else {
        gps_ports_ = std::make_unique<atlas::PosixGpsPorts>();
    }
    gps_ = std::make_unique<atlas::GpsLink>(*gps_ports_);
    gps_->set_log([](const std::string &s) { log_line("%s", s.c_str()); });

    /* position.state: the launcher's state folder (demonstration mode: the harness's runtime folder) */
    std::string dir = atlas::position_state_dir();
    if (demo_ && dir.empty()) dir = config_dir_ + "/../state";
    pos_writer_ = std::make_unique<atlas::PositionStateWriter>(dir, static_cast<int>(::getpid()));
    if (settings_.gps) log_line("gps: on, position.state in %s", dir.empty() ? "(no state folder)" : dir.c_str());
    gps_apply_settings();
}

void AtlasApp::gps_stop()
{
    if (pos_writer_) pos_writer_->clear();
    gps_.reset();       /* closes the port (its original settings put back) before the ports layer goes */
    gps_ports_.reset();
    pos_writer_.reset();
}

void AtlasApp::gps_apply_settings()
{
    if (!gps_) return;
    atlas::GpsConfig c;
    c.on = settings_.gps;
    c.port = settings_.gps_port;
    c.baud = settings_.gps_baud;
    gps_->configure(c);
    if (!settings_.gps) {
        follow_.set(false, 0, 0);
        gps_status_ = atlas::GpsStatus();
        gps_chip_text_.clear();
        if (pos_writer_) pos_writer_->clear();
    }
    gps_show_overlays();
}

void AtlasApp::gps_refresh_ports()
{
    gps_present_.clear();
    if (!gps_ports_) return;
    for (const atlas::PortInfo &p : gps_ports_->list_ports()) gps_present_.push_back(p.path);
    gps_ports_at_ = mono_ms();
}

/* ------------------------------------------------------------------------------------------------ building */

void AtlasApp::build_gps_layer()
{
    gps_layer_ = lv_obj_create(map_box_);
    plain(gps_layer_);
    lv_obj_set_pos(gps_layer_, 0, 0);
    lv_obj_set_size(gps_layer_, w_, view_h_);
    lv_obj_add_event_cb(gps_layer_, gps_layer_cb, LV_EVENT_DRAW_MAIN, this);
}

void AtlasApp::build_gps_controls()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const atlas::MapMetrics mm = atlas::map_metrics(overlay_input());

    gps_chip_ = lv_obj_create(map_box_);
    plain(gps_chip_);
    lv_obj_set_size(gps_chip_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(gps_chip_, std::max(40, w_ - 3 * mm.margin - mm.plate), 0);
    lv_obj_set_style_bg_color(gps_chip_, rgb(kPlateRgb), 0);
    lv_obj_set_style_bg_opa(gps_chip_, LV_OPA_80, 0);
    lv_obj_set_style_radius(gps_chip_, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_pad_hor(gps_chip_, mm.pad, 0);
    lv_obj_set_style_pad_ver(gps_chip_, mm.pad, 0);
    gps_chip_label_ = lv_label_create(gps_chip_);
    lv_obj_set_style_text_font(gps_chip_label_, cp0_ui_font(CP0_TEXT_CAPTION), 0);
    lv_obj_set_style_text_color(gps_chip_label_, rgb(pal::kText), 0);
    lv_label_set_long_mode(gps_chip_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(gps_chip_label_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(gps_chip_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(gps_chip_label_, std::max(24, w_ - 3 * mm.margin - mm.plate - 2 * mm.pad), 0);
    lv_label_set_text(gps_chip_label_, "");
    lv_obj_set_pos(gps_chip_, mm.margin, mm.margin + mm.row_h + mm.margin / 2); /* the row under the engine's top row */
    lv_obj_add_flag(gps_chip_, LV_OBJ_FLAG_HIDDEN);

    /* the follow button: the touch square (TARGET) of the zoom buttons' column, the plate drawn in it like theirs */
    follow_btn_ = lv_obj_create(map_box_);
    plain(follow_btn_);
    lv_obj_add_flag(follow_btn_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(follow_btn_, target_, target_);
    lv_obj_add_event_cb(follow_btn_, follow_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(follow_btn_, follow_draw_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(follow_btn_, follow_draw_cb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(follow_btn_, follow_draw_cb, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(follow_btn_, follow_click_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(follow_btn_, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------------------------------ the overlays */

void AtlasApp::gps_layout()
{
    if (!map_ || !follow_btn_ || !settings_.gps) return;
    lv_area_t box;
    lv_obj_get_coords(map_box_, &box);
    lv_area_t zi, zo;
    if (cp0_map_control_area(map_, CP0_MAP_CONTROL_ZOOM_IN, &zi) != CP0_MAP_OK ||
        cp0_map_control_area(map_, CP0_MAP_CONTROL_ZOOM_OUT, &zo) != CP0_MAP_OK || zo.x2 <= zo.x1 || zo.y2 <= zo.y1 ||
        zi.y2 <= zi.y1) {
        lv_obj_add_flag(follow_btn_, LV_OBJ_FLAG_HIDDEN); /* the widget has not laid itself out yet */
        return;
    }
    const atlas::MapMetrics mm = atlas::map_metrics(overlay_input());
    /* the engine's plates, rebuilt from its layout rules (cp0_map_layout): the right edge of the column is the widget's
     * edge less the margin (the touch squares may be cut by the widget's edge, so their centre is not the plate's), the
     * top of a plate is its touch square's top plus half the difference; the pitch is the distance of two plates */
    lv_area_t mapa;
    lv_obj_get_coords(map_, &mapa);
    const int plate = mm.plate;
    const int zi_th = zi.y2 - zi.y1 + 1, zo_th = zo.y2 - zo.y1 + 1;
    const int top_in = zi.y1 + zi_th / 2 - plate / 2, top_out = zo.y1 + zo_th / 2 - plate / 2;
    const int pitch = top_out - top_in;
    const int gap = pitch - plate;
    const int col_x = mapa.x2 + 1 - mm.margin - plate;
    /* one pitch below the zoom-out button, like the zoom pair (the same gap as between + and -); when that does not
     * fit above the bottom margin and the credit's row at the bottom right, above the zoom-in button (the engine's own
     * locate button goes there); failing that, beside the zoom-out button on its left (the only place left that touches
     * neither the credit, the scale bar nor the other buttons), the same gap again */
    int px = col_x, py = top_out + pitch;
    bool side = false;
    int limit = box.y2 + 1 - mm.margin;
    if (attr_row_top_ >= 0) limit = std::min(limit, box.y1 + attr_row_top_ - 1);
    if (py + plate > limit) {
        const int above = top_in - pitch;
        if (above >= box.y1 + mm.margin + mm.row_h + 2) {
            py = above;
        } else {
            px = col_x - gap - plate;
            py = top_out;
            side = true;
        }
    }
    /* the touch square (TARGET, as every control of the app) centred on the plate; beside the zoom-out button it stops at
     * that button's own touch square, the plate still inside it */
    const int touch = target_;
    int ox = px + plate / 2 - touch / 2;
    const int oy = py + plate / 2 - touch / 2;
    if (side && ox + touch > zo.x1 && zo.x1 - touch <= px && px + plate <= zo.x1) ox = zo.x1 - touch;
    follow_plate_ = plate;
    follow_dx_ = px - ox;
    follow_dy_ = py - oy;
    if (lv_obj_get_width(follow_btn_) != touch || lv_obj_get_height(follow_btn_) != touch)
        lv_obj_set_size(follow_btn_, touch, touch);
    if (lv_obj_get_x(follow_btn_) != ox - box.x1 || lv_obj_get_y(follow_btn_) != oy - box.y1)
        lv_obj_set_pos(follow_btn_, ox - box.x1, oy - box.y1);
    lv_obj_remove_flag(follow_btn_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(follow_btn_);
}

void AtlasApp::gps_show_overlays()
{
    if (!gps_chip_) return;
    if (!settings_.gps) {
        lv_obj_add_flag(gps_chip_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(follow_btn_, LV_OBJ_FLAG_HIDDEN);
        if (gps_layer_) lv_obj_invalidate(gps_layer_);
        return;
    }
    const std::string text = gps_chip_text_;
    if (text.empty()) {
        lv_obj_add_flag(gps_chip_, LV_OBJ_FLAG_HIDDEN);
    } else {
        const bool problem = gps_status_.kind == atlas::GpsStatus::Kind::NoDevice ||
                             gps_status_.kind == atlas::GpsStatus::Kind::NoReceiver ||
                             gps_status_.kind == atlas::GpsStatus::Kind::NoAccess || gps_status_.stale;
        lv_obj_set_style_text_color(gps_chip_label_, rgb(problem ? pal::kWarn : pal::kText), 0);
        lv_label_set_text(gps_chip_label_, text.c_str());
        lv_obj_remove_flag(gps_chip_, LV_OBJ_FLAG_HIDDEN);
    }
    gps_layout();
    if (follow_btn_) lv_obj_invalidate(follow_btn_);
    if (gps_layer_) lv_obj_invalidate(gps_layer_);
}

void AtlasApp::gps_tick(std::int64_t now)
{
    if (!gps_) return;
    gps_->tick(now);
    if (!settings_.gps) return;
    const atlas::GpsStatus st = gps_->status(now);
    const std::string chip = atlas::chip_text(st, settings_.units == atlas::Units::Mi);
    const bool changed = st.kind != gps_status_.kind || st.stale != gps_status_.stale || st.fix.fix_ms != gps_status_.fix.fix_ms ||
                         chip != gps_chip_text_;
    const bool new_fix = st.has_fix() && !st.stale && st.fix.fix_ms != gps_status_.fix.fix_ms;
    gps_status_ = st;
    gps_chip_text_ = chip;
    if (gps_->port().path.empty()) {
        gps_source_.clear();
    } else {
        char buf[48];
        if (gps_->port().vid) std::snprintf(buf, sizeof buf, "usb:%04x:%04x", gps_->port().vid, gps_->port().pid);
        else std::snprintf(buf, sizeof buf, "serial:%s", gps_->port().tty.c_str());
        gps_source_ = buf;
    }
    if (pos_writer_) pos_writer_->update(st, gps_source_, now);
    if (!changed) {
        if (follow_btn_ && lv_obj_has_flag(follow_btn_, LV_OBJ_FLAG_HIDDEN)) gps_layout(); /* the widget laid out since */
        return;
    }
    if (new_fix && map_ && follow_.on_fix(st.fix.lat, st.fix.lon)) recentre_on(st.fix.lat, st.fix.lon);
    gps_show_overlays();
}

/* ------------------------------------------------------------------------------------------------ follow mode */

void AtlasApp::recentre_on(double lat, double lon)
{
    if (map_) cp0_map_set_center(map_, lat, lon); /* the zoom stays */
}

void AtlasApp::set_follow(bool on)
{
    if (!map_ || !settings_.gps) return;
    double lat = 0, lon = 0;
    cp0_map_get_center(map_, &lat, &lon);
    follow_.set(on, lat, lon);
    if (on && gps_status_.has_fix() && follow_.on_fix(gps_status_.fix.lat, gps_status_.fix.lon))
        recentre_on(gps_status_.fix.lat, gps_status_.fix.lon);
    log_line("follow %s", on ? "on" : "off");
    if (follow_btn_) lv_obj_invalidate(follow_btn_);
    if (gps_layer_) lv_obj_invalidate(gps_layer_);
}

void AtlasApp::on_follow_off()
{
    log_line("follow off (the map was moved by hand)");
    if (follow_btn_) lv_obj_invalidate(follow_btn_);
}

/* G toggles follow, Home turns it on and recentres (the engine's own "centre on me" key). ch: the character of a key the
 * on-screen keyboard typed without a key code. True when the key was used. */
bool AtlasApp::gps_key(std::uint32_t key_code, char ch)
{
    if (!settings_.gps || !map_) return false;
    if (key_code == KEY_G || ch == 'g' || ch == 'G') {
        set_follow(!follow_.on());
        return true;
    }
    if (key_code == KEY_HOME) {
        set_follow(true);
        return true;
    }
    return false;
}

void AtlasApp::follow_click_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    self->post([self] { self->set_follow(!self->follow_.on()); });
}

void AtlasApp::follow_draw_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN: {
        lv_area_t a;
        lv_obj_get_coords(self->follow_btn_, &a);
        self->draw_follow_button(lv_event_get_layer(e), a);
        break;
    }
    default: lv_obj_invalidate(self->follow_btn_); break; /* pressed, released: the plate changes colour */
    }
}

/* A third zoom button: the engine's own draw_button (cp0_map_widget.c) for its locate control, plate, ring and crosshair
 * the same, only the on state is added: a 2 px ring in the accent blue round the plate. */
void AtlasApp::draw_follow_button(lv_layer_t *layer, const lv_area_t &o)
{
    const int p = follow_plate_ > 0 ? follow_plate_ : atlas::map_metrics(overlay_input()).plate;
    const int radius = cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S];
    const lv_area_t a = {o.x1 + follow_dx_, o.y1 + follow_dy_, o.x1 + follow_dx_ + p - 1, o.y1 + follow_dy_ + p - 1};
    const int cx = a.x1 + p / 2, cy = a.y1 + p / 2;
    const bool pressed = lv_obj_has_state(follow_btn_, LV_STATE_PRESSED);
    fill(layer, a, pressed ? pal::kAccent : kPlateRgb, LV_OPA_80, radius);
    ring(layer, a, pal::kText, LV_OPA_30, 1, radius);
    if (follow_.on()) ring(layer, a, pal::kAccent, LV_OPA_COVER, 2, radius);
    /* the crosshair of the engine's own locate button: a ring and four ticks */
    const int stroke = std::max(p / 12, 2), len = p * 2 / 5, rr = p / 4;
    const lv_area_t ca = {cx - rr, cy - rr, cx + rr, cy + rr};
    ring(layer, ca, pal::kText, LV_OPA_COVER, stroke, LV_RADIUS_CIRCLE);
    line(layer, cx - len, cy, cx - rr, cy, pal::kText, stroke);
    line(layer, cx + rr, cy, cx + len, cy, pal::kText, stroke);
    line(layer, cx, cy - len, cx, cy - rr, pal::kText, stroke);
    line(layer, cx, cy + rr, cx, cy + len, pal::kText, stroke);
}

/* ------------------------------------------------------------------------------------------------ the dot */

void AtlasApp::gps_layer_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(self->gps_layer_, &a);
    self->draw_gps(lv_event_get_layer(e), a);
}

/* Fresh fix: an accuracy ring (HDOP x 5 m, 3..100 m, only when it is wider than the dot), a heading wedge above 1 m/s
 * with a known course, a filled blue dot with a white outline. Stale (no valid sentence for 5 s): a hollow dot, no ring,
 * no wedge. No fix: nothing. */
void AtlasApp::draw_gps(lv_layer_t *layer, const lv_area_t &o)
{
    if (!map_ || !settings_.gps || !gps_status_.has_fix()) return;
    const atlas::Fix &f = gps_status_.fix;
    const bool stale = gps_status_.stale;
    double clat = 0, clon = 0;
    cp0_map_get_center(map_, &clat, &clon);
    const int zoom = cp0_map_get_zoom(map_);
    const double scale = cp0_map_get_scale(map_) > 0 ? cp0_map_get_scale(map_) : 1.0;
    double cx = 0, cy = 0, px = 0, py = 0;
    atlas::to_world_px(clat, clon, zoom, cx, cy);
    atlas::to_world_px(f.lat, f.lon, zoom, px, py);
    const double sx = o.x1 + w_ / 2.0 + (px - cx) * scale, sy = o.y1 + view_h_ / 2.0 + (py - cy) * scale;
    const int r = std::max(6, target_ / 7);
    const double mpp = metres_per_px(f.lat, zoom, scale);
    double ar = atlas::accuracy_from_hdop(f.hdop) / mpp;
    const double cap = std::max(w_, view_h_);
    if (ar > cap) ar = cap;
    const double reach = std::max(ar, 3.0 * r);
    if (sx < o.x1 - reach || sx > o.x2 + reach || sy < o.y1 - reach || sy > o.y2 + reach) return; /* off the view */

    if (!stale && ar > r + 2) {
        const int ir = static_cast<int>(std::lround(ar));
        const lv_area_t a = square(sx, sy, ir);
        fill(layer, a, pal::kAccent, LV_OPA_20, LV_RADIUS_CIRCLE);
        ring(layer, a, pal::kAccent, LV_OPA_60, 1, LV_RADIUS_CIRCLE);
    }
    if (!stale && f.speed_mps > atlas::kMovingMps && f.course_deg >= 0) {
        const double a = f.course_deg * M_PI / 180.0, ux = std::sin(a), uy = -std::cos(a);
        for (int pass = 0; pass < 2; ++pass) {
            const double e = pass ? 0 : 1.5; /* the white outline first, the blue wedge over it */
            lv_draw_triangle_dsc_t t;
            lv_draw_triangle_dsc_init(&t);
            t.color = rgb(pass ? pal::kAccent : pal::kText);
            t.opa = LV_OPA_COVER;
            const double tip = 2.6 * r + e, base = 1.2 * r, half = 0.9 * r + e;
            t.p[0].x = static_cast<lv_value_precise_t>(sx + ux * tip);
            t.p[0].y = static_cast<lv_value_precise_t>(sy + uy * tip);
            t.p[1].x = static_cast<lv_value_precise_t>(sx + ux * base - uy * half);
            t.p[1].y = static_cast<lv_value_precise_t>(sy + uy * base + ux * half);
            t.p[2].x = static_cast<lv_value_precise_t>(sx + ux * base + uy * half);
            t.p[2].y = static_cast<lv_value_precise_t>(sy + uy * base - ux * half);
            lv_draw_triangle(layer, &t);
        }
    }
    const int bw = std::max(2, r / 3);
    const lv_area_t d = square(sx, sy, r);
    if (!stale) {
        fill(layer, d, pal::kAccent, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        ring(layer, d, pal::kText, LV_OPA_COVER, bw, LV_RADIUS_CIRCLE);
    } else {
        /* hollow: a white ring with a blue ring inside it, the middle open */
        ring(layer, d, pal::kText, LV_OPA_COVER, bw, LV_RADIUS_CIRCLE);
        const lv_area_t inner = square(sx, sy, r - bw);
        ring(layer, inner, pal::kAccent, LV_OPA_COVER, bw, LV_RADIUS_CIRCLE);
    }
}

} // namespace atlasui
