/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the list of the pages that mix rows with blocks (LVGL thread only).
 *
 * CompactList (cp0_ui_widgets.hpp) has one row height and one line of text per row; the 0.2 pages need rows of other
 * heights: a value that wraps instead of being cut (a whois registrar, a URL), a paragraph, a hop of the traceroute
 * chain, the HTTP check's badge and waterfall. RichList is the same list (the same maths: cp0_ui_list.h, the same keys,
 * the same tap rule: a tap selects, a tap on the selected row acts, the same look: the highlight bar, the value tones,
 * the section headers) over rows of any height, and it holds the whole list in ONE clickable scrolling object: a tap is
 * resolved by its position, so a row can be as low as its content needs.
 *
 *   Item     a label at the left and a value at the right (the value is never cut). When both do not fit on one line the
 *            label goes above the value and the value wraps over the full width
 *   Section  a heading (grey, upper case), never selected
 *   Text     a paragraph (wrapped), or one line cut with dots (cut: the raw lines of a whois answer); selectable only to
 *            let the keys scroll to it
 *   Custom   a block drawn by a RowView of the page (a hop, a badge, the waterfall); its height is given
 *
 * ActionBar: a short row of large cells (Start / Target / Save log) above a list, each one an action: a tap acts at once
 * (the cells are ROW high), Left / Right move between them, Down leaves the bar, Enter acts.
 */
#pragma once

#include "view_model.hpp"

#include "cp0_ui_list.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* the content of a Custom row: the list makes the row's object (host) and hands it over */
class RowView {
public:
    virtual ~RowView() = default;
    virtual void attach(lv_obj_t *host, int w, int h) = 0;
    virtual void detach() {}                      /* the host is about to be deleted: forget its objects */
    virtual void resized(int w, int h) { (void)w; (void)h; }
};

/* The row-affordance rule of the launcher (cp0_ui_list.h) for Aff, and the tone of a value: the same mapping CompactList
 * draws by, so a RichList row and a CompactList row look and measure alike. */
cp0_ui_list_affordance_t c_affordance(Aff aff);
cp0_ui_list_tone_t c_tone(Tone tone);

struct RichRow {
    enum class Kind { Item, Section, Text, Custom };
    Kind kind = Kind::Item;
    int id = 0;
    std::string label, value;
    Tone tone = Tone::Normal;
    bool selectable = true; /* Text, Custom: false = the highlight skips it */
    bool small = false;     /* Text: the small font */
    bool cut = false;       /* Text: one line, cut with dots */
    int height = 0;         /* Custom */
    std::shared_ptr<RowView> view;
    /* Item, Custom: what activating the row does (a chevron, a dot, a grey read-out); Unspecified draws the row as it was
     * before the rule (no marker, the width untouched). A Custom view is attached to the width left of the marker. */
    Aff aff = Aff::Unspecified;

    static RichRow item(std::string label, std::string value, Tone tone = Tone::Normal, int id = 0, Aff aff = Aff::Unspecified);
    static RichRow section(std::string label);
    static RichRow text(std::string text, Tone tone = Tone::Normal, bool selectable = false, bool small = false,
                        bool cut = false);
    static RichRow custom(std::shared_ptr<RowView> view, int height, bool selectable = false, int id = 0,
                          Aff aff = Aff::Unspecified);
};

struct RichListEvents {
    std::function<void(int index, std::intptr_t id)> activate; /* Enter, Right, a tap on the selected row */
    std::function<void()> back;                                /* Esc, Left */
    std::function<void()> up_out;                              /* Up on the first item: leave the list upwards */
    std::function<void()> tab;                                 /* Tab: on to the next thing of the page */
};

class RichList {
public:
    /* the list in the area x, y, w, h of parent; the metrics are the compact CompactList's */
    RichList(lv_obj_t *parent, int x, int y, int w, int h, RichListEvents events);
    ~RichList();
    RichList(const RichList &) = delete;
    RichList &operator=(const RichList &) = delete;

    /* new rows in place: only what changed is touched; the selection keeps its index (clamped), the scroll offset stays */
    void set_rows(const std::vector<RichRow> &rows);
    /* new content: scrolled to the top, `selected` highlighted (an item) and in view */
    void reset_rows(const std::vector<RichRow> &rows, int selected = 0);
    /* highlight an item and scroll just enough to show it (a row taller than the view shows its top) */
    void select(int index);
    /* scroll so that the top of row `index` is at the top of the view (a result that arrived below the fold) */
    void scroll_row_to_top(int index);
    int selected() const { return selected_; }
    /* the id of the selected row (0 when there is none or it has none), and the row of an id (-1 when there is none) */
    int selected_id() const;
    int index_of_id(int id) const;
    void set_up_out(std::function<void()> fn) { events_.up_out = std::move(fn); }
    void set_tab(std::function<void()> fn) { events_.tab = std::move(fn); }
    int count() const { return static_cast<int>(rows_.size()); }
    int row_top(int index) const;
    int row_height(int index) const;
    int content_height() const { return tops_.empty() ? 0 : tops_.back(); }
    int view_height() const { return layout_.h; }
    int scroll_y() const;
    /* rows (of any height) that lie wholly inside the view now */
    int rows_in_view() const;
    const cp0_ui_list_layout_t &layout() const { return layout_; }
    lv_obj_t *focus_object() const { return list_; }
    /* the fonts an Item's label and value are drawn with (a page that cuts its own labels measures with them) */
    struct Fonts {
        const lv_font_t *label = nullptr;
        const lv_font_t *value = nullptr;
    };
    Fonts fonts() const { return {label_font(), value_font()}; }
    /* what a row of this Aff keeps free at its right (0 for no marker): a page that cuts its own labels takes it off its width */
    int trail_w(Aff aff) const { return cp0_ui_list_trail_w(&layout_, c_affordance(aff)); }

private:
    struct Row {
        RichRow spec;
        lv_obj_t *host = nullptr;
        lv_obj_t *label = nullptr;
        lv_obj_t *value = nullptr;
        lv_obj_t *chevron = nullptr; /* the marker of an Open / Edit row, hidden when the row has none */
        lv_obj_t *dot = nullptr;     /* the marker of an Action row */
        int h = 0;
        bool wrapped = false;
    };
    static void list_event_cb(lv_event_t *event);
    void on_key(std::uint32_t key);
    void on_click();
    int measure(const RichRow &spec, bool &wrapped) const;
    void build_host(Row &row, int top);
    void destroy_host(Row &row);
    void fill_texts(Row &row);
    void place_marker(Row &row);
    int trail_of(const RichRow &spec) const;
    void relayout();
    void restyle();
    cp0_ui_list_state_t state() const;
    void apply(const cp0_ui_list_result_t &result);
    const lv_font_t *label_font() const;
    const lv_font_t *value_font() const;
    const lv_font_t *section_font() const;
    const lv_font_t *small_font() const;

    cp0_ui_list_layout_t layout_{};
    RichListEvents events_;
    lv_obj_t *list_ = nullptr;
    std::vector<Row> rows_;
    std::vector<cp0_ui_list_kind_t> kinds_;
    std::vector<int> tops_;
    int selected_ = -1;
    int pad_ = 4;
    bool focused_ = false;
};

struct ActionCell {
    int id = 0;
    std::string label;
    std::string value; /* a second, smaller line (the target); "" for none */
};

struct ActionBarEvents {
    std::function<void(int id)> activate;
    std::function<void()> back;
    std::function<void()> down; /* Down: go on to the list under the bar */
};

class ActionBar {
public:
    /* cells in a row at x, y, w, h; label_font / value_font from the page's tokens */
    ActionBar(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *label_font, const lv_font_t *value_font,
              std::vector<ActionCell> cells, ActionBarEvents events);
    ~ActionBar();
    ActionBar(const ActionBar &) = delete;
    ActionBar &operator=(const ActionBar &) = delete;

    void set_cell(int index, const std::string &label, const std::string &value);
    int count() const { return static_cast<int>(cells_.size()); }
    lv_obj_t *cell_object(int index) const;
    lv_obj_t *root() const { return root_; }
    /* the cell objects, for the page's input group (in order) */
    std::vector<lv_obj_t *> focus_objects() const;

private:
    struct Cell {
        ActionCell spec;
        lv_obj_t *obj = nullptr;
        lv_obj_t *label = nullptr;
        lv_obj_t *value = nullptr;
    };
    static void cell_event_cb(lv_event_t *event);
    static void root_delete_cb(lv_event_t *event);
    void place(Cell &cell);
    void on_key(int index, std::uint32_t key);

    lv_obj_t *root_ = nullptr;
    std::vector<Cell> cells_;
    ActionBarEvents events_;
    const lv_font_t *label_font_ = nullptr;
    const lv_font_t *value_font_ = nullptr;
    int cell_w_ = 0, h_ = 0;
};

} // namespace ui
} // namespace nt
