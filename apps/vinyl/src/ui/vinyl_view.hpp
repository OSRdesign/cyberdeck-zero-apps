/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the pages as drawn (docs/design (the .dc.html drafts)), built once for the canvas's size class:
 *
 *   Now      now playing: the header chips (queue position, centred Esc, volume), the cream panel, the sleeve with the
 *            record sliding out behind it (turning only while playing), title / artist / album, the format and output
 *            chips (information), the output button (the large class: its output chip), the volume group ("-", the level
 *            on teal, "+"), the time row with the seek bar (a tap seeks, a drag scrubs: core/seek), the transport keys, the class's
 *            extras (up next, the bottom row, the queue pane of the large class);
 *   Browser  the folder list with the mini player;
 *   Queue    the play order with Move up, Move down, Remove, Clear all (no dragging);
 *   Output   the output chooser with its hint strip (or the "not set up" notice strip);
 *   Notices  (a demonstration page for the scenes only: the four notices of the notices draft);
 *   Menu ... About  the Menu pages (P2a: ui/menu_pages.hpp); P2d adds the Crossfade and Preset pick lists there and the
 *            Equalizer page (ui/eq_page.hpp).
 *   LibHub ... LibFolders  the Library (P2b): the hub is the Browser's list page with the Search chip, the scan strip or
 *            the drive note over the mini player and the empty message; the other pages are ui/library_pages.hpp; the
 *            Browser also has a choose mode (Music folders > Add a folder).
 * Where the drawing makes a control smaller than the toolkit's touch target, the control keeps its look and gets a larger
 * transparent hit box, never overlapping a neighbour's (layout::place_now_hits). The previous and next buttons report
 * press, release and cancel (the transport press decides). Knock-knock adds the cover zone and the shield (below). The
 * view only draws: every action goes to the controller through Actions. LVGL thread only.
 */
#pragma once

#include "layout/layout.hpp"
#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <memory>

namespace vinyl {
namespace ui {

class View {
public:
    View(lv_obj_t *screen, Actions actions);
    ~View();
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    layout::SizeClass size_class() const;
    void show_page(Page p);
    Page page() const;
    void update_now(const NowView &v);
    void update_list(Page p, const ListView &v);
    /* the Menu pages (P2a): a list page's rows, the Keys page's table, the About page's texts; Keys and About scroll */
    void update_menu(Page p, const MenuView &v);
    /* P2d: the Equalizer page (ui/eq_page.hpp) */
    void update_eq(const EqView &v);
    /* P2b: the Library pages (LibHub is the Browser's list page with the mini player), the search field and the actions sheet */
    void update_lib(Page p, const LibView &v);
    void search_open(const std::string &text, bool edit); /* the field's text; edit: it takes the keys (the page is shown) */
    void search_edit();
    void search_end_edit();
    bool search_editing() const;
    /* where a Library page's list is scrolled to, in pixels (kept when the place is left, given back by LibView::scroll_to) */
    int lib_scroll_y(Page p) const;
    void show_sheet(const SheetView &v);
    void hide_sheet();
    bool sheet_shown() const;
    void sheet_select(int i);
    void set_keys(const std::vector<menu::KeyHelp> &rows);
    void set_about(const menu::About &a);
    void scroll_page(Page p, int rows);
    void show_notice(const Notice &n);
    void hide_notice();
    bool notice_shown() const;
    /* P2c: a question with a destructive answer on the toolkit's dialog (Delete "Evening"?): Cancel is the Primary button (Enter),
     * the letter Y (named in the text) or a tap on `ok` answers Actions::confirm_answer(true) */
    void ask_confirm(const std::string &title, const std::string &body, const std::string &ok);
    /* P2c: the name dialog (Save the queue, New playlist, Rename, Import as): the toolkit's Dialog with its TextField, which opens
     * the on-screen keyboard by itself when no keyboard is connected (the same policy as the Library's search field). The answer is
     * Actions::name_answer. A notice shown while it is up is written in its message line (the page behind it is hidden). */
    void ask_name(const NameAsk &a);
    bool name_dialog_shown() const;
    void close_name_dialog(); /* cancels it without an answer */
    /* the design-QA scenes (VINYL_DEMO_SCRUB): the seek bar's scrubbing state as if a finger were held at `frac` */
    void simulate_scrub(double frac);
    bool dialog_shown() const;
    bool dialog_key(uint32_t key, uint32_t evdev = 0); /* evdev: the physical key (Y clears), 0 for a keypad key */
    /* the record turns while `playing` and the now-playing page shows; call it often (it keeps its own clock) */
    void tick(bool playing);
    /* the row a list page shows as selected (for the keys) */
    int list_rows(Page p) const;
    /* the sleeve's size on this canvas (the cover is decoded to it) */
    int cover_side() const;
    /* the toolkit's TARGET in px (the touch size) */
    int touch_target() const;
    /* Knock-knock (2026-11-16). Every press and release of the touch screen goes to Actions::touch (the pointer's own
     * events, seen before any object gets them); the moves and lost presses of the two boxes below too.
     *   the cover zone   a transparent box exactly over the sleeve, under every control drawn after it (a neighbour's
     *                    touch area keeps its taps), shown while the taps on the cover count; it does nothing itself
     *   the shield       a fully opaque black box over the whole display on its system layer (over the header, the
     *                    notices, the dialogs and the on-screen keyboard), shown while the screen is off: only black pixels
     *                    remain (a panel whose backlight only dims at 0 shows nothing), and every touch lands on it, so nothing
     *                    under it is pressed, the first touch of the wake taps included
     * freeze_drawing: the pending areas drawn now (the black frame reaches the panel), then the display marks nothing more;
     * thaw_drawing: marking again (each freeze thawed once: LVGL counts them), repaint: the whole display at the next
     * refresh. core/screen BlankCover decides when. */
    void set_knock_zone(bool on);
    void set_input_shield(bool on);
    bool input_shield() const;
    void freeze_drawing();
    void thaw_drawing(bool repaint);
    /* the cover zone's place in screen px (the log at start) */
    layout::R cover_rect() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace ui
} // namespace vinyl
