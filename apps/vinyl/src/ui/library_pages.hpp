/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the Library pages (P2b, 0.3.0), drawn as the canvas boards draw them (Artists, Artist, Albums, Album, Songs,
 * Genres, TrackInfo, EmptyNoResults, Search-480x320 / -320x480, LibrarySettings, MusicFolders, TrackActions-480x320 /
 * -320x480, AlbumActions), in the list pages' look (the dark panel of the Browser at the class's scale, the same rows:
 * core/layout list_geom) under the launcher's header (the View's cp0::ui::Page). Smaller pieces the hub (drawn by the
 * View on the Browser's list page: the mini player stays) shares with these pages are the helpers at the end.
 *
 *   Artists, Artist, Albums, Album, Songs, Genres   a heading line, the rows (chevron opens, dot plays), an optional chip at
 *                                                   the heading's right ("Play album"); the 40-row pages are the
 *                                                   controller's (it appends rows as the selection nears the end)
 *   Search                                          the toolkit's TextField (cp0::ui::TextField, unrestyled; its
 *                                                   on-screen keyboard and the writing band are the toolkit's and
 *                                                   _common/osk_policy's: D-38, D-39, D-41) over the result rows, or the
 *                                                   "Nothing found" message
 *   Track info                                      label and value pairs, scrolling
 *   Library settings                                three rows and, while a scan runs, the cream block with its bar
 *   Music folders                                   the folders, the Add and Remove buttons, a note
 *   the sheet                                       the cream actions sheet over the dimmed page (a tap on a row acts, a
 *                                                   tap outside closes it)
 * Draws only: every action goes to the callbacks; the controller (ui/library_ctl.cpp) decides. LVGL thread only.
 */
#pragma once

#include "layout/layout.hpp"
#include "view_model.hpp"

#include "cp0_ui_roller.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>

namespace vinyl {
namespace ui {

/* ---------------------------------------------------------------------------------------------- shared pieces */

/* a transparent clickable box over (x, y, w, h) */
lv_obj_t *lib_hit(lv_obj_t *parent, int x, int y, int w, int h, std::function<void()> fn);

/* a drawn button: its box, its word, and a hit box (at least `target` high around it, never beyond `bounds` when given) */
struct LibButton {
    lv_obj_t *vis = nullptr, *word = nullptr, *hitbox = nullptr;
    int x = 0, y = 0, w = 0, h = 0;
    void build(lv_obj_t *parent, int x, int y, int w, int h, int px, uint32_t bg, uint32_t ink, const std::string &text,
               int target, std::function<void()> on_tap);
    void set_text(const std::string &t);
    void set_enabled(bool on); /* off: dimmed, inert */
    void show(bool shown);
};

/* the message of an empty page: a title, a line under it and an optional primary button, centred in the panel */
struct LibEmpty {
    lv_obj_t *title = nullptr, *sub = nullptr;
    LibButton button;
    int bx = 0, by = 0;
    void build(lv_obj_t *parent, const layout::R &panel, double s, int target, std::function<void()> on_button);
    void set(const std::string &t, const std::string &sub_text, const std::string &button_text);
    void show(bool shown);
    bool shown() const { return shown_; }

private:
    bool shown_ = false;
};

/* the hub's strip over the mini player: the scan's text and bar (amber, red while paused for heat) or the drive note (an
 * amber edge, no bar). `y` and `h` are set by place(). */
struct LibNoteStrip {
    lv_obj_t *box = nullptr, *text = nullptr, *bar = nullptr, *fill = nullptr, *edge = nullptr;
    int x = 0, w = 0;
    double s = 1;
    void build(lv_obj_t *parent, int x, int w, double s);
    /* fills it for `n` and returns the height it needs (0: no strip) */
    int set(const LibNote &n);
    void place(int y, int h);
    void show(bool shown);
};

/* ---------------------------------------------------------------------------------------------- the pages */

struct LibraryCallbacks {
    std::function<void(Page, int)> on_row, on_hold, on_button;
    std::function<void(Page)> on_chip;
    std::function<void(const std::string &)> on_query;
    std::function<void()> on_submit, on_field_escape;
    std::function<void(int)> on_sheet_pick;
    std::function<void()> on_sheet_dismiss;
};

class LibraryPages {
public:
    /* make_root: a page root of the View (full screen under the header, hidden at first); tk: the toolkit's page layout
     * (the search field's) */
    LibraryPages(std::function<lv_obj_t *()> make_root, lv_obj_t *sheet_parent, const cp0_ui_roller_page_t &tk, int screen_w,
                 int screen_h, int body_y, int target, int list_row, LibraryCallbacks cb);
    ~LibraryPages();
    LibraryPages(const LibraryPages &) = delete;
    LibraryPages &operator=(const LibraryPages &) = delete;

    lv_obj_t *root(Page p) const;
    void update(Page p, const LibView &v);
    int rows(Page p) const;
    /* where a page's list is scrolled to, in pixels (0: none) */
    int scroll_y(Page p) const;
    /* Track info: scroll by rows */
    void scroll(Page p, int rows);

    /* the search field: the page opens with it editing; the text; Enter / Down leave it for the rows */
    void search_open(const std::string &text, bool edit);
    void search_edit();                 /* start editing again (Up from the first row, a tap on the field) */
    void search_end_edit();
    bool search_editing() const;
    std::string search_text() const;
    /* the field's own keyboard events go to the screen: true while the field edits (the app's shortcuts stand down) */

    /* the actions sheet */
    void show_sheet(const SheetView &v);
    void hide_sheet();
    bool sheet_shown() const;
    void sheet_select(int i);

    /* where a notice may sit on a Library page (screen px) */
    layout::R notice_room(Page p) const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace ui
} // namespace vinyl
