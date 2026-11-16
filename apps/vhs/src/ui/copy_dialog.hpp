/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: copying a scene to a USB drive (brief 9.3; canvas CopyToUsb-480x320) over the dimmed Scenes list:
 *   progress   the cream box: "Copying to SANDISK", "<name>, 1.4 GB. The file appears on the drive only when the copy
 *              is complete.", the bar, "630 MB of 1.4 GB, about 40 s left", Cancel (the primary button; at least TARGET
 *              high, the canvas's 34u is under the touch rule; Esc and Enter press it, as the toolkit's Dialog does)
 *              (the texts come from the core: app/copy_flow.hpp)
 *   drives     several drives: the same box with one row per drive (its label, its free space) under "Copy to which
 *              drive?" (provisional: brief 12.4 item 5); a tap or Enter picks, Esc closes
 * Geometry in canvas units from the Scenes page's panel. LVGL thread only.
 */
#pragma once

#include "core/layout/scenes_layout.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vhs {
namespace ui {

constexpr const char *kDrivesTitle = "Copy to which drive?"; /* provisional (brief 12.4 item 5) */

class CopyDialog {
public:
    CopyDialog(lv_obj_t *root, const ScenesLayout &geo, int target, std::function<void()> cancel,
               std::function<void(int)> pick, std::function<void()> close);
    ~CopyDialog() = default;
    CopyDialog(const CopyDialog &) = delete;
    CopyDialog &operator=(const CopyDialog &) = delete;

    void show_progress(const std::string &title, const std::string &text, double fraction, const std::string &progress);
    void show_drives(const std::vector<std::pair<std::string, std::string>> &rows);
    void hide();
    bool shown() const { return shown_; }
    bool picking() const { return picking_; }
    void move(int delta);
    void choose();

private:
    void style_rows();
    static void cb(lv_event_t *e);

    ScenesLayout geo_;
    std::function<void()> cancel_;
    std::function<void(int)> pick_;
    std::function<void()> close_;
    lv_obj_t *dim_ = nullptr, *box_ = nullptr, *title_ = nullptr, *text_ = nullptr, *track_ = nullptr, *fill_ = nullptr;
    lv_obj_t *progress_ = nullptr, *cancel_btn_ = nullptr;
    std::vector<lv_obj_t *> rows_;
    int bar_w_ = 340, sel_ = 0, n_rows_ = 0;
    int box_x_ = 0, box_y_ = 0, box_w_ = 376, box_h_ = 170, pad_ = 18, row_h_ = 42, btn_h_ = 34;
    bool shown_ = false, picking_ = false;
};

} // namespace ui
} // namespace vhs
