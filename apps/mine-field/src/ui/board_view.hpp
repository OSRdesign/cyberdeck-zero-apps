/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field: the board widget. ONE LVGL object draws every cell in its DRAW_MAIN event, only the cells that intersect
 * the area LVGL asks to redraw; no object exists per cell (a 30 x 30 board would be 900 of them, each with a style, a
 * layout pass and a draw task, on a Pi Zero 2 W). A change invalidates only the rectangles of the cells that changed
 * (sync() compares a one-byte code per cell with the last drawn one); a win sweep redraws the board once per
 * step. Cells are drawn with LVGL's rectangle / line / triangle / label primitives (no images, no per-cell buffers).
 *
 * The same object reads the pointer: a tap on a cell, a long press (0.4 s, held still) on a cell; a touch that moves 10 px is
 * neither (nothing is panned: the board always fits the screen, core/view_layout.hpp). LVGL thread only.
 */
#pragma once

#include "game.hpp"
#include "view_layout.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <vector>

namespace mfui {

constexpr int kLongPressMs = 400;
constexpr int kMoveThresholdPx = 10;

struct BoardEvents {
    std::function<void(int cell)> tap;         /* a short touch on a cell (index) */
    std::function<void(int cell)> long_press;  /* held 0.4 s on a cell */
    std::function<void()> touched;             /* a finger came down (the keyboard cursor goes away) */
};

class BoardView {
public:
    BoardView(lv_obj_t *parent, BoardEvents events);
    ~BoardView();
    BoardView(const BoardView &) = delete;
    BoardView &operator=(const BoardView &) = delete;

    lv_obj_t *surface() const { return obj_; }
    void set_hidden(bool hidden);
    void set_input_enabled(bool on) { input_ = on; }
    void bind(const minefield::Game *game);

    /* geometry: the viewport (canvas coordinates) and the grid that fits it */
    void configure(const lv_area_t &viewport, const minefield::BoardPlan &plan);
    const minefield::BoardPlan &plan() const { return plan_; }
    lv_area_t cell_area(int cell) const;
    int center_cell() const;                                       /* the cell at the middle of the grid */
    lv_area_t viewport_area() const { return viewport_; }

    /* drawing */
    void sync();                         /* invalidate the cells whose picture changed */
    void invalidate_all();
    void set_cursor(int cell);           /* -1: none */
    void set_hint(int cell, bool animate);   /* -1: none */
    void flash(const std::vector<int> &cells, int ms);
    void pulse(int cell, int ms);
    void set_sweep(double position);     /* < 0: off, else 0..1 across the board */
    void set_mine_reveal(const std::vector<int> &order, int shown);  /* the loss: the first `shown` of `order` show; shown < 0 = none managed */
    void reveal_all_mines(bool on);
    void clear_loss();

private:
    static void event_cb(lv_event_t *e);
    static void anim_cb(lv_timer_t *t);
    void draw(lv_event_t *e);
    void draw_cell(lv_layer_t *layer, int cell, const lv_area_t &ca);
    void draw_flag(lv_layer_t *layer, const lv_area_t &t, bool cross);
    void draw_mine(lv_layer_t *layer, const lv_area_t &t);
    void on_pointer(lv_event_code_t code);
    void invalidate_cell(int cell);
    std::uint8_t code_of(int cell) const;
    void arm_anim();
    void tick();
    int cell_at(int sx, int sy) const;     /* screen point -> cell or -1 */

    lv_obj_t *obj_ = nullptr;
    lv_timer_t *anim_ = nullptr;
    BoardEvents ev_;
    const minefield::Game *game_ = nullptr;
    lv_area_t viewport_{0, 0, 0, 0};
    minefield::BoardPlan plan_;
    int cols_ = 0, rows_ = 0;
    std::vector<std::uint8_t> drawn_;

    int cursor_ = -1, hint_ = -1, pulse_cell_ = -1;
    bool hint_animate_ = false, hint_big_ = false;
    std::vector<int> flash_cells_;
    std::uint32_t flash_until_ = 0, pulse_until_ = 0, last_hint_toggle_ = 0;
    double sweep_ = -1;
    std::vector<char> mine_shown_;
    bool mines_all_ = false;

    bool input_ = true;
    bool pressed_ = false, moved_ = false, fired_ = false;
    int press_x_ = 0, press_y_ = 0, press_cell_ = -1;
    std::uint32_t press_ms_ = 0;
};

} // namespace mfui
