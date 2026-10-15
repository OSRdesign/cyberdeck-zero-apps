/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the blocks of the new pages (see chain.hpp).
 */
#include "chain.hpp"

#include "style.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace nt {
namespace ui {

namespace {

constexpr std::uint32_t kChainLine = 0x5A5A5A; /* the line the nodes hang on (the highlight bar is #2B2B2B) */

std::uint32_t look_rgb(HopLook look)
{
    switch (look) {
    case HopLook::Ok: return kOk;
    case HopLook::Slow: return kWarn;
    case HopLook::Lossy: return kBad;
    default: return kLabel;
    }
}

lv_obj_t *label_at(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb, lv_label_long_mode_t mode, int x, int y, int w,
                   int h)
{
    lv_obj_t *l = make_label(parent, font, rgb);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, std::max(1, w), std::max(1, h));
    return l;
}

int wrapped_height(const std::string &text, const lv_font_t *font, int width)
{
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, std::max(1, width), LV_TEXT_FLAG_NONE);
    return std::max(line_height(font), static_cast<int>(size.y));
}

} // namespace

/* ---------------------------------------------------------------- the columns */

int ChainStyle::min_row_h(const lv_font_t *main, const lv_font_t *small)
{
    return line_height(main) + line_height(small) + 3;
}

ChainStyle ChainStyle::make(int w, int row_h, int inset, const lv_font_t *main, const lv_font_t *small)
{
    ChainStyle s;
    s.main = main;
    s.small = small;
    s.w = w;
    s.row_h = row_h;
    s.inset = inset;
    s.gap = cp0_ui_tok(CP0_TOK_SPACE_S);
    s.lh = line_height(main);
    s.slh = line_height(small);
    s.y0 = std::max(1, (row_h - (s.lh + s.slh + 1)) / 2);
    s.node_r = std::max(5, s.lh * 42 / 100);
    s.node_cx = inset / 2 + s.node_r + 5;
    s.node_cy = s.y0 + s.lh / 2;
    s.right = w - inset;
    s.num_x = s.node_cx + s.node_r + 7;
    s.num_w = text_width("30", main) + 2;
    s.name_x = s.num_x + s.num_w + s.gap;
    s.tag_w = text_width("100 %", main) + 2;
    s.tag_x = s.right - s.tag_w;
    s.ms_w = text_width("1234 ms", main) + 2;
    s.ms_x = s.tag_x - s.gap - s.ms_w;
    const int region = std::max(10, s.ms_x - s.gap - s.name_x);
    s.ip_w = std::min(text_width("255.255.255.255", small) + 2, region / 3);
    const int share = std::max(10, region - s.ip_w - 2 * s.gap);
    s.name_w = std::max(1, share * 55 / 100);
    s.bar_w = std::max(1, share - s.name_w);
    s.ip_x = s.name_x + s.name_w + s.gap;
    s.bar_x = s.ip_x + s.ip_w + s.gap;
    s.bar_h = std::max(6, s.lh * 45 / 100);
    s.bar_full = std::max(1, s.bar_w * 85 / 100);
    s.stub = 3;
    return s;
}

/* ---------------------------------------------------------------- the node */

void draw_hop_node(lv_layer_t *layer, int cx, int cy, int r, HopLook look, bool destination)
{
    const std::uint32_t rgb = look_rgb(look);
    if (destination) paint_ring(layer, lv_area_t{cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4}, rgb, 2, LV_RADIUS_CIRCLE);
    switch (look) {
    case HopLook::Ok: paint_dot(layer, cx, cy, r, kOk); break;
    case HopLook::Slow: { /* an amber disc with a notch cut into its top */
        paint_dot(layer, cx, cy, r, kWarn);
        const int nw = std::max(2, r / 2);
        paint_fill(layer, lv_area_t{cx - nw / 2, cy - r - 1, cx + nw / 2, cy}, kInk);
        break;
    }
    case HopLook::Lossy: draw_mark(layer, cx, cy, r, Mark::Cross, kBad); break;
    case HopLook::Silent: { /* a hollow circle of dots */
        const int n = 10, dr = std::max(1, r / 5);
        for (int k = 0; k < n; ++k) {
            const double a = 6.283185307179586 * k / n;
            paint_dot(layer, cx + static_cast<int>(std::lround((r - dr) * std::cos(a))),
                      cy + static_cast<int>(std::lround((r - dr) * std::sin(a))), dr, kLabel);
        }
        break;
    }
    case HopLook::RateLimited:
        paint_ring(layer, lv_area_t{cx - r, cy - r, cx + r, cy + r}, kLabel, std::max(2, r / 3), LV_RADIUS_CIRCLE);
        break;
    case HopLook::Pending:
        paint_ring(layer, lv_area_t{cx - r, cy - r, cx + r, cy + r}, 0x6A6A6A, 1, LV_RADIUS_CIRCLE);
        break;
    }
}

/* ---------------------------------------------------------------- a hop */

HopRowView::HopRowView(const ChainStyle &style) : style_(style), h_(style.row_h) {}

HopRowView::~HopRowView() = default;

void HopRowView::set(const TraceHop &hop, double scale_ms, bool first, bool last)
{
    texts_ = hop_texts(hop);
    detail_ = hop_detail(hop);
    scale_ms_ = scale_ms;
    first_ = first;
    last_ = last;
    note_ = texts_.note;
    if (expanded_) measure_details();
    apply();
}

void HopRowView::set_expanded(bool expanded)
{
    expanded_ = expanded;
    if (expanded_) measure_details();
    else details_h_ = 0;
    apply();
}

void HopRowView::measure_details()
{
    const int w = std::max(1, style_.right - style_.name_x);
    const std::string lines[4] = {detail_.addresses, detail_.timing, detail_.counts, detail_.note};
    details_h_ = style_.gap / 2;
    for (int i = 0; i < 4; ++i) {
        heights_[i] = wrapped_height(lines[i], style_.small, w);
        details_h_ += heights_[i] + 2;
    }
    details_h_ += style_.gap / 2;
}

void HopRowView::attach(lv_obj_t *host, int w, int h)
{
    host_ = host;
    h_ = h;
    style_.w = w;
    const ChainStyle &s = style_;
    painter_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) { paint(layer, a); });
    painter_->place(0, 0, w, h);
    num_ = label_at(host, s.main, kLabel, LV_LABEL_LONG_MODE_CLIP, s.num_x, s.y0, s.num_w, s.lh);
    name_ = label_at(host, s.main, kText, LV_LABEL_LONG_MODE_DOTS, s.name_x, s.y0, s.name_w, s.lh);
    ip_ = label_at(host, s.small, kLabel, LV_LABEL_LONG_MODE_DOTS, s.ip_x, s.y0 + s.lh - s.slh, s.ip_w, s.slh);
    bar_word_ = label_at(host, s.main, kLabel, LV_LABEL_LONG_MODE_DOTS, s.bar_x, s.y0, s.bar_w, s.lh);
    avg_ = label_at(host, s.main, kText, LV_LABEL_LONG_MODE_CLIP, s.ms_x, s.y0, s.ms_w, s.lh);
    lv_obj_set_style_text_align(avg_, LV_TEXT_ALIGN_RIGHT, 0);
    loss_ = label_at(host, s.main, kText, LV_LABEL_LONG_MODE_CLIP, s.tag_x, s.y0, s.tag_w, s.lh);
    lv_obj_set_style_text_align(loss_, LV_TEXT_ALIGN_RIGHT, 0);
    note_label_ = label_at(host, s.small, kLabel, LV_LABEL_LONG_MODE_DOTS, s.name_x, s.y0 + s.lh + 1, s.right - s.name_x, s.slh);
    for (int i = 0; i < 4; ++i)
        detail_label_[i] = label_at(host, s.small, i == 3 ? kLabel : kText, LV_LABEL_LONG_MODE_WRAP, s.name_x, s.row_h, s.right - s.name_x,
                                    s.slh);
    apply();
}

void HopRowView::detach()
{
    painter_.reset();
    host_ = nullptr;
    num_ = name_ = ip_ = avg_ = loss_ = bar_word_ = note_label_ = nullptr;
    for (lv_obj_t *&l : detail_label_) l = nullptr;
}

void HopRowView::resized(int w, int h)
{
    style_.w = w;
    h_ = h;
    if (painter_) painter_->place(0, 0, w, h);
    layout_details();
}

void HopRowView::layout_details()
{
    int y = style_.row_h + style_.gap / 2;
    const std::string lines[4] = {detail_.addresses, detail_.timing, detail_.counts, detail_.note};
    for (int i = 0; i < 4; ++i) {
        if (!detail_label_[i]) continue;
        if (!expanded_) {
            lv_obj_add_flag(detail_label_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(detail_label_[i], LV_OBJ_FLAG_HIDDEN);
        set_text_if(detail_label_[i], lines[i]);
        lv_obj_set_pos(detail_label_[i], style_.name_x, y);
        lv_obj_set_size(detail_label_[i], std::max(1, style_.right - style_.name_x), heights_[i]);
        y += heights_[i] + 2;
    }
}

/* the texts and the colours of the labels, then a repaint of the row */
void HopRowView::apply()
{
    if (!host_) return;
    const HopTexts &t = texts_;
    set_text_if(num_, t.number);
    /* the name takes the address's column too when there is no address beside it */
    const int name_w = t.ip.empty() ? style_.ip_x + style_.ip_w - style_.name_x : style_.name_w;
    lv_obj_set_width(name_, std::max(1, name_w));
    set_text_if(name_, t.name);
    set_text_if(ip_, t.ip);
    set_text_if(bar_word_, t.has_bar ? std::string() : t.bar_word);
    set_text_if(avg_, t.avg);
    set_text_if(loss_, t.loss);
    set_color_if(loss_, tone_rgb(t.loss_tone, kLabel));
    set_text_if(note_label_, t.note);
    set_color_if(note_label_, t.note_tone == Tone::Normal ? kLabel : tone_rgb(t.note_tone));
    layout_details();
    if (painter_) painter_->invalidate();
}

void HopRowView::paint(lv_layer_t *layer, const lv_area_t &a)
{
    const ChainStyle &s = style_;
    const int x = a.x1, y = a.y1, h = static_cast<int>(a.y2 - a.y1 + 1);
    const int cx = x + s.node_cx, cy = y + s.node_cy;
    /* the line the nodes hang on: the first hop starts at its node, the last one ends there (open: down the details) */
    const int top = first_ ? cy : y;
    const int bottom = last_ && !expanded_ ? cy : y + h - 1;
    paint_line(layer, cx, top, cx, bottom, kChainLine, 2);
    draw_hop_node(layer, cx, cy, s.node_r, texts_.look, texts_.destination);
    if (!texts_.has_bar) return;
    /* the bar: the best-to-worst range lighter, the average solid over it, both on the chain's one scale */
    const int by = cy - s.bar_h / 2, bx = x + s.bar_x;
    const int avg_px = bar_px(texts_.avg_ms, scale_ms_, s.bar_full, s.stub);
    const int best_px = bar_px(texts_.best_ms, scale_ms_, s.bar_full, 1);
    const int worst_px = std::min(s.bar_w, bar_px(texts_.worst_ms, scale_ms_, s.bar_full, s.stub));
    if (worst_px > avg_px)
        paint_fill(layer, lv_area_t{bx + std::min(best_px, avg_px), by, bx + worst_px - 1, by + s.bar_h - 1}, kBlue, 0, LV_OPA_40);
    paint_fill(layer, lv_area_t{bx, by, bx + avg_px - 1, by + s.bar_h - 1}, kBlue, 0);
}

/* ---------------------------------------------------------------- the status badge */

BadgeBlock::BadgeBlock(int inset, const lv_font_t *caption, const lv_font_t *big)
    : inset_(inset), caption_(caption), big_(big)
{
    pad_ = cp0_ui_tok(CP0_TOK_SPACE_S);
    height_ = ResultBadge::height_for(caption, big, pad_) + 2 * pad_;
}

BadgeBlock::~BadgeBlock() = default;

void BadgeBlock::attach(lv_obj_t *host, int w, int h)
{
    w_ = w;
    badge_ = std::make_unique<ResultBadge>(host, inset_, pad_, std::max(1, w - 2 * inset_), std::max(1, h - 2 * pad_), caption_, big_);
    badge_->update(shown_);
}

void BadgeBlock::detach()
{
    badge_.reset();
}

void BadgeBlock::set(const PortSummary &summary)
{
    shown_ = summary;
    if (badge_) badge_->update(summary);
}

bool BadgeBlock::detail_fits() const
{
    if (w_ <= 0) return true;
    const int room_w = w_ - 2 * inset_;
    lv_point_t size{};
    lv_text_get_size(&size, shown_.detail.c_str(), caption_, 0, 0, room_w, LV_TEXT_FLAG_NONE);
    const int h = height_ - 2 * pad_;
    const int detail_h = h - (line_height(caption_) + pad_ + line_height(big_) + pad_);
    return static_cast<int>(size.y) <= detail_h && text_width(shown_.host.c_str(), caption_) <= room_w;
}

/* ---------------------------------------------------------------- the waterfall */

std::uint32_t WaterfallBlock::part_rgb(WaterPart part)
{
    switch (part) {
    case WaterPart::Dns: return 0x4DB6AC;      /* teal */
    case WaterPart::Connect: return 0x2D9CDB;  /* blue */
    case WaterPart::Tls: return 0x9575CD;      /* violet */
    case WaterPart::Wait: return 0xE57FB0;     /* pink */
    case WaterPart::Transfer: return 0xB0BEC5; /* light grey */
    }
    return kLabel;
}

WaterfallBlock::WaterfallBlock(const HttpTimings &timings, int w, int inset, const lv_font_t *font, const lv_font_t *bold)
    : t_(timings), font_(font), bold_(bold), inset_(inset)
{
    pad_ = cp0_ui_tok(CP0_TOK_SPACE_S);
    gap_ = pad_;
    lh_ = line_height(font);
    bar_h_ = std::max(14, lh_ * 80 / 100);
    sw_ = std::max(8, lh_ * 55 / 100);
    bar_w_ = std::max(10, w - 2 * inset);
    segs_ = waterfall(t_, bar_w_, 2);
    total_ = waterfall_total(t_);
    bar_y_ = pad_ + line_height(bold) + gap_;
    legend_y_ = bar_y_ + bar_h_ + gap_;
    int x = 0, y = legend_y_;
    for (const WaterSeg &s : segs_) {
        Legend l;
        l.text = water_label(s);
        l.part = s.part;
        const int item_w = sw_ + 4 + text_width(l.text.c_str(), font) + 3 * gap_;
        if (x > 0 && x + item_w - 3 * gap_ > bar_w_) {
            x = 0;
            y += lh_ + 2;
        }
        l.x = x;
        l.y = y;
        x += item_w;
        legend_.push_back(l);
    }
    height_ = y + lh_ + pad_ + gap_ / 2;
}

WaterfallBlock::~WaterfallBlock() = default;

void WaterfallBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)w;
    (void)h;
    const int x0 = inset_;
    lv_obj_t *title = label_at(host, bold_, kText, LV_LABEL_LONG_MODE_DOTS, x0, pad_, bar_w_, line_height(bold_));
    set_text_if(title, total_);
    bar_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) {
        paint_fill(layer, a, kBlack, 2);
        for (const WaterSeg &s : segs_) {
            if (s.w <= 0) continue;
            const int gapx = s.w >= 5 ? 1 : 0; /* a hairline between neighbours, never so much that a thin one vanishes */
            paint_fill(layer, lv_area_t{a.x1 + s.x, a.y1, a.x1 + s.x + s.w - 1 - gapx, a.y2}, part_rgb(s.part), 0);
        }
        paint_ring(layer, a, kBorder, 1, 2);
    });
    bar_->place(x0, bar_y_, bar_w_, bar_h_);
    for (const Legend &l : legend_) {
        lv_obj_t *lab = label_at(host, font_, kText, LV_LABEL_LONG_MODE_CLIP, x0 + l.x + sw_ + 4, l.y, text_width(l.text.c_str(), font_) + 2, lh_);
        set_text_if(lab, l.text);
    }
    swatches_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) {
        for (const Legend &l : legend_)
            paint_fill(layer, lv_area_t{a.x1 + l.x, a.y1 + l.y + (lh_ - sw_) / 2, a.x1 + l.x + sw_ - 1, a.y1 + l.y + (lh_ - sw_) / 2 + sw_ - 1},
                       part_rgb(l.part), 2);
    });
    swatches_->place(x0, 0, bar_w_, height_);
}

void WaterfallBlock::detach()
{
    bar_.reset();
    swatches_.reset();
}

/* ---------------------------------------------------------------- the certificate */

CertBlock::CertBlock(const CertBadge &badge, int inset, const lv_font_t *caption, const lv_font_t *big)
    : badge_(badge), caption_(caption), big_(big), inset_(inset)
{
    pad_ = cp0_ui_tok(CP0_TOK_SPACE_S);
    gap_ = pad_;
    mark_d_ = line_height(big);
    height_ = pad_ + line_height(caption) + gap_ + mark_d_ + (badge.detail.empty() ? 0 : gap_ + line_height(caption)) + pad_;
}

CertBlock::~CertBlock() = default;

void CertBlock::attach(lv_obj_t *host, int w, int h)
{
    (void)h;
    const int cw = std::max(1, w - 2 * inset_);
    const int lh = line_height(caption_);
    lv_obj_t *head = label_at(host, caption_, kLabel, LV_LABEL_LONG_MODE_DOTS, inset_, pad_, cw, lh);
    set_text_if(head, "Certificate");
    const int big_y = pad_ + lh + gap_;
    mark_ = std::make_unique<Painter>(host, [this](lv_layer_t *layer, const lv_area_t &a) {
        const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = std::max(4, mark_d_ / 2 - 1);
        if (badge_.mark == Mark::None) draw_mark(layer, cx, cy, r, Mark::None, kLabel);
        else if (badge_.mark == Mark::Dash && badge_.tone == Tone::Normal) paint_glyph(layer, cx, cy, r, Mark::Dash, kLabel);
        else draw_mark(layer, cx, cy, r, badge_.mark, tone_rgb(badge_.tone, kLabel));
    });
    mark_->place(inset_, big_y, mark_d_, mark_d_);
    lv_obj_t *word = label_at(host, big_, badge_.tone == Tone::Normal ? kText : tone_rgb(badge_.tone), LV_LABEL_LONG_MODE_DOTS,
                              inset_ + mark_d_ + 2 * gap_, big_y, std::max(1, cw - mark_d_ - 2 * gap_), line_height(big_));
    set_text_if(word, badge_.word);
    if (!badge_.detail.empty()) {
        lv_obj_t *detail = label_at(host, caption_, kText, LV_LABEL_LONG_MODE_DOTS, inset_, big_y + mark_d_ + gap_, cw, lh);
        set_text_if(detail, badge_.detail);
    }
}

void CertBlock::detach()
{
    mark_.reset();
}

} // namespace ui
} // namespace nt
