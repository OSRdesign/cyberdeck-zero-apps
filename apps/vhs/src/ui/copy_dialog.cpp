/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the copy dialog and the drive list (see copy_dialog.hpp).
 */
#include "copy_dialog.hpp"

#include "theme.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vhs {
namespace ui {
namespace {

constexpr int kMaxRows = 4;

void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *label(lv_obj_t *parent, int px, std::uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, cp0_ui_font_px(px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, "");
    return l;
}

} // namespace

CopyDialog::CopyDialog(lv_obj_t *root, const ScenesLayout &geo, int target, std::function<void()> cancel,
                       std::function<void(int)> pick, std::function<void()> close)
    : geo_(geo), cancel_(std::move(cancel)), pick_(std::move(pick)), close_(std::move(close))
{
    const double u = geo.u;
    auto R = [u](double v) { return static_cast<int>(std::floor(v * u + 1e-9)); };
    /* the canvas box: 376x170u at (44, 46) in the panel; narrower panels: the panel less 8u each side */
    box_w_ = std::min(R(376), geo.panel.w - 2 * R(8));
    pad_ = R(18);
    bar_w_ = box_w_ - 2 * pad_;
    const int btn_h = std::max(R(34), target);
    btn_h_ = btn_h;
    row_h_ = std::max(R(36), cp0_ui_tok(CP0_TOK_ROW_H)); /* a drive row: at least ROW (the touch rule) */
    box_h_ = R(128) + btn_h + R(8);
    box_x_ = geo.panel.x + (geo.panel.w - box_w_) / 2;
    box_y_ = geo.panel.y + std::max(R(8), std::min(R(46), (geo.panel.h - box_h_) / 2));

    dim_ = lv_obj_create(root);
    plain(dim_);
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_CLICKABLE); /* a tap outside: nothing (the copy goes on); closes the drive list */
    lv_obj_set_pos(dim_, geo.panel.x, geo.panel.y);
    lv_obj_set_size(dim_, geo.panel.w, geo.panel.h);
    lv_obj_set_style_bg_color(dim_, lv_color_hex(col::kPanel), 0);
    lv_obj_set_style_bg_opa(dim_, 178, 0); /* the canvas: the list at 30 % */
    lv_obj_set_user_data(dim_, reinterpret_cast<void *>(static_cast<std::intptr_t>(-2)));
    lv_obj_add_event_cb(dim_, cb, LV_EVENT_CLICKED, this);

    box_ = lv_obj_create(root);
    plain(box_);
    lv_obj_set_pos(box_, box_x_, box_y_);
    lv_obj_set_size(box_, box_w_, box_h_);
    lv_obj_set_style_bg_color(box_, lv_color_hex(col::kCream), 0);
    lv_obj_set_style_bg_opa(box_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box_, R(12), 0);
    lv_obj_set_style_shadow_width(box_, R(20), 0);
    lv_obj_set_style_shadow_opa(box_, LV_OPA_60, 0);
    title_ = label(box_, R(16), col::kInk);
    lv_obj_set_pos(title_, pad_, R(14));
    lv_obj_set_width(title_, bar_w_);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
    text_ = label(box_, R(12), col::kSheetKey);
    lv_obj_set_pos(text_, pad_, R(42));
    lv_obj_set_width(text_, bar_w_);
    lv_label_set_long_mode(text_, LV_LABEL_LONG_MODE_WRAP);
    track_ = lv_obj_create(box_);
    plain(track_);
    lv_obj_set_pos(track_, pad_, R(94));
    lv_obj_set_size(track_, bar_w_, std::max(4, R(10)));
    lv_obj_set_style_radius(track_, R(5), 0);
    lv_obj_set_style_bg_color(track_, lv_color_hex(col::kSheetSel), 0);
    lv_obj_set_style_bg_opa(track_, LV_OPA_COVER, 0);
    fill_ = lv_obj_create(track_);
    plain(fill_);
    lv_obj_set_size(fill_, 0, std::max(4, R(10)));
    lv_obj_set_style_radius(fill_, R(5), 0);
    lv_obj_set_style_bg_color(fill_, lv_color_hex(col::kPrimary), 0);
    lv_obj_set_style_bg_opa(fill_, LV_OPA_COVER, 0);
    progress_ = label(box_, R(12), col::kInk);
    lv_obj_set_pos(progress_, pad_, R(110));
    cancel_btn_ = lv_button_create(box_);
    lv_obj_remove_style_all(cancel_btn_);
    lv_obj_add_flag(cancel_btn_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(cancel_btn_, pad_, R(128));
    lv_obj_set_size(cancel_btn_, bar_w_, btn_h);
    lv_obj_set_style_radius(cancel_btn_, R(6), 0);
    lv_obj_set_style_bg_color(cancel_btn_, lv_color_hex(col::kPrimary), 0);
    lv_obj_set_style_bg_opa(cancel_btn_, LV_OPA_COVER, 0);
    lv_obj_t *cl = label(cancel_btn_, R(14), col::kWhite);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
    lv_obj_set_user_data(cancel_btn_, reinterpret_cast<void *>(static_cast<std::intptr_t>(-1)));
    lv_obj_add_event_cb(cancel_btn_, cb, LV_EVENT_CLICKED, this);
    for (int i = 0; i < kMaxRows; ++i) {
        lv_obj_t *r = lv_obj_create(box_);
        plain(r);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(r, 0, R(46) + i * row_h_);
        lv_obj_set_size(r, box_w_, row_h_);
        lv_obj_set_style_bg_color(r, lv_color_hex(col::kSheetSel), 0);
        lv_obj_t *a = label(r, R(15), col::kInk);
        lv_obj_align(a, LV_ALIGN_LEFT_MID, pad_, 0);
        lv_obj_t *b = label(r, R(13), col::kSheetKey);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -pad_, 0);
        lv_obj_set_user_data(r, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, this);
        rows_.push_back(r);
    }
    hide();
}

void CopyDialog::cb(lv_event_t *e)
{
    auto *self = static_cast<CopyDialog *>(lv_event_get_user_data(e));
    const int id = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(e)))));
    if (id == -1) {
        if (self->cancel_) self->cancel_();
    } else if (id == -2) {
        if (self->picking_ && self->close_) self->close_();
    } else if (self->picking_) {
        self->sel_ = id;
        self->style_rows();
        self->choose();
    }
}

void CopyDialog::show_progress(const std::string &title, const std::string &text, double fraction, const std::string &progress)
{
    picking_ = false;
    for (lv_obj_t *r : rows_) lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *o : {text_, track_, progress_, cancel_btn_}) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(title_, title.c_str());
    lv_label_set_text(text_, text.c_str());
    /* the canvas's places (bar at 94u, Cancel at 128u); a line that wraps to more rows (narrow panels) moves them down */
    lv_obj_update_layout(text_);
    auto R = [this](double v) { return static_cast<int>(std::floor(v * geo_.u + 1e-9)); };
    const int track_y = std::max(R(94), R(42) + lv_obj_get_height(text_) + R(10));
    lv_obj_set_y(track_, track_y);
    lv_obj_set_y(progress_, track_y + R(16));
    lv_obj_set_y(cancel_btn_, track_y + R(34));
    lv_obj_set_height(box_, track_y + R(34) + btn_h_ + R(8));
    lv_label_set_text(progress_, progress.c_str());
    const double f = std::max(0.0, std::min(1.0, fraction));
    lv_obj_set_width(fill_, static_cast<int>(std::lround(f * bar_w_)));
    lv_obj_remove_flag(dim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(box_, LV_OBJ_FLAG_HIDDEN);
    if (!shown_) {
        lv_obj_move_foreground(dim_);
        lv_obj_move_foreground(box_);
    }
    shown_ = true;
}

void CopyDialog::show_drives(const std::vector<std::pair<std::string, std::string>> &rows)
{
    picking_ = true;
    n_rows_ = std::min(kMaxRows, static_cast<int>(rows.size()));
    sel_ = 0;
    for (lv_obj_t *o : {text_, track_, progress_, cancel_btn_}) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(title_, kDrivesTitle);
    for (int i = 0; i < kMaxRows; ++i) {
        lv_obj_t *r = rows_[static_cast<std::size_t>(i)];
        if (i >= n_rows_) {
            lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_t *name = lv_obj_get_child(r, 0), *value = lv_obj_get_child(r, 1);
        lv_label_set_text(value, rows[static_cast<std::size_t>(i)].second.c_str());
        /* the value whole at the right, the name cut with dots before it where both do not fit */
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, LV_SIZE_CONTENT);
        lv_label_set_text(name, rows[static_cast<std::size_t>(i)].first.c_str());
        lv_obj_update_layout(r);
        const int room = box_w_ - 2 * pad_ - lv_obj_get_width(value) - static_cast<int>(12 * geo_.u);
        if (lv_obj_get_width(name) > room) lv_obj_set_width(name, std::max(1, room));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, pad_, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
    }
    const int h = static_cast<int>(46 * geo_.u) + n_rows_ * row_h_ + static_cast<int>(12 * geo_.u);
    lv_obj_set_height(box_, std::min(h, geo_.panel.h - static_cast<int>(16 * geo_.u)));
    style_rows();
    lv_obj_remove_flag(dim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(dim_);
    lv_obj_move_foreground(box_);
    shown_ = true;
}

void CopyDialog::hide()
{
    shown_ = picking_ = false;
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
}

void CopyDialog::style_rows()
{
    for (int i = 0; i < n_rows_; ++i)
        lv_obj_set_style_bg_opa(rows_[static_cast<std::size_t>(i)], i == sel_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void CopyDialog::move(int delta)
{
    if (!picking_ || n_rows_ == 0) return;
    sel_ = (sel_ + delta + n_rows_) % n_rows_;
    style_rows();
}

void CopyDialog::choose()
{
    if (picking_ && pick_ && sel_ < n_rows_) pick_(sel_);
}

} // namespace ui
} // namespace vhs
