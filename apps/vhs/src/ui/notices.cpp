/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the toast (see notices.hpp).
 */
#include "notices.hpp"

#include "theme.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>

namespace vhs {
namespace ui {

Toast::Toast(lv_obj_t *parent, const Rect &area, int font_px, std::function<void()> on_tap)
    : area_(area), on_tap_(std::move(on_tap))
{
    bg_ = lv_obj_create(parent);
    lv_obj_remove_style_all(bg_);
    lv_obj_remove_flag(bg_, LV_OBJ_FLAG_SCROLLABLE);
    /* tappable only when asked: the canvas's 30 px pill is under the touch target */
    if (on_tap_) lv_obj_add_flag(bg_, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(bg_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bg_, area.x, area.y);
    lv_obj_set_size(bg_, area.w, area.h);
    lv_obj_set_style_bg_opa(bg_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bg_, lv_color_hex(col::kCream), 0);
    lv_obj_set_style_radius(bg_, area.h / 2, 0);
    if (on_tap_) lv_obj_add_event_cb(bg_, click_cb, LV_EVENT_CLICKED, this);
    text_ = lv_label_create(bg_);
    lv_obj_remove_flag(text_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(text_, cp0_ui_font_px(font_px), 0);
    lv_obj_set_style_text_align(text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(text_, std::max(1, area.w - area.h));
    lv_label_set_text(text_, "");
    lv_obj_add_flag(bg_, LV_OBJ_FLAG_HIDDEN);
}

void Toast::click_cb(lv_event_t *e)
{
    auto *self = static_cast<Toast *>(lv_event_get_user_data(e));
    if (self->on_tap_) self->on_tap_();
}

bool Toast::set(const Notice &n)
{
    if (n == cur_) return shown_;
    cur_ = n;
    shown_ = !n.text.empty();
    if (!shown_) {
        lv_obj_add_flag(bg_, LV_OBJ_FLAG_HIDDEN);
        return false;
    }
    lv_obj_set_style_bg_color(bg_, lv_color_hex(n.red ? col::kHot : col::kCream), 0);
    lv_obj_set_style_text_color(text_, lv_color_hex(n.red ? col::kWhite : col::kInk), 0);
    lv_label_set_text(text_, n.text.c_str());
    lv_obj_update_layout(text_);
    /* one line: the canvas's pill; more: taller, the same padding above and below */
    const int th = lv_obj_get_height(text_);
    const int pad_v = std::max(2, area_.h / 5);
    const int h = std::max(area_.h, th + 2 * pad_v);
    lv_obj_set_height(bg_, h);
    lv_obj_set_style_radius(bg_, std::min(h, area_.h) / 2, 0);
    lv_obj_center(text_);
    lv_obj_remove_flag(bg_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bg_);
    return true;
}

} // namespace ui
} // namespace vhs
