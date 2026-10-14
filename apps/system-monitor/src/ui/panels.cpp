/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the graphic panels (see panels.hpp). Every size from the layout service.
 */
#include "panels.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace sm {
namespace {

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *text(lv_obj_t *parent, std::uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, caption_font(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, "");
    return l;
}

void set_text_if(lv_obj_t *label, const std::string &t)
{
    const char *now = lv_label_get_text(label);
    if (!now || t != now) lv_label_set_text(label, t.c_str());
}

int line_height() { return lv_font_get_line_height(caption_font()); }

} // namespace

std::uint32_t tone_rgb(Tone tone, std::uint32_t normal)
{
    switch (tone) {
    case Tone::Warn: return kRgbWarn;
    case Tone::Error: return kRgbError;
    default: return normal;
    }
}

const lv_font_t *caption_font()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int px = m ? m->text_px[CP0_TEXT_CAPTION] : 18;
    if (const lv_font_t *f = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

int text_width(const char *t, const lv_font_t *font)
{
    if (!t || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, t, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* ------------------------------------------------------------------ the history chart */

int HistoryPanel::height_for(const cp0_ui_roller_page_t &l)
{
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    int h = std::max(2 * l.row_h, l.body_h * 2 / 5);
    h = std::min(h, l.body_h - 3 * l.row_h - 2 * s);
    return std::max(h, 2 * line_height() + s);
}

HistoryPanel::HistoryPanel(lv_obj_t *parent, const Box &box, int series, int marks, const cp0_ui_roller_page_t &l)
    : marks_(marks), width_(box.w), gap_(l.gap)
{
    const int s = cp0_ui_tok(CP0_TOK_SPACE_S);
    root_ = plain(parent);
    lv_obj_set_pos(root_, box.x, box.y);
    lv_obj_set_size(root_, box.w, box.h);
    title_[0] = text(root_, kRgbValue);
    title_[1] = text(root_, kRgbBlue);
    range_ = text(root_, kRgbLabel);
    lv_obj_set_pos(title_[0], 0, 0);

    chart_ = lv_chart_create(root_);
    lv_obj_remove_flag(chart_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(chart_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(chart_, 0, line_height() + s);
    lv_obj_set_size(chart_, box.w, box.h - line_height() - s);
    lv_obj_set_style_bg_color(chart_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(chart_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chart_, lv_color_hex(kRgbBorder), 0);
    lv_obj_set_style_border_width(chart_, 1, 0);
    lv_obj_set_style_radius(chart_, cp0_ui_tok(CP0_TOK_RADIUS_S), 0);
    lv_obj_set_style_pad_all(chart_, std::max(2, s / 2), 0);
    lv_obj_set_style_line_color(chart_, lv_color_hex(kRgbTrack), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart_, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart_, std::max(2, s / 3), LV_PART_ITEMS);
    lv_obj_set_style_width(chart_, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart_, 0, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(chart_, 0, 0);
    lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_, static_cast<uint32_t>(kHistory));
    lv_chart_set_div_line_count(chart_, 5, 0); /* the edges and 25 / 50 / 75 % */
    lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_axis_range(chart_, LV_CHART_AXIS_PRIMARY_Y, lo_, hi_);
    // the constant marks first (drawn under the data), then the data series
    const int total = marks + std::max(1, std::min(2, series));
    data_.assign(static_cast<std::size_t>(total), std::vector<std::int32_t>(kHistory, LV_CHART_POINT_NONE));
    for (int i = 0; i < total; ++i) {
        const bool data = i >= marks;
        const std::uint32_t rgb = !data ? kRgbTrack : (i - marks == 0 ? kRgbValue : kRgbBlue);
        lv_chart_series_t *ser = lv_chart_add_series(chart_, lv_color_hex(rgb), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_series_ext_y_array(chart_, ser, data_[static_cast<std::size_t>(i)].data());
        series_.push_back(ser);
    }
}

HistoryPanel::~HistoryPanel()
{
    if (root_) lv_obj_delete(root_);
}

void HistoryPanel::layout_title()
{
    const lv_font_t *font = caption_font();
    const int w0 = text_width(lv_label_get_text(title_[0]), font);
    const char *t1 = lv_label_get_text(title_[1]);
    const bool second = t1 && t1[0];
    const int w1 = second ? text_width(t1, font) : 0;
    const int used = w0 + (second ? gap_ + w1 : 0);
    // the range at the right; without its time window when the line has no room
    std::string range = range_text_;
    if (used + gap_ + text_width(range.c_str(), font) > width_) {
        const std::size_t comma = range.find(", ");
        if (comma != std::string::npos) range = range.substr(0, comma);
    }
    if (used + gap_ + text_width(range.c_str(), font) > width_) range.clear();
    set_text_if(range_, range);
    lv_obj_set_pos(title_[1], w0 + gap_, 0);
    if (second) lv_obj_remove_flag(title_[1], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(title_[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(range_, width_ - text_width(range.c_str(), font), 0);
}

void HistoryPanel::set_text(const std::string &first, std::uint32_t first_rgb, const std::string &second,
                            const std::string &range)
{
    if (first == shown_first_ && second == shown_second_ && range == range_text_ && first_rgb == shown_rgb_) return;
    shown_first_ = first;
    shown_second_ = second;
    shown_rgb_ = first_rgb;
    set_text_if(title_[0], first);
    lv_obj_set_style_text_color(title_[0], lv_color_hex(first_rgb), 0);
    set_text_if(title_[1], second);
    range_text_ = range;
    layout_title();
}

void HistoryPanel::set_range(std::int32_t lo, std::int32_t hi)
{
    if (hi <= lo) hi = lo + 1;
    if (lo == lo_ && hi == hi_) return;
    lo_ = lo;
    hi_ = hi;
    dirty_ = true;
    lv_chart_set_axis_range(chart_, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
}

void HistoryPanel::set_series(int index, const Ring &ring, double scale)
{
    const std::size_t i = static_cast<std::size_t>(marks_ + index);
    if (i >= data_.size()) return;
    std::vector<std::int32_t> &d = data_[i];
    const std::size_t n = std::min(ring.size(), d.size());
    const std::size_t pad = d.size() - n;
    for (std::size_t k = 0; k < d.size(); ++k) {
        const std::int32_t v = k < pad ? LV_CHART_POINT_NONE
                                       : static_cast<std::int32_t>(std::lround(ring.at(ring.size() - n + (k - pad)) * scale));
        if (d[k] != v) {
            d[k] = v;
            dirty_ = true;
        }
    }
}

void HistoryPanel::set_mark(int index, std::int32_t value, std::uint32_t rgb)
{
    if (index < 0 || index >= marks_) return;
    std::vector<std::int32_t> &d = data_[static_cast<std::size_t>(index)];
    std::fill(d.begin(), d.end(), value);
    dirty_ = true;
    lv_chart_set_series_color(chart_, series_[static_cast<std::size_t>(index)], lv_color_hex(rgb));
}

void HistoryPanel::refresh()
{
    if (!dirty_) return;
    dirty_ = false;
    lv_chart_refresh(chart_);
}

/* ------------------------------------------------------------------ the status line */

int StatusLine::height() { return line_height(); }

StatusLine::StatusLine(lv_obj_t *parent, const Box &box)
{
    label_ = text(parent, kRgbOk);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(label_, box.x, box.y);
    lv_obj_set_size(label_, box.w, line_height()); /* one line: dots, never a wrap */
}

StatusLine::~StatusLine()
{
    if (label_) lv_obj_delete(label_);
}

void StatusLine::set_text(const std::string &t) { set_text_if(label_, t); }

} // namespace sm
