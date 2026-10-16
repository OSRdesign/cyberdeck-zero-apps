/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: a row of large action cells above a list (Net Toolbox's ActionBar, apps/net-toolbox/src/ui/rich_list.*,
 * trimmed). Each cell is an action: a tap acts at once, Enter acts, Left / Right move between the cells (Left on the first
 * one goes back), Down and Tab go on to the list under the bar, Esc goes back. The page gives the area (the TARGET token
 * high) and the font from the layout service: nothing here is a pixel size of a screen. LVGL thread only.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace viz {
namespace ui {

struct ActionCell {
    int id = 0;
    std::string label;
};

struct ActionBarEvents {
    std::function<void(int id)> activate;
    std::function<void()> back;
    std::function<void()> down; /* Down / Tab: on to the list under the bar */
};

class ActionBar {
public:
    ActionBar(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font, std::vector<ActionCell> cells,
              ActionBarEvents events);
    ~ActionBar();
    ActionBar(const ActionBar &) = delete;
    ActionBar &operator=(const ActionBar &) = delete;

    int count() const { return static_cast<int>(cells_.size()); }
    lv_obj_t *cell_object(int index) const;
    std::vector<lv_obj_t *> focus_objects() const;

private:
    struct Cell {
        ActionCell spec;
        lv_obj_t *obj = nullptr;
        lv_obj_t *label = nullptr;
    };
    static void cell_event_cb(lv_event_t *event);
    static void root_delete_cb(lv_event_t *event);
    void on_key(int index, std::uint32_t key);

    lv_obj_t *root_ = nullptr;
    std::vector<Cell> cells_;
    ActionBarEvents events_;
};

} // namespace ui
} // namespace viz
