/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Settings page (canvas Settings-480x320; brief 11) in the Capture page's dark panel: the toolkit's list
 * (cp0::ui::CompactList, LIST_ROW rows: the label at the left, the value grey at the right, then the chevron) under the head line "VHS SETTINGS" (0.1.2: the canvas's sound sentence is gone, the input is a row now). A row opens
 * either a pick list (Vinyl's style: the choices replace the rows, the current one marked with the toolkit's chosen
 * dot, a choice applies and closes the list, Esc closes it unchanged) or the app's text box (Name prefix). The rows and
 * the choices come from the app (vhs_app.cpp). LVGL thread only.
 */
#pragma once

#include "core/layout/capture_layout.hpp"
#include "notices.hpp"

#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vhs {
namespace ui {

constexpr const char *kSettingsHead = "VHS SETTINGS";

struct SettingsItem {
    std::string id;     /* "folder", "prefix" ... (pick lists: the choice's own key) */
    std::string label;
    std::string value;
    bool chosen = false; /* pick lists: the current choice */
    bool dimmed = false;
    bool chevron = true; /* main rows: false draws the canvas's Playback output row (no chevron) */
};

class SettingsPage {
public:
    /* activate(id): a row (main list) or a choice (pick list) taken; back(): Esc / Left at the main list. */
    /* toast / toast_px: the toast line (the Scenes page's place: provisional, the canvas draws none here) */
    SettingsPage(lv_obj_t *root, const CaptureLayout &geo, const Rect &toast, int toast_px,
                 std::function<void(const std::string &id)> activate, std::function<void()> back);
    ~SettingsPage();
    SettingsPage(const SettingsPage &) = delete;
    SettingsPage &operator=(const SettingsPage &) = delete;

    void show(bool on);
    /* The main rows (in place: the selection stays). */
    void set_rows(const std::vector<SettingsItem> &rows);
    /* A pick list: its choices replace the rows (the chosen one selected); close_picker() brings the rows back. */
    void open_picker(const std::string &head, const std::vector<SettingsItem> &choices);
    void close_picker();
    bool in_picker() const { return picker_; }
    void set_notice(const Notice &n);
    lv_obj_t *focus_object() const;

private:
    std::vector<cp0::ui::ListRow> to_rows(const std::string &head, const std::vector<SettingsItem> &items, bool picker) const;

    lv_obj_t *root_ = nullptr, *panel_ = nullptr;
    std::unique_ptr<cp0::ui::CompactList> list_;
    std::unique_ptr<Toast> toast_;
    std::function<void(const std::string &)> activate_;
    std::function<void()> back_;
    std::vector<SettingsItem> rows_, choices_;
    int main_selected_ = 1;
    bool picker_ = false;
    bool square_ = false;
};

} // namespace ui
} // namespace vhs
