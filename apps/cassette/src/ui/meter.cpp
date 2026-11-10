/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the LED bar and the counter (see meter.hpp).
 */
#include "meter.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

/* ------------------------------------------------------------------------------------------------ LedBar */

LedBar::LedBar(lv_obj_t *parent, const Fit &fit, double x0, double y, double led_w, double led_h, double gap,
               const LedStyle &style)
{
    led_w_ = fit.px(led_w);
    led_h_ = fit.h(y, led_h);
    gap_ = fit.px(gap);
    over_ = fit.px(style.over);
    tick_w_ = fit.px(style.tick_w);
    radius_ = style.radius <= 1 ? 1 : fit.px(style.radius); /* the boards' 1 px stays 1 px */
    obj_ = lv_obj_create(parent);
    plain(obj_);
    lv_obj_set_pos(obj_, fit.x(x0), fit.y(y) - over_);
    w_ = 15 * led_w_ + 14 * gap_ + gap_;
    lv_obj_set_size(obj_, w_, led_h_ + 2 * over_);
    lv_obj_add_event_cb(obj_, draw_cb, LV_EVENT_DRAW_MAIN, this);
}

void LedBar::set_height(const Fit &fit, double y, double led_h)
{
    led_h_ = fit.h(y, led_h);
    lv_obj_set_size(obj_, w_, led_h_ + 2 * over_);
    lv_obj_invalidate(obj_);
}

void LedBar::update(const MeterVm &m)
{
    bool same = !first_ && m.channels == shown_.channels;
    for (int c = 0; same && c < 2; ++c)
        same = m.ch[c].bar == shown_.ch[c].bar && m.ch[c].hold_bar == shown_.ch[c].hold_bar;
    if (same) return;
    first_ = false;
    shown_ = m;
    lv_obj_invalidate(obj_);
}

void LedBar::draw_cb(lv_event_t *e)
{
    auto *self = static_cast<LedBar *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->obj_, &a);
    const int n = std::clamp(self->shown_.channels, 1, 2);
    const int y0 = a.y1 + self->over_;
    const int bar_gap = n == 2 ? std::max(1, self->gap_) : 0;
    const int bh = (self->led_h_ - bar_gap * (n - 1)) / n;
    const int pitch = self->led_w_ + self->gap_;
    for (int c = 0; c < n; ++c) {
        const ChannelMeter &ch = self->shown_.ch[c];
        const int y = y0 + c * (bh + bar_gap);
        for (int i = 0; i < 15; ++i) {
            const LedBand b = led_band(i);
            const std::uint32_t rgb = b == LedBand::Green ? col::kLedGreen : b == LedBand::Amber ? col::kLedAmber : col::kLedRed;
            const bool lit = i < ch.bar;
            fill(layer, area(a.x1 + i * pitch, y, self->led_w_, bh), rgb, self->radius_, static_cast<lv_opa_t>(lit ? 255 : 46));
        }
        if (ch.hold_bar > 0) {
            const int hold = std::min(ch.hold_bar, 15);
            const int tx = a.x1 + hold * pitch - (self->gap_ + self->tick_w_) / 2;
            const int ty = n == 1 ? a.y1 : y - (c == 0 ? self->over_ : 0);
            const int th = n == 1 ? self->led_h_ + 2 * self->over_ : bh + self->over_;
            fill(layer, area(tx, ty, self->tick_w_, th), col::kWhite, 0);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ Counter */

Counter::Counter(lv_obj_t *parent, const Fit &fit, double x, double y, double w, double h, double cell_w, double cell_h,
                 double colon_w, double px, double radius)
{
    cell_w_ = fit.px(cell_w);
    cell_h_ = fit.px(cell_h);
    colon_w_ = fit.px(colon_w);
    px_ = fit.px(px);
    radius_ = fit.px(radius);
    obj_ = lv_obj_create(parent);
    plain(obj_);
    lv_obj_set_pos(obj_, fit.x(x), fit.y(y));
    lv_obj_set_size(obj_, fit.w(x, w), fit.h(y, h));
    lv_obj_add_event_cb(obj_, draw_cb, LV_EVENT_DRAW_MAIN, this);
    set_ms(0);
}

void Counter::set_ms(std::int64_t ms)
{
    const std::string t = fmt_counter(ms);
    if (t == text_) return;
    text_ = t;
    lv_obj_invalidate(obj_);
}

void Counter::draw_cb(lv_event_t *e)
{
    auto *self = static_cast<Counter *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(self->obj_, &a);
    const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = self->radius_;
    d.bg_color = lv_color_hex(col::kCounterBox);
    d.border_color = lv_color_hex(col::kCounterEdge);
    d.border_width = 1;
    lv_draw_rect(layer, &d, &a);

    int digits = 0, colons = 0;
    for (char c : self->text_) (c == ':' ? colons : digits)++;
    const int gap = 1;
    const int parts = digits + colons;
    int cw = self->cell_w_, colw = self->colon_w_, ch = self->cell_h_, px = self->px_;
    const int need = digits * cw + colons * colw + (parts - 1) * gap;
    const int avail = w - 4;
    if (need > avail) { /* H:MM:SS: smaller cells and type in the same box */
        const double k = static_cast<double>(avail - (parts - 1) * gap) / (digits * cw + colons * colw);
        cw = std::max(4, static_cast<int>(cw * k));
        colw = std::max(2, static_cast<int>(colw * k));
        ch = std::max(8, static_cast<int>(ch * k));
        px = std::max(8, static_cast<int>(px * k));
    }
    const int total = digits * cw + colons * colw + (parts - 1) * gap;
    int x = a.x1 + (w - total) / 2;
    const int y = a.y1 + (h - ch) / 2;
    const lv_font_t *f = font(Face::Mono, px);
    const int lh = lv_font_get_line_height(f);
    char one[2] = {0, 0};
    for (char c : self->text_) {
        one[0] = c;
        if (c == ':') {
            text(layer, f, one, area(x - 2, y + (ch - lh) / 2, colw + 4, lh), col::kDigit, LV_TEXT_ALIGN_CENTER);
            x += colw + gap;
            continue;
        }
        fill(layer, area(x, y, cw, ch), col::kCell, std::max(1, self->radius_ * 3 / 5));
        text(layer, f, one, area(x - 2, y + (ch - lh) / 2, cw + 4, lh), col::kDigit, LV_TEXT_ALIGN_CENTER);
        x += cw + gap;
    }
}

} // namespace ui
} // namespace cassette
