/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the pages as drawn (docs/design (the .dc.html drafts)), built once for the canvas's size class:
 *
 *   Now      now playing: the header chips (queue position, centred Esc, volume), the cream panel, the sleeve with the
 *            record sliding out behind it (turning only while playing), title / artist / album, the format and output
 *            chips, the time row with the seek bar (a tap seeks: no dragging), the transport keys, the class's extras
 *            (up next, the bottom row, the queue pane of the large class);
 *   Browser  the folder list with the mini player;
 *   Queue    the play order with Move up, Move down, Remove, Clear all (no dragging);
 *   Output   the output chooser with its hint strip (or the "not set up" notice strip);
 *   Notices  (a demonstration page for the scenes only: the four notices of the notices draft).
 * Where the drawing makes a control smaller than the toolkit's touch target, the control keeps its look and gets a larger
 * transparent hit box. The view only draws: every action goes to the controller through Actions. LVGL thread only.
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
    void show_notice(const Notice &n);
    void hide_notice();
    bool notice_shown() const;
    /* the record turns while `playing` and the now-playing page shows; call it often (it keeps its own clock) */
    void tick(bool playing);
    /* the row a list page shows as selected (for the keys) */
    int list_rows(Page p) const;
    /* the sleeve's size on this canvas (the cover is decoded to it) */
    int cover_side() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace ui
} // namespace vinyl
