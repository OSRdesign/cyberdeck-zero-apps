/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the blocks of the speed test, Wake-on-LAN and radar pages (see widgets_03.hpp).
 */
#include "widgets_03.hpp"

#include "style.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace nt {
namespace ui {

namespace {

constexpr int kNeedleMs = 180;  /* the needle eases to a new value in this time */
constexpr int kFadeMs = 200;    /* the numbers of the dial cross-fade when the scale steps */
constexpr std::uint32_t kMinorTick = 0x555555;
constexpr std::uint32_t kGrey = 0x6A6A6A;      /* a second of the wake strip that waited */
constexpr std::uint32_t kCellEmpty = 0x1C1C1C; /* a second to come */

int space_s() { return cp0_ui_tok(CP0_TOK_SPACE_S); }
int space_m() { return cp0_ui_tok(CP0_TOK_SPACE_M); }

lv_obj_t *label_at(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb, lv_label_long_mode_t mode, int x, int y, int w, int h)
{
    lv_obj_t *l = make_label(parent, font, rgb);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, std::max(1, w), std::max(1, h));
    return l;
}

/* an arc between two angles in degrees around straight up (clockwise) */
void arc(lv_layer_t *layer, int cx, int cy, int radius, int width, double from_up, double to_up, std::uint32_t rgb, int opa,
         bool rounded)
{
    const auto lv = [](double deg) { return static_cast<lv_value_precise_t>(std::fmod(std::fmod(deg + 270.0, 360.0) + 360.0, 360.0)); };
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.width = width;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = static_cast<uint16_t>(std::max(1, radius));
    d.start_angle = lv(from_up);
    d.end_angle = lv(to_up);
    d.opa = static_cast<lv_opa_t>(std::min(255, std::max(0, opa)));
    d.rounded = rounded ? 1 : 0;
    lv_draw_arc(layer, &d);
}

void triangle(lv_layer_t *layer, double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgb)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = static_cast<lv_value_precise_t>(x1);
    d.p[0].y = static_cast<lv_value_precise_t>(y1);
    d.p[1].x = static_cast<lv_value_precise_t>(x2);
    d.p[1].y = static_cast<lv_value_precise_t>(y2);
    d.p[2].x = static_cast<lv_value_precise_t>(x3);
    d.p[2].y = static_cast<lv_value_precise_t>(y3);
    d.color = lv_color_hex(rgb);
    d.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &d);
}

bool overlap(const Box &a, const Box &b, int margin = 0)
{
    return a.x < b.x + b.w + margin && b.x < a.x + a.w + margin && a.y < b.y + b.h + margin && b.y < a.y + a.h + margin;
}

std::string box_text(const Box &b)
{
    return "(" + std::to_string(b.x) + "," + std::to_string(b.y) + " " + std::to_string(b.w) + "x" + std::to_string(b.h) + ")";
}

} // namespace

/* ================================================================ the speed gauge */

SpeedLiveBlock::SpeedLiveBlock(int w, int h, int inset, const lv_font_t *caption, const lv_font_t *small, const lv_font_t *big)
    : w_(w), h_(h), inset_(inset), caption_(caption), small_(small), big_(big)
{
    layout_ = speed_layout(std::max(1, w - 2 * inset), h, line_height(caption), line_height(big), space_s(), portrait());
    radii_ = gauge_radii(layout_.radius);
    scaler_.reset();
    labels_scale_ = scaler_.scale;
    num_h_ = line_height(small);
    centre_w_ = std::max(20, radii_.inner * 2 * 82 / 100);
    const int hub_below = static_cast<int>(0.08 * layout_.radius);
    phase_y_ = layout_.cy + hub_below;
    value_y_ = phase_y_ + line_height(caption);
    unit_y_ = value_y_ + line_height(big);
}

SpeedLiveBlock::~SpeedLiveBlock()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
}

void SpeedLiveBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    host_ = host;
    const int gx = inset_ + layout_.gauge.x, gy = layout_.gauge.y;
    gauge_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_gauge(layer, a); });
    gauge_->place(gx, gy, layout_.gauge.w, layout_.gauge.h);
    for (int i = 0; i < 6; ++i) ticks_.push_back(make_label(host, small_, kLabel));
    place_numbers();
    const int centre_x = inset_ + layout_.cx - centre_w_ / 2;
    phase_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_CLIP, centre_x, phase_y_, centre_w_, line_height(caption_));
    value_ = label_at(host, big_, kText, LV_LABEL_LONG_MODE_CLIP, centre_x, value_y_, centre_w_, line_height(big_));
    unit_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_CLIP, centre_x, unit_y_, centre_w_, line_height(caption_));
    for (lv_obj_t *l : {phase_, value_, unit_}) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    const Box &c = layout_.chart;
    chart_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_chart(layer, a); });
    chart_->place(inset_ + c.x, c.y, c.w, c.h);
    const int pad = space_s();
    chart_top_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, inset_ + c.x + pad + 2, c.y + 2, std::max(1, c.w / 2), num_h_);
    chart_zero_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, inset_ + c.x + pad + 2, c.y + c.h - num_h_ - 2, 24, num_h_);
    set_text_if(chart_zero_, "0");
    chart_empty_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_WRAP, inset_ + c.x + pad + 24, c.y + std::max(0, (c.h - 2 * num_h_) / 2),
                            std::max(1, c.w - 2 * pad - 48), 2 * num_h_);
    lv_obj_set_style_text_align(chart_empty_, LV_TEXT_ALIGN_CENTER, 0);
    set_text_if(chart_empty_, "Chart appears during a run");

    const Box &p = layout_.progress;
    const int label_w = text_width("10 s of 10 s", caption_) + pad;
    progress_label_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_CLIP, inset_ + p.x, p.y + (p.h - line_height(caption_)) / 2,
                               label_w, line_height(caption_));
    progress_bar_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_progress(layer, a); });
    progress_bar_->place(inset_ + p.x + label_w, p.y, std::max(1, p.w - label_w), p.h);
    apply_texts();
    if (!std::isfinite(shown_)) shown_ = 0;
}

void SpeedLiveBlock::detach()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    gauge_.reset();
    chart_.reset();
    progress_bar_.reset();
    ticks_.clear();
    phase_ = value_ = unit_ = chart_top_ = chart_zero_ = chart_empty_ = progress_label_ = nullptr;
    host_ = nullptr;
}

void SpeedLiveBlock::anim_cb(lv_timer_t *timer)
{
    auto *self = static_cast<SpeedLiveBlock *>(lv_timer_get_user_data(timer));
    if (self) self->step();
}

void SpeedLiveBlock::start_anim()
{
    if (!timer_ && host_) timer_ = lv_timer_create(anim_cb, 33, this);
}

void SpeedLiveBlock::step()
{
    bool busy = false;
    const std::uint32_t e = lv_tick_elaps(anim_t0_);
    if (e < static_cast<std::uint32_t>(kNeedleMs)) {
        shown_ = ease_out(from_, to_, static_cast<double>(e), kNeedleMs);
        busy = true;
    } else {
        shown_ = to_;
    }
    if (fading_) {
        const std::uint32_t f = lv_tick_elaps(fade_t0_);
        int opa = 255;
        if (f < static_cast<std::uint32_t>(kFadeMs / 2)) {
            opa = 255 - static_cast<int>(255 * f / (kFadeMs / 2));
        } else {
            if (labels_scale_ != scaler_.scale) {
                labels_scale_ = scaler_.scale;
                place_numbers();
            }
            opa = f >= static_cast<std::uint32_t>(kFadeMs) ? 255 : static_cast<int>(255 * (f - kFadeMs / 2) / (kFadeMs / 2));
        }
        fade_opa_ = opa;
        for (lv_obj_t *l : ticks_) lv_obj_set_style_text_opa(l, static_cast<lv_opa_t>(opa), 0);
        if (f >= static_cast<std::uint32_t>(kFadeMs)) fading_ = false;
        else busy = true;
        if (labels_scale_ != scaler_.scale && !fading_) { /* the scale stepped again while fading */
            fading_ = true;
            fade_t0_ = lv_tick_get();
            busy = true;
        }
    }
    if (gauge_) gauge_->invalidate();
    if (!busy && timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void SpeedLiveBlock::reset()
{
    scaler_.reset();
    labels_scale_ = scaler_.scale;
    fading_ = false;
    fade_opa_ = 255;
    shown_ = from_ = to_ = 0;
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    for (lv_obj_t *l : ticks_) lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    if (!ticks_.empty()) place_numbers();
    if (gauge_) gauge_->invalidate();
    if (chart_) chart_->invalidate();
}

void SpeedLiveBlock::update(const SpeedSnapshot &s, double target_s)
{
    target_s_ = target_s;
    const bool running = s.running();
    if (running && !was_running_) reset();
    was_running_ = running;
    snap_ = s;
    bool scale_changed = false;
    const bool moving = running && (s.phase == SpeedSnapshot::Phase::Download || s.phase == SpeedSnapshot::Phase::Upload);
    if (moving) {
        scale_changed |= scaler_.feed(s.now_mbps);
        const std::vector<SpeedSample> &hist = s.phase == SpeedSnapshot::Phase::Download ? s.result.down.history : s.result.up.history;
        for (const SpeedSample &p : hist) scale_changed |= scaler_.feed(p.mbps);
    }
    const double target = moving ? gauge_fraction(s.now_mbps, scaler_.scale) : 0.0;
    if (std::fabs(target - to_) > 1e-4 || scale_changed) {
        from_ = shown_;
        to_ = target;
        anim_t0_ = lv_tick_get();
        start_anim();
    }
    if (scaler_.scale != labels_scale_ && !fading_ && !ticks_.empty()) {
        fading_ = true;
        fade_t0_ = lv_tick_get();
        start_anim();
    }
    progress_ = speed_progress(s, target_s);
    progress_text_ = speed_progress_text(s, target_s);
    apply_texts();
    if (chart_) chart_->invalidate();
    if (progress_bar_) progress_bar_->invalidate();
    if (gauge_ && !timer_) gauge_->invalidate();
}

void SpeedLiveBlock::apply_texts()
{
    if (!phase_) return;
    set_text_if(phase_, speed_phase_label(snap_));
    const bool latency = snap_.running() && snap_.phase == SpeedSnapshot::Phase::Latency;
    const bool moving = snap_.running() && (snap_.phase == SpeedSnapshot::Phase::Download || snap_.phase == SpeedSnapshot::Phase::Upload);
    if (latency) {
        set_text_if(value_, snap_.result.latency_ok ? speed_value_text(snap_.result.latency_ms) : std::string("--"));
        set_text_if(unit_, "ms");
    } else {
        set_text_if(value_, moving ? speed_value_text(snap_.now_mbps) : std::string("--"));
        set_text_if(unit_, "Mbit/s");
    }
    set_text_if(chart_top_, gauge_number(scaler_.scale) + " Mbit/s");
    const std::vector<SpeedSample> &hist = snap_.phase == SpeedSnapshot::Phase::Upload ? snap_.result.up.history : snap_.result.down.history;
    if (chart_empty_) {
        if (moving && !hist.empty()) lv_obj_add_flag(chart_empty_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(chart_empty_, LV_OBJ_FLAG_HIDDEN);
    }
    set_text_if(progress_label_, progress_text_);
}

/* the six numbers of the dial for labels_scale_, centred on their ticks */
void SpeedLiveBlock::place_numbers()
{
    const std::vector<GaugeTick> ticks = gauge_ticks(labels_scale_);
    int k = 0;
    for (const GaugeTick &t : ticks) {
        if (!t.major || k >= static_cast<int>(ticks_.size())) continue;
        const double deg = -kGaugeHalfSweepDeg + kGaugeSweepDeg * (static_cast<double>(k) / 5.0);
        double x, y;
        polar_point(inset_ + layout_.cx, layout_.cy, radii_.label, deg, x, y);
        const int tw = text_width(t.label.c_str(), small_) + 2;
        num_w_[k] = tw;
        lv_obj_t *l = ticks_[static_cast<std::size_t>(k)];
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(l, tw, num_h_);
        lv_obj_set_pos(l, static_cast<int>(std::lround(x)) - tw / 2, static_cast<int>(std::lround(y)) - num_h_ / 2);
        set_text_if(l, t.label);
        ++k;
    }
}

std::string SpeedLiveBlock::layout_problem() const
{
    std::vector<Box> boxes;
    std::vector<std::string> names;
    const std::vector<GaugeTick> ticks = gauge_ticks(labels_scale_);
    int k = 0;
    for (const GaugeTick &t : ticks) {
        if (!t.major) continue;
        const double deg = -kGaugeHalfSweepDeg + kGaugeSweepDeg * (static_cast<double>(k) / 5.0);
        double x, y;
        polar_point(layout_.cx, layout_.cy, radii_.label, deg, x, y);
        const int tw = text_width(t.label.c_str(), small_) + 2;
        boxes.push_back({static_cast<int>(std::lround(x)) - tw / 2, static_cast<int>(std::lround(y)) - num_h_ / 2, tw, num_h_});
        names.push_back("number " + t.label);
        ++k;
    }
    const int phase_w = std::min(centre_w_, text_width(speed_phase_label(snap_).c_str(), caption_));
    const int value_w = std::min(centre_w_, text_width("1000", big_));
    const int unit_w = std::min(centre_w_, text_width("Mbit/s", caption_));
    boxes.push_back({layout_.cx - phase_w / 2, phase_y_, phase_w, line_height(caption_)});
    names.push_back("phase");
    boxes.push_back({layout_.cx - value_w / 2, value_y_, value_w, line_height(big_)});
    names.push_back("value");
    boxes.push_back({layout_.cx - unit_w / 2, unit_y_, unit_w, line_height(caption_)});
    names.push_back("unit");
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const Box &b = boxes[i];
        if (b.x < layout_.gauge.x || b.y < 0 || b.x + b.w > layout_.gauge.x + layout_.gauge.w || b.y + b.h > h_)
            return names[i] + " " + box_text(b) + " leaves the gauge's box";
        for (std::size_t j = i + 1; j < boxes.size(); ++j)
            if (overlap(b, boxes[j])) return names[i] + " " + box_text(b) + " overlaps " + names[j] + " " + box_text(boxes[j]);
    }
    return "";
}

void SpeedLiveBlock::paint_gauge(lv_layer_t *layer, const lv_area_t &a)
{
    const int cx = a.x1 + layout_.cx - layout_.gauge.x, cy = a.y1 + layout_.cy - layout_.gauge.y;
    const GaugeRadii &R = radii_;
    arc(layer, cx, cy, R.outer, R.track, -kGaugeHalfSweepDeg, kGaugeHalfSweepDeg, kTrack, LV_OPA_COVER, true);
    if (shown_ > 0.004) arc(layer, cx, cy, R.outer, R.track, -kGaugeHalfSweepDeg, -kGaugeHalfSweepDeg + kGaugeSweepDeg * shown_, kBlue, LV_OPA_COVER, true);
    for (int i = 0; i <= 10; ++i) {
        const double deg = -kGaugeHalfSweepDeg + kGaugeSweepDeg * i / 10.0;
        const bool major = i % 2 == 0;
        const int len = major ? R.tick_major : R.tick_minor;
        double x1, y1, x2, y2;
        polar_point(cx, cy, R.inner - 2, deg, x1, y1);
        polar_point(cx, cy, R.inner - 2 - len, deg, x2, y2);
        paint_line(layer, static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)), static_cast<int>(std::lround(x2)),
                   static_cast<int>(std::lround(y2)), major ? kLabel : kMinorTick, major ? 2 : 1);
    }
    /* the needle: a tapered triangle from the hub, with a short tail */
    const double deg = -kGaugeHalfSweepDeg + kGaugeSweepDeg * shown_;
    const double bw = std::max(3.0, R.outer * 0.045);
    double tx, ty, b1x, b1y, b2x, b2y, ex, ey;
    polar_point(cx, cy, R.needle, deg, tx, ty);
    polar_point(cx, cy, bw, deg + 90.0, b1x, b1y);
    polar_point(cx, cy, bw, deg - 90.0, b2x, b2y);
    polar_point(cx, cy, R.hub * 1.5, deg + 180.0, ex, ey);
    triangle(layer, tx, ty, b1x, b1y, b2x, b2y, kText);
    triangle(layer, ex, ey, b1x, b1y, b2x, b2y, kText);
    paint_dot(layer, cx, cy, R.hub, kText);
    paint_dot(layer, cx, cy, std::max(2, R.hub / 2), kBlue);
}

void SpeedLiveBlock::paint_chart(lv_layer_t *layer, const lv_area_t &a)
{
    paint_fill(layer, a, kBlack, cp0_ui_tok(CP0_TOK_RADIUS_S));
    paint_ring(layer, a, kBorder, 1, cp0_ui_tok(CP0_TOK_RADIUS_S));
    const int pad = std::max(3, space_s() / 2);
    const int ix1 = a.x1 + pad, ix2 = a.x2 - pad, iy1 = a.y1 + pad, iy2 = a.y2 - pad;
    const int iw = ix2 - ix1 + 1, ih = iy2 - iy1 + 1;
    if (iw < 8 || ih < 8) return;
    paint_line(layer, ix1, iy1 + ih / 2, ix2, iy1 + ih / 2, kTrack, 1);
    paint_line(layer, ix1, iy1, ix2, iy1, kTrack, 1);
    paint_line(layer, ix1, iy2, ix2, iy2, kBorder, 1);
    const bool moving = snap_.running() && (snap_.phase == SpeedSnapshot::Phase::Download || snap_.phase == SpeedSnapshot::Phase::Upload);
    if (!moving) return;
    const std::vector<SpeedSample> &hist = snap_.phase == SpeedSnapshot::Phase::Download ? snap_.result.down.history : snap_.result.up.history;
    const double scale = scaler_.scale > 0 ? scaler_.scale : 10.0;
    const double span = target_s_ > 0 ? target_s_ : 8.0;
    int px = ix1, py = iy2;
    bool have = false;
    const int lw = std::max(2, space_s() / 3);
    for (const SpeedSample &s : hist) {
        const int x = ix1 + static_cast<int>(std::lround(std::min(1.0, s.t_s / span) * (iw - 1)));
        const int y = iy2 - static_cast<int>(std::lround(gauge_fraction(s.mbps, scale) * (ih - 1)));
        if (have && x > px) {
            paint_fill(layer, lv_area_t{px, std::min(py, y), x, iy2}, kBlue, 0, LV_OPA_30);
            paint_line(layer, px, py, x, y, kBlue, lw, true);
        }
        px = x;
        py = y;
        have = true;
    }
    if (have) paint_dot(layer, px, py, std::max(3, lw + 1), kText);
}

void SpeedLiveBlock::paint_progress(lv_layer_t *layer, const lv_area_t &a)
{
    const int bar_h = std::max(4, line_height(caption_) / 4);
    const int y = (a.y1 + a.y2) / 2 - bar_h / 2;
    const lv_area_t track{a.x1, y, a.x2, y + bar_h - 1};
    paint_fill(layer, track, kTrack, bar_h / 2);
    const int fill = static_cast<int>(std::lround(progress_ * (a.x2 - a.x1 + 1)));
    if (fill > 0) paint_fill(layer, lv_area_t{a.x1, y, std::min(a.x2, a.x1 + std::max(bar_h, fill) - 1), y + bar_h - 1}, kBlue, bar_h / 2);
}

/* ================================================================ the results */

SpeedResultsBlock::SpeedResultsBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *big)
    : w_(w), inset_(inset), caption_(caption), big_(big)
{
    pad_ = space_s();
    h_ = pad_ + line_height(caption) + line_height(big) + pad_;
}

SpeedResultsBlock::~SpeedResultsBlock() = default;

void SpeedResultsBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)h;
    const int col = std::max(1, (w - 2 * inset_) / 2);
    const char *names[2] = {"Download", "Upload"};
    lv_obj_t **labels[2] = {&down_label_, &up_label_};
    for (int i = 0; i < 2; ++i) {
        const int x = inset_ + i * col;
        lv_obj_t *cap = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_CLIP, x, pad_, col, line_height(caption_));
        set_text_if(cap, names[i]);
        *labels[i] = label_at(host, big_, kText, LV_LABEL_LONG_MODE_DOTS, x, pad_ + line_height(caption_), col, line_height(big_));
    }
    set(down_, up_);
}

void SpeedResultsBlock::detach()
{
    down_label_ = up_label_ = nullptr;
}

void SpeedResultsBlock::set(const std::string &download, const std::string &upload)
{
    down_ = download;
    up_ = upload;
    if (down_label_) set_text_if(down_label_, down_ + (down_ == "--" ? std::string() : " Mbit/s"));
    if (up_label_) set_text_if(up_label_, up_ + (up_ == "--" ? std::string() : " Mbit/s"));
}

GradeBlock::GradeBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *big)
    : w_(w), inset_(inset), caption_(caption), big_(big)
{
    pad_ = space_s();
    badge_ = std::max(line_height(big) * 3 / 2, line_height(caption) * 2);
    text_x_ = inset + badge_ + 2 * space_m();
    text_w_ = std::max(1, w - text_x_ - inset);
    const BloatGrade all[] = {BloatGrade::A, BloatGrade::B, BloatGrade::C, BloatGrade::D, BloatGrade::F};
    std::vector<std::string> lines;
    for (const BloatGrade g : all) lines.push_back(grade_line(g));
    lines.push_back("Not measured: No ping target: the latency under load is not measured");
    for (const std::string &l : lines) {
        lv_point_t size{};
        lv_text_get_size(&size, l.c_str(), caption, 0, 0, text_w_, LV_TEXT_FLAG_NONE);
        line_h_ = std::max(line_h_, static_cast<int>(size.y));
    }
    fits_ = line_h_ <= 3 * line_height(caption);
    h_ = 2 * pad_ + std::max(badge_, line_height(caption) + 2 + line_h_);
}

GradeBlock::~GradeBlock() = default;

void GradeBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    const int by = (h_ - badge_) / 2;
    shape_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) {
        const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = (a.x2 - a.x1) / 2;
        switch (shown_.tone) {
        case Tone::Ok: paint_dot(layer, cx, cy, r, kOk); break;
        case Tone::Warn: triangle(layer, cx, a.y1 + 1, a.x1 + 1, a.y2 - 1, a.x2 - 1, a.y2 - 1, kWarn); break;
        case Tone::Bad: paint_fill(layer, lv_area_t{a.x1 + 2, a.y1 + 2, a.x2 - 2, a.y2 - 2}, kBad, std::max(3, r / 5)); break;
        default: paint_ring(layer, lv_area_t{cx - r, cy - r, cx + r, cy + r}, kLabel, 3, LV_RADIUS_CIRCLE); break;
        }
    });
    shape_->place(inset_, by, badge_, badge_);
    const int lh = line_height(big_);
    const int ly = by + (badge_ - lh) / 2 + (shown_.tone == Tone::Warn ? badge_ / 8 : 0);
    letter_ = label_at(host, big_, kInk, LV_LABEL_LONG_MODE_CLIP, inset_, ly, badge_, lh);
    lv_obj_set_style_text_align(letter_, LV_TEXT_ALIGN_CENTER, 0);
    head_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_DOTS, text_x_, pad_, text_w_, line_height(caption_));
    set_text_if(head_, "Bufferbloat grade");
    line_ = label_at(host, caption_, kText, LV_LABEL_LONG_MODE_WRAP, text_x_, pad_ + line_height(caption_) + 2, text_w_, line_h_);
    set(shown_);
}

void GradeBlock::detach()
{
    shape_.reset();
    letter_ = line_ = head_ = nullptr;
}

void GradeBlock::set(const SpeedGrade &grade)
{
    shown_ = grade;
    if (shape_) shape_->invalidate();
    if (!letter_) return;
    set_text_if(letter_, grade.letter);
    set_color_if(letter_, grade.tone == Tone::Normal ? kLabel : kInk);
    const int lh = line_height(big_), by = (h_ - badge_) / 2;
    lv_obj_set_y(letter_, by + (badge_ - lh) / 2 + (grade.tone == Tone::Warn ? badge_ / 8 : 0));
    set_text_if(line_, grade.line);
    set_color_if(line_, tone_rgb(grade.tone, kText));
}

/* ================================================================ the wake watch */

WakeBlock::WakeBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *big)
    : w_(w), inset_(inset), caption_(caption), big_(big)
{
    pad_ = space_s();
    lh_ = line_height(caption);
    big_h_ = line_height(big);
    ring_ = std::clamp(big_h_ * 3 / 2, 40, 72);
    strip_h_ = std::max(10, lh_ / 2);
    text_x_ = inset + ring_ + 2 * space_m();
    text_w_ = std::max(1, w - text_x_ - inset);
    const int top = std::max(ring_, big_h_ + 2 * lh_ + pad_ / 2);
    strip_y_ = pad_ + top + pad_;
    h_ = strip_y_ + strip_h_ + 2 + lh_ + pad_;
    cells_.assign(kWakeStripCells, StripCell::Future);
}

WakeBlock::~WakeBlock() = default;

void WakeBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    ring_painter_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_ring(layer, a); });
    ring_painter_->place(inset_, pad_ + (std::max(ring_, big_h_ + 2 * lh_ + pad_ / 2) - ring_) / 2, ring_, ring_);
    word_ = label_at(host, big_, kText, LV_LABEL_LONG_MODE_DOTS, text_x_, pad_, text_w_, big_h_);
    detail_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_WRAP, text_x_, pad_ + big_h_ + pad_ / 2, text_w_, 2 * lh_);
    strip_painter_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_strip(layer, a); });
    strip_painter_->place(inset_, strip_y_, std::max(1, w_ - 2 * inset_), strip_h_);
    from_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_CLIP, inset_, strip_y_ + strip_h_ + 2, std::max(1, w_ - 2 * inset_) - 60, lh_);
    to_ = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_CLIP, w_ - inset_ - 60, strip_y_ + strip_h_ + 2, 60, lh_);
    lv_obj_set_style_text_align(to_, LV_TEXT_ALIGN_RIGHT, 0);
    set_text_if(to_, "120 s");
    set(snap_, has_host_);
}

void WakeBlock::detach()
{
    ring_painter_.reset();
    strip_painter_.reset();
    word_ = detail_ = from_ = to_ = nullptr;
}

void WakeBlock::set(const WolSnapshot &snapshot, bool has_host)
{
    snap_ = snapshot;
    status_ = wake_status(snapshot);
    cells_ = wake_strip(snapshot);
    has_host_ = has_host;
    if (!word_) return;
    set_text_if(word_, status_.word);
    set_color_if(word_, tone_rgb(status_.tone, kText));
    std::string detail = status_.detail;
    if (detail.empty() && snapshot.state == WolSnapshot::State::Idle)
        detail = has_host ? "Press Wake now: the strip below shows the seconds until it answers." : "Press Wake now to send the magic packet.";
    set_text_if(detail_, detail);
    set_text_if(from_, has_host ? std::string("0 s") : std::string("Add an address to watch to see when it wakes up"));
    if (has_host) lv_obj_remove_flag(to_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(to_, LV_OBJ_FLAG_HIDDEN);
    ring_painter_->invalidate();
    strip_painter_->invalidate();
}

bool WakeBlock::detail_fits() const
{
    if (!detail_) return true;
    lv_point_t size{};
    lv_text_get_size(&size, lv_label_get_text(detail_), caption_, 0, 0, text_w_, LV_TEXT_FLAG_NONE);
    return size.y <= 2 * lh_;
}

void WakeBlock::tick(std::int64_t now_ms)
{
    now_ = now_ms;
    if (status_.spinning && ring_painter_) ring_painter_->invalidate();
}

void WakeBlock::paint_ring(lv_layer_t *layer, const lv_area_t &a)
{
    const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = std::max(6, (a.x2 - a.x1) / 2 - 1);
    switch (status_.kind) {
    case WakeKind::None: draw_mark(layer, cx, cy, r, Mark::None, kLabel); break;
    case WakeKind::Sending:
    case WakeKind::Waking: draw_spinner(layer, cx, cy, r, kBlue, static_cast<int>(now_ * 360 / 1200)); break;
    case WakeKind::Sent: draw_mark(layer, cx, cy, r, Mark::Arrow, kBlue); break;
    case WakeKind::Awake: draw_mark(layer, cx, cy, r, Mark::Check, kOk); break;
    case WakeKind::NoAnswer: draw_mark(layer, cx, cy, r, Mark::Dash, kWarn); break;
    case WakeKind::Failed: draw_mark(layer, cx, cy, r, Mark::Cross, kBad); break;
    case WakeKind::Stopped: draw_mark(layer, cx, cy, r, Mark::Dash, kLabel); break;
    }
}

void WakeBlock::paint_strip(lv_layer_t *layer, const lv_area_t &a)
{
    if (!has_host_) return;
    const int n = static_cast<int>(cells_.size());
    const int w = a.x2 - a.x1 + 1;
    if (n <= 0 || w < n) return;
    for (int i = 0; i < n; ++i) {
        const int x1 = a.x1 + i * w / n;
        int x2 = a.x1 + (i + 1) * w / n - 1;
        if (x2 - x1 >= 3) --x2; /* a hairline between the seconds when they are wide enough */
        std::uint32_t rgb = kCellEmpty;
        switch (cells_[static_cast<std::size_t>(i)]) {
        case StripCell::Waiting: rgb = kGrey; break;
        case StripCell::Awake: rgb = kOk; break;
        case StripCell::Future: break;
        }
        paint_fill(layer, lv_area_t{x1, a.y1, x2, a.y2}, rgb, 1);
    }
}

/* ================================================================ a saved machine in the list */

MachineRowView::MachineRowView(int w, int inset, const lv_font_t *main, const lv_font_t *small, int min_h)
    : w_(w), inset_(inset), main_(main), small_(small)
{
    const int pad = std::max(4, space_s() / 2);
    h_ = std::max(min_h, line_height(main) + line_height(small) + 2 * pad);
}

MachineRowView::~MachineRowView() = default;

void MachineRowView::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    name_label_ = make_label(host, main_, 0xE0E0E0);
    second_label_ = make_label(host, small_, kLabel);
    status_label_ = make_label(host, small_, kText);
    lv_label_set_long_mode(name_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(second_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_MODE_CLIP);
    apply();
}

void MachineRowView::detach()
{
    name_label_ = second_label_ = status_label_ = nullptr;
}

void MachineRowView::set(const std::string &name, const std::string &second, const std::string &status, Tone tone)
{
    name_ = name;
    second_ = second;
    status_ = status;
    tone_ = tone;
    apply();
}

void MachineRowView::apply()
{
    if (!name_label_) return;
    const int gap = space_m();
    const int sw = text_width(status_.c_str(), small_) + 2;
    const int avail = std::max(1, w_ - 2 * inset_);
    const int left_w = std::max(1, avail - sw - gap);
    const int block = line_height(main_) + line_height(small_);
    const int top = std::max(0, (h_ - block) / 2);
    set_text_if(name_label_, name_);
    set_text_if(second_label_, second_);
    set_text_if(status_label_, status_);
    set_color_if(status_label_, tone_rgb(tone_, kText));
    /* portrait: when the MAC and the address do not fit beside the status, the status goes up beside the name and the
     * second line takes the whole width */
    const bool up = portrait() && text_width(second_.c_str(), small_) + 2 > left_w;
    lv_obj_set_pos(name_label_, inset_, top);
    lv_obj_set_size(name_label_, left_w, line_height(main_));
    lv_obj_set_pos(second_label_, inset_, top + line_height(main_));
    lv_obj_set_size(second_label_, up ? avail : left_w, line_height(small_));
    lv_obj_set_pos(status_label_, w_ - inset_ - sw, up ? top + (line_height(main_) - line_height(small_)) / 2 : (h_ - line_height(small_)) / 2);
    lv_obj_set_size(status_label_, sw, line_height(small_));
}

/* ================================================================ the radar */

RadarView::RadarView(lv_obj_t *parent, const Box &box, const lv_font_t *font) : w_(box.w), h_(box.h), font_(font)
{
    lv_obj_t *root = plain(parent);
    adopt(root);
    lv_obj_set_pos(root, box.x, box.y);
    lv_obj_set_size(root, box.w, box.h);
    r_ = std::max(10, std::min(box.w, box.h) / 2 - 3);
    cx_ = box.w / 2;
    cy_ = box.h / 2;
    painter_ = std::make_unique<Painter>(root, [this](lv_layer_t *layer, const lv_area_t &a) { paint(layer, a); });
    painter_->place(0, 0, box.w, box.h);
    const char *names[3] = {"near", "medium", "far"};
    const int lh = line_height(font);
    for (int k = 0; k < 3; ++k) {
        const int ring = r_ * (k + 1) / 3;
        ring_labels_[k] = label_at(root, font, kLabel, LV_LABEL_LONG_MODE_CLIP, cx_ + 4, cy_ - ring + 2, text_width(names[k], font) + 2, lh);
        set_text_if(ring_labels_[k], names[k]);
    }
    name_ = label_at(root, font, kText, LV_LABEL_LONG_MODE_CLIP, 0, 0, 10, lh);
    lv_obj_set_style_bg_color(name_, lv_color_hex(kBlack), 0);
    lv_obj_set_style_bg_opa(name_, LV_OPA_70, 0);
    lv_obj_add_flag(name_, LV_OBJ_FLAG_HIDDEN);
}

RadarView::~RadarView()
{
    painter_.reset();
}

void RadarView::set_devices(const std::vector<BleDevice> &devices, std::int64_t now_ms, bool scanning, int expiry_ms)
{
    now_ = now_ms;
    scanning_ = scanning;
    expiry_ms_ = expiry_ms;
    blips_.clear();
    blips_.reserve(devices.size());
    for (const BleDevice &d : devices) {
        if (!d.has_rssi) continue;
        Blip b;
        b.address = d.address;
        b.name = d.name.empty() ? d.address : d.name;
        b.angle = blip_angle_deg(d.address);
        b.frac = blip_radius_frac(d.smooth_dbm);
        b.last_seen_ms = d.last_seen_ms;
        blips_.push_back(std::move(b));
    }
    if (!scanning_) sweep_ = -1;
    place_name();
    painter_->invalidate();
}

void RadarView::set_selected(const std::string &address)
{
    if (address == selected_) return;
    selected_ = address;
    place_name();
    painter_->invalidate();
}

void RadarView::tick(std::int64_t ms)
{
    if (!scanning_) return;
    now_ = ms;
    sweep_ = sweep_deg_at(ms);
    painter_->invalidate();
}

void RadarView::place_name()
{
    if (!name_) return;
    const Blip *sel = nullptr;
    for (const Blip &b : blips_)
        if (b.address == selected_) sel = &b;
    if (!sel) {
        lv_obj_add_flag(name_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    double x, y;
    polar_point(cx_, cy_, sel->frac * r_, sel->angle, x, y);
    const int lh = line_height(font_);
    const int tw = std::min(w_ - 4, text_width(sel->name.c_str(), font_) + 6);
    int lx = static_cast<int>(std::lround(x)) + 9;
    if (lx + tw > w_ - 2) lx = static_cast<int>(std::lround(x)) - 9 - tw;
    lx = std::clamp(lx, 2, std::max(2, w_ - tw - 2));
    const int ly = std::clamp(static_cast<int>(std::lround(y)) - lh / 2, 2, std::max(2, h_ - lh - 2));
    lv_obj_set_size(name_, tw, lh);
    lv_obj_set_pos(name_, lx, ly);
    lv_obj_set_style_pad_left(name_, 3, 0);
    set_text_if(name_, sel->name);
    lv_obj_remove_flag(name_, LV_OBJ_FLAG_HIDDEN);
}

bool RadarView::inside_box() const
{
    if (2 * r_ > std::min(w_, h_)) return false;
    if (name_ && !lv_obj_has_flag(name_, LV_OBJ_FLAG_HIDDEN)) {
        const int x = lv_obj_get_x(name_), y = lv_obj_get_y(name_), w = lv_obj_get_width(name_), h = lv_obj_get_height(name_);
        if (x < 0 || y < 0 || x + w > w_ || y + h > h_) return false;
    }
    for (const Blip &b : blips_)
        if (b.frac < 0 || b.frac > 0.95 + 1e-9) return false;
    return true;
}

void RadarView::paint(lv_layer_t *layer, const lv_area_t &a)
{
    const int cx = a.x1 + cx_, cy = a.y1 + cy_;
    const lv_area_t disc{cx - r_, cy - r_, cx + r_, cy + r_};
    paint_fill(layer, disc, kBlack, LV_RADIUS_CIRCLE);
    for (int k = 1; k <= 2; ++k) {
        const int rr = r_ * k / 3;
        paint_ring(layer, lv_area_t{cx - rr, cy - rr, cx + rr, cy + rr}, kBorder, 1, LV_RADIUS_CIRCLE);
    }
    paint_line(layer, cx - r_, cy, cx + r_, cy, kTrack, 1);
    paint_line(layer, cx, cy - r_, cx, cy + r_, kTrack, 1);
    if (sweep_ >= 0) { /* the trailing wedge: slices of a few degrees, fainter the further behind the line */
        constexpr int kSlices = 6;
        constexpr double kSliceDeg = 7.0;
        for (int k = 0; k < kSlices; ++k) {
            const double to = sweep_ - k * kSliceDeg, from = to - kSliceDeg;
            arc(layer, cx, cy, r_, r_, from, to, kBlue, 70 - k * 11, false);
        }
        double x, y;
        polar_point(cx, cy, r_, sweep_, x, y);
        paint_line(layer, cx, cy, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), kBlue, 2);
    }
    const int dot = std::max(3, r_ / 28);
    for (const Blip &b : blips_) {
        const int alpha = blip_alpha(now_ - b.last_seen_ms, expiry_ms_, sweep_, b.angle);
        if (alpha <= 0) continue;
        double x, y;
        polar_point(cx, cy, b.frac * r_, b.angle, x, y);
        const int bx = static_cast<int>(std::lround(x)), by = static_cast<int>(std::lround(y));
        paint_fill(layer, lv_area_t{bx - dot, by - dot, bx + dot, by + dot}, kBlue, LV_RADIUS_CIRCLE, alpha);
        if (b.address == selected_) {
            const int rr = dot + 5;
            paint_ring(layer, lv_area_t{bx - rr, by - rr, bx + rr, by + rr}, kText, 2, LV_RADIUS_CIRCLE);
        }
    }
    paint_ring(layer, disc, kBorder, 2, LV_RADIUS_CIRCLE);
}

} // namespace ui
} // namespace nt
