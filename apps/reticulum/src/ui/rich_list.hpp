/*
 * SPDX-License-Identifier: MIT
 *
 * RichList: the app's list with two-line rows (a name and a time, then a second line with an optional count and a
 * marker), for Messages and Heard recently. A list of your own in the sense of the guidelines ("rows of any height"):
 * the key, tap and scroll rules, the colours, the chevron and the dot are cp0_lvgl's (cp0_ui_list.h), so it looks and
 * behaves like cp0::ui::CompactList; only the row heights differ. Single-line items and section headers work as in
 * CompactList, so one list can hold a header, two-line rows and an action row. A Block is a wrapped text that is not selectable
 * (the address, or a note in a frame).
 *
 * LVGL thread only. The rows are rebuilt when a set_rows() changes anything (the lists are short: 200 rows at most); the
 * scroll offset and the selected row stay.
 */
#pragma once

#include "cp0_ui_widgets.hpp"

#include <functional>
#include <string>
#include <vector>

namespace retui {

struct RichRow {
    enum class Kind { Section, Item, Two, Block };
    Kind kind = Kind::Item;
    std::string label;  /* Item: the label; Two: the first line's name; Section: its title */
    std::string value;  /* Item: right, never cut */
    cp0::ui::ValueTone tone = cp0::ui::ValueTone::Normal; /* Item: the value's tone */
    std::string time;   /* Two: right end of the first line, grey */
    std::string sub;    /* Two: the second line */
    cp0::ui::ValueTone sub_tone = cp0::ui::ValueTone::Muted;
    bool bold = false;  /* Two: the name in bold (unread) */
    int badge = 0;      /* Two: a count in an amber pill at the end of the second line (0: none) */
    bool panel = false; /* Block: a note in an amber frame (otherwise the text is big and bold: an address) */
    cp0::ui::RowAffordance affordance = cp0::ui::RowAffordance::Unspecified;
    std::intptr_t id = 0;

    static RichRow section(std::string title)
    {
        RichRow r;
        r.kind = Kind::Section;
        r.label = std::move(title);
        return r;
    }
    static RichRow item(std::string label, std::string value, cp0::ui::ValueTone tone, std::intptr_t id,
                        cp0::ui::RowAffordance aff)
    {
        RichRow r;
        r.label = std::move(label);
        r.value = std::move(value);
        r.tone = tone;
        r.id = id;
        r.affordance = aff;
        return r;
    }
    bool same(const RichRow &o) const;
};

class RichList {
public:
    using Activate = std::function<void(int index, std::intptr_t id)>;
    RichList(lv_obj_t *parent, const cp0_ui_list_layout_t &layout, Activate activate, std::function<void()> back);
    ~RichList();
    RichList(const RichList &) = delete;
    RichList &operator=(const RichList &) = delete;

    /* Live data: the rows are replaced when anything changed; scroll and selection stay (clamped). */
    void set_rows(const std::vector<RichRow> &rows);
    /* New content: scrolled to the top with `selected` highlighted (the next item when it is a section). */
    void reset_rows(const std::vector<RichRow> &rows, int selected);
    int selected() const { return selected_; }
    int count() const { return static_cast<int>(rows_.size()); }
    lv_obj_t *focus_object() const { return list_; }

private:
    struct Obj {
        lv_obj_t *row = nullptr;
        lv_obj_t *label = nullptr; /* the name: goes bold on the selected row */
    };
    static void list_event_cb(lv_event_t *event);
    static void row_event_cb(lv_event_t *event);
    static void list_delete_cb(lv_event_t *event);

    void rebuild();
    void build_row(int index, int top, int height);
    void style_selected(int index);
    int height_of(const RichRow &row) const;
    const lv_font_t *block_font(const RichRow &row) const;
    int block_text_w(const RichRow &row) const;
    std::vector<std::string> block_lines(const RichRow &row) const;
    cp0_ui_list_state_t state() const;
    void move_to(int index, int scroll_y);
    void on_key(uint32_t key);
    void on_tap(int index);

    cp0_ui_list_layout_t layout_;
    Activate activate_;
    std::function<void()> back_;
    lv_obj_t *list_ = nullptr;
    std::vector<RichRow> rows_;
    std::vector<Obj> objs_;
    std::vector<cp0_ui_list_kind_t> kinds_;
    std::vector<int> tops_;
    int selected_ = -1;
    int two_h_ = 0;
};

} // namespace retui
