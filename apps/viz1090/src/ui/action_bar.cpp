/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the action bar (see action_bar.hpp). The look is Net Toolbox's: a dark cell with a grey border, the
 * list's highlight colour while it has the focus, lighter while pressed.
 */
#include "action_bar.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdint>

namespace viz {
namespace ui {

namespace {

constexpr std::uint32_t kCellRgb = 0x161616;
constexpr std::uint32_t kBorderRgb = 0x3A3A3A;
constexpr std::uint32_t kFocusBorderRgb = 0x9A9A9A;
constexpr std::uint32_t kPressedRgb = 0x3A3A3A;
constexpr std::uint32_t kKeyTab = 15; /* the keyboard bridge hands Tab over as its evdev code (KEY_TAB) */

} // namespace

ActionBar::ActionBar(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font, std::vector<ActionCell> cells,
                     ActionBarEvents events)
    : events_(std::move(events))
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root_, x, y);
    lv_obj_set_size(root_, w, h);
    lv_obj_add_event_cb(root_, root_delete_cb, LV_EVENT_DELETE, this);
    const int gap = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int pad = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int n = static_cast<int>(cells.size());
    const int cell_w = n > 0 ? std::max(1, (w - (n - 1) * gap) / n) : w;
    const int line_h = font ? static_cast<int>(lv_font_get_line_height(font)) : 0;
    cells_.reserve(cells.size());
    for (int i = 0; i < n; ++i) {
        Cell c;
        c.spec = cells[static_cast<std::size_t>(i)];
        c.obj = lv_obj_create(root_);
        lv_obj_remove_style_all(c.obj);
        if (lv_obj_get_group(c.obj)) lv_group_remove_obj(c.obj); /* the page adds it to its own group */
        lv_obj_remove_flag(c.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(c.obj, i * (cell_w + gap), 0);
        lv_obj_set_size(c.obj, cell_w, h);
        lv_obj_set_style_radius(c.obj, cp0_ui_tok(CP0_TOK_RADIUS_S), 0);
        lv_obj_set_style_bg_opa(c.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(kCellRgb), 0);
        lv_obj_set_style_border_width(c.obj, 1, 0);
        lv_obj_set_style_border_color(c.obj, lv_color_hex(kBorderRgb), 0);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(CP0_UI_LIST_BAR_RGB), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(c.obj, lv_color_hex(kFocusBorderRgb), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(c.obj, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(kPressedRgb), LV_STATE_PRESSED);
        lv_obj_set_user_data(c.obj, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(c.obj, cell_event_cb, LV_EVENT_ALL, this);
        c.label = lv_label_create(c.obj);
        lv_obj_remove_flag(c.label, LV_OBJ_FLAG_CLICKABLE);
        if (font) lv_obj_set_style_text_font(c.label, font, 0);
        lv_obj_set_style_text_color(c.label, lv_color_hex(CP0_UI_LIST_TEXT_RGB), 0);
        lv_obj_set_style_text_align(c.label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(c.label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(c.label, c.spec.label.c_str());
        lv_obj_set_size(c.label, std::max(1, cell_w - 2 * pad), line_h);
        lv_obj_set_pos(c.label, pad, std::max(0, (h - line_h) / 2));
        cells_.push_back(c);
    }
}

ActionBar::~ActionBar()
{
    if (!root_) return;
    lv_obj_remove_event_cb_with_user_data(root_, root_delete_cb, this);
    lv_obj_delete(root_);
    root_ = nullptr;
}

void ActionBar::root_delete_cb(lv_event_t *event)
{
    auto *self = static_cast<ActionBar *>(lv_event_get_user_data(event));
    if (!self) return;
    self->root_ = nullptr;
    self->cells_.clear();
}

lv_obj_t *ActionBar::cell_object(int index) const
{
    return index >= 0 && index < count() ? cells_[static_cast<std::size_t>(index)].obj : nullptr;
}

std::vector<lv_obj_t *> ActionBar::focus_objects() const
{
    std::vector<lv_obj_t *> out;
    for (const Cell &c : cells_) out.push_back(c.obj);
    return out;
}

void ActionBar::on_key(int index, std::uint32_t key)
{
    const int n = count();
    switch (key) {
    case LV_KEY_RIGHT:
        if (index + 1 < n) lv_group_focus_obj(cells_[static_cast<std::size_t>(index) + 1].obj);
        break;
    case LV_KEY_LEFT:
        if (index > 0) lv_group_focus_obj(cells_[static_cast<std::size_t>(index) - 1].obj);
        else if (events_.back) events_.back();
        break;
    case LV_KEY_DOWN:
        if (events_.down) events_.down();
        break;
    case LV_KEY_ESC:
        if (events_.back) events_.back();
        break;
    case kKeyTab:
        if (index + 1 < n) lv_group_focus_obj(cells_[static_cast<std::size_t>(index) + 1].obj);
        else if (events_.down) events_.down();
        break;
    case LV_KEY_ENTER:
        if (events_.activate) events_.activate(cells_[static_cast<std::size_t>(index)].spec.id);
        break;
    default: break;
    }
}

void ActionBar::cell_event_cb(lv_event_t *event)
{
    auto *self = static_cast<ActionBar *>(lv_event_get_user_data(event));
    if (!self || !self->root_) return;
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj)));
    if (index < 0 || index >= self->count()) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_KEY: self->on_key(index, lv_event_get_key(event)); break;
    case LV_EVENT_CLICKED: { /* a tap; the click a key makes belongs to the key (and may land on the next page's cell) */
        lv_indev_t *indev = lv_indev_active();
        if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) break;
        if (self->events_.activate) self->events_.activate(self->cells_[static_cast<std::size_t>(index)].spec.id);
        break;
    }
    default: break;
    }
}

} // namespace ui
} // namespace viz
