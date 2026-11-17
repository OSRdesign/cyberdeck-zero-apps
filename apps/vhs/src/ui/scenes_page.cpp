/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Scenes page (see scenes_page.hpp).
 */
#include "scenes_page.hpp"

#include "core/live/clock.hpp"
#include "theme.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace vhs {
namespace ui {
namespace {

constexpr std::int64_t kHoldMs = 1500;
constexpr int kHoldSlop = 16;
const char *const kChipNames[3] = {"All", "Movies", "Photos"};

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

void set_text(lv_obj_t *l, const std::string &t)
{
    if (std::strcmp(lv_label_get_text(l), t.c_str()) != 0) lv_label_set_text(l, t.c_str());
}

void vis(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

ScenesPage::ScenesPage(lv_obj_t *root, const ScenesLayout &geo, ScenesActions act) : geo_(geo), act_(std::move(act))
{
    root_ = lv_obj_create(root);
    plain(root_);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));

    panel_ = lv_obj_create(root_);
    plain(panel_);
    lv_obj_set_pos(panel_, geo_.panel.x, geo_.panel.y);
    lv_obj_set_size(panel_, geo_.panel.w, geo_.panel.h);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(col::kPanel), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel_, static_cast<int>(10 * geo_.u), 0);

    if (geo_.head_right) {
        /* the Square class: "N scenes" right-aligned at the chips' line */
        head_ = label(root_, geo_.head_px, col::kStatus);
        lv_obj_set_pos(head_, geo_.head.x, geo_.head.y + (geo_.head.h - geo_.head_px * 5 / 4) / 2);
        lv_obj_set_size(head_, geo_.head.w, geo_.head_px * 5 / 4);
        lv_obj_set_style_text_align(head_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(head_, LV_LABEL_LONG_MODE_CLIP);
    } else {
        head_ = label(root_, geo_.head_px, col::kMuted);
        lv_obj_set_pos(head_, geo_.head.x, geo_.head.y + (geo_.head.h - geo_.head_px * 5 / 4) / 2);
        lv_obj_set_style_text_letter_space(head_, 1, 0);
        lv_obj_set_size(head_, std::max(1, geo_.chip_hit[0].x - geo_.head.x), geo_.head_px * 5 / 4); /* never under the chips */
        lv_label_set_long_mode(head_, LV_LABEL_LONG_MODE_CLIP);
    }

    /* the rows: a vertical scrolling area, the pool of rows in view over a spacer of the whole height */
    list_ = lv_obj_create(root_);
    plain(list_);
    lv_obj_set_pos(list_, geo_.list.x, geo_.list.y);
    lv_obj_set_size(list_, geo_.list.w, geo_.list.h);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(list_, scroll_cb, LV_EVENT_SCROLL, this);
    spacer_ = lv_obj_create(list_);
    plain(spacer_);
    lv_obj_set_size(spacer_, 1, 1);
    const int c = geo_.chev;
    chev_pts_[0] = lv_point_precise_t{c * 9 / 24, c * 6 / 24};
    chev_pts_[1] = lv_point_precise_t{c * 15 / 24, c * 12 / 24};
    chev_pts_[2] = lv_point_precise_t{c * 9 / 24, c * 18 / 24};
    const int visible = geo_.list.h / std::max(1, geo_.row_h) + 2;
    for (int i = 0; i < visible; ++i) pool_.push_back(make_row());

    empty_ = label(root_, geo_.empty_px, col::kMuted);
    lv_obj_set_width(empty_, geo_.list.w - static_cast<int>(48 * geo_.u));
    lv_label_set_long_mode(empty_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(empty_, geo_.list.x + static_cast<int>(24 * geo_.u), geo_.list.y + geo_.list.h / 3);
    set_text(empty_, kScenesEmpty);
    vis(empty_, false);

    for (int i = 0; i < 3; ++i) {
        chip_[i] = lv_obj_create(root_);
        plain(chip_[i]);
        lv_obj_set_pos(chip_[i], geo_.chip[i].x, geo_.chip[i].y);
        lv_obj_set_size(chip_[i], geo_.chip[i].w, geo_.chip[i].h);
        lv_obj_set_style_radius(chip_[i], geo_.chip_radius, 0);
        lv_obj_set_style_bg_opa(chip_[i], LV_OPA_COVER, 0);
        chip_text_[i] = label(chip_[i], geo_.chip_px, col::kText);
        lv_label_set_text(chip_text_[i], kChipNames[i]);
        lv_obj_center(chip_text_[i]);
        /* the hit box: at least ROW high (the canvas's chip is 24u) */
        chip_hit_[i] = lv_obj_create(root_);
        plain(chip_hit_[i]);
        lv_obj_add_flag(chip_hit_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(chip_hit_[i], geo_.chip_hit[i].x, geo_.chip_hit[i].y);
        lv_obj_set_size(chip_hit_[i], geo_.chip_hit[i].w, geo_.chip_hit[i].h);
        lv_obj_set_user_data(chip_hit_[i], reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(chip_hit_[i], chip_cb, LV_EVENT_CLICKED, this);
    }
    if (geo_.square) {
        /* the hint line under the list (canvas Scenes): what a tap and a hold do */
        hint_l_ = label(root_, geo_.hint_px, col::kStatus);
        lv_label_set_text(hint_l_, "Tap to select, tap again to open");
        hint_r_ = label(root_, geo_.hint_px, col::kStatus);
        lv_label_set_text(hint_r_, "Hold a row for options");
        lv_point_t sz{0, 0};
        lv_text_get_size(&sz, "Hold a row for options", cp0_ui_font_px(geo_.hint_px), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int line = geo_.hint_px * 5 / 4;
        const int y = geo_.hint.y + (geo_.hint.h - line) / 2;
        const int rw = std::min(geo_.hint.w, sz.x + 2);
        lv_obj_set_size(hint_r_, rw, line);
        lv_obj_set_pos(hint_r_, geo_.hint.x + geo_.hint.w - rw, y);
        lv_obj_set_size(hint_l_, std::max(1, geo_.hint.w - rw - geo_.hint_px), line);
        lv_label_set_long_mode(hint_l_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(hint_l_, geo_.hint.x, y);
    }
    toast_ = std::make_unique<Toast>(root_, geo_.toast, geo_.toast_px, nullptr);
    vis(root_, false);
}

ScenesPage::~ScenesPage()
{
    for (RowW &w : pool_)
        if (w.img && lv_obj_is_valid(w.img)) lv_image_set_src(w.img, nullptr);
}

ScenesPage::RowW ScenesPage::make_row()
{
    RowW w;
    w.row = lv_obj_create(list_);
    plain(w.row);
    lv_obj_add_flag(w.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(w.row, LV_OBJ_FLAG_EVENT_BUBBLE); /* a drag scrolls the list */
    lv_obj_set_size(w.row, geo_.list.w, geo_.row_h);
    lv_obj_set_style_bg_color(w.row, lv_color_hex(col::kButton), 0);
    lv_obj_set_style_bg_opa(w.row, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(w.row, row_cb, LV_EVENT_ALL, this);
    w.thumb_box = lv_obj_create(w.row);
    plain(w.thumb_box);
    lv_obj_set_pos(w.thumb_box, geo_.thumb.x, geo_.thumb.y);
    lv_obj_set_size(w.thumb_box, geo_.thumb.w, geo_.thumb.h);
    lv_obj_set_style_bg_color(w.thumb_box, lv_color_hex(col::kThumb), 0);
    lv_obj_set_style_bg_opa(w.thumb_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(w.thumb_box, std::max(1, static_cast<int>(3 * geo_.u)), 0);
    w.img = lv_image_create(w.thumb_box);
    lv_obj_remove_flag(w.img, LV_OBJ_FLAG_CLICKABLE);
    w.name = label(w.row, geo_.name_px, col::kText);
    lv_obj_set_pos(w.name, geo_.name_x, geo_.name_y);
    lv_obj_set_height(w.name, geo_.name_line); /* one line: cut with dots, never wrapped */
    lv_label_set_long_mode(w.name, LV_LABEL_LONG_MODE_DOTS);
    w.lock = label(w.row, geo_.lock_px, col::kAccent);
    lv_label_set_text(w.lock, "locked");
    w.meta = label(w.row, geo_.meta_px, col::kMuted);
    lv_obj_set_pos(w.meta, geo_.name_x, geo_.meta_y);
    lv_obj_set_size(w.meta, std::max(1, geo_.name_w + (geo_.date_r ? 0 : static_cast<int>(20 * geo_.u))), geo_.meta_line);
    lv_label_set_long_mode(w.meta, LV_LABEL_LONG_MODE_DOTS);
    if (geo_.date_r > 0) {
        w.date = label(w.row, geo_.date_px, col::kMuted);
        lv_obj_set_style_text_align(w.date, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (geo_.chev > 0) { /* the Square canvas draws none */
        w.chev = lv_line_create(w.row);
        lv_line_set_points(w.chev, chev_pts_, 3);
        lv_obj_set_style_line_color(w.chev, lv_color_hex(col::kMuted), 0);
        lv_obj_set_style_line_width(w.chev, std::max(2, geo_.chev / 8), 0);
        lv_obj_set_style_line_rounded(w.chev, true, 0);
        lv_obj_remove_flag(w.chev, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(w.chev, geo_.chev, geo_.chev);
        lv_obj_set_pos(w.chev, geo_.list.w - geo_.chev_r - geo_.chev, geo_.chev_y);
    }
    vis(w.row, false);
    return w;
}

void ScenesPage::fill(RowW &w, int index)
{
    const SceneRow &r = rows_[static_cast<std::size_t>(index)];
    w.index = index;
    lv_obj_set_user_data(w.row, reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_set_pos(w.row, 0, index * geo_.row_h);
    vis(w.row, true);
    lv_obj_set_style_bg_opa(w.row, index == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    /* the name, then " locked" right after it (canvas row 3) */
    set_text(w.name, r.label);
    lv_obj_set_width(w.name, LV_SIZE_CONTENT);
    lv_obj_update_layout(w.name);
    const int room = std::max(static_cast<int>(40 * geo_.u), geo_.name_w - (r.e.locked ? static_cast<int>(52 * geo_.u) : 0));
    if (lv_obj_get_width(w.name) > room) lv_obj_set_width(w.name, room);
    vis(w.lock, r.e.locked);
    if (r.e.locked) {
        lv_obj_update_layout(w.name);
        lv_obj_set_pos(w.lock, geo_.name_x + lv_obj_get_width(w.name) + static_cast<int>(5 * geo_.u),
                       geo_.name_y + (geo_.name_px - geo_.lock_px) * 3 / 4);
    }
    set_text(w.meta, r.meta);
    if (w.date) {
        set_text(w.date, r.date);
        lv_obj_update_layout(w.date);
        lv_obj_set_pos(w.date, geo_.list.w - geo_.date_r - lv_obj_get_width(w.date), geo_.date_y + (geo_.name_px - geo_.date_px) / 2);
    }
    const Picture *p = r.thumb.get();
    if (p != w.pic) {
        w.pic = p;
        if (p && p->w > 0) {
            std::memset(&w.dsc, 0, sizeof w.dsc);
            w.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            w.dsc.header.cf = p->fmt == PixFmt::RGB565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_XRGB8888;
            w.dsc.header.w = static_cast<std::uint32_t>(p->w);
            w.dsc.header.h = static_cast<std::uint32_t>(p->h);
            w.dsc.header.stride = static_cast<std::uint32_t>(p->stride);
            w.dsc.data = p->px.data();
            w.dsc.data_size = static_cast<std::uint32_t>(p->px.size());
            lv_image_cache_drop(&w.dsc);
            lv_image_set_src(w.img, &w.dsc);
            lv_obj_set_pos(w.img, (geo_.thumb.w - p->w) / 2, (geo_.thumb.h - p->h) / 2);
            vis(w.img, true);
        } else {
            lv_image_set_src(w.img, nullptr);
            vis(w.img, false);
        }
    }
}

void ScenesPage::layout_rows(bool force)
{
    const int n = static_cast<int>(rows_.size());
    lv_obj_set_size(spacer_, 1, std::max(1, n * geo_.row_h));
    const int first = std::max(0, static_cast<int>(lv_obj_get_scroll_y(list_)) / std::max(1, geo_.row_h));
    std::vector<bool> used(pool_.size(), false);
    /* rows already showing an index in view keep it */
    for (std::size_t k = 0; k < pool_.size(); ++k) {
        RowW &w = pool_[k];
        if (w.index >= first && w.index < first + static_cast<int>(pool_.size()) && w.index < n) used[k] = true;
        else w.index = -1;
    }
    for (int i = first; i < std::min(n, first + static_cast<int>(pool_.size())); ++i) {
        int slot = -1;
        for (std::size_t k = 0; k < pool_.size(); ++k)
            if (pool_[k].index == i) slot = static_cast<int>(k);
        if (slot < 0) {
            for (std::size_t k = 0; k < pool_.size() && slot < 0; ++k)
                if (!used[k]) slot = static_cast<int>(k);
            if (slot < 0) break;
            used[static_cast<std::size_t>(slot)] = true;
            fill(pool_[static_cast<std::size_t>(slot)], i);
        } else if (force) {
            fill(pool_[static_cast<std::size_t>(slot)], i);
        }
    }
    for (RowW &w : pool_)
        if (w.index < 0) vis(w.row, false);
}

void ScenesPage::show(bool on)
{
    shown_ = on;
    vis(root_, on);
    if (!on) cancel_hold();
}

void ScenesPage::update(const ScenesView &v, const Notice &notice)
{
    const int total = v.movies + v.photos;
    if (geo_.square) set_text(head_, std::to_string(total) + (total == 1 ? " scene" : " scenes"));
    else set_text(head_, "SCENES, " + std::to_string(total));
    const int f = static_cast<int>(v.filter);
    if (f != filter_) {
        filter_ = f;
        for (int i = 0; i < 3; ++i) {
            if (geo_.square) {
                /* the chosen filter: the pressed grey with an amber ring (canvas Scenes) */
                lv_obj_set_style_bg_color(chip_[i], lv_color_hex(i == f ? col::kButtonPressed : col::kButton), 0);
                lv_obj_set_style_border_color(chip_[i], lv_color_hex(col::kAccent), 0);
                lv_obj_set_style_border_width(chip_[i], i == f ? std::max(2, geo_.chip[i].h / 18) : 0, 0);
            } else {
                lv_obj_set_style_bg_color(chip_[i], lv_color_hex(i == f ? col::kPrimary : col::kButton), 0);
            }
            lv_obj_set_style_text_color(chip_text_[i], lv_color_hex(i == f ? col::kWhite : col::kText), 0);
        }
        lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
        selected_ = 0;
        gen_ = ~0ull;
    }
    if (v.gen != gen_) {
        gen_ = v.gen;
        rows_ = v.rows;
        selected_ = std::max(0, std::min(selected_, static_cast<int>(rows_.size()) - 1));
        layout_rows(true);
    }
    vis(empty_, !v.loading && rows_.empty());
    toast_->set(notice);
}

void ScenesPage::select(int index)
{
    if (rows_.empty()) return;
    selected_ = std::max(0, std::min(index, static_cast<int>(rows_.size()) - 1));
    for (RowW &w : pool_)
        if (w.index >= 0) lv_obj_set_style_bg_opa(w.row, w.index == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    scroll_into_view(selected_);
}

void ScenesPage::move(int delta)
{
    select(selected_ + delta);
}

void ScenesPage::scroll_into_view(int index)
{
    const int y = index * geo_.row_h, top = static_cast<int>(lv_obj_get_scroll_y(list_));
    if (y < top) lv_obj_scroll_to_y(list_, y, LV_ANIM_OFF);
    else if (y + geo_.row_h > top + geo_.list.h) lv_obj_scroll_to_y(list_, y + geo_.row_h - geo_.list.h, LV_ANIM_OFF);
    layout_rows(false);
}

void ScenesPage::scroll_cb(lv_event_t *e)
{
    auto *self = static_cast<ScenesPage *>(lv_event_get_user_data(e));
    self->cancel_hold();
    self->layout_rows(false);
}

void ScenesPage::chip_cb(lv_event_t *e)
{
    auto *self = static_cast<ScenesPage *>(lv_event_get_user_data(e));
    const int i = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(e)))));
    if (self->act_.filter) self->act_.filter(static_cast<SceneFilter>(i));
}

void ScenesPage::row_cb(lv_event_t *e)
{
    auto *self = static_cast<ScenesPage *>(lv_event_get_user_data(e));
    lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(row)));
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p{};
    if (in) lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        self->hold_index_ = index;
        self->hold_t0_ = mono_us() / 1000;
        self->hold_at_ = p;
        self->hold_fired_ = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (self->hold_index_ >= 0 && (std::abs(p.x - self->hold_at_.x) > kHoldSlop || std::abs(p.y - self->hold_at_.y) > kHoldSlop))
            self->hold_index_ = -1;
    } else if (code == LV_EVENT_PRESS_LOST) {
        self->hold_index_ = -1;
    } else if (code == LV_EVENT_CLICKED) {
        const bool fired = self->hold_fired_;
        self->hold_index_ = -1;
        self->hold_fired_ = false;
        if (fired) return; /* the release after a hold opened the sheet */
        if (index == self->selected_) {
            if (self->act_.open) self->act_.open(index);
        } else {
            self->select(index);
        }
    } else if (code == LV_EVENT_RELEASED) {
        self->hold_index_ = -1;
    }
}

void ScenesPage::tick(std::int64_t now_ms)
{
    if (hold_index_ < 0 || hold_fired_) return;
    if (now_ms - hold_t0_ < kHoldMs) return;
    hold_fired_ = true;
    const int index = hold_index_;
    hold_index_ = -1;
    select(index);
    if (act_.hold) act_.hold(index);
}

void ScenesPage::cancel_hold()
{
    hold_index_ = -1;
}

} // namespace ui
} // namespace vhs
