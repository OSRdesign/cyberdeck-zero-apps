/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the options sheet of a scene (canvas SceneOptions-480x320; brief 9.2): the cream sheet over the dimmed list,
 * its header (the file's name, upper case), five rows with their keys (Play Enter, Rename N, Lock L, Copy to a USB drive
 * U, Delete Del; the canvas's Send to Notes is hidden until it is built: owner, G6) and the note "Hold a row for 1.5 s,
 * or press Ctrl+O, to open this sheet.".
 * The highlighted row has the canvas's darker band; Up / Down move it, Enter or a tap acts, a tap outside or Esc closes.
 * Lock reads "Unlock" on a locked file (provisional). LVGL thread only.
 */
#pragma once

#include "core/layout/scenes_layout.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace vhs {
namespace ui {

enum class SheetAction { Play = 0, Rename, Lock, Copy, Notes, Delete };

class SceneSheet {
public:
    SceneSheet(lv_obj_t *root, const ScenesLayout &geo, std::function<void(SheetAction)> act, std::function<void()> close);
    ~SceneSheet() = default;
    SceneSheet(const SceneSheet &) = delete;
    SceneSheet &operator=(const SceneSheet &) = delete;

    void open(const std::string &name, bool photo, bool locked);
    void close();
    bool shown() const { return shown_; }
    void move(int delta);
    void choose(); /* the highlighted row */
    bool enabled(SheetAction a) const;

private:
    void style_rows();
    static void row_cb(lv_event_t *e);
    static void dim_cb(lv_event_t *e);

    ScenesLayout geo_;
    std::function<void(SheetAction)> act_;
    std::function<void()> close_;
    lv_obj_t *dim_ = nullptr, *sheet_ = nullptr, *head_ = nullptr, *note_ = nullptr;
    lv_obj_t *rows_[6] = {}, *labels_[6] = {}, *keys_[6] = {};
    int sel_ = 0;
    bool photo_ = false, shown_ = false;
};

} // namespace ui
} // namespace vhs
