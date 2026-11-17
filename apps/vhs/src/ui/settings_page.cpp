/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Settings page (see settings_page.hpp).
 */
#include "settings_page.hpp"

#include "app.hpp"
#include "theme.hpp"

#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>

namespace vhs {
namespace ui {

SettingsPage::SettingsPage(lv_obj_t *root, const CaptureLayout &geo, const Rect &toast, int toast_px,
                           std::function<void(const std::string &)> activate, std::function<void()> back)
    : activate_(std::move(activate)), back_(std::move(back)), square_(geo.square)
{
    root_ = lv_obj_create(root);
    lv_obj_remove_style_all(root_);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    panel_ = lv_obj_create(root_);
    lv_obj_remove_style_all(panel_);
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(panel_, geo.panel.x, geo.panel.y);
    lv_obj_set_size(panel_, geo.panel.w, geo.panel.h);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(col::kPanel), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel_, geo.panel_radius, 0);
    /* the toolkit's list inside the panel: LIST_ROW rows (the canvas's 34 px at 480x320; the list scrolls where they do
     * not fit); its tap rule: a tap selects a row, a tap on the selected row opens it */
    cp0_ui_list_layout_t lay;
    const int inset = geo.square ? std::max(1, static_cast<int>(geo.k * 8 + 0.5)) : static_cast<int>(4 * geo.u);
    int hint_h = 0;
    if (geo.square) hint_h = static_cast<int>(geo.k * 36 + 0.5);
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMPACT, geo.panel.x + inset, geo.panel.y + inset,
                       geo.panel.w - 2 * inset, geo.panel.h - 2 * inset - hint_h, &lay);
    if (geo.square) {
        /* the Square class (canvas Settings, 720x720 times k): rows 68 (at least LIST_ROW), labels 26, values 24, the
         * head 30 on a 52 band, the text inset 14 */
        auto S = [&geo](double v) { return std::max(1, static_cast<int>(v * geo.k + 0.5)); };
        auto F = [&geo](double v) { return std::max(12, static_cast<int>(v * geo.k + 0.5)); };
        lay.row_h = std::min(lay.h, std::max(lay.row_h, S(68)));
        lay.section_h = std::min(lay.h, S(52));
        lay.text_px = F(26);
        lay.value_px = F(24);
        lay.section_px = F(30);
        lay.inset = S(14);
    }
    cp0::ui::CompactListEvents ev;
    ev.activate = [this](int index, std::intptr_t) {
        const std::vector<SettingsItem> &items = picker_ ? choices_ : rows_;
        const int i = index - 1; /* row 0 is the head */
        if (i < 0 || i >= static_cast<int>(items.size())) return;
        if (!picker_) main_selected_ = index;
        if (activate_) activate_(items[static_cast<std::size_t>(i)].id);
    };
    ev.back = [this] {
        if (picker_) close_picker();
        else if (back_) back_();
    };
    list_ = std::make_unique<cp0::ui::CompactList>(root_, lay, ev);
    if (geo.square) {
        /* the hint line at the panel's bottom (canvas Settings, 20 px) */
        lv_obj_t *h = lv_label_create(root_);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(h, cp0_ui_font_px(std::max(12, static_cast<int>(geo.k * 20 + 0.5))), 0);
        lv_obj_set_style_text_color(h, lv_color_hex(col::kStatus), 0);
        lv_label_set_text(h, "Tap a row to select it, tap again to change it");
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(h, geo.panel.w - 2 * inset - 2 * static_cast<int>(geo.k * 6 + 0.5));
        lv_obj_set_pos(h, geo.panel.x + inset + static_cast<int>(geo.k * 6 + 0.5),
                       geo.panel.bottom() - inset - hint_h + (hint_h - static_cast<int>(geo.k * 20 + 0.5) * 5 / 4) / 2);
    }
    toast_ = std::make_unique<Toast>(root_, toast, toast_px, nullptr);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

SettingsPage::~SettingsPage() = default;

std::vector<cp0::ui::ListRow> SettingsPage::to_rows(const std::string &head, const std::vector<SettingsItem> &items, bool picker) const
{
    std::vector<cp0::ui::ListRow> out;
    out.push_back(cp0::ui::ListRow::section(square_ && !picker ? std::string("Settings") : head));
    for (const SettingsItem &it : items) {
        /* the canvas: a row's value grey, then the chevron (Edit: changed through a pick list or the text box); the
         * Playback output row without one (Info's look); a pick list's choices: Vinyl's dots (the chosen one blue) */
        cp0::ui::RowAffordance a = it.chevron ? cp0::ui::RowAffordance::Edit : cp0::ui::RowAffordance::Info;
        cp0::ui::ValueTone tone = cp0::ui::ValueTone::Muted;
        if (picker) {
            a = it.chosen ? cp0::ui::RowAffordance::ActionRunning : cp0::ui::RowAffordance::Action;
            tone = it.dimmed ? cp0::ui::ValueTone::Muted : cp0::ui::ValueTone::Normal;
        }
        out.push_back(cp0::ui::ListRow::item(it.label, it.value, tone, 0, a));
    }
    return out;
}

void SettingsPage::show(bool on)
{
    if (on) lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    list_->set_hidden(!on);
    lv_group_t *g = cdk::input_group();
    if (g) {
        if (on) {
            lv_group_add_obj(g, list_->focus_object());
            lv_group_focus_obj(list_->focus_object());
        } else {
            lv_group_remove_obj(list_->focus_object());
        }
    }
}

void SettingsPage::set_rows(const std::vector<SettingsItem> &rows)
{
    rows_ = rows;
    if (!picker_) list_->set_rows(to_rows(kSettingsHead, rows_, false));
}

void SettingsPage::open_picker(const std::string &head, const std::vector<SettingsItem> &choices)
{
    choices_ = choices;
    picker_ = true;
    int sel = 1;
    for (std::size_t i = 0; i < choices_.size(); ++i)
        if (choices_[i].chosen) sel = static_cast<int>(i) + 1;
    list_->reset_rows(to_rows(head, choices_, true), sel);
}

void SettingsPage::close_picker()
{
    if (!picker_) return;
    picker_ = false;
    list_->reset_rows(to_rows(kSettingsHead, rows_, false), main_selected_);
}

void SettingsPage::set_notice(const Notice &n)
{
    toast_->set(n);
}

lv_obj_t *SettingsPage::focus_object() const
{
    return list_->focus_object();
}

} // namespace ui
} // namespace vhs
