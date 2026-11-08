/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the small widgets (see widgets.hpp).
 */
#include "widgets.hpp"

#include "app.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

namespace {

lv_area_t coords_of(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a;
}

/* Buttons are pressed by touch; the app's keys come through its own handler (no focus ring, no group). */
void touch_only(lv_obj_t *b)
{
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (lv_obj_get_group(b)) lv_group_remove_obj(b);
}

int ls_px(double ls, double s)
{
    return static_cast<int>(ls * s); /* truncated: Montserrat is already wider than the boards' condensed face */
}

} // namespace

/* ------------------------------------------------------------------------------------------------ PianoKey */

PianoKey::PianoKey(lv_obj_t *parent, const Fit &fit, DRect r, KeyType type, std::function<void(KeyEdge)> on_edge)
    : s_(fit.s), on_edge_(std::move(on_edge))
{
    obj_ = lv_button_create(parent);
    lv_obj_remove_style_all(obj_);
    touch_only(obj_);
    body_h_ = fit.h(r.y, r.h);
    lip_ = fit.px(4);
    off_ = fit.px(3);
    radius_ = fit.px(6);
    gap_ = fit.px(type.gap);
    icon_h_ = fit.px(15);
    px_ = fit.px(type.px);
    ls_ = ls_px(type.ls, fit.s);
    lv_obj_set_pos(obj_, fit.x(r.x), fit.y(r.y));
    lv_obj_set_size(obj_, fit.w(r.x, r.w), body_h_ + lip_);
    caption_ = lv_label_create(obj_);
    lv_obj_remove_flag(caption_, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(caption_, font(Face::Bold, px_), 0);
    lv_obj_set_style_text_letter_space(caption_, ls_, 0);
    lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(caption_, fit.w(r.x, r.w));
    lv_obj_add_event_cb(obj_, draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(obj_, event_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(obj_, event_cb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(obj_, event_cb, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(obj_, event_cb, LV_EVENT_CLICKED, this);
}

void PianoKey::set(Icon icon, const std::string &caption, KeyLook look, bool enabled, bool latched)
{
    const bool changed = icon != icon_ || look != look_ || latched != latched_ || caption != text_;
    icon_ = icon;
    look_ = look;
    latched_ = latched;
    if (caption != text_) {
        text_ = caption;
        const int avail = lv_obj_get_style_width(obj_, LV_PART_MAIN) - 2 * std::max(2, static_cast<int>(3 * s_));
        int ls = ls_;
        const int px = fit_px(Face::Bold, px_, ls_, caption, avail, std::max(8, px_ * 3 / 4), &ls);
        lv_obj_set_style_text_font(caption_, font(Face::Bold, px), 0);
        lv_obj_set_style_text_letter_space(caption_, ls, 0);
        lv_label_set_text(caption_, caption.c_str());
    }
    lv_obj_set_style_text_color(caption_, lv_color_hex(look == KeyLook::Charcoal && !latched ? col::kKeyText : col::kWhite), 0);
    if (enabled != enabled_) {
        enabled_ = enabled;
        lv_obj_set_style_opa_layered(obj_, static_cast<lv_opa_t>(enabled ? 255 : 89), 0); /* 35 % */
    }
    place_caption();
    if (changed) lv_obj_invalidate(obj_);
}

void PianoKey::place_caption()
{
    const bool down = latched_ || pressed_;
    const int lh = lv_font_get_line_height(lv_obj_get_style_text_font(caption_, LV_PART_MAIN));
    const int content = icon_h_ + gap_ + lh;
    const int top = (down ? off_ : 0) + (body_h_ - content) / 2;
    lv_obj_set_pos(caption_, 0, top + icon_h_ + gap_);
}

void PianoKey::draw_cb(lv_event_t *e)
{
    auto *self = static_cast<PianoKey *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t a = coords_of(self->obj_);
    const bool down = self->latched_ || self->pressed_;
    const int off = down ? self->off_ : 0;
    const int lip = down ? std::max(1, static_cast<int>(std::lround(self->s_))) : self->lip_;
    const int w = a.x2 - a.x1 + 1;
    const lv_area_t body = area(a.x1, a.y1 + off, w, self->body_h_);
    fill(layer, area(body.x1, body.y1 + lip, w, self->body_h_), col::kHinge, self->radius_);
    std::uint32_t top = col::kKeyTop, bottom = col::kKeyBottom, hi = col::kKeyHi;
    if (self->look_ == KeyLook::Red) top = col::kRecTop, bottom = col::kRecBottom, hi = col::kRecHi;
    if (self->look_ == KeyLook::Green) top = col::kOkTop, bottom = col::kOkBottom, hi = col::kOkHi;
    if (self->latched_) top = col::kOnTop, bottom = col::kOnBottom;
    fill_vgrad(layer, body, top, bottom, self->radius_);
    if (self->latched_) {
        /* the board's inset shadow (0 3px 6px #3D0D0A): a dark band fading down from the top edge */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = self->radius_;
        d.bg_grad.dir = LV_GRAD_DIR_VER;
        d.bg_grad.stops_count = 2;
        d.bg_grad.stops[0].color = lv_color_hex(col::kOnInset);
        d.bg_grad.stops[0].opa = 230;
        d.bg_grad.stops[0].frac = 0;
        d.bg_grad.stops[1].color = lv_color_hex(col::kOnInset);
        d.bg_grad.stops[1].opa = 0;
        d.bg_grad.stops[1].frac = 255;
        d.bg_opa = LV_OPA_COVER;
        const lv_area_t band = area(body.x1, body.y1, w, std::max(2, static_cast<int>(9 * self->s_)));
        lv_draw_rect(layer, &d, &band);
    } else {
        line(layer, body.x1 + self->radius_, body.y1, body.x2 - self->radius_, body.y1, 1, hi);
    }
    const int lh = lv_font_get_line_height(lv_obj_get_style_text_font(self->caption_, LV_PART_MAIN));
    const int content = self->icon_h_ + self->gap_ + lh;
    const int icon_cy = body.y1 + (self->body_h_ - content) / 2 + self->icon_h_ / 2;
    const std::uint32_t ink = self->look_ == KeyLook::Charcoal && !self->latched_ ? col::kKeyText : col::kWhite;
    icon(layer, self->icon_, (a.x1 + a.x2) / 2, icon_cy, self->s_, ink);
}

void PianoKey::event_cb(lv_event_t *e)
{
    auto *self = static_cast<PianoKey *>(lv_event_get_user_data(e));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        self->pressed_ = true;
        self->place_caption();
        lv_obj_invalidate(self->obj_);
        if (self->on_edge_) self->on_edge_(KeyEdge::Down);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (!self->pressed_) break;
        self->pressed_ = false;
        self->place_caption();
        lv_obj_invalidate(self->obj_);
        if (self->on_edge_) self->on_edge_(KeyEdge::Up);
        break;
    case LV_EVENT_CLICKED:
        if (self->on_edge_) self->on_edge_(KeyEdge::Click);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ IconButton */

IconButton::IconButton(lv_obj_t *parent, const Fit &fit, DRect r, Icon icon, std::function<void()> on_click)
    : icon_(icon), s_(fit.s), on_click_(std::move(on_click))
{
    obj_ = lv_button_create(parent);
    lv_obj_remove_style_all(obj_);
    touch_only(obj_);
    lv_obj_set_pos(obj_, fit.x(r.x), fit.y(r.y));
    lv_obj_set_size(obj_, fit.w(r.x, r.w), fit.h(r.y, r.h));
    lv_obj_add_event_cb(obj_, draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(obj_, click_cb, LV_EVENT_CLICKED, this);
}

void IconButton::set_enabled(bool enabled)
{
    if (enabled == enabled_) return;
    enabled_ = enabled;
    lv_obj_set_style_opa_layered(obj_, static_cast<lv_opa_t>(enabled ? 255 : 89), 0);
}

void IconButton::set_icon(Icon icon)
{
    if (icon == icon_) return;
    icon_ = icon;
    lv_obj_invalidate(obj_);
}

void IconButton::draw_cb(lv_event_t *e)
{
    auto *self = static_cast<IconButton *>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t a = coords_of(self->obj_);
    const int r = std::max(2, static_cast<int>(std::lround(6 * self->s_)));
    const bool pressed = lv_obj_has_state(self->obj_, LV_STATE_PRESSED);
    fill_vgrad(layer, a, pressed ? col::kKeyBottom : col::kKeyTop, pressed ? col::kKeyTop : col::kKeyBottom, r);
    line(layer, a.x1 + r, a.y1, a.x2 - r, a.y1, 1, col::kKeyHi);
    icon(layer, self->icon_, (a.x1 + a.x2 + 1) / 2, (a.y1 + a.y2 + 1) / 2, self->s_, col::kKeyText);
}

void IconButton::click_cb(lv_event_t *e)
{
    auto *self = static_cast<IconButton *>(lv_event_get_user_data(e));
    if (self->on_click_) self->on_click_();
}

/* ------------------------------------------------------------------------------------------------ Stepper */

Stepper::Stepper(lv_obj_t *parent, const Fit &fit, DRect r, const char *caption, double value_px, Icon minus, Icon plus,
                 std::function<void(int)> on_step)
    : value_px_(value_px), s_(fit.s)
{
    frame_ = box(parent, 0, 0, 10, 10, col::kPanel, fit.px(8));
    border(frame_, fit.px(3), col::kFrame);
    caption_ = label(parent, Face::Bold, fit.px(11), col::kScale, 0, 0, fit.w(r.x + 44, r.w - 88), LV_TEXT_ALIGN_CENTER,
                     ls_px(1.2, fit.s));
    set_fitted(caption_, Face::Bold, fit.px(11), ls_px(1.2, fit.s), std::max(8, fit.px(9)), caption);
    value_ = label(parent, Face::Mono, fit.px(value_px), col::kDigit, 0, 0, 10, LV_TEXT_ALIGN_CENTER);
    auto step = on_step;
    down_ = std::make_unique<IconButton>(parent, fit, DRect{r.x, r.y, 44, r.h}, minus, [step] { if (step) step(-1); });
    up_ = std::make_unique<IconButton>(parent, fit, DRect{r.x + r.w - 44, r.y, 44, r.h}, plus, [step] { if (step) step(1); });
    place(fit, r, value_px);
}

void Stepper::place(const Fit &fit, DRect r, double value_px)
{
    value_px_ = value_px;
    lv_obj_set_pos(frame_, fit.x(r.x), fit.y(r.y));
    lv_obj_set_size(frame_, fit.w(r.x, r.w), fit.h(r.y, r.h));
    lv_obj_set_pos(down_->obj(), fit.x(r.x), fit.y(r.y));
    lv_obj_set_pos(up_->obj(), fit.x(r.x + r.w - 44), fit.y(r.y));
    const int cx = fit.x(r.x + 44), cw = fit.w(r.x + 44, r.w - 88);
    /* caption: line 15 under a 6 px padding; value: line 20 under it (the boards' text boxes) */
    const lv_font_t *cf = lv_obj_get_style_text_font(caption_, LV_PART_MAIN);
    lv_obj_set_pos(caption_, cx, fit.y(r.y + 6) + (fit.px(15) - lv_font_get_line_height(cf)) / 2);
    lv_obj_set_width(caption_, cw);
    lv_obj_set_width(value_, cw);
    const std::string keep = value_text_;
    value_text_.clear();
    set_value(keep);
    const lv_font_t *vf = lv_obj_get_style_text_font(value_, LV_PART_MAIN);
    lv_obj_set_pos(value_, cx, fit.y(r.y + 21) + (fit.px(20) - lv_font_get_line_height(vf)) / 2);
}

void Stepper::set_value(const std::string &value)
{
    if (value == value_text_) return;
    value_text_ = value;
    const int px = static_cast<int>(std::lround(value_px_ * s_));
    set_fitted(value_, Face::Mono, px, 0, std::max(8, px * 3 / 4), value);
}

void Stepper::set_enabled(bool down, bool up)
{
    down_->set_enabled(down);
    up_->set_enabled(up);
}

void Stepper::set_shown(bool shown)
{
    show(frame_, shown);
    show(caption_, shown);
    show(value_, shown);
    show(down_->obj(), shown);
    show(up_->obj(), shown);
}

/* ------------------------------------------------------------------------------------------------ Lamp */

Lamp::Lamp(lv_obj_t *parent, int x, int y, int d, std::uint32_t rgb, int glow)
{
    obj_ = box(parent, x, y, d, d, rgb, LV_RADIUS_CIRCLE);
    lv_obj_set_style_shadow_width(obj_, glow, 0);
    lv_obj_set_style_shadow_color(obj_, lv_color_hex(rgb), 0);
    lv_obj_set_style_shadow_opa(obj_, LV_OPA_COVER, 0);
}

void Lamp::set(bool lit, std::uint32_t rgb)
{
    show(obj_, lit);
    lv_obj_set_style_bg_color(obj_, lv_color_hex(rgb), 0);
    lv_obj_set_style_shadow_color(obj_, lv_color_hex(rgb), 0);
}

/* ------------------------------------------------------------------------------------------------ drawn controls */

namespace {

void toggle_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t a = coords_of(o);
    const bool on = lv_obj_get_user_data(o) != nullptr;
    const int h = a.y2 - a.y1 + 1, w = a.x2 - a.x1 + 1;
    fill(layer, a, on ? col::kRecTop : col::kHinge, h / 2);
    const int pad = std::max(2, h / 8), d = h - 2 * pad;
    const int kx = on ? a.x1 + w - pad - d : a.x1 + pad;
    fill(layer, area(kx, a.y1 + pad, d, d), col::kKeyText, LV_RADIUS_CIRCLE);
}

void slider_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t a = coords_of(o);
    const int permille = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(o)));
    const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    const int knob = h, track_h = std::max(4, h * 2 / 5);
    const int track_w = w - knob; /* the knob's centre travels over the track */
    const int tx = a.x1 + knob / 2, ty = a.y1 + (h - track_h) / 2;
    fill(layer, area(tx - knob / 2 + 1, ty, w - 2, track_h), col::kHinge, track_h / 2);
    const int fx = tx + track_w * permille / 1000;
    fill(layer, area(tx - knob / 2 + 1, ty, fx - (tx - knob / 2 + 1), track_h), col::kRecTop, track_h / 2);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(col::kKeyText);
    d.border_color = lv_color_hex(col::kKeyTop);
    d.border_width = std::max(1, knob / 10);
    const lv_area_t k = area(fx - knob / 2, a.y1, knob, knob);
    lv_draw_rect(layer, &d, &k);
}

void chevron_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t a = coords_of(o);
    const double s = (a.y2 - a.y1 + 1) / 16.0;
    icon(layer, Icon::Chevron, (a.x1 + a.x2 + 1) / 2, (a.y1 + a.y2 + 1) / 2, s,
         static_cast<std::uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(o))));
}

lv_obj_t *drawn(lv_obj_t *parent, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_set_size(o, w, h);
    lv_obj_add_event_cb(o, cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

} // namespace

lv_obj_t *make_toggle(lv_obj_t *parent, const Fit &fit, double w, double h)
{
    return drawn(parent, fit.px(w), fit.px(h), toggle_draw);
}

void set_toggle(lv_obj_t *t, bool on)
{
    if ((lv_obj_get_user_data(t) != nullptr) == on) return;
    lv_obj_set_user_data(t, on ? reinterpret_cast<void *>(1) : nullptr);
    lv_obj_invalidate(t);
}

lv_obj_t *make_slider_bar(lv_obj_t *parent, const Fit &fit, double w, double h)
{
    return drawn(parent, fit.px(w), fit.px(h), slider_draw);
}

void set_slider_bar(lv_obj_t *b, int min, int max, int pos)
{
    const int span = std::max(1, max - min);
    const int permille = std::clamp((pos - min) * 1000 / span, 0, 1000);
    if (static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(b))) == permille) return;
    lv_obj_set_user_data(b, reinterpret_cast<void *>(static_cast<intptr_t>(permille)));
    lv_obj_invalidate(b);
}

lv_obj_t *make_chevron(lv_obj_t *parent, const Fit &fit, std::uint32_t rgb)
{
    lv_obj_t *o = drawn(parent, fit.px(10), fit.px(16), chevron_draw);
    lv_obj_set_user_data(o, reinterpret_cast<void *>(static_cast<uintptr_t>(rgb)));
    return o;
}

lv_obj_t *make_sticker(lv_obj_t *parent, const Fit &fit, double w, double h, double stripe, double px, double text_x,
                       double text_y, int radius)
{
    lv_obj_t *paper = box(parent, 0, 0, fit.px(w), fit.px(h), col::kPaper, radius);
    border(paper, 1, col::kPaperLine);
    lv_obj_t *band = box(paper, 0, 0, fit.px(w), fit.px(stripe), col::kStripe, radius);
    (void)band;
    (void)text_y;
    const int lh = lv_font_get_line_height(font(Face::Bold, fit.px(px)));
    const int y = fit.px(stripe) + (fit.px(h) - fit.px(stripe) - lh) / 2; /* centred under the stripe */
    lv_obj_t *t = label(paper, Face::Bold, fit.px(px), col::kLabelInk, fit.px(text_x) - 1, y, fit.px(w) - fit.px(text_x) - 1);
    lv_obj_set_user_data(paper, t);
    lv_obj_set_user_data(t, reinterpret_cast<void *>(static_cast<intptr_t>(fit.px(px))));
    return paper;
}

void set_sticker(lv_obj_t *paper, const std::string &text)
{
    auto *t = static_cast<lv_obj_t *>(lv_obj_get_user_data(paper));
    const int px = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(t)));
    set_fitted(t, Face::Bold, px, 0, std::max(8, px * 2 / 3), text);
}

int ls_screen(double ls, double s)
{
    return ls_px(ls, s);
}

lv_obj_t *fit_label(lv_obj_t *parent, const Fit &fit, Face face, double px, std::uint32_t rgb, double x, double y, double w,
                    double line_h, lv_text_align_t align, double ls)
{
    const int p = fit.px(px);
    const int lh = lv_font_get_line_height(font(face, p));
    const int top = fit.y(y) + (fit.px(line_h) - lh) / 2;
    return label(parent, face, p, rgb, fit.x(x), top, w > 0 ? fit.w(x, w) : 0, align, ls_px(ls, fit.s));
}

lv_obj_t *framed_panel(lv_obj_t *parent, const Fit &fit, DRect r, std::uint32_t bg)
{
    lv_obj_t *o = box(parent, fit.x(r.x), fit.y(r.y), fit.w(r.x, r.w), fit.h(r.y, r.h), bg, fit.px(8));
    border(o, fit.px(3), col::kFrame);
    return o;
}

lv_obj_t *deck_body(lv_obj_t *parent, const Fit &fit)
{
    const int m = fit.px(8);
    const int y = fit.y(46);
    lv_obj_t *o = box(parent, m, y, fit.sw - 2 * m, fit.sh - m - y, col::kBody, fit.px(10));
    border(o, 1, col::kBodyEdge);
    return o;
}

} // namespace ui
} // namespace cassette
