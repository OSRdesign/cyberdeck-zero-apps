/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the overview's tile grid (LVGL thread only). The area under the page header, all from the layout
 * service: margins SPACE_M, gaps SPACE_S, a footer row ROW high, tiles placed by core/grid.hpp (2 x 3 at 640x480,
 * 3 x 2 on wide screens, Power and Network across the width on a square one; never scrolling).
 *
 *   tile     the launcher home grid's tile look (ground #1E1E1E, frame #3A3A3A, radius RADIUS_M): a title and a dim
 *            hint at the top, one big value (the largest text size its middle has room for, TITLE size at least
 *            when possible, 48 px at most), a small line at the bottom, and beside the value a history line (lv_chart,
 *            the last 120 samples) or a fill bar when the tile has room. The Power tile's frame and ground take its
 *            state colour (green / amber / red).
 *   footer   Processes, System, Sampling: ROW high, same look.
 *   focus    a blue ring (the launcher's selection colour) around one tile or footer item; arrows move it by position
 *            (core/grid.hpp next_focus), Enter opens, Esc goes back (at the overview: the app ends). A tap opens a
 *            tile or a footer item at once (each is a large target) and moves the ring there.
 * Labels, bars and charts are only touched when what they show changed, so an idle second redraws little.
 */
#pragma once

#include "grid.hpp"
#include "monitor_core.hpp"
#include "sampler.hpp"

#include "cp0_ui_roller.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sm {

class OverviewGrid {
public:
    struct Events {
        std::function<void(int index)> open; /* the item: tiles 0..kTileCount-1, then the footer items */
        std::function<void()> back;
    };

    /* focus: the item that has the ring at first (the one the user came back from) */
    OverviewGrid(lv_obj_t *parent, const cp0_ui_roller_page_t &layout, Events events, int focus);
    ~OverviewGrid();
    OverviewGrid(const OverviewGrid &) = delete;
    OverviewGrid &operator=(const OverviewGrid &) = delete;

    void update(const Snapshot *s, int period_s);
    lv_obj_t *focus_object() const { return root_; }
    int focused() const { return focus_; }
    const GridLayout &grid() const { return grid_; }

private:
    struct Item {
        lv_obj_t *box = nullptr;
        lv_obj_t *title = nullptr, *hint = nullptr, *value = nullptr, *detail = nullptr;
        lv_obj_t *chart = nullptr, *bar = nullptr;
        lv_chart_series_t *series = nullptr;
        std::vector<std::int32_t> data;
        std::vector<float> bins;
        Rect rect;
        int value_px = 0;
        const lv_font_t *value_font = nullptr;
        int spark_x = -1;           /* where the history line / bar starts (-1: none) */
        Spark spark = Spark::None;
        std::size_t shown_points = 0;
        float shown_last = -1e30f;
        std::int32_t shown_lo = 0, shown_hi = 0;
        int shown_bar = -1;
        int shown_value_rgb = -1, shown_detail_rgb = -1, shown_frame = -1, shown_spark_rgb = -1;
        std::vector<std::string> shown_value, shown_detail; /* the variants last fitted */
        std::string shown_hint;
    };

    static void key_cb(lv_event_t *event);
    static void click_cb(lv_event_t *event);
    void on_key(std::uint32_t key);
    void set_focus(int index);
    Item make_box(const Rect &r, bool footer);
    void build_tile(Item &item, int id);
    void place_spark(Item &item);
    void update_tile(Item &item, const TileText &t, const Snapshot *s);
    void update_chart(Item &item, const Ring &ring, std::int32_t lo, std::int32_t hi, double scale);
    std::vector<Rect> rects() const;

    cp0_ui_roller_page_t layout_;
    Events events_;
    GridLayout grid_;
    int x0_ = 0, y0_ = 0;
    int pad_ = 0, gap_ = 0;
    const lv_font_t *small_ = nullptr;   /* CAPTION: titles, hints, small lines */
    const lv_font_t *label_ = nullptr;   /* BODY: footer items */
    int small_h_ = 0;
    lv_obj_t *root_ = nullptr;
    std::vector<Item> tiles_, footer_;
    int focus_ = 0;
    std::vector<std::vector<std::string>> shown_footer_;
};

} // namespace sm
