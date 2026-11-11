/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the app's own layout check for the scenes (CASSETTE_LAYOUT_CHECK=1), on top of the harness's checks:
 *   text      a visible label's text fits its label (one line: its width; wrapping: its height) and is not cut by a
 *             non-scrolling parent
 *   overlap   no two visible labels' texts overlap
 *   targets   every touch target (a clickable leaf that listens to a press) is at least 44 x 44 px; a row of a scrolling
 *             list at least 40 px high (the toolkit's ROW floor: the boards' rows are 42 px); `narrow_ok` objects (the
 *             two 40 x 44 header-side buttons of the boards) at least 40 x 44
 *   screen    every visible object lies inside the screen (a scrolling list may hide part of its rows)
 * Only `root` and its descendants are checked (the page shown, or the box over it). LVGL thread.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <string>
#include <vector>

namespace cassette {
namespace ui {

struct CheckStats {
    int labels = 0, targets = 0;
    int min_target = 1 << 20;
};

/* An object (and its descendants) the check leaves out while `on`: the cassette of the load animation, which is out of its
 * place and has its name cut by the reveal on purpose. */
void check_exempt(lv_obj_t *o, bool on);

std::vector<std::string> check_layout(lv_obj_t *root, int screen_w, int screen_h, const std::vector<lv_obj_t *> &narrow_ok,
                                      CheckStats *stats);

} // namespace ui
} // namespace cassette
