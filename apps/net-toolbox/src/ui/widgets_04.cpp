/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the blocks of the Local devices, Wi-Fi networks and Wi-Fi walk test pages (see widgets_04.hpp).
 */
#include "widgets_04.hpp"

#include "style.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace nt {
namespace ui {

namespace {

constexpr std::uint32_t kBarBlue = kBlue;
constexpr std::uint32_t kWhite = 0xF2F2F2;

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

/* a diamond of half-size r */
void diamond(lv_layer_t *layer, int cx, int cy, int r, std::uint32_t rgb)
{
    triangle(layer, cx, cy - r, cx - r, cy, cx + r, cy, rgb);
    triangle(layer, cx, cy + r, cx - r, cy, cx + r, cy, rgb);
}

} // namespace

/* ================================================================ a device of the list */

DeviceRowView::DeviceRowView(int w, int inset, const lv_font_t *main, const lv_font_t *small, int min_h)
    : w_(w), inset_(inset), main_(main), small_(small)
{
    const int pad = std::max(4, space_s() / 2);
    h_ = std::max(min_h, line_height(main) + line_height(small) + 2 * pad);
    pill_h_ = line_height(small) + 2;
}

DeviceRowView::~DeviceRowView() = default;

int DeviceRowView::pill_width() const
{
    if (t_.tag.empty()) return 0;
    const int pad = pill_h_ / 2;
    return pad + pill_h_ / 2 + 4 + text_width(t_.tag.c_str(), small_) + pad + 1;
}

void DeviceRowView::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    title_ = make_label(host, main_, 0xE0E0E0);
    ip_ = make_label(host, main_, kLabel);
    pill_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) {
        paint_ring(layer, a, kLabel, 1, LV_RADIUS_CIRCLE);
        const int gr = std::max(3, pill_h_ / 4);
        const int cx = a.x1 + pill_h_ / 2 + 1, cy = (a.y1 + a.y2) / 2;
        if (t_.self) paint_dot(layer, cx, cy, gr, kText);
        else diamond(layer, cx, cy, gr + 1, kText);
    });
    tag_ = make_label(host, small_, kText);
    detail_ = make_label(host, small_, kLabel);
    rtt_ = make_label(host, small_, kText);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(ip_, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_long_mode(tag_, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_long_mode(detail_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(rtt_, LV_LABEL_LONG_MODE_CLIP);
    apply();
}

void DeviceRowView::detach()
{
    pill_.reset();
    title_ = ip_ = detail_ = rtt_ = tag_ = nullptr;
}

void DeviceRowView::set(const DeviceRowTexts &texts)
{
    t_ = texts;
    apply();
}

void DeviceRowView::apply()
{
    if (!title_) return;
    const int gap = space_m();
    const int avail = std::max(1, w_ - 2 * inset_);
    const int lh1 = line_height(main_), lh2 = line_height(small_);
    const int top = std::max(0, (h_ - lh1 - lh2) / 2);
    const int ip_w = t_.ip.empty() ? 0 : text_width(t_.ip.c_str(), main_) + 2;
    const int pill_w = pill_width();
    set_text_if(title_, t_.title);
    lv_obj_set_pos(title_, inset_, top);
    lv_obj_set_size(title_, std::max(1, avail - (ip_w ? ip_w + gap : 0)), lh1);
    set_text_if(ip_, t_.ip);
    lv_obj_set_pos(ip_, w_ - inset_ - std::max(1, ip_w), top);
    lv_obj_set_size(ip_, std::max(1, ip_w), lh1);
    /* line 2: the vendor and the MAC at the left; at the right the tag (a shape and a word), then the round trip */
    const int rtt_w = t_.rtt.empty() ? 0 : text_width(t_.rtt.c_str(), small_) + 2;
    set_text_if(rtt_, t_.rtt);
    lv_obj_set_pos(rtt_, w_ - inset_ - std::max(1, rtt_w), top + lh1);
    lv_obj_set_size(rtt_, std::max(1, rtt_w), lh2);
    int right = w_ - inset_ - (rtt_w ? rtt_w + gap : 0);
    if (pill_w) {
        const int px = right - pill_w, py = top + lh1 + (lh2 - pill_h_) / 2;
        pill_->place(px, py, pill_w, pill_h_);
        lv_obj_remove_flag(pill_->obj(), LV_OBJ_FLAG_HIDDEN);
        pill_->invalidate();
        set_text_if(tag_, t_.tag);
        lv_obj_remove_flag(tag_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(tag_, px + pill_h_ / 2 + pill_h_ / 2 + 4, top + lh1 + (lh2 - lh2) / 2);
        lv_obj_set_size(tag_, std::max(1, text_width(t_.tag.c_str(), small_) + 2), lh2);
        right = px - gap;
    } else {
        lv_obj_add_flag(pill_->obj(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tag_, LV_OBJ_FLAG_HIDDEN);
    }
    set_text_if(detail_, t_.detail);
    lv_obj_set_pos(detail_, inset_, top + lh1);
    lv_obj_set_size(detail_, std::max(1, right - inset_), lh2);
}

/* ================================================================ a network of the list */

WifiRowView::WifiRowView(int w, int inset, const lv_font_t *main, const lv_font_t *small, int min_h)
    : w_(w), inset_(inset), main_(main), small_(small)
{
    const int pad = std::max(4, space_s() / 2);
    h_ = std::max(min_h, line_height(main) + line_height(small) + 2 * pad);
}

WifiRowView::~WifiRowView() = default;

void WifiRowView::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    glyphs_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint(layer, a); });
    glyphs_->place(0, 0, w_, h_);
    title_ = make_label(host, main_, 0xE0E0E0);
    dbm_ = make_label(host, main_, kText);
    connected_ = make_label(host, small_, kOk);
    caption_ = make_label(host, small_, kLabel);
    security_ = make_label(host, small_, kLabel);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(dbm_, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_long_mode(connected_, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(security_, LV_LABEL_LONG_MODE_CLIP);
    apply();
}

void WifiRowView::detach()
{
    glyphs_.reset();
    title_ = dbm_ = connected_ = caption_ = security_ = nullptr;
}

void WifiRowView::set(const WifiRowTexts &texts)
{
    t_ = texts;
    apply();
}

void WifiRowView::apply()
{
    if (!title_) return;
    const int gap = space_m();
    const int avail = std::max(1, w_ - 2 * inset_);
    const int lh1 = line_height(main_), lh2 = line_height(small_);
    const int top = std::max(0, (h_ - lh1 - lh2) / 2);
    /* the right cluster of line 1: [check connected] [bars] dBm */
    const int dbm_w = text_width(t_.dbm.c_str(), main_) + 2;
    int x = w_ - inset_ - dbm_w;
    set_text_if(dbm_, t_.dbm);
    lv_obj_set_pos(dbm_, x, top);
    lv_obj_set_size(dbm_, dbm_w, lh1);
    bar_w_ = std::max(3, lh2 / 5);
    bar_gap_ = std::max(1, bar_w_ / 3);
    bar_h_ = std::max(8, lh1 * 7 / 10);
    const int bars_w = 4 * bar_w_ + 3 * bar_gap_;
    x -= gap / 2 + bars_w;
    bars_x_ = x;
    bars_y_ = top + (lh1 - bar_h_) / 2 + bar_h_;
    int left = x;
    if (t_.in_use) {
        const int cw = text_width(t_.connected.c_str(), small_) + 2;
        left = x - gap - cw;
        set_text_if(connected_, t_.connected);
        lv_obj_remove_flag(connected_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(connected_, left, top + (lh1 - lh2) / 2);
        lv_obj_set_size(connected_, cw, lh2);
        check_r_ = std::max(5, lh2 * 6 / 10);
        left -= 4 + 2 * check_r_;
        check_x_ = left + check_r_;
        check_y_ = top + lh1 / 2;
    } else {
        lv_obj_add_flag(connected_, LV_OBJ_FLAG_HIDDEN);
    }
    set_text_if(title_, t_.title);
    lv_obj_set_pos(title_, inset_, top);
    lv_obj_set_size(title_, std::max(1, left - gap - inset_), lh1);
    /* line 2: "ch 36, 5 GHz" then the security words (amber when weak) */
    const std::string cap = t_.security.empty() ? t_.caption : t_.caption + (t_.caption.empty() ? "" : ", ");
    const int cap_w = std::min(avail, text_width(cap.c_str(), small_) + 2);
    set_text_if(caption_, cap);
    lv_obj_set_pos(caption_, inset_, top + lh1);
    lv_obj_set_size(caption_, std::max(1, cap_w), lh2);
    set_text_if(security_, t_.security);
    set_color_if(security_, t_.weak_security ? kWarn : kLabel);
    lv_obj_set_pos(security_, inset_ + cap_w, top + lh1);
    lv_obj_set_size(security_, std::max(1, avail - cap_w), lh2);
    if (glyphs_) glyphs_->invalidate();
}

void WifiRowView::paint(lv_layer_t *layer, const lv_area_t &a)
{
    const std::uint32_t on = t_.bars >= 3 ? kOk : t_.bars == 2 ? kWarn : kBad;
    for (int i = 0; i < 4; ++i) {
        const int bh = std::max(3, bar_h_ * (i + 1) / 4);
        const int x1 = a.x1 + bars_x_ + i * (bar_w_ + bar_gap_);
        const int y2 = a.y1 + bars_y_;
        paint_fill(layer, lv_area_t{x1, y2 - bh, x1 + bar_w_ - 1, y2}, i < t_.bars ? on : kTrack, 1);
    }
    if (t_.in_use) draw_mark(layer, a.x1 + check_x_, a.y1 + check_y_, check_r_, Mark::Check, kOk);
}

/* ================================================================ the channels */

ChannelsBlock::ChannelsBlock(int w, int inset, const lv_font_t *caption, const lv_font_t *small)
    : w_(w), inset_(inset), caption_(caption), small_(small)
{
    pad_ = space_s();
    bars_h_ = 6 * line_height(small);
    layout_ = channels_layout(std::max(1, w - 2 * inset), line_height(caption), line_height(small), bars_h_, pad_, false, false);
}

ChannelsBlock::~ChannelsBlock() = default;

Box ChannelsBlock::part_box(int i) const
{
    const Box &b = i == 0 ? layout_.first : layout_.second;
    return Box{b.x + inset_, b.y, b.w, b.h};
}

void ChannelsBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    host_ = host;
    ui_.clear();
    build();
    place();
}

void ChannelsBlock::detach()
{
    for (Part &p : ui_) p.bars.reset();
    ui_.clear();
    host_ = nullptr;
}

void ChannelsBlock::set(const std::vector<ChannelsView> &parts)
{
    parts_ = parts;
    const bool d0 = parts_.size() > 0 && parts_[0].empty.empty();
    const bool d1 = parts_.size() > 1 && parts_[1].empty.empty();
    layout_ = channels_layout(std::max(1, w_ - 2 * inset_), line_height(caption_), line_height(small_), bars_h_, pad_, d0, d1);
    if (host_) {
        build();
        place();
    }
}

/* the labels of the parts: made as needed, the unused ones hidden */
void ChannelsBlock::build()
{
    if (!host_) return;
    const std::size_t np = std::min<std::size_t>(2, parts_.size());
    while (ui_.size() < np) {
        Part p;
        const int index = static_cast<int>(ui_.size());
        p.bars = std::make_unique<Painter>(host_, [this, index](lv_layer_t *layer, const lv_area_t &a) { paint_bars(index, layer, a); });
        p.title = label_at(host_, caption_, kText, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
        p.info = label_at(host_, small_, kLabel, LV_LABEL_LONG_MODE_WRAP, 0, 0, 1, 1);
        ui_.push_back(std::move(p));
    }
    for (std::size_t i = 0; i < ui_.size(); ++i) {
        Part &p = ui_[i];
        const std::size_t n = i < np ? parts_[i].bars.size() : 0;
        while (p.counts.size() < n) {
            p.counts.push_back(label_at(host_, small_, kText, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1));
            p.numbers.push_back(label_at(host_, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1));
            lv_obj_set_style_text_align(p.counts.back(), LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_align(p.numbers.back(), LV_TEXT_ALIGN_CENTER, 0);
        }
    }
}

void ChannelsBlock::place()
{
    if (!host_) return;
    const int lh = line_height(caption_), slh = line_height(small_);
    fits_ = true;
    for (std::size_t i = 0; i < ui_.size(); ++i) {
        Part &p = ui_[i];
        const bool used = i < parts_.size();
        const ChannelsView empty_view;
        const ChannelsView &v = used ? parts_[i] : empty_view;
        const Box b = part_box(static_cast<int>(i));
        const bool data = used && v.empty.empty();
        for (lv_obj_t *o : {p.title, p.info}) {
            if (used) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
        if (used) {
            lv_obj_remove_flag(p.bars->obj(), LV_OBJ_FLAG_HIDDEN);
            set_text_if(p.title, v.title);
            lv_obj_set_pos(p.title, b.x, b.y);
            lv_obj_set_size(p.title, b.w, lh);
            if (data) {
                p.bars->place(b.x, b.y + lh, b.w, bars_h_);
                set_text_if(p.info, v.busiest);
                lv_obj_set_pos(p.info, b.x, b.y + lh + bars_h_ + slh);
                lv_obj_set_size(p.info, b.w, slh);
            } else {
                p.bars->place(b.x, b.y + lh, 1, 1);
                set_text_if(p.info, v.empty);
                lv_obj_set_pos(p.info, b.x, b.y + lh);
                lv_obj_set_size(p.info, b.w, 2 * slh);
                lv_point_t s2{};
                lv_text_get_size(&s2, v.empty.c_str(), small_, 0, 0, b.w, LV_TEXT_FLAG_NONE);
                fits_ = fits_ && s2.y <= 2 * slh;
            }
            p.bars->invalidate();
        } else {
            lv_obj_add_flag(p.bars->obj(), LV_OBJ_FLAG_HIDDEN);
        }
        const std::size_t n = data ? v.bars.size() : 0;
        int best = -1;
        for (std::size_t k = 0; k < n; ++k)
            if (v.bars[k].best) best = static_cast<int>(k);
        const int slot = n ? std::max(1, b.w / static_cast<int>(n)) : 1;
        const int numw = text_width("149", small_) + 4;
        const int step = std::max(1, (numw + slot - 1) / slot);
        for (std::size_t k = 0; k < p.counts.size(); ++k) {
            lv_obj_t *cnt = p.counts[k], *num = p.numbers[k];
            if (k >= n) {
                lv_obj_add_flag(cnt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(num, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            const ChannelBar &bar = v.bars[k];
            const int cx = b.x + static_cast<int>(k) * slot + slot / 2;
            const int maxh = bars_h_ - slh - 2;
            const int bh = bar.weight > 0 ? std::max(3, maxh * bar.weight / std::max(1, v.top)) : 0;
            const int w = std::max(slot, text_width(std::to_string(bar.networks).c_str(), small_) + 2);
            if (bar.networks > 0) {
                lv_obj_remove_flag(cnt, LV_OBJ_FLAG_HIDDEN);
                set_text_if(cnt, std::to_string(bar.networks));
                lv_obj_set_pos(cnt, cx - w / 2, b.y + lh + bars_h_ - bh - slh - 2);
                lv_obj_set_size(cnt, w, slh);
            } else {
                lv_obj_add_flag(cnt, LV_OBJ_FLAG_HIDDEN);
            }
            const int ki = static_cast<int>(k);
            const bool show = ki == best || (ki % step == 0 && (best < 0 || std::abs(ki - best) >= step));
            if (show) {
                lv_obj_remove_flag(num, LV_OBJ_FLAG_HIDDEN);
                set_text_if(num, std::to_string(bar.channel));
                set_color_if(num, bar.best ? kOk : kLabel);
                const int nw = std::max(slot * step, numw);
                lv_obj_set_pos(num, cx - nw / 2, b.y + lh + bars_h_);
                lv_obj_set_size(num, nw, slh);
            } else {
                lv_obj_add_flag(num, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void ChannelsBlock::paint_bars(int part, lv_layer_t *layer, const lv_area_t &a)
{
    if (part >= static_cast<int>(parts_.size())) return;
    const ChannelsView &v = parts_[static_cast<std::size_t>(part)];
    if (!v.empty.empty() || v.bars.empty()) return;
    const int n = static_cast<int>(v.bars.size());
    const int w = a.x2 - a.x1 + 1;
    const int slot = std::max(1, w / n);
    const int slh = line_height(small_);
    const int maxh = bars_h_ - slh - 2;
    const int base = a.y2;
    paint_line(layer, a.x1, base, a.x2, base, kBorder, 1);
    for (int i = 0; i < n; ++i) {
        const ChannelBar &bar = v.bars[static_cast<std::size_t>(i)];
        const int bw = std::max(2, slot - std::max(2, slot / 5));
        const int x1 = a.x1 + i * slot + (slot - bw) / 2;
        if (bar.weight <= 0) {
            paint_fill(layer, lv_area_t{x1, base - 2, x1 + bw - 1, base - 1}, kTrack, 1);
            continue;
        }
        const int bh = std::max(3, maxh * bar.weight / std::max(1, v.top));
        const lv_area_t r{x1, base - bh, x1 + bw - 1, base - 1};
        const std::uint32_t rgb = bar.best ? kOk : bar.busiest ? kWarn : kBarBlue;
        paint_fill(layer, r, rgb, 1);
        if (bar.best) paint_ring(layer, lv_area_t{r.x1 - 1, r.y1 - 1, r.x2 + 1, r.y2 + 1}, kWhite, 2, 2);
        if (bar.busiest) paint_line(layer, r.x1, r.y1 - 1, r.x2, r.y1 - 1, kWhite, 1);
    }
}

/* ================================================================ the walk test */

WalkLiveBlock::WalkLiveBlock(int w, int h, int inset, const lv_font_t *caption, const lv_font_t *small, const lv_font_t *big)
    : w_(w), h_(h), inset_(inset), caption_(caption), small_(small), big_(big)
{
    layout_ = walk_layout(std::max(1, w - 2 * inset), h, line_height(caption), line_height(small), line_height(big), space_s());
    ylab_w_ = text_width("-100", small) + 2;
}

WalkLiveBlock::~WalkLiveBlock() = default;

void WalkLiveBlock::set_height(int h)
{
    h_ = h;
    layout_ = walk_layout(std::max(1, w_ - 2 * inset_), h_, line_height(caption_), line_height(small_), line_height(big_), space_s());
}

void WalkLiveBlock::resized(int w, int h)
{
    (void)w;
    set_height(h);
    relayout();
}

void WalkLiveBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    host_ = host;
    chart_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_chart(layer, a); });
    mark_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint_mark(layer, a); });
    big_label_ = label_at(host, big_, kText, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    unit_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    word_ = label_at(host, caption_, kText, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    zone_ = label_at(host, small_, kWarn, LV_LABEL_LONG_MODE_WRAP, 0, 0, 1, 1);
    where_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_DOTS, 0, 0, 1, 1);
    elapsed_ = label_at(host, caption_, kText, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    legend_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_WRAP, 0, 0, 1, 1);
    y_top_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    y_weak_ = label_at(host, small_, kWarn, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    y_bottom_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    x_left_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    x_right_ = label_at(host, small_, kLabel, LV_LABEL_LONG_MODE_CLIP, 0, 0, 1, 1);
    lv_obj_set_style_text_align(y_top_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(y_weak_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(y_bottom_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(x_right_, LV_TEXT_ALIGN_RIGHT, 0);
    relayout();
}

void WalkLiveBlock::detach()
{
    chart_.reset();
    mark_.reset();
    host_ = big_label_ = unit_ = word_ = zone_ = where_ = elapsed_ = legend_ = y_top_ = y_weak_ = y_bottom_ = x_left_ = x_right_ = nullptr;
}

void WalkLiveBlock::update(const WalkSnapshot &s)
{
    snap_ = s;
    live_ = walk_live(s);
    apply_texts();
}

/* the plot's rectangle inside a chart box at (x0, y0) of size w x h: the y labels at its left, the time labels under it */
namespace {
struct Plot {
    int x1, y1, x2, y2;
};
Plot plot_of(int x0, int y0, int w, int h, int ylab_w, int slh, int pad)
{
    Plot p;
    p.x1 = x0 + pad + ylab_w + 2;
    p.x2 = x0 + w - 1 - pad;
    p.y1 = y0 + pad + slh / 2;
    p.y2 = y0 + h - 1 - pad - slh;
    return p;
}
int y_of(const Plot &p, const WalkYScale &sc, double dbm)
{
    const double f = (sc.top - dbm) / static_cast<double>(std::max(1, sc.top - sc.bottom));
    return p.y1 + static_cast<int>(std::lround(std::min(1.0, std::max(0.0, f)) * (p.y2 - p.y1)));
}
} // namespace

void WalkLiveBlock::relayout()
{
    if (!host_) return;
    const int lh = line_height(caption_), slh = line_height(small_), bigh = line_height(big_), pad = space_s();
    const WalkLayout &L = layout_;
    problem_.clear();
    /* the chart and its labels */
    const Box cb{inset_ + L.chart.x, L.chart.y, L.chart.w, L.chart.h};
    chart_->place(cb.x, cb.y, cb.w, cb.h);
    chart_->invalidate();
    const int cpad = std::max(3, pad / 2);
    const Plot pl = plot_of(cb.x, cb.y, cb.w, cb.h, ylab_w_, slh, cpad);
    const WalkYScale sc = walk_y_scale(snap_.samples, snap_.weak_dbm);
    const int yw = y_of(pl, sc, snap_.weak_dbm);
    const auto place_y = [&](lv_obj_t *l, int y, const std::string &text) {
        set_text_if(l, text);
        lv_obj_set_pos(l, cb.x + cpad, y - slh / 2);
        lv_obj_set_size(l, ylab_w_, slh);
    };
    place_y(y_top_, pl.y1, std::to_string(sc.top));
    place_y(y_bottom_, pl.y2, std::to_string(sc.bottom));
    place_y(y_weak_, yw, std::to_string(snap_.weak_dbm));
    if (yw - pl.y1 < slh) lv_obj_add_flag(y_top_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(y_top_, LV_OBJ_FLAG_HIDDEN);
    if (pl.y2 - yw < slh) lv_obj_add_flag(y_bottom_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(y_bottom_, LV_OBJ_FLAG_HIDDEN);
    const WalkWindow win = walk_window(snap_);
    std::string left, right;
    if (!snap_.samples.empty()) {
        if (snap_.running()) {
            left = "-" + mmss(win.t1_s - win.t0_s);
            right = "now";
        } else {
            left = mmss(win.t0_s);
            right = mmss(win.t1_s);
        }
    }
    const int xw = std::max(1, (pl.x2 - pl.x1) / 2);
    set_text_if(x_left_, left);
    lv_obj_set_pos(x_left_, pl.x1, pl.y2 + cpad);
    lv_obj_set_size(x_left_, xw, slh);
    set_text_if(x_right_, right);
    lv_obj_set_pos(x_right_, pl.x2 - xw, pl.y2 + cpad);
    lv_obj_set_size(x_right_, xw, slh);
    set_text_if(legend_, walk_legend(snap_));
    lv_obj_set_pos(legend_, inset_ + L.legend.x, L.legend.y);
    lv_obj_set_size(legend_, L.legend.w, L.legend.h);
    lv_point_t sz{};
    lv_text_get_size(&sz, lv_label_get_text(legend_), small_, 0, 0, L.legend.w, LV_TEXT_FLAG_NONE);
    if (sz.y > L.legend.h) problem_ += "the legend, ";
    /* the reading */
    mark_d_ = lh;
    const int rx = inset_ + L.reading.x, ry = L.reading.y, rw = L.reading.w;
    const int bigw = text_width(live_.big.c_str(), big_) + 2;
    const int unitw = live_.unit.empty() ? 0 : text_width(live_.unit.c_str(), small_) + 2;
    set_text_if(big_label_, live_.big);
    set_text_if(unit_, live_.unit);
    set_text_if(word_, live_.word);
    set_color_if(word_, tone_rgb(live_.tone, kText));
    set_text_if(zone_, live_.zone);
    set_text_if(where_, live_.where);
    set_text_if(elapsed_, "Time " + live_.elapsed);
    mark_->invalidate();
    const int word_w = text_width(live_.word.c_str(), caption_) + 2;
    if (L.side) {
        int y = ry;
        mark_->place(rx, y, mark_d_, mark_d_);
        lv_obj_set_pos(word_, rx + mark_d_ + pad, y);
        lv_obj_set_size(word_, std::max(1, rw - mark_d_ - pad), lh);
        y += lh + pad / 2;
        lv_obj_set_pos(big_label_, rx, y);
        lv_obj_set_size(big_label_, bigw, bigh);
        lv_obj_set_pos(unit_, rx + bigw + 2, y + bigh - slh - 2);
        lv_obj_set_size(unit_, std::max(1, unitw), slh);
        if (bigw + 2 + unitw > rw) problem_ += "the reading, ";
        y += bigh;
        lv_obj_set_pos(zone_, rx, y);
        lv_obj_set_size(zone_, rw, 2 * slh);
        lv_text_get_size(&sz, live_.zone.c_str(), small_, 0, 0, rw, LV_TEXT_FLAG_NONE);
        if (sz.y > 2 * slh) problem_ += "the weak zone line, ";
        y += live_.zone.empty() ? 0 : sz.y + 2;
        lv_obj_set_pos(where_, rx, y);
        lv_obj_set_size(where_, rw, slh);
        const int ey = std::max(y + slh + 2, ry + L.reading.h - lh);
        lv_obj_set_pos(elapsed_, rx, ey);
        lv_obj_set_size(elapsed_, rw, lh);
        if (ey + lh > ry + L.reading.h + 1) problem_ += "the time, ";
    } else {
        const int bx = rx;
        lv_obj_set_pos(big_label_, bx, ry);
        lv_obj_set_size(big_label_, bigw, bigh);
        lv_obj_set_pos(unit_, bx + bigw + 2, ry + bigh - slh - 2);
        lv_obj_set_size(unit_, std::max(1, unitw), slh);
        const int cx = bx + std::max(bigw + unitw + 2 * pad, text_width("-100", big_) + text_width("dBm", small_) + 3 * pad);
        const int cw = std::max(1, inset_ + L.reading.x + rw - cx);
        int y = ry;
        mark_->place(cx, y, mark_d_, mark_d_);
        lv_obj_set_pos(word_, cx + mark_d_ + pad, y);
        lv_obj_set_size(word_, std::max(1, cw - mark_d_ - pad), lh);
        const int tw = text_width(("Time " + live_.elapsed).c_str(), caption_) + 2;
        lv_obj_set_pos(elapsed_, cx + cw - tw, y);
        lv_obj_set_size(elapsed_, tw, lh);
        if (mark_d_ + pad + word_w + pad + tw > cw) problem_ += "the word and the time, ";
        y += lh;
        const bool zone = !live_.zone.empty();
        lv_obj_set_pos(zone_, cx, y);
        lv_obj_set_size(zone_, cw, zone ? slh : 1);
        lv_obj_set_pos(where_, cx, y + (zone ? slh : 0));
        lv_obj_set_size(where_, cw, slh);
        if (zone && text_width(live_.zone.c_str(), small_) > cw) problem_ += "the weak zone line, ";
    }
    (void)lh;
}

void WalkLiveBlock::apply_texts() { relayout(); }

std::string WalkLiveBlock::layout_problem() const
{
    if (problem_.empty()) return "";
    std::string p = problem_;
    if (p.size() >= 2) p.erase(p.size() - 2);
    return p;
}

void WalkLiveBlock::paint_mark(lv_layer_t *layer, const lv_area_t &a)
{
    const int r = std::max(4, (a.x2 - a.x1) / 2 - 1);
    const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    const std::uint32_t rgb = live_.tone == Tone::Ok ? kOk : live_.tone == Tone::Warn ? kWarn : live_.tone == Tone::Bad ? kBad : kLabel;
    draw_mark(layer, cx, cy, r, live_.mark, rgb);
}

void WalkLiveBlock::paint_chart(lv_layer_t *layer, const lv_area_t &a)
{
    paint_fill(layer, a, kBlack, cp0_ui_tok(CP0_TOK_RADIUS_S));
    paint_ring(layer, a, kBorder, 1, cp0_ui_tok(CP0_TOK_RADIUS_S));
    const int slh = line_height(small_);
    const int cpad = std::max(3, space_s() / 2);
    const Plot pl = plot_of(a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1, ylab_w_, slh, cpad);
    const int iw = pl.x2 - pl.x1 + 1, ih = pl.y2 - pl.y1 + 1;
    if (iw < 12 || ih < 12) return;
    const WalkYScale sc = walk_y_scale(snap_.samples, snap_.weak_dbm);
    const WalkWindow win = walk_window(snap_);
    const int lw = std::max(2, space_s() / 3);
    paint_line(layer, pl.x1, pl.y1, pl.x2, pl.y1, kTrack, 1);
    paint_line(layer, pl.x1, pl.y2, pl.x2, pl.y2, kBorder, 1);
    /* the weak threshold, dashed */
    const int yw = y_of(pl, sc, snap_.weak_dbm);
    const int dash = std::max(6, lw * 3), gap = std::max(4, lw * 2);
    for (int x = pl.x1; x < pl.x2; x += dash + gap) paint_line(layer, x, yw, std::min(pl.x2, x + dash), yw, kWarn, std::max(2, lw - 1));
    const double span = std::max(1.0, win.t1_s - win.t0_s);
    const auto x_of = [&](double t_s) { return pl.x1 + static_cast<int>(std::lround((t_s - win.t0_s) / span * (iw - 1))); };
    int px = 0, py = 0;
    bool have = false;
    const int sq = std::max(3, lw + 2);
    for (const WalkSample &s : snap_.samples) {
        const double t = static_cast<double>(s.t_ms) / 1000.0;
        if (t < win.t0_s - 1e-6) continue;
        const int x = x_of(t);
        if (!s.linked) { /* no link: a red tick up from the baseline, the line breaks */
            paint_line(layer, x, pl.y2, x, pl.y2 - std::max(6, ih / 5), kBad, 2);
            have = false;
            continue;
        }
        const int y = y_of(pl, sc, s.dbm);
        if (have) paint_line(layer, px, py, x, y, s.weak ? kWarn : kBarBlue, s.weak ? lw + 1 : lw, true);
        if (s.weak) paint_fill(layer, lv_area_t{x - sq / 2, y - sq / 2, x - sq / 2 + sq - 1, y - sq / 2 + sq - 1}, kWarn, 0);
        px = x;
        py = y;
        have = true;
    }
    if (have && snap_.running()) paint_dot(layer, px, py, std::max(3, lw + 1), kText);
    /* access point changes: a tick down from the top edge ending in a diamond */
    for (const WalkRoam &r : snap_.roams) {
        const double t = static_cast<double>(r.t_ms) / 1000.0;
        if (t < win.t0_s || t > win.t1_s) continue;
        const int x = x_of(t);
        const int th = std::max(8, slh);
        paint_line(layer, x, pl.y1, x, pl.y1 + th, kText, 2);
        diamond(layer, x, pl.y1 + th + 3, 4, kText);
    }
}

} // namespace ui
} // namespace nt
