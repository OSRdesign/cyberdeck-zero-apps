/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader's list: a scrollable list of rows of any height and any cells, virtual (the data of every row is kept, only
 * the rows in view plus a margin exist as widgets, in a small pool re-used while scrolling), in the launcher's
 * CompactList look (the same highlight bar, no separators) with the row kinds' markers of cp0_ui_list.h at the right
 * edge: a chevron (the row opens something), a dot (the row acts; blue: in use / running), an empty ring (the favorite
 * toggle while the feed is not pinned), and an optional unread dot at the left. It is a minimal copy of Mesh Hop's
 * RowList (mesh_app.hpp), which is tied to Mesh Hop's App: sharing one in apps/_common is a follow-up.
 *
 * Touch: a drag scrolls, a tap reports the row (the page decides: the first tap selects, a tap on the selected row acts),
 * a press reports its start / moves / end for the 3 s hold. Keys: the page moves the selection (set_selected).
 */
#pragma once

#include "cp0_ui_list.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rssui {

struct Cell {
    std::string text;
    std::uint32_t color = 0xFFFFFF;
    int x = 0, y = 0, w = 100;
    const lv_font_t *font = nullptr;
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
    int lines = 1;                   /* more than 1: wraps over that many lines (cut with dots otherwise) */
};

enum class Mark { None, Chevron, Dot, DotRunning, Ring, Filled };

struct Row {
    int h = 40;
    bool selectable = true;
    bool section = false;            /* a grey heading row: not selectable, no highlight */
    std::string id;                  /* what the row stands for: taps resolve it from here */
    std::vector<Cell> cells;
    bool lead_dot = false;           /* the unread dot at the left (the accent blue) */
    Mark trail = Mark::None;         /* the marker in the slot at the right edge */
};

class RowList {
public:
    struct Config {
        int max_cells = 4;
        int min_row_h = 40;          /* the smallest row height: sizes the pool */
        cp0_ui_list_layout_t marks{};/* the markers' sizes (cp0_ui_list_layout of this list's area, text_px = the labels') */
        int lead_x = 0;              /* the unread dot's left edge in a row */
    };

    std::function<void(int index)> on_click;
    /* what: 0 pressed, 1 moved (x, y on the screen), 2 released or lost */
    std::function<void(int what, int index, int x, int y)> on_press;

    void create(lv_obj_t *parent, int x, int y, int w, int h, const Config &cfg);
    void resize(int h);
    void set_y(int y);
    /* Replaces every row; keeps the scroll position (clamped). */
    void set(std::vector<Row> rows, int selected);
    /* Moves the selection only (cheap). scroll: bring the selected row into view. */
    void set_selected(int selected, bool scroll);
    void scroll_to_selected();
    void scroll_to_top();
    void scroll_by(int dy);
    /* A grey line shown while the list has no row (wraps over the list's width). */
    void set_empty_text(const std::string &text, const lv_font_t *font, std::uint32_t color);
    void set_hidden(bool hidden);

    lv_obj_t *obj() const { return cont_; }
    int selected() const { return selected_; }
    int count() const { return static_cast<int>(rows_.size()); }
    const std::string &id_at(int index) const;
    const Row *row(int index) const;
    int index_of(const std::string &id) const;
    int width() const { return w_; }
    int height() const { return h_; }
    int bound_rows() const;
    void update_window();            /* from the scroll event */

private:
    struct Slot {
        lv_obj_t *row = nullptr, *lead = nullptr, *chevron = nullptr, *dot = nullptr;
        std::vector<lv_obj_t *> labels;
        int index = -1;
    };
    static void row_event_cb(lv_event_t *e);
    static void scroll_event_cb(lv_event_t *e);
    void bind(Slot &s, int index);
    void hide(Slot &s);

    lv_obj_t *cont_ = nullptr, *spacer_ = nullptr, *empty_ = nullptr;
    Config cfg_;
    int w_ = 0, h_ = 0;
    std::vector<Slot> pool_;
    std::vector<Row> rows_;
    std::vector<int> off_;
    int total_ = 0;
    int selected_ = -1;
    int chev_ink_top_ = 0, chev_ink_h_ = 0, chev_line_h_ = 0;
};

/* The list scrollbar (Mesh Hop's and Net Toolbox's: the launcher list's sizes and grey, drawn whenever the content is
 * taller than its box). */
void style_list_scrollbar(lv_obj_t *obj);
/* The marker font (the launcher's Montserrat: the chevron U+203A). */
const lv_font_t *mark_font(int px);

} // namespace rssui
