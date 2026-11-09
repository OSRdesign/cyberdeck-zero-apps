/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the cassette window (see cassette_window.hpp).
 */
#include "cassette_window.hpp"

#include "paint.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace cassette {
namespace ui {

AnimOptions anim_options_from_env()
{
    AnimOptions o;
    if (const char *a = std::getenv("CASSETTE_ANIM"); a && std::strcmp(a, "0") == 0) o.enabled = false;
    if (const char *f = std::getenv("CASSETTE_ANIM_FPS"); f && *f) o.fps = std::clamp(std::atoi(f), 1, 60);
    if (const char *n = std::getenv("CASSETTE_ANIM_TICKS"); n && *n) o.ticks = std::max(0, std::atoi(n));
    return o;
}

namespace {

/* The largest turn of a hub in one timer tick. The spokes repeat every 60 degrees, so a step of 30 degrees or more reads
 * as standing still or turning backwards (the old 5x scan: 40 and 60 degrees a tick at 20 fps showed one hub still and
 * the other turning the wrong way). 20 degrees is two whole frames: the direction is always clear. At 20 fps the 450
 * degrees a second of REW / FWD (22.5 a tick) is held to 20 (about 2.7x the play speed); at 30 fps and more it is exact. */
constexpr double kMaxStepDeg = 20.0;

int top_for_baseline(const lv_font_t *f, int baseline)
{
    return baseline - (lv_font_get_line_height(f) - f->base_line);
}

} // namespace

CassetteWindow::CassetteWindow(lv_obj_t *parent, const Fit &fit, const Layout &lay, AnimOptions anim,
                               std::function<void()> on_cancel)
    : fit_(fit), lay_(lay), anim_(anim), on_cancel_(std::move(on_cancel))
{
    window_ = framed_panel(parent, fit, lay.window, col::kWindow);
    /* the window's screen box (its height follows the page's bands); everything inside is placed from it at scale s:
     * the cassette centred (on the boards it is), the badge from the top, the countdown's hint from the bottom */
    const DRect &w = lay.window;
    const int wx = fit.x(w.x), wy = fit.y(w.y), ww = fit.w(w.x, w.w), wh = fit.h(w.y, w.h);
    const double margin = (w.h - 104 * lay.tape_scale) / 2.0; /* the board's gap above and under the cassette */
    k_ = std::min(lay.tape_scale * fit.s, (wh - 2 * margin * fit.s) / 104.0);

    /* the cassette: drawn once into a canvas in the display's colour format (opaque, on the window colour) */
    const int cw = static_cast<int>(std::lround(200 * k_)), ch = static_cast<int>(std::lround(104 * k_));
    const int tx = fit.x(lay.tape_x) + (static_cast<int>(std::lround(200 * lay.tape_scale * fit.s)) - cw) / 2;
    const int ty = wy + (wh - ch) / 2;
    canvas_ = lv_canvas_create(parent);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(canvas_, tx, ty);
    tape_buf_ = lv_draw_buf_create(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch),
                                   lv_display_get_color_format(lv_obj_get_display(parent)), LV_STRIDE_AUTO);
    if (tape_buf_) lv_canvas_set_draw_buf(canvas_, tape_buf_);
    else lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);

    /* the hubs: 12 pre-rendered frames shared by both image objects */
    hub_box_ = static_cast<int>(std::ceil(24 * k_)) + 2;
    render_hub_frames();
    const double hx[2] = {64, 136};
    for (int i = 0; i < 2; ++i) {
        hub_[i] = lv_image_create(parent);
        lv_obj_remove_flag(hub_[i], LV_OBJ_FLAG_CLICKABLE);
        const int cx = tx + static_cast<int>(std::lround(hx[i] * k_));
        const int cy = ty + static_cast<int>(std::lround(74 * k_));
        lv_obj_set_pos(hub_[i], cx - hub_box_ / 2, cy - hub_box_ / 2);
        lv_obj_set_size(hub_[i], hub_box_, hub_box_);
        if (frames_[0]) lv_image_set_src(hub_[i], frames_[0]);
    }

    /* the scan badge */
    badge_ = lv_obj_create(parent);
    plain(badge_);
    lv_obj_set_pos(badge_, fit.x(lay.badge.x), wy + fit.px(lay.badge.y - w.y));
    lv_obj_set_size(badge_, fit.w(lay.badge.x, lay.badge.w), fit.px(lay.badge.h));
    lv_obj_add_event_cb(badge_, badge_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_flag(badge_, LV_OBJ_FLAG_HIDDEN);

    /* the empty window (no sound input) */
    empty_ = box(parent, wx, wy, ww, wh, col::kWindow, fit.px(8), LV_OPA_TRANSP);
    const int pad = fit.px(12);
    const int ew = ww - 2 * pad;
    empty_msg_ = label(empty_, Face::Bold, fit.px(15), col::kKeyText, pad, 0, ew, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(empty_msg_, LV_LABEL_LONG_MODE_WRAP);
    empty_hint_ = label(empty_, Face::Medium, fit.px(12), col::kHint, pad, 0, ew, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(empty_hint_, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(empty_msg_, "No sound input found. Plug in a USB sound card or headset.");
    lv_label_set_text(empty_hint_, "Found by itself when plugged in.");
    lv_obj_update_layout(empty_);
    const int mh = lv_obj_get_height(empty_msg_), hh = lv_obj_get_height(empty_hint_), gap = fit.px(8);
    const int top = (wh - (mh + gap + hh)) / 2;
    lv_obj_set_y(empty_msg_, top);
    lv_obj_set_y(empty_hint_, top + mh + gap);
    lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);

    /* the countdown: a clickable shell, the framed shade, the texts and the ring */
    count_ = lv_obj_create(parent);
    plain(count_);
    lv_obj_add_flag(count_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(count_, wx, wy);
    const int ow = ww, oh = wh;
    lv_obj_set_size(count_, ow, oh);
    lv_obj_add_event_cb(count_, cancel_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *shade = box(count_, 0, 0, ow, oh, col::kShade, fit.px(8), static_cast<lv_opa_t>(214)); /* 84 % */
    border(shade, fit.px(3), col::kFrame);
    /* the ring and the number centred on the board's centre line (2 px under the window's middle) */
    const int ring_cy = oh / 2 + fit.px(lay.ring_y + lay.ring_d / 2 - (w.y + w.h / 2));
    count_title_ = label(count_, Face::Bold, fit.px(12), col::kScale, 0, fit.px(lay.label_y - w.y), ow, LV_TEXT_ALIGN_CENTER,
                         static_cast<int>(2.5 * fit.s));
    lv_label_set_text(count_title_, "RECORDING IN");
    const int rd = fit.px(lay.ring_d);
    count_ring_ = lv_obj_create(count_);
    plain(count_ring_);
    lv_obj_set_pos(count_ring_, (ow - rd) / 2, ring_cy - rd / 2);
    lv_obj_set_size(count_ring_, rd, rd);
    lv_obj_add_event_cb(count_ring_, ring_draw_cb, LV_EVENT_DRAW_MAIN, this);
    const int npx = fit.px(lay.number_px);
    count_number_ = label(count_, Face::Mono, npx, col::kAmber, 0, 0, ow, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_y(count_number_, ring_cy - lv_font_get_line_height(font(Face::Mono, npx)) / 2);
    count_hint_ = label(count_, Face::Medium, fit.px(12), col::kHint, 0, oh - fit.px(w.y + w.h - lay.hint_y), ow,
                        LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(count_hint_, "tap here, STOP or Esc cancels");
    lv_obj_add_flag(count_, LV_OBJ_FLAG_HIDDEN);

    if (anim_.enabled) timer_ = lv_timer_create(timer_cb, static_cast<uint32_t>(std::max(1, 1000 / anim_.fps)), this);
    last_ms_ = lv_tick_get();
    render_tape();
}

CassetteWindow::~CassetteWindow()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    /* the images and the canvas may outlive this object until their parent goes: point them away first */
    for (lv_obj_t *h : hub_)
        if (h && lv_obj_is_valid(h)) lv_image_set_src(h, nullptr);
    if (canvas_ && lv_obj_is_valid(canvas_)) lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    for (lv_draw_buf_t *&f : frames_) {
        if (f) lv_draw_buf_destroy(f);
        f = nullptr;
    }
    if (canvas_ && lv_obj_is_valid(canvas_)) lv_obj_delete(canvas_);
    if (tape_buf_) lv_draw_buf_destroy(tape_buf_);
    tape_buf_ = nullptr;
}

void CassetteWindow::render_hub_frames()
{
    lv_obj_t *tmp = lv_canvas_create(lv_obj_get_parent(window_));
    lv_obj_add_flag(tmp, LV_OBJ_FLAG_HIDDEN);
    const lv_color_format_t cf = lv_display_get_color_format(lv_obj_get_display(window_));
    const double c = hub_box_ / 2.0;
    for (int f = 0; f < 12; ++f) {
        frames_[f] = lv_draw_buf_create(static_cast<uint32_t>(hub_box_), static_cast<uint32_t>(hub_box_), cf, LV_STRIDE_AUTO);
        if (!frames_[f]) continue;
        lv_canvas_set_draw_buf(tmp, frames_[f]);
        lv_canvas_fill_bg(tmp, lv_color_hex(col::kHubWindow), LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(tmp, &layer);
        const int ci = static_cast<int>(std::lround(c - 0.5));
        disc(&layer, ci, ci, static_cast<int>(std::lround(12 * k_)), col::kHub);
        disc(&layer, ci, ci, static_cast<int>(std::lround(7 * k_)), col::kHubInner);
        const int sw = std::max(1, static_cast<int>(std::lround(2.4 * k_)));
        const double r = 7 * k_;
        for (int s = 0; s < 3; ++s) {
            /* counterclockwise on the screen as f grows (y points down) */
            const double a = (90.0 + 60.0 * s + 10.0 * f) * M_PI / 180.0;
            const double dx = std::cos(a) * r, dy = -std::sin(a) * r;
            line(&layer, static_cast<int>(std::lround(ci + dx)), static_cast<int>(std::lround(ci + dy)),
                 static_cast<int>(std::lround(ci - dx)), static_cast<int>(std::lround(ci - dy)), sw, col::kHub);
        }
        lv_canvas_finish_layer(tmp, &layer);
    }
    lv_obj_delete(tmp);
}

void CassetteWindow::render_tape()
{
    if (!tape_buf_) return;
    const double k = k_;
    auto X = [k](double v) { return static_cast<int>(std::lround(v * k)); };
    lv_canvas_fill_bg(canvas_, lv_color_hex(col::kWindow), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);
    if (loaded_) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = X(8);
        d.bg_color = lv_color_hex(col::kShell);
        d.border_color = lv_color_hex(col::kShellEdge);
        d.border_width = 1;
        const lv_area_t shell = area(0, 0, X(200), X(104));
        lv_draw_rect(&layer, &d, &shell);
        fill(&layer, area(X(10), X(8), X(180), X(46)), col::kPaper, X(3));
        fill(&layer, area(X(10), X(8), X(180), X(8)), col::kStripe, X(3));
        line(&layer, X(20), X(46), X(180), X(46), 1, col::kPaperLine);
        /* the handwritten name: the bold face (the toolkit has no handwriting face), as large as fits up to 22 */
        const lv_font_t *fl = font(Face::Bold, fit_px(Face::Bold, X(22), 0, label_, X(118), X(12), nullptr));
        text(&layer, fl, label_.c_str(), area(X(20), top_for_baseline(fl, X(40)), X(118), lv_font_get_line_height(fl)),
             col::kLabelInk);
        const lv_font_t *fs = font(Face::Bold, X(10));
        text(&layer, fs, side_.c_str(), area(X(100), top_for_baseline(fs, X(31)), X(76), lv_font_get_line_height(fs)),
             col::kLabelInk, LV_TEXT_ALIGN_RIGHT, std::max(0, X(1) - 1));
        const lv_font_t *ff = font(Face::Medium, X(9));
        text(&layer, ff, format_.c_str(), area(X(100), top_for_baseline(ff, X(51)), X(76), lv_font_get_line_height(ff)),
             col::kLabelGrey, LV_TEXT_ALIGN_RIGHT, std::max(0, X(1) - 1));
        lv_draw_rect_dsc_init(&d);
        d.radius = X(16);
        d.bg_color = lv_color_hex(col::kHubWindow);
        d.border_color = lv_color_hex(col::kShellEdge);
        d.border_width = 1;
        const lv_area_t hw = area(X(40), X(58), X(120), X(32));
        lv_draw_rect(&layer, &d, &hw);
        /* the head opening: a trapezoid as two triangles and a box */
        triangle(&layer, X(62), X(96), X(68), X(104), X(68), X(96), col::kHead);
        fill(&layer, area(X(68), X(96), X(132) - X(68), X(104) - X(96)), col::kHead, 0);
        triangle(&layer, X(132), X(96), X(138), X(96), X(132), X(104), col::kHead);
        for (double px : {82.0, 100.0, 118.0}) disc(&layer, X(px), X(100), std::max(1, X(2.2)), col::kHubWindow);
        disc(&layer, X(8), X(96), std::max(1, X(3)), col::kShellEdge);
        disc(&layer, X(192), X(96), std::max(1, X(3)), col::kShellEdge);
    }
    lv_canvas_finish_layer(canvas_, &layer);
    lv_obj_invalidate(canvas_);
}

void CassetteWindow::update(const RecorderVm &rec)
{
    const bool loaded = rec.input_present && rec.tape.loaded;
    if (loaded != loaded_ || rec.tape.label != label_ || rec.tape.side != side_ || rec.tape.format != format_) {
        loaded_ = loaded;
        label_ = rec.tape.label;
        side_ = rec.tape.side;
        format_ = rec.tape.format;
        render_tape();
    }
    show(canvas_, loaded_ && tape_buf_);
    show(hub_[0], loaded_);
    show(hub_[1], loaded_);
    show(empty_, !rec.input_present);

    if (rec.hub_dps[0] != speed_[0] || rec.hub_dps[1] != speed_[1]) {
        speed_[0] = rec.hub_dps[0];
        speed_[1] = rec.hub_dps[1];
        last_ms_ = lv_tick_get();
    }

    const bool counting = rec.transport == Transport::Countdown;
    show(count_, counting);
    if (counting) {
        set_text(count_number_, std::to_string(std::max(0, rec.countdown_left)));
        const float f = std::clamp(rec.countdown_fraction, 0.0f, 1.0f);
        if (std::fabs(f - ring_fraction_) > 0.002f) {
            ring_fraction_ = f;
            lv_obj_invalidate(count_ring_);
        }
    }

    const bool scanning = rec.transport == Transport::ScanForward || rec.transport == Transport::ScanReverse;
    const std::string bt = std::to_string(rec.scan_speed) + "\xC3\x97"; /* "5×" */
    if (scanning && (bt != badge_text_ || badge_reverse_ != (rec.transport == Transport::ScanReverse))) {
        badge_text_ = bt;
        badge_reverse_ = rec.transport == Transport::ScanReverse;
        lv_obj_invalidate(badge_);
    }
    show(badge_, scanning);
}

void CassetteWindow::set_visible(bool visible)
{
    visible_ = visible;
    last_ms_ = lv_tick_get();
}

void CassetteWindow::timer_cb(lv_timer_t *t)
{
    static_cast<CassetteWindow *>(lv_timer_get_user_data(t))->tick();
}

void CassetteWindow::tick()
{
    const std::uint32_t now = lv_tick_get();
    double dt = static_cast<double>(lv_tick_diff(now, last_ms_)) / 1000.0;
    last_ms_ = now;
    if (anim_.ticks > 0) { /* CASSETTE_ANIM_TICKS: a fixed step per tick, then the hubs stop (the scenes' fixed angles) */
        if (ticks_done_ >= anim_.ticks || (speed_[0] == 0 && speed_[1] == 0)) return; /* only turning ticks count */
        ++ticks_done_;
        dt = 1.0 / anim_.fps;
    }
    if (!visible_ || !loaded_) return;
    for (int i = 0; i < 2; ++i) {
        if (speed_[i] == 0) continue;
        /* both hubs get the same clamped step from the same speed: they stay in step */
        const double step = std::clamp(speed_[i] * dt, -kMaxStepDeg, kMaxStepDeg);
        angle_[i] = std::fmod(angle_[i] + step, 120.0);
        if (angle_[i] < 0) angle_[i] += 120.0;
        const int f = static_cast<int>(angle_[i] / 10.0) % 12;
        if (f != frame_[i] && frames_[f]) {
            frame_[i] = f;
            lv_image_set_src(hub_[i], frames_[f]);
            ++swaps_;
        }
    }
}

void CassetteWindow::ring_draw_cb(lv_event_t *e)
{
    auto *self = static_cast<CassetteWindow *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->count_ring_, &a);
    const int d = a.x2 - a.x1 + 1;
    const int cx = a.x1 + d / 2, cy = a.y1 + d / 2;
    /* the boards' ring: r = (d / 2 - 6), stroke 5, so the outer radius is d / 2 - 3.5 */
    const double sc = d / self->lay_.ring_d;
    const int width = std::max(2, static_cast<int>(std::lround(5 * sc)));
    const int outer = static_cast<int>(std::lround((self->lay_.ring_d / 2 - 6) * sc + width / 2.0));
    ring(layer, cx, cy, outer, width, 0, 360, col::kRingBack, false);
    const float f = self->ring_fraction_;
    if (f >= 0.999f) ring(layer, cx, cy, outer, width, 0, 360, col::kRingRed, false);
    else if (f > 0.001f) {
        const int end = (270 + static_cast<int>(std::lround(f * 360.0))) % 360;
        ring(layer, cx, cy, outer, width, 270, end, col::kRingRed, true);
    }
}

void CassetteWindow::badge_draw_cb(lv_event_t *e)
{
    auto *self = static_cast<CassetteWindow *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->badge_, &a);
    const double s = self->fit_.s;
    fill(layer, a, col::kAmber, self->fit_.px(4));
    const lv_font_t *f = font(Face::Bold, self->fit_.px(14));
    const int tw = text_width(f, self->badge_text_.c_str(), 0);
    /* the board's "◀◀" glyphs: two triangles about 9 x 11 px, 1 px apart */
    const int tri_w = static_cast<int>(std::lround(10 * s)), tri_h = static_cast<int>(std::lround(12 * s));
    const int gap = static_cast<int>(std::lround(5 * s)), sep = std::max(1, static_cast<int>(std::lround(s)));
    const int total = 2 * tri_w + sep + gap + tw;
    int x = (a.x1 + a.x2 + 1) / 2 - total / 2;
    const int cy = (a.y1 + a.y2 + 1) / 2;
    for (int i = 0; i < 2; ++i) {
        if (self->badge_reverse_) triangle(layer, x + tri_w, cy - tri_h / 2, x, cy, x + tri_w, cy + tri_h / 2, col::kWindow);
        else triangle(layer, x, cy - tri_h / 2, x + tri_w, cy, x, cy + tri_h / 2, col::kWindow);
        x += tri_w + (i == 0 ? sep : 0);
    }
    x += gap;
    const int lh = lv_font_get_line_height(f);
    text(layer, f, self->badge_text_.c_str(), area(x, cy - lh / 2, tw + 2, lh), col::kWindow);
}

void CassetteWindow::cancel_cb(lv_event_t *e)
{
    auto *self = static_cast<CassetteWindow *>(lv_event_get_user_data(e));
    if (self->on_cancel_) self->on_cancel_();
}

} // namespace ui
} // namespace cassette
