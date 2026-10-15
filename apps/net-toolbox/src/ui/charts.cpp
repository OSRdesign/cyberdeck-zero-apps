/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the graphic panels (see charts.hpp). Shapes are drawn with LVGL's draw API on the object's own paint
 * event; texts are ordinary labels (so FreeType, wrapping and cutting work as everywhere else).
 */
#include "charts.hpp"

#include "style.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace nt {
namespace ui {
namespace {

lv_color_t rgb_of(std::uint32_t rgb) { return lv_color_hex(rgb); }

void fill_rect(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius = 0, int opa = LV_OPA_COVER)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = rgb_of(rgb);
    d.bg_opa = static_cast<lv_opa_t>(opa);
    d.radius = radius;
    d.border_width = 0;
    lv_draw_rect(layer, &d, &a);
}

void ring_rect(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int width, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.radius = radius;
    d.border_color = rgb_of(rgb);
    d.border_width = width;
    d.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &d, &a);
}

void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, std::uint32_t rgb, int width, bool round = false)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.color = rgb_of(rgb);
    d.width = width;
    d.opa = LV_OPA_COVER;
    d.round_start = round;
    d.round_end = round;
    lv_draw_line(layer, &d);
}

void dot(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb)
{
    const lv_area_t a{cx - r, cy - r, cx + r, cy + r};
    fill_rect(layer, a, rgb, LV_RADIUS_CIRCLE);
}

/* the glyph of a mark inside a disc of radius r */
void glyph(lv_layer_t *layer, int cx, int cy, int r, Mark mark, std::uint32_t rgb)
{
    const int w = std::max(2, r / 4);
    const auto at = [&](double fx, double fy, int &x, int &y) {
        x = cx + static_cast<int>(std::lround(fx * r));
        y = cy + static_cast<int>(std::lround(fy * r));
    };
    int x1, y1, x2, y2, x3, y3;
    switch (mark) {
    case Mark::Check:
        at(-0.46, 0.04, x1, y1);
        at(-0.10, 0.40, x2, y2);
        at(0.50, -0.38, x3, y3);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        line(layer, x2, y2, x3, y3, rgb, w, true);
        break;
    case Mark::Cross:
        at(-0.38, -0.38, x1, y1);
        at(0.38, 0.38, x2, y2);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        at(0.38, -0.38, x1, y1);
        at(-0.38, 0.38, x2, y2);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        break;
    case Mark::Arrow: /* a right arrow: the shaft and the head */
        at(-0.50, 0, x1, y1);
        at(0.48, 0, x2, y2);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        at(0.02, -0.46, x1, y1);
        at(0.50, 0, x2, y2);
        at(0.02, 0.46, x3, y3);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        line(layer, x3, y3, x2, y2, rgb, w, true);
        break;
    case Mark::Dash:
        at(-0.48, 0, x1, y1);
        at(0.48, 0, x2, y2);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        break;
    case Mark::Bang:
        at(0, -0.52, x1, y1);
        at(0, 0.12, x2, y2);
        line(layer, x1, y1, x2, y2, rgb, w, true);
        at(0, 0.48, x3, y3);
        dot(layer, x3, y3, std::max(1, w / 2), rgb);
        break;
    case Mark::None: break;
    }
}

int space_s() { return cp0_ui_tok(CP0_TOK_SPACE_S); }
int space_m() { return cp0_ui_tok(CP0_TOK_SPACE_M); }

} // namespace

/* ---------------------------------------------------------------- the drawing primitives (for chain.cpp) */

void paint_fill(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int radius, int opa)
{
    fill_rect(layer, a, rgb, radius, opa);
}

void paint_ring(lv_layer_t *layer, const lv_area_t &a, std::uint32_t rgb, int width, int radius)
{
    ring_rect(layer, a, rgb, width, radius);
}

void paint_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, std::uint32_t rgb, int width, bool round)
{
    line(layer, x1, y1, x2, y2, rgb, width, round);
}

void paint_dot(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb)
{
    dot(layer, cx, cy, r, rgb);
}

void paint_glyph(lv_layer_t *layer, int cx, int cy, int r, Mark mark, std::uint32_t rgb)
{
    glyph(layer, cx, cy, r, mark, rgb);
}

/* ---------------------------------------------------------------- Owned, Painter */

void Owned::adopt(lv_obj_t *root)
{
    root_ = root;
    lv_obj_add_event_cb(root_, deleted_cb, LV_EVENT_DELETE, this);
}

void Owned::deleted_cb(lv_event_t *event)
{
    auto *self = static_cast<Owned *>(lv_event_get_user_data(event));
    if (self) self->root_ = nullptr;
}

Owned::~Owned()
{
    if (!root_) return;
    lv_obj_remove_event_cb_with_user_data(root_, deleted_cb, this);
    lv_obj_delete(root_);
    root_ = nullptr;
}

Painter::Painter(lv_obj_t *parent, Draw draw) : draw_(std::move(draw))
{
    obj_ = plain(parent);
    lv_obj_add_event_cb(obj_, draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(obj_, deleted_cb, LV_EVENT_DELETE, this);
}

Painter::~Painter()
{
    if (!obj_) return;
    lv_obj_remove_event_cb_with_user_data(obj_, draw_cb, this);
    lv_obj_remove_event_cb_with_user_data(obj_, deleted_cb, this);
    lv_obj_delete(obj_);
    obj_ = nullptr;
}

void Painter::deleted_cb(lv_event_t *event)
{
    auto *self = static_cast<Painter *>(lv_event_get_user_data(event));
    if (self) self->obj_ = nullptr;
}

void Painter::draw_cb(lv_event_t *event)
{
    auto *self = static_cast<Painter *>(lv_event_get_user_data(event));
    if (!self || !self->obj_ || !self->draw_) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(self->obj_, &a);
    self->draw_(layer, a);
}

void Painter::place(int x, int y, int w, int h)
{
    if (!obj_) return;
    lv_obj_set_pos(obj_, x, y);
    lv_obj_set_size(obj_, w, h);
}

void Painter::invalidate()
{
    if (obj_) lv_obj_invalidate(obj_);
}

/* ---------------------------------------------------------------- marks */

void draw_mark(lv_layer_t *layer, int cx, int cy, int r, Mark mark, std::uint32_t rgb, int flash)
{
    const lv_area_t a{cx - r, cy - r, cx + r, cy + r};
    if (mark == Mark::None) {
        ring_rect(layer, a, rgb, std::max(2, r / 4), LV_RADIUS_CIRCLE);
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    lv_color_t c = rgb_of(rgb);
    if (flash > 0) c = lv_color_mix(lv_color_white(), c, static_cast<uint8_t>(std::min(flash, 255)));
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    d.border_width = 0;
    lv_draw_rect(layer, &d, &a);
    glyph(layer, cx, cy, r, mark, kInk);
}

void draw_spinner(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb, int phase)
{
    const int w = std::max(2, r / 4);
    const lv_area_t a{cx - r, cy - r, cx + r, cy + r};
    ring_rect(layer, a, kTrack, w, LV_RADIUS_CIRCLE);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = rgb_of(rgb);
    d.width = w;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = static_cast<uint16_t>(r);
    d.start_angle = static_cast<lv_value_precise_t>(((phase % 360) + 360) % 360);
    d.end_angle = static_cast<lv_value_precise_t>(((phase + 110) % 360 + 360) % 360);
    d.opa = LV_OPA_COVER;
    d.rounded = 1;
    lv_draw_arc(layer, &d);
}

/* ---------------------------------------------------------------- the health ladder */

HealthLadder::HealthLadder(lv_obj_t *parent, int x, int y, int w, int rung_h, const lv_font_t *font)
    : rung_h_(rung_h), w_(w)
{
    lv_obj_t *root = plain(parent);
    adopt(root);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_size(root, w, rung_h * kStepCount);
    const int lh = line_height(font);
    mark_ = std::max(8, std::min(rung_h - 4, lh + 6));
    const int gap = space_m();
    int name_w = 0;
    for (int i = 0; i < kStepCount; ++i) name_w = std::max(name_w, text_width(step_title(static_cast<Step>(i)), font));
    const int name_x = mark_ + gap;
    const int detail_x = name_x + name_w + gap;
    states_.assign(kStepCount, StepState::Pending);
    flash_until_.assign(kStepCount, 0);

    const int center = mark_ / 2;
    track_ = std::make_unique<Painter>(root, [this, center](lv_layer_t *layer, const lv_area_t &a) {
        /* the line the marks hang on */
        const int top = a.y1 + rung_h_ / 2, bottom = a.y1 + rung_h_ * (kStepCount - 1) + rung_h_ / 2;
        line(layer, a.x1 + center, top, a.x1 + center, bottom, kTrack, 2);
    });
    track_->place(0, 0, w, rung_h * kStepCount);
    for (int i = 0; i < kStepCount; ++i) {
        Rung r;
        const int top = i * rung_h;
        r.mark = std::make_unique<Painter>(root, [this, i](lv_layer_t *layer, const lv_area_t &a) {
            const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, rad = std::max(3, mark_ / 2 - 1);
            switch (states_[static_cast<std::size_t>(i)]) {
            case StepState::Pending: draw_mark(layer, cx, cy, rad, Mark::None, kLabel); break;
            case StepState::Running:
                draw_spinner(layer, cx, cy, rad, kWarn, static_cast<int>(now_ * 360 / 900));
                break;
            case StepState::Pass: {
                const std::int64_t left = flash_until_[static_cast<std::size_t>(i)] - now_;
                draw_mark(layer, cx, cy, rad, Mark::Check, kOk, left > 0 ? static_cast<int>(left * 150 / 350) : 0);
                break;
            }
            case StepState::Fail: draw_mark(layer, cx, cy, rad, Mark::Cross, kBad); break;
            case StepState::Skipped: glyph(layer, cx, cy, rad, Mark::Dash, kLabel); break;
            }
        });
        r.mark->place(0, top + (rung_h - mark_) / 2, mark_, mark_);
        r.name = make_label(root, font, kLabel);
        lv_obj_set_pos(r.name, name_x, top + (rung_h - lh) / 2);
        set_text_if(r.name, step_title(static_cast<Step>(i)));
        r.detail = make_label(root, font, kLabel);
        lv_label_set_long_mode(r.detail, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(r.detail, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(r.detail, detail_x, top + (rung_h - lh) / 2);
        lv_obj_set_size(r.detail, std::max(1, w - detail_x), lh);
        rungs_.push_back(std::move(r));
    }
    DiagReport blank;
    update(blank, 0);
}

HealthLadder::~HealthLadder()
{
    rungs_.clear();
    track_.reset();
}

void HealthLadder::update(const DiagReport &report, std::int64_t now_ms)
{
    now_ = now_ms;
    for (int i = 0; i < kStepCount && i < static_cast<int>(report.steps.size()); ++i) {
        const DiagStep &s = report.steps[static_cast<std::size_t>(i)];
        Rung &r = rungs_[static_cast<std::size_t>(i)];
        const StepState before = states_[static_cast<std::size_t>(i)];
        if (before != s.state && s.state == StepState::Pass) flash_until_[static_cast<std::size_t>(i)] = now_ms + 350;
        states_[static_cast<std::size_t>(i)] = s.state;
        const bool lit = s.state == StepState::Pass || s.state == StepState::Fail || s.state == StepState::Running;
        set_color_if(r.name, lit ? kText : kLabel);
        set_text_if(r.detail, step_text(s));
        set_color_if(r.detail, tone_rgb(step_tone(s.state), kLabel));
        if (before != s.state) r.mark->invalidate();
    }
}

void HealthLadder::tick(std::int64_t now_ms)
{
    now_ = now_ms;
    for (int i = 0; i < kStepCount; ++i)
        if (states_[static_cast<std::size_t>(i)] == StepState::Running || flash_until_[static_cast<std::size_t>(i)] > now_ms - 100)
            rungs_[static_cast<std::size_t>(i)].mark->invalidate();
}

/* ---------------------------------------------------------------- the ping panel */

int PingPanel::height_for(int graph_min, const lv_font_t *font)
{
    const int lh = line_height(font), s = space_s();
    const int strip = lh + s;
    return lh + s + graph_min + s + strip + s + lh + s + 2 * lh;
}

PingPanel::PingPanel(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font)
    : w_(w), h_(h), font_(font)
{
    lv_obj_t *root = plain(parent);
    adopt(root);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_size(root, w, h);
    line_h_ = line_height(font);
    gap_ = space_s();
    strip_h_ = line_h_ + gap_;
    graph_h_ = std::max(2 * line_h_, h - (line_h_ + gap_ + gap_ + strip_h_ + gap_ + line_h_ + gap_ + 2 * line_h_));

    head_ = make_label(root, font, kText);
    lv_label_set_long_mode(head_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(head_, 0, 0);
    lv_obj_set_size(head_, w / 2 - gap_, line_h_);
    scale_ = make_label(root, font, kLabel);
    lv_label_set_long_mode(scale_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(scale_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(scale_, w / 2, 0);
    lv_obj_set_size(scale_, w - w / 2, line_h_);

    const int pad = std::max(2, gap_ / 2);
    const int graph_y = line_h_ + gap_;
    graph_ = std::make_unique<Painter>(root, [this, pad](lv_layer_t *layer, const lv_area_t &a) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = rgb_of(kBlack);
        d.bg_opa = LV_OPA_COVER;
        d.border_color = rgb_of(kBorder);
        d.border_width = 1;
        d.radius = cp0_ui_tok(CP0_TOK_RADIUS_S);
        lv_draw_rect(layer, &d, &a);
        const int ix1 = a.x1 + pad, ix2 = a.x2 - pad, iy1 = a.y1 + pad, iy2 = a.y2 - pad;
        const int iw = ix2 - ix1 + 1, ih = iy2 - iy1 + 1;
        if (iw < 4 || ih < 4) return;
        for (int k = 1; k <= 3; ++k) line(layer, ix1, iy1 + ih * k / 4, ix2, iy1 + ih * k / 4, kTrack, 1);
        const std::vector<PacketResult> &p = snap_.recent;
        const std::size_t n = p.size();
        const auto X = [&](std::size_t i) {
            return ix1 + (ring_slot(i, n) * iw + iw / 2) / static_cast<int>(kRingSize);
        };
        const auto Y = [&](double ms) {
            const double v = std::min(std::max(ms, 0.0), top_ms_);
            return iy2 - static_cast<int>(std::lround(v * (ih - 1) / top_ms_));
        };
        const PingStats &st = snap_.stats;
        if (st.received >= 2 && st.jitter_ms() > 0) { /* the jitter band: the average plus and minus the jitter */
            const int y_hi = Y(st.avg_ms() + st.jitter_ms()), y_lo = Y(std::max(0.0, st.avg_ms() - st.jitter_ms()));
            const lv_area_t band{ix1, std::min(y_hi, y_lo), ix2, std::max(std::max(y_hi, y_lo), std::min(y_hi, y_lo) + 1)};
            fill_rect(layer, band, kBlue, 0, LV_OPA_40);
        }
        const int lw = std::max(2, gap_ / 3);
        for (std::size_t i = 0; i < n; ++i) {
            if (ring_slot(i, n) < 0) continue;
            const bool reply = p[i].state == PacketState::Ok || p[i].state == PacketState::Slow;
            if (p[i].state == PacketState::Lost) { /* a red tick on the baseline and a gap in the line */
                line(layer, X(i), iy2, X(i), iy2 - std::max(4, ih * 3 / 10), kBad, lw);
                continue;
            }
            if (!reply) continue;
            if (i > 0 && (p[i - 1].state == PacketState::Ok || p[i - 1].state == PacketState::Slow) && ring_slot(i - 1, n) >= 0)
                line(layer, X(i - 1), Y(p[i - 1].rtt_ms), X(i), Y(p[i].rtt_ms), kOk, lw);
        }
        for (std::size_t i = 0; i < n; ++i) { /* the points that need a word: slow ones, and the newest */
            if (ring_slot(i, n) < 0) continue;
            if (p[i].state == PacketState::Slow) dot(layer, X(i), Y(p[i].rtt_ms), std::max(2, lw), kWarn);
        }
        for (std::size_t i = n; i-- > 0;) {
            if (p[i].state == PacketState::Pending) continue;
            if (p[i].state != PacketState::Lost)
                dot(layer, X(i), Y(p[i].rtt_ms), std::max(2, lw), p[i].state == PacketState::Slow ? kWarn : kOk);
            break;
        }
    });
    graph_->place(0, graph_y, w, graph_h_);

    const int strip_y = graph_y + graph_h_ + gap_;
    strip_ = std::make_unique<Painter>(root, [this, pad](lv_layer_t *layer, const lv_area_t &a) {
        const int ix1 = a.x1 + pad, ix2 = a.x2 - pad;
        const int iw = ix2 - ix1 + 1, ah = a.y2 - a.y1 + 1;
        if (iw < static_cast<int>(kRingSize)) return;
        const int slot_w = iw / static_cast<int>(kRingSize);
        line(layer, ix1, a.y2, ix2, a.y2, kTrack, 1);
        const std::vector<PacketResult> &p = snap_.recent;
        const std::size_t n = p.size();
        for (std::size_t i = 0; i < n; ++i) {
            const int slot = ring_slot(i, n);
            if (slot < 0) continue;
            const Cell c = packet_cell(p[i].state);
            const int x1 = ix1 + slot * iw / static_cast<int>(kRingSize);
            int x2 = ix1 + (slot + 1) * iw / static_cast<int>(kRingSize) - 1 - (slot_w >= 3 ? 1 : 0);
            x2 = std::max(x1, x2);
            const int ch = std::max(2, ah * c.height_pct / 100);
            const std::uint32_t rgb = c.state == PacketState::Ok     ? kOk
                                      : c.state == PacketState::Slow ? kWarn
                                      : c.state == PacketState::Lost ? kBad
                                                                     : kLabel;
            fill_rect(layer, lv_area_t{x1, a.y2 - ch + 1, x2, a.y2}, rgb);
        }
    });
    strip_->place(0, strip_y, w, strip_h_);

    // the legend: a swatch (as tall as its cell) and the word
    const int legend_y = strip_y + strip_h_ + gap_;
    const PacketState order[4] = {PacketState::Ok, PacketState::Slow, PacketState::Lost, PacketState::Pending};
    std::vector<int> swatch_x;
    int lx = 0;
    const int sw = std::max(4, line_h_ / 3);
    for (const PacketState st : order) {
        lv_obj_t *l = make_label(root, font, kLabel);
        set_text_if(l, packet_word(st));
        swatch_x.push_back(lx);
        lv_obj_set_pos(l, lx + sw + gap_, legend_y);
        legend_.push_back(l);
        lx += sw + gap_ + text_width(packet_word(st), font) + 3 * gap_;
    }
    swatches_ = std::make_unique<Painter>(root, [this, swatch_x, sw, order](lv_layer_t *layer, const lv_area_t &a) {
        const int ah = a.y2 - a.y1 + 1;
        for (int k = 0; k < 4; ++k) {
            const Cell c = packet_cell(order[k]);
            const int ch = std::max(2, ah * c.height_pct / 100);
            const std::uint32_t rgb = order[k] == PacketState::Ok     ? kOk
                                      : order[k] == PacketState::Slow ? kWarn
                                      : order[k] == PacketState::Lost ? kBad
                                                                      : kLabel;
            fill_rect(layer, lv_area_t{a.x1 + swatch_x[static_cast<std::size_t>(k)], a.y2 - ch + 1,
                                       a.x1 + swatch_x[static_cast<std::size_t>(k)] + sw - 1, a.y2},
                      rgb);
        }
    });
    swatches_->place(0, legend_y, w, line_h_);

    const int stats_y = legend_y + line_h_ + gap_;
    stats_ = make_label(root, font, kText);
    lv_label_set_long_mode(stats_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(stats_, 0, stats_y);
    lv_obj_set_size(stats_, w, std::max(line_h_, h - stats_y));
}

PingPanel::~PingPanel()
{
    graph_.reset();
    strip_.reset();
    swatches_.reset();
}

void PingPanel::update(const PingSnapshot &s)
{
    snap_ = s;
    top_ms_ = nice_top_ms(max_rtt_ms(s.recent));
    const Band head = ping_head(s);
    set_text_if(head_, head.text);
    set_color_if(head_, tone_rgb(head.tone, kText));
    set_text_if(scale_, s.recent.empty() ? std::string() : ping_scale(top_ms_, s.recent.size()));
    set_text_if(stats_, ping_stats(s.stats));
    graph_->invalidate();
    strip_->invalidate();
}

/* ---------------------------------------------------------------- the result badge */

int ResultBadge::height_for(const lv_font_t *caption, const lv_font_t *big, int gap)
{
    return line_height(caption) + gap + line_height(big) + gap + 2 * line_height(caption);
}

ResultBadge::ResultBadge(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *caption, const lv_font_t *big)
    : w_(w)
{
    lv_obj_t *root = plain(parent);
    adopt(root);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_size(root, w, h);
    gap_ = space_s();
    const int ch = line_height(caption), bh = line_height(big);
    host_ = make_label(root, caption, kLabel);
    lv_label_set_long_mode(host_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(host_, 0, 0);
    lv_obj_set_size(host_, w, ch);
    mark_d_ = bh;
    const int big_y = ch + gap_;
    mark_ = std::make_unique<Painter>(root, [this](lv_layer_t *layer, const lv_area_t &a) {
        const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = std::max(4, mark_d_ / 2 - 1);
        if (shown_.mark == Mark::None) {
            if (!shown_.word.empty()) draw_mark(layer, cx, cy, r, Mark::None, kLabel);
            return;
        }
        draw_mark(layer, cx, cy, r, shown_.mark, tone_rgb(shown_.tone, kLabel));
    });
    mark_->place(0, big_y, mark_d_, mark_d_);
    word_ = make_label(root, big, kText);
    lv_label_set_long_mode(word_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(word_, mark_d_ + 2 * gap_, big_y);
    lv_obj_set_size(word_, std::max(1, w - mark_d_ - 2 * gap_), bh);
    detail_ = make_label(root, caption, kText);
    lv_label_set_long_mode(detail_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(detail_, 0, big_y + bh + gap_);
    lv_obj_set_size(detail_, w, std::max(ch, h - (big_y + bh + gap_)));
}

ResultBadge::~ResultBadge() { mark_.reset(); }

void ResultBadge::update(const PortSummary &s)
{
    shown_ = s;
    set_text_if(host_, s.host);
    set_text_if(word_, s.word);
    set_color_if(word_, tone_rgb(s.tone, kText));
    set_text_if(detail_, s.detail);
    mark_->invalidate();
}

/* ---------------------------------------------------------------- the notice */

NoticeBox::NoticeBox(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font, const std::string &title,
                     const std::string &text)
{
    lv_obj_t *root = plain(parent);
    adopt(root);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_size(root, w, h);
    lv_obj_set_style_border_width(root, 2, 0);
    lv_obj_set_style_border_color(root, rgb_of(kWarn), 0);
    lv_obj_set_style_radius(root, cp0_ui_tok(CP0_TOK_RADIUS_S), 0);
    const int pad = space_m();
    const int lh = line_height(font);
    lv_obj_t *head = make_label(root, font, kWarn);
    lv_obj_set_pos(head, pad, pad / 2);
    set_text_if(head, title);
    lv_obj_t *body = make_label(root, font, kText);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(body, pad, pad / 2 + lh + space_s());
    lv_obj_set_size(body, std::max(1, w - 2 * pad - 4), std::max(lh, h - (pad / 2 + lh + space_s()) - pad / 2 - 4));
    set_text_if(body, text);
}

NoticeBox::~NoticeBox() = default;

} // namespace ui
} // namespace nt
