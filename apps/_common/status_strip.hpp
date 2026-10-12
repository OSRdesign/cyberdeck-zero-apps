/*
 * SPDX-License-Identifier: MIT
 *
 * The shared status strip (clock, Wi-Fi, Bluetooth) and the page title, as the home grid draws them (report 024
 * section 1, glue piece 3). Apps call these three functions only; status_strip.cpp is the ONE file that talks to
 * cp0_lvgl's cp0::ui::StatusStrip (task L1). Until L1 lands, a local stand-in of that API (stub/) is compiled instead.
 * LVGL thread only.
 */
#pragma once

#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

namespace cdk {
namespace status_strip {

/* Adds the strip at the top right of `header` (a Page's header). It keeps itself up to date and goes with header. */
void attach(lv_obj_t *header);
/* The screen x of the leftmost pixel the strip draws: cp0_ui_roller_page()'s strip_left (the Esc button's place). */
int left();
/* The title label factory for cp0::ui::Page: the home grid's title place, size and colour. */
cp0::ui::Page::TitleFactory title_factory();

} // namespace status_strip
} // namespace cdk
