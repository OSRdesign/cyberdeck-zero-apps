/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the level meter's LED bar and the counter, each one custom-drawn object (no widget per LED or digit).
 *
 *   LedBar   15 steps (green 10, amber 3, red 2; unlit at 18 % opacity), the white peak-hold tick in the gap after the
 *            held step (3 px taller than the LEDs at both ends); with two channels the bars stack (L over R).
 *            Redrawn only when a step count changes.
 *   Counter  the digit box: one dark cell per digit, the colon between; MM:SS, or H:MM:SS from one hour in smaller
 *            cells and type that fit the same box.
 */
#pragma once

#include "fit.hpp"
#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <string>

namespace cassette {
namespace ui {

/* How far the white tick reaches over the bars at both ends, its width and the LEDs' corner radius, in board px. */
struct LedStyle {
    double over = 3, tick_w = 2, radius = 1;
};

class LedBar {
public:
    /* x0, y: the first LED's top-left in board px; led_w x led_h, gap: one step. */
    LedBar(lv_obj_t *parent, const Fit &fit, double x0, double y, double led_w, double led_h, double gap,
           const LedStyle &style = LedStyle());
    /* The whole bar's height (one row, or two rows stacked): the square layout grows it for two channels. */
    void set_height(const Fit &fit, double y, double led_h);
    void update(const MeterVm &m);

private:
    static void draw_cb(lv_event_t *e);
    lv_obj_t *obj_ = nullptr;
    int led_w_ = 9, led_h_ = 22, gap_ = 2, over_ = 3, tick_w_ = 2, radius_ = 1, w_ = 0;
    MeterVm shown_;
    bool first_ = true;
};

class Counter {
public:
    /* r: the box; cell_w x cell_h, colon_w, digit px, radius: the boards' cells (12 x 26, 7, 20 px landscape;
     * 15 x 30, 8, 24 px portrait). */
    Counter(lv_obj_t *parent, const Fit &fit, double x, double y, double w, double h, double cell_w, double cell_h,
            double colon_w, double px, double radius);
    void set_ms(std::int64_t ms);
    lv_obj_t *obj() const { return obj_; }

private:
    static void draw_cb(lv_event_t *e);
    lv_obj_t *obj_ = nullptr;
    int cell_w_, cell_h_, colon_w_, px_, radius_;
    std::string text_;
};

} // namespace ui
} // namespace cassette
