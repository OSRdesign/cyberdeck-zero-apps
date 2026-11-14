/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the list of fixed-height rows the Browser, the Queue, the Output page and (P2b) the Library pages share. It
 * only builds the rows in view (a folder or a queue can hold thousands). A row is a text (bold) with a value at its right,
 * and a mark: a chevron opens, a dot acts (blue: the playing track, the chosen output), none informs (design QA D-45). A
 * heading row (P2b: "DISC 2", "SONGS" in the search results) is grey capitals, never tapped. A press held for 1.5 s on a
 * row calls on_hold (the Track, Album and Artist actions; owner ruling 2026-11-14, B44, down from the checklist's 3 s; the tap
 * that follows the release is dropped); from 0.5 s on a cue shows over the bottom of the list, as Markdown Notes' does: a bar
 * that fills and "Keep holding for options" (enable_hold_cue). A drag scrolls and gives the hold up, as a release does. LVGL
 * thread only.
 */
#pragma once

#include "gfx.hpp"
#include "theme.hpp"
#include "view_model.hpp"

#include "nav/hold_tracker.hpp"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace vinyl {
namespace ui {

struct RowW {
    lv_obj_t *row = nullptr, *dot = nullptr, *chev = nullptr, *text = nullptr, *value = nullptr;
    int index = -1;
};

/* A list of fixed-height rows that only builds the rows in view (a folder or a queue can hold thousands). */
class RowList {
public:
    RowList(lv_obj_t *parent, int x, int y, int w, int h, int row_h, double s, int text_px, int value_px, double dot_x,
            double text_x, double value_r, double chev_r, std::function<void(int)> on_tap)
        : row_h_(row_h), s_(s), w_(w), text_px_(text_px), value_px_(value_px), dot_x_(dot_x), text_x_(text_x),
          value_r_(value_r), chev_r_(chev_r), on_tap_(std::move(on_tap)) {
        cont_ = lv_obj_create(parent);
        plain(cont_);
        lv_obj_set_pos(cont_, x, y);
        lv_obj_set_size(cont_, w, h);
        lv_obj_add_flag(cont_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(cont_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(cont_, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_width(cont_, std::max(2, ri(3 * s)), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(cont_, lv_color_hex(col::kRowValue), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(cont_, LV_OPA_50, LV_PART_SCROLLBAR);
        lv_obj_add_event_cb(cont_, scroll_cb, LV_EVENT_SCROLL, this);
        spacer_ = lv_obj_create(cont_);
        plain(spacer_);
        lv_obj_set_size(spacer_, 1, 1);
        visible_ = std::max(1, h / std::max(1, row_h)) + 2;
        head_px_ = std::max(1, value_px - 1);
        for (int i = 0; i < visible_; ++i) pool_.push_back(make_row());
    }

    /* a press held HoldTracker::kFireMs on a row (its index) */
    std::function<void(int)> on_hold;
    /* P2c: which rows have something to open (null: every row but a heading); a row without one starts no hold and shows no cue */
    std::function<bool(int)> hold_ok;

    /* the hold's cue (Markdown Notes' look: a box with an accent border, the message, a bar that fills): over the bottom of
     * the area x .. x + w, `bottom` being its lowest edge, in `parent`'s coordinates */
    void enable_hold_cue(lv_obj_t *parent, int x, int bottom, int w) {
        cue_x_ = x;
        cue_bottom_ = bottom;
        const int pad = std::max(4, ri(8 * s_));
        const int px = std::max(1, ri(12 * s_));
        const lv_font_t *f = font(Face::Bold, px);
        const int lh = lv_font_get_line_height(f);
        const int cue_h = pad + lh + pad / 2 + pad + pad; /* (a hidden object's height is not laid out yet) */
        cue_box_ = ui::box(parent, x, 0, w, cue_h, col::kRowSel, std::max(4, ri(8 * s_)));
        lv_obj_set_style_border_width(cue_box_, 2, 0);
        lv_obj_set_style_border_color(cue_box_, lv_color_hex(col::kTeal), 0);
        lv_obj_set_style_border_opa(cue_box_, LV_OPA_COVER, 0);
        cue_label_ = label(cue_box_, Face::Bold, px, col::kRowText, pad, pad - 2, w - 2 * pad - 4);
        set_text(cue_label_, "Keep holding for options");
        cue_track_w_ = std::max(1, w - 2 * pad - 4);
        lv_obj_t *track = ui::box(cue_box_, pad, pad + lh + pad / 2 - 2, cue_track_w_, pad, col::kList, LV_RADIUS_CIRCLE);
        cue_bar_ = ui::box(track, 0, 0, 1, pad, col::kTeal, LV_RADIUS_CIRCLE);
        lv_obj_set_y(cue_box_, bottom - cue_h);
        lv_obj_add_flag(cue_box_, LV_OBJ_FLAG_HIDDEN);
    }
    bool cue_shown() const { return cue_box_ && !lv_obj_has_flag(cue_box_, LV_OBJ_FLAG_HIDDEN); }

    /* where the list is scrolled to, in pixels (a page's place, kept when it is left and given back by scroll_y_to) */
    int scroll_y() const { return lv_obj_get_scroll_y(cont_); }
    void scroll_y_to(int px) {
        lv_obj_update_layout(cont_);
        const int max_y = std::max(0, static_cast<int>(rows_.size()) * row_h_ - lv_obj_get_height(cont_));
        lv_obj_scroll_to_y(cont_, std::clamp(px, 0, max_y), LV_ANIM_OFF);
        rebind(true);
    }

    void set_rows(const std::vector<RowView> &rows, int selected, bool reset_scroll, int scroll_to) {
        rows_ = rows;
        selected_ = selected;
        lv_obj_set_pos(spacer_, 0, std::max(0, static_cast<int>(rows_.size()) * row_h_ - 1));
        if (reset_scroll) lv_obj_scroll_to_y(cont_, 0, LV_ANIM_OFF);
        if (scroll_to >= 0 && scroll_to < static_cast<int>(rows_.size())) {
            lv_obj_update_layout(cont_);
            const int max_y = std::max(0, static_cast<int>(rows_.size()) * row_h_ - lv_obj_get_height(cont_));
            lv_obj_scroll_to_y(cont_, std::min(max_y, scroll_to * row_h_), LV_ANIM_OFF);
        }
        if (!reset_scroll && scroll_to < 0 && selected >= 0) { /* keep the highlight in view (a row moved) */
            const int top = lv_obj_get_scroll_y(cont_), h = lv_obj_get_height(cont_);
            if (selected * row_h_ < top) lv_obj_scroll_to_y(cont_, selected * row_h_, LV_ANIM_OFF);
            else if ((selected + 1) * row_h_ > top + h) lv_obj_scroll_to_y(cont_, (selected + 1) * row_h_ - h, LV_ANIM_OFF);
        }
        rebind(true);
    }

    void set_selected(int i, bool ensure_visible) {
        selected_ = i;
        if (ensure_visible && i >= 0) {
            const int top = lv_obj_get_scroll_y(cont_), h = lv_obj_get_height(cont_);
            if (i * row_h_ < top) lv_obj_scroll_to_y(cont_, i * row_h_, LV_ANIM_OFF);
            else if ((i + 1) * row_h_ > top + h) lv_obj_scroll_to_y(cont_, (i + 1) * row_h_ - h, LV_ANIM_OFF);
        }
        rebind(true);
    }

    /* the list's area changed (a strip came or went): the rows in view are built again */
    void resize(int h) {
        lv_obj_set_height(cont_, std::max(1, h));
        const int want = std::max(1, h / std::max(1, row_h_)) + 2;
        while (static_cast<int>(pool_.size()) < want) pool_.push_back(make_row());
        visible_ = static_cast<int>(pool_.size());
        rebind(true);
    }

    int size() const { return static_cast<int>(rows_.size()); }
    lv_obj_t *obj() const { return cont_; }

private:
    static int ri(double v) { return static_cast<int>(std::lround(v)); }

    RowW make_row() {
        RowW r;
        r.row = lv_obj_create(cont_);
        plain(r.row);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_EVENT_BUBBLE); /* the drag scrolls the list */
        lv_obj_set_size(r.row, w_, row_h_);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(col::kRowSel), 0);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(r.row, row_click_cb, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(r.row, row_press_cb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(r.row, row_press_cb, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(r.row, row_press_cb, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(r.row, row_press_cb, LV_EVENT_PRESS_LOST, this);
        const int dot = std::max(4, ri(10 * s_));
        r.dot = box(r.row, ri(dot_x_), (row_h_ - dot) / 2, dot, dot, col::kWhite, LV_RADIUS_CIRCLE);
        const int cw = std::max(8, ri(16 * s_));
        r.chev = icon_obj(r.row, Icon::Chevron, w_ - ri(chev_r_) - cw, (row_h_ - cw) / 2, cw, cw, cw, col::kRowValue);
        r.text = label(r.row, Face::Bold, text_px_, col::kRowText, ri(text_x_), 0, 10);
        lv_obj_set_style_text_line_space(r.text, 0, 0);
        r.value = label(r.row, Face::Mono, value_px_, col::kRowValue, 0, 0, 10, LV_TEXT_ALIGN_RIGHT); /* one line, dots */
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
        return r;
    }

    void bind(RowW &w, int i) {
        const RowView &v = rows_[static_cast<size_t>(i)];
        w.index = i;
        lv_obj_set_y(w.row, i * row_h_);
        show(w.row, true);
        if (v.heading) { /* grey capitals, no mark, no tap */
            lv_obj_remove_flag(w.row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(w.row, LV_OPA_TRANSP, 0);
            show(w.dot, false);
            show(w.chev, false);
            show(w.value, false);
            const lv_font_t *hf = font(Face::Bold, head_px_);
            lv_obj_set_style_text_font(w.text, hf, 0);
            lv_obj_set_style_text_letter_space(w.text, std::max(0, ri(0.06 * head_px_)), 0);
            lv_obj_set_style_text_color(w.text, lv_color_hex(col::kRowValue), 0);
            set_text(w.text, v.text);
            lv_obj_set_width(w.text, std::max(10, w_ - ri(text_x_) - ri(value_r_)));
            lv_obj_set_pos(w.text, ri(dot_x_), (row_h_ - lv_font_get_line_height(hf)) / 2);
            return;
        }
        lv_obj_add_flag(w.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(w.text, font(Face::Bold, text_px_), 0);
        lv_obj_set_style_text_letter_space(w.text, 0, 0);
        lv_obj_set_style_bg_opa(w.row, i == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        /* the row's kind (design QA D-45): a chevron opens (folders, playlists), a dot acts (tracks, queue rows, outputs;
         * blue for the playing track and the chosen output), no mark informs */
        const bool dot = v.mark == RowView::Mark::Dot;
        const bool chev = v.mark == RowView::Mark::Chevron;
        show(w.dot, dot);
        if (dot) set_bg(w.dot, v.dimmed ? col::kDotDim : v.playing ? col::kPlaying : v.amber ? col::kAmber : col::kWhite);
        show(w.chev, chev);
        if (chev) icon_set(w.chev, Icon::Chevron, v.dimmed ? col::kDotDim : col::kRowValue);
        const lv_font_t *vf = font(v.mono_value ? Face::Mono : Face::Bold, value_px_);
        if (lv_obj_get_style_text_font(w.value, LV_PART_MAIN) != vf) lv_obj_set_style_text_font(w.value, vf, 0);
        set_text(w.value, v.value);
        const lv_font_t *tf = font(Face::Bold, text_px_);
        const int right = chev ? ri(chev_r_) + ri(16 * s_) + ri(4 * s_) : ri(value_r_);
        /* a row without a mark keeps its text where a marked row has it, unless it says so (flush: the Music folders) */
        const int tx = ri(row_title_x(v, dot_x_, text_x_)); /* after the dot when there is one */
        /* the name and the value share the row; when both do not fit, the name keeps its width up to 60 % of the room
         * and the value ends with dots (a narrow screen: "Auto" before "follows system: Evolve 65") */
        const int room = std::max(20, w_ - tx - right - ri(8 * s_));
        const int vnat = v.value.empty() ? 0 : text_width(vf, v.value) + 2;
        const int tnat = text_width(tf, v.text) + 2;
        int vw = vnat, tw = room - vnat;
        if (vnat > 0 && tnat + vnat > room) {
            tw = std::min(tnat, std::max(room - vnat, room * 3 / 5));
            vw = std::max(0, room - tw);
        }
        tw = std::max(10, tw);
        show(w.value, vw > 0);
        lv_obj_set_size(w.value, std::max(1, vw), lv_font_get_line_height(vf));
        lv_obj_set_pos(w.value, w_ - right - vw, (row_h_ - lv_font_get_line_height(vf)) / 2);
        const uint32_t tc = v.dimmed ? col::kRowDim : col::kRowText;
        lv_obj_set_style_text_color(w.text, lv_color_hex(tc), 0);
        lv_obj_set_style_text_color(w.value, lv_color_hex(v.dimmed ? col::kDotDim : col::kRowValue), 0);
        set_text(w.text, v.text);
        lv_obj_set_width(w.text, tw);
        lv_obj_set_pos(w.text, tx, (row_h_ - lv_font_get_line_height(tf)) / 2);
    }

    void rebind(bool force) {
        const int top = lv_obj_get_scroll_y(cont_);
        const int first = std::max(0, top / std::max(1, row_h_));
        if (!force && first == first_) return;
        first_ = first;
        for (int k = 0; k < visible_; ++k) {
            const int i = first + k;
            if (i < static_cast<int>(rows_.size())) bind(pool_[static_cast<size_t>(k)], i);
            else {
                pool_[static_cast<size_t>(k)].index = -1;
                show(pool_[static_cast<size_t>(k)].row, false);
            }
        }
    }

    int index_of(lv_obj_t *row) const {
        for (const RowW &w : pool_)
            if (w.row == row) return w.index;
        return -1;
    }

    static void scroll_cb(lv_event_t *e) { static_cast<RowList *>(lv_event_get_user_data(e))->rebind(false); }
    static void row_click_cb(lv_event_t *e) {
        auto *self = static_cast<RowList *>(lv_event_get_user_data(e));
        if (self->hold_fired_) { /* the release of a 3 s hold: the sheet is open, no tap */
            self->hold_fired_ = false;
            return;
        }
        if (self->swallow_tap_) { /* a hold given up after its cue showed: not a tap */
            self->swallow_tap_ = false;
            return;
        }
        const int i = self->index_of(lv_event_get_target_obj(e));
        if (i >= 0 && !self->rows_[static_cast<size_t>(i)].heading && self->on_tap_) self->on_tap_(i);
    }
    void hide_cue() {
        if (cue_box_) lv_obj_add_flag(cue_box_, LV_OBJ_FLAG_HIDDEN);
    }
    static lv_point_t pointer() {
        lv_point_t p{0, 0};
        if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
        return p;
    }
    static void row_press_cb(lv_event_t *e) {
        auto *self = static_cast<RowList *>(lv_event_get_user_data(e));
        const uint64_t now = lv_tick_get();
        switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED: {
            const int idx = self->index_of(lv_event_get_target_obj(e));
            self->hold_fired_ = false;
            self->swallow_tap_ = false;
            self->hold_.cancel();
            self->hide_cue();
            if (idx >= 0 && self->on_hold && !self->rows_[static_cast<size_t>(idx)].heading && (!self->hold_ok || self->hold_ok(idx))) {
                const lv_point_t p = pointer();
                self->hold_.begin(idx, p.x, p.y, now);
            }
            break;
        }
        case LV_EVENT_PRESSING: {
            if (!self->hold_.active() || self->hold_fired_) break;
            const lv_point_t p = pointer();
            self->hold_.move(p.x, p.y);
            if (lv_indev_get_scroll_obj(lv_indev_active())) self->hold_.cancel(); /* a drag: the list scrolls, no hold */
            if (!self->hold_.active()) {
                self->hide_cue();
                break;
            }
            const int idx = self->hold_.target();
            if (self->hold_.poll(now)) {
                self->hold_fired_ = true;
                self->hide_cue();
                self->on_hold(idx);
            } else if (self->cue_box_ && self->hold_.visible(now)) {
                lv_obj_set_width(lv_obj_get_child(lv_obj_get_child(self->cue_box_, 1), 0),
                                 std::max(1, static_cast<int>(self->hold_.progress(now) * self->cue_track_w_)));
                lv_obj_remove_flag(self->cue_box_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(self->cue_box_);
            }
            break;
        }
        default: /* released or lost: a hold given up after its cue showed is not a tap either */
            if (lv_event_get_code(e) == LV_EVENT_RELEASED && self->hold_.visible(now)) self->swallow_tap_ = true;
            self->hold_.cancel();
            self->hide_cue();
            break;
        }
    }

    lv_obj_t *cont_ = nullptr, *spacer_ = nullptr;
    int row_h_;
    double s_;
    int w_, text_px_, value_px_;
    double dot_x_, text_x_, value_r_, chev_r_;
    std::function<void(int)> on_tap_;
    std::vector<RowW> pool_;
    std::vector<RowView> rows_;
    int visible_ = 1, first_ = -1, selected_ = -1, head_px_ = 12;
    HoldTracker hold_;
    bool hold_fired_ = false, swallow_tap_ = false;
    lv_obj_t *cue_box_ = nullptr, *cue_label_ = nullptr, *cue_bar_ = nullptr;
    int cue_x_ = 0, cue_bottom_ = 0, cue_track_w_ = 1;
};

} // namespace ui
} // namespace vinyl
