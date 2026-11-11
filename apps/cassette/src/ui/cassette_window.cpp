/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the cassette window (see cassette_window.hpp).
 */
#include "cassette_window.hpp"

#include "handwriting.hpp"
#include "paint.hpp"
#include "theme.hpp"
#include "ui_check.hpp"
#include "ui_format.hpp"

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
    if (const char *m = std::getenv("CASSETTE_LOAD_STEP_MS"); m && *m) o.load_step_ms = std::clamp(std::atoi(m), 0, 100);
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
    /* the window's inner area (inside its 3 px frame) clips the cassette object, which holds the canvas, the hubs and the name */
    const int bw = fit.px(3);
    clip_ = lv_obj_create(parent);
    plain(clip_);
    lv_obj_set_pos(clip_, wx + bw, wy + bw);
    lv_obj_set_size(clip_, ww - 2 * bw, wh - 2 * bw);
    cass_ = lv_obj_create(clip_);
    plain(cass_);
    base_y_ = ty - (wy + bw);
    lv_obj_set_pos(cass_, tx - (wx + bw), base_y_);
    lv_obj_set_size(cass_, cw, ch);
    box_w_ = static_cast<int>(std::lround(160 * k_)); /* the writing area: the label's width inside the 20 unit margins */
    canvas_ = lv_canvas_create(cass_);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(canvas_, 0, 0);
    tape_buf_ = lv_draw_buf_create(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch),
                                   lv_display_get_color_format(lv_obj_get_display(parent)), LV_STRIDE_AUTO);
    if (tape_buf_) lv_canvas_set_draw_buf(canvas_, tape_buf_);
    else lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);

    /* the hubs: 12 pre-rendered frames shared by both image objects */
    hub_box_ = static_cast<int>(std::ceil(24 * k_)) + 2;
    render_hub_frames();
    const double hx[2] = {64, 136};
    for (int i = 0; i < 2; ++i) {
        hub_[i] = lv_image_create(cass_);
        lv_obj_remove_flag(hub_[i], LV_OBJ_FLAG_CLICKABLE);
        const int cx = static_cast<int>(std::lround(hx[i] * k_)); /* in cass_ */
        const int cy = static_cast<int>(std::lround(74 * k_));
        lv_obj_set_pos(hub_[i], cx - hub_box_ / 2, cy - hub_box_ / 2);
        lv_obj_set_size(hub_[i], hub_box_, hub_box_);
        if (frames_[0]) lv_image_set_src(hub_[i], frames_[0]);
    }

    /* the label's ruled line under a one-line name (gone with a two-line name, which fills the writing area) */
    rule_ = box(cass_, static_cast<int>(std::lround(20 * k_)), static_cast<int>(std::lround(46 * k_)), box_w_, 1, col::kPaperLine, 0);
    lv_obj_add_flag(rule_, LV_OBJ_FLAG_HIDDEN);

    /* the take name: a label in the handwriting face inside a container whose width is the reveal */
    name_clip_ = lv_obj_create(cass_);
    plain(name_clip_);
    name_label_ = label(name_clip_, Face::Bold, std::max(8, static_cast<int>(std::lround(22 * k_))), col::kLabelInk, 0, 0, box_w_);
    lv_obj_add_flag(name_clip_, LV_OBJ_FLAG_HIDDEN);

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
    empty_msg_ = label(empty_, Face::Bold, fit.px(15 * lay.text_scale), col::kKeyText, pad, 0, ew, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(empty_msg_, LV_LABEL_LONG_MODE_WRAP);
    empty_hint_ = label(empty_, Face::Medium, fit.px(12 * lay.text_scale), col::kHint, pad, 0, ew, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(empty_hint_, LV_LABEL_LONG_MODE_WRAP);
    empty_h_ = wh;
    empty_gap_ = fit.px(8);
    set_empty_text(false);
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
    count_title_ = label(count_, Face::Bold, fit.px(12 * lay.text_scale), col::kScale, 0, fit.px(lay.label_y - w.y), ow,
                         LV_TEXT_ALIGN_CENTER, static_cast<int>(2.5 * fit.s * lay.text_scale));
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
    count_hint_ = label(count_, Face::Medium, fit.px(12 * lay.text_scale), col::kHint, 0, oh - fit.px(w.y + w.h - lay.hint_y), ow,
                        LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(count_hint_, "tap here, STOP or Esc cancels");
    lv_obj_add_flag(count_, LV_OBJ_FLAG_HIDDEN);

    if (anim_.enabled) timer_ = lv_timer_create(timer_cb, static_cast<uint32_t>(std::max(1, 1000 / anim_.fps)), this);
    last_ms_ = lv_tick_get();
    render_tape();
    layout_name();
}

void CassetteWindow::set_empty_text(bool bt)
{
    empty_bt_ = bt;
    lv_label_set_text(empty_msg_, bt ? kBtMissingMessage : kNoInputMessage);
    lv_label_set_text(empty_hint_, bt ? kBtMissingHint : kNoInputHint);
    const bool hidden = lv_obj_has_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    if (hidden) lv_obj_remove_flag(empty_, LV_OBJ_FLAG_HIDDEN); /* measured as shown */
    lv_obj_update_layout(empty_);
    if (hidden) lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    const int mh = lv_obj_get_height(empty_msg_), hh = lv_obj_get_height(empty_hint_);
    const int top = (empty_h_ - (mh + empty_gap_ + hh)) / 2;
    lv_obj_set_y(empty_msg_, top);
    lv_obj_set_y(empty_hint_, top + mh + empty_gap_);
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
    if (clip_ && lv_obj_is_valid(clip_)) lv_obj_delete(clip_);
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
        /* the label carries nothing but the handwritten name: a label of its own (name_label_), written in by the load
         * animation; the ruled line under a one-line name is an object of its own too (rule_) */
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
    /* what the model says */
    t_loaded_ = rec.input_present && rec.tape.loaded;
    if (t_label_ != rec.tape.label) t_label_ = rec.tape.label;
    const bool event = have_seq_ && rec.tape.load_seq != seq_;
    seq_ = rec.tape.load_seq;
    have_seq_ = true;
    if (event) {
        /* a LOAD: the animation (not when there is no cassette to show or no timer); when the list is still in front the
         * old cassette stays until the recorder shows (set_visible): LOAD closes the list right after it asks */
        if (load_.active()) finish_load();
        pending_load_ = false;
        if (t_loaded_ && anim_.enabled) {
            if (visible_) begin_load();
            else pending_load_ = true;
        } else {
            apply_target();
        }
    } else if (pending_load_) {
        if (!t_loaded_) {
            pending_load_ = false;
            apply_target();
        }
    } else if (load_.active()) {
        if (!t_loaded_) {
            finish_load();
            apply_target();
        } else if (swapped_ && (t_loaded_ != loaded_ || t_label_ != label_)) {
            apply_target(); /* a text changed under the animation (rename): shown at once */
        }
    } else {
        apply_target();
    }
    if (!rec.input_present && rec.bt_missing != empty_bt_) set_empty_text(rec.bt_missing);
    show(empty_, !rec.input_present);

    if (rec.hub_dps[0] != speed_[0] || rec.hub_dps[1] != speed_[1]) {
        speed_[0] = rec.hub_dps[0];
        speed_[1] = rec.hub_dps[1];
        last_ms_ = lv_tick_get();
    }

    const bool counting = rec.transport == Transport::Countdown;
    if (counting != counting_) {
        counting_ = counting;
        if (!load_.active()) check_exempt(cass_, counting_); /* the name lies under the dark panel and the big number */
    }
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

/* The cassette as the model says, at rest (the target's texts, in its place); during a load only the texts follow. */
void CassetteWindow::apply_target()
{
    const bool changed = t_loaded_ != loaded_ || t_label_ != label_;
    if (changed) {
        const bool canvas_changed = t_loaded_ != loaded_;
        loaded_ = t_loaded_;
        label_ = t_label_;
        if (canvas_changed) render_tape();
        layout_name();
    }
    if (load_.active()) return;
    if (cass_y_ != 0) {
        cass_y_ = 0;
        lv_obj_set_y(cass_, base_y_);
    }
    if (cass_shown_ != loaded_) {
        cass_shown_ = loaded_;
        show(cass_, loaded_);
    }
    show(canvas_, tape_buf_ != nullptr);
}

/* The font rung, the lines, the size and the place of the name label; the reveal at 100 %. The name has the label's whole
 * writing area (160 x 36 units under the red stripe): the largest rung of the handwriting ladder (at most 22 units, as
 * before) in which it fits on one line, else on two lines (ui_format.hpp layout_name_text). One line sits where it always
 * sat (baseline 40, on the ruled line at 46); two lines are centred in the writing area and the ruled line is left out. */
void CassetteWindow::layout_name()
{
    const bool has = loaded_ && !label_.empty();
    show(name_clip_, has);
    if (!has) {
        show(rule_, loaded_);
        return;
    }
    const double k = k_;
    auto X = [k](double v) { return static_cast<int>(std::lround(v * k)); };
    std::vector<int> rungs;
    for (int i = kHandRungCount - 1; i >= 0; --i) rungs.push_back(kHandRungs[i]);
    const NameLayout nl = layout_name_text(
        label_, rungs, hand_rung(X(22)), box_w_, X(36),
        [](int r, const std::string &s) { return text_width(handwriting_font(r), s.c_str(), 0); },
        [](int r) { return static_cast<int>(lv_font_get_line_height(handwriting_font(r))); });
    const lv_font_t *f = handwriting_font(nl.rung);
    const int lh = lv_font_get_line_height(f);
    std::string text;
    int tw = 0;
    for (std::size_t i = 0; i < nl.wrap.lines.size(); ++i) {
        if (i) text += '\n';
        text += nl.wrap.lines[i];
        tw = std::max(tw, text_width(f, nl.wrap.lines[i].c_str(), 0));
    }
    const bool two = nl.wrap.lines.size() > 1;
    show(rule_, !two);
    lv_obj_set_style_text_font(name_label_, f, 0);
    lv_obj_set_style_text_line_space(name_label_, nl.pitch - lh, 0);
    lv_label_set_long_mode(name_label_, LV_LABEL_LONG_MODE_CLIP);
    set_text(name_label_, text);
    lv_obj_set_size(name_label_, box_w_, nl.text_h);
    lv_obj_set_pos(name_clip_, X(20), two ? X(17) + (X(36) - nl.text_h) / 2 : top_for_baseline(f, X(40)));
    lv_obj_set_height(name_clip_, nl.text_h);
    name_w_ = std::min(box_w_, tw + 2);
    reveal_w_ = -1;
    set_reveal(name_w_);
}

void CassetteWindow::set_reveal(int w)
{
    w = std::max(0, w);
    if (w == reveal_w_) return;
    reveal_w_ = w;
    lv_obj_set_width(name_clip_, w);
}

void CassetteWindow::begin_load()
{
    const bool had_old = loaded_;
    load_.start(had_old);
    swapped_ = false;
    landed_ = false;
    load_ticks_ = 0;
    load_last_ms_ = lv_tick_get();
    check_exempt(cass_, true); /* the scenes' layout check: the cassette is out of place and the name is cut on purpose */
    if (!had_old) swap_to_target(); /* nothing to drop out: the new cassette falls in at once */
    apply_pose();
}

/* The new cassette's texts into the canvas and the name label; called when nothing of the cassette is in view. */
void CassetteWindow::swap_to_target()
{
    const bool canvas_changed = t_loaded_ != loaded_ || !load_.had_old();
    loaded_ = t_loaded_;
    label_ = t_label_;
    if (canvas_changed) render_tape();
    /* the hubs at rest on their first frame */
    angle_[0] = angle_[1] = 0;
    for (int i = 0; i < 2; ++i)
        if (frame_[i] != 0 && frames_[0]) {
            frame_[i] = 0;
            lv_image_set_src(hub_[i], frames_[0]);
        }
    layout_name();
    set_reveal(0);
    swapped_ = true;
}

void CassetteWindow::apply_pose()
{
    const LoadPose p = load_.pose();
    if (!swapped_ && p.phase != LoadPose::Phase::Out) swap_to_target();
    bool shown = true;
    int y = 0;
    switch (p.phase) {
    case LoadPose::Phase::Out: y = static_cast<int>(std::lround(p.old_y * k_)); break;
    case LoadPose::Phase::Gap: shown = false; break;
    default: y = static_cast<int>(std::lround(p.new_y * k_)); break;
    }
    if (shown != cass_shown_) {
        cass_shown_ = shown;
        show(cass_, shown);
    }
    if (shown && y != cass_y_) { /* the one object that moves */
        cass_y_ = y;
        lv_obj_set_y(cass_, base_y_ + y);
    }
    if (p.phase == LoadPose::Phase::Out) set_reveal(name_w_); /* the old name stays written */
    else if (swapped_) set_reveal(static_cast<int>(std::lround(name_w_ * p.name)));
    if (p.landed && !landed_) {
        landed_ = true;
        if (on_landed_) on_landed_();
    }
    if (!load_.active()) { /* over: the cassette at rest, the name written, the checks watch it again */
        check_exempt(cass_, counting_);
        if (cass_y_ != 0) {
            cass_y_ = 0;
            lv_obj_set_y(cass_, base_y_);
        }
        cass_shown_ = loaded_;
        show(cass_, loaded_);
        show(canvas_, tape_buf_ != nullptr);
        set_reveal(name_w_);
    }
}

void CassetteWindow::finish_load()
{
    if (!load_.active()) return;
    load_.finish();
    apply_pose();
}

void CassetteWindow::step_load(std::uint32_t now)
{
    int dt;
    if (anim_.ticks > 0) { /* the scenes: a fixed step per tick, then the frame stays */
        if (load_ticks_ >= anim_.ticks) return;
        ++load_ticks_;
        dt = anim_.load_step_ms;
    } else {
        dt = static_cast<int>(std::min<std::uint32_t>(100, lv_tick_diff(now, load_last_ms_)));
    }
    load_last_ms_ = now;
    if (!visible_) load_.finish(); /* another page in front: nothing to animate */
    else load_.advance(dt);
    apply_pose();
}

void CassetteWindow::set_visible(bool visible)
{
    visible_ = visible;
    last_ms_ = lv_tick_get();
    if (visible && pending_load_) {
        pending_load_ = false;
        if (t_loaded_ && anim_.enabled) {
            begin_load();
        } else {
            apply_target();
            if (on_landed_) on_landed_(); /* the strip text and the counter were held for the load */
        }
    }
}

void CassetteWindow::timer_cb(lv_timer_t *t)
{
    static_cast<CassetteWindow *>(lv_timer_get_user_data(t))->tick();
}

void CassetteWindow::tick()
{
    const std::uint32_t now = lv_tick_get();
    if (load_.active()) {
        step_load(now);
        last_ms_ = now;
        return; /* the hubs stay still while a cassette is loaded */
    }
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
    const double s = self->fit_.s * self->lay_.text_scale;
    fill(layer, a, col::kAmber, self->fit_.px(4 * self->lay_.text_scale));
    const lv_font_t *f = font(Face::Bold, self->fit_.px(14 * self->lay_.text_scale));
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
