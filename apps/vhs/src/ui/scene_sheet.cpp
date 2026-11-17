/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the options sheet (see scene_sheet.hpp).
 */
#include "scene_sheet.hpp"

#include "theme.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <cstdint>

namespace vhs {
namespace ui {
namespace {

const char *const kLabels[6] = {"Play", "Rename", "Lock", "Copy to a USB drive", "Send to Notes", "Delete"};
const char *const kKeys[6] = {"Enter", "N", "L", "U", "photos only", "Del"};
constexpr const char *kNote = "Hold a row for 1.5 s, or press Ctrl+O, to open this sheet.";
/* the rows shown, in order: Send to Notes is hidden until it is built (owner, G6: a P2 candidate) */
const SheetAction kShown[kSheetRows] = {SheetAction::Play, SheetAction::Rename, SheetAction::Lock, SheetAction::Copy,
                                        SheetAction::Delete};

int slot_of(int action)
{
    for (int s = 0; s < kSheetRows; ++s)
        if (static_cast<int>(kShown[s]) == action) return s;
    return -1;
}

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
    return l;
}

std::string upper(const std::string &s)
{
    std::string u = s;
    for (char &c : u) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    return u;
}

} // namespace

SceneSheet::SceneSheet(lv_obj_t *root, const ScenesLayout &geo, std::function<void(SheetAction)> act, std::function<void()> close)
    : geo_(geo), act_(std::move(act)), close_(std::move(close))
{
    /* the list behind is dimmed (canvas: at 35 %): the panel's colour at 65 % over it; a tap there closes */
    dim_ = lv_obj_create(root);
    plain(dim_);
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(dim_, geo_.list.x, geo_.list.y);
    lv_obj_set_size(dim_, geo_.list.w, geo_.list.h);
    lv_obj_set_style_bg_color(dim_, lv_color_hex(col::kPanel), 0);
    lv_obj_set_style_bg_opa(dim_, 166, 0);
    lv_obj_add_event_cb(dim_, dim_cb, LV_EVENT_CLICKED, this);

    sheet_ = lv_obj_create(root);
    plain(sheet_);
    lv_obj_set_pos(sheet_, geo_.sheet.x, geo_.sheet.y);
    lv_obj_set_size(sheet_, geo_.sheet.w, geo_.sheet.h);
    lv_obj_set_style_bg_color(sheet_, lv_color_hex(col::kCream), 0);
    lv_obj_set_style_bg_opa(sheet_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sheet_, geo_.sheet_radius, 0);
    lv_obj_set_style_shadow_width(sheet_, static_cast<int>(14 * geo_.u), 0);
    lv_obj_set_style_shadow_color(sheet_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(sheet_, LV_OPA_50, 0);
    const std::uint32_t key_col = geo_.square ? col::kSheetKeyDark : col::kSheetKey;
    head_ = label(sheet_, geo_.sheet_head_px, geo_.square ? col::kInk : col::kSheetKey);
    lv_obj_set_pos(head_, geo_.sheet_head.x, geo_.sheet_head.y);
    lv_obj_set_width(head_, geo_.sheet_head.w);
    lv_label_set_long_mode(head_, LV_LABEL_LONG_MODE_DOTS);
    if (!geo_.square) lv_obj_set_style_text_letter_space(head_, 1, 0);
    if (geo_.square) {
        /* the name left (ink), the detail right (darkened for contrast), on the header's one line */
        detail_ = label(sheet_, geo_.sheet_detail_px, key_col);
        lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_MODE_DOTS);
    }
    /* the rows sit in a vertical scrolling box (the LIST_ROW rule; they all fit, nothing scrolls) */
    lv_obj_t *box = lv_obj_create(sheet_);
    plain(box);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(box, 0, geo_.sheet_row_y);
    lv_obj_set_size(box, geo_.sheet.w, geo_.sheet_rows_h);
    for (int i = 0; i < 6; ++i) {
        rows_[i] = lv_obj_create(box);
        plain(rows_[i]);
        lv_obj_add_flag(rows_[i], LV_OBJ_FLAG_CLICKABLE);
        const int slot = slot_of(i);
        lv_obj_set_pos(rows_[i], 0, (slot < 0 ? 0 : slot) * geo_.sheet_row_h);
        if (slot < 0) lv_obj_add_flag(rows_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(rows_[i], geo_.sheet.w, geo_.sheet_row_h);
        lv_obj_set_style_bg_color(rows_[i], lv_color_hex(col::kSheetSel), 0);
        lv_obj_set_user_data(rows_[i], reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(rows_[i], row_cb, LV_EVENT_CLICKED, this);
        labels_[i] = label(rows_[i], geo_.sheet_label_px, col::kInk);
        lv_label_set_text(labels_[i], kLabels[i]);
        lv_obj_align(labels_[i], LV_ALIGN_LEFT_MID, geo_.sheet_label_x, 0);
        keys_[i] = label(rows_[i], geo_.sheet_key_px, key_col);
        if (geo_.square && i == static_cast<int>(SheetAction::Delete)) lv_obj_set_style_text_color(labels_[i], lv_color_hex(col::kDanger), 0);
        lv_label_set_text(keys_[i], kKeys[i]);
        lv_obj_align(keys_[i], LV_ALIGN_RIGHT_MID, -geo_.sheet_key_r, 0);
    }
    note_ = label(sheet_, geo_.sheet_note_px, key_col);
    if (geo_.sheet_note.h <= 0) lv_obj_add_flag(note_, LV_OBJ_FLAG_HIDDEN); /* the Square canvas draws none */
    lv_obj_set_pos(note_, geo_.sheet_note.x, geo_.sheet_note.y);
    lv_obj_set_width(note_, geo_.sheet_note.w);
    lv_label_set_long_mode(note_, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(note_, kNote);
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sheet_, LV_OBJ_FLAG_HIDDEN);
}

bool SceneSheet::enabled(SheetAction a) const
{
    if (slot_of(static_cast<int>(a)) < 0) return false; /* not shown */
    return a != SheetAction::Notes || photo_;         /* canvas: Send to Notes is for photos only */
}

void SceneSheet::open(const std::string &name, bool photo, bool locked, const std::string &detail)
{
    photo_ = photo;
    lv_label_set_text(head_, geo_.square ? name.c_str() : upper(name).c_str());
    if (detail_) {
        /* the detail takes what it needs (up to 55 % of the line), the name the rest */
        lv_label_set_text(detail_, detail.c_str());
        lv_point_t sz{0, 0};
        if (!detail.empty())
            lv_text_get_size(&sz, detail.c_str(), cp0_ui_font_px(geo_.sheet_detail_px), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int dw = std::min(geo_.sheet_head.w * 55 / 100, sz.x + 2);
        const int line = geo_.sheet_detail_px * 5 / 4;
        lv_obj_set_size(detail_, dw, line);
        lv_obj_set_pos(detail_, geo_.sheet_head.x + geo_.sheet_head.w - dw,
                       geo_.sheet_head.y + (geo_.sheet_head.h - line) / 2);
        const int hl = geo_.sheet_head_px * 5 / 4;
        lv_obj_set_size(head_, std::max(1, geo_.sheet_head.w - dw - geo_.sheet_detail_px), hl);
        lv_obj_set_pos(head_, geo_.sheet_head.x, geo_.sheet_head.y + (geo_.sheet_head.h - hl) / 2);
    }
    lv_label_set_text(labels_[static_cast<int>(SheetAction::Lock)], locked ? "Unlock" : "Lock");
    sel_ = 0;
    style_rows();
    lv_obj_scroll_to_y(lv_obj_get_parent(rows_[0]), 0, LV_ANIM_OFF);
    lv_obj_remove_flag(dim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sheet_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(dim_);
    lv_obj_move_foreground(sheet_);
    shown_ = true;
}

void SceneSheet::close()
{
    if (!shown_) return;
    shown_ = false;
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sheet_, LV_OBJ_FLAG_HIDDEN);
}

void SceneSheet::style_rows()
{
    for (int i = 0; i < 6; ++i) {
        lv_obj_set_style_bg_opa(rows_[i], i == sel_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(rows_[i], enabled(static_cast<SheetAction>(i)) ? LV_OPA_COVER : 115, 0); /* .45 */
    }
}

void SceneSheet::move(int delta)
{
    const int s = slot_of(sel_) < 0 ? 0 : slot_of(sel_);
    sel_ = static_cast<int>(kShown[(s + delta + kSheetRows) % kSheetRows]);
    style_rows();
    lv_obj_scroll_to_view(rows_[sel_], LV_ANIM_OFF); /* when the rows' box scrolls */
}

void SceneSheet::choose()
{
    const SheetAction a = static_cast<SheetAction>(sel_);
    if (!enabled(a)) return;
    if (act_) act_(a);
}

void SceneSheet::row_cb(lv_event_t *e)
{
    auto *self = static_cast<SceneSheet *>(lv_event_get_user_data(e));
    self->sel_ = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(e)))));
    self->style_rows();
    self->choose();
}

void SceneSheet::dim_cb(lv_event_t *e)
{
    auto *self = static_cast<SceneSheet *>(lv_event_get_user_data(e));
    if (self->close_) self->close_();
}

} // namespace ui
} // namespace vhs
