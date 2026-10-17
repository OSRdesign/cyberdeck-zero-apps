/*
 * SPDX-License-Identifier: MIT
 *
 * The texts and the small layout rules of the pages that follow the screen (phase P3: Contacts, a contact's details,
 * Nearby, Settings, Statistics, the packet log, the message search). No LVGL, no I/O: unit tested on the PC
 * (tests/unit/test_pages_ui.inc). The LVGL side is mesh_app.cpp.
 */

#pragma once

#include "ui_logic.hpp"

#include <string>
#include <vector>

namespace meshhop {

/* ------------------------------------------------------------------ the page column */

/* The column the pages are laid out in: one SPACE_L margin on each side on the smallest screen (min_w, the app's
 * X-MinSize), two once the screen is that much wider (Net Toolbox's and Serial Console's 40 px at 800x480), never wider
 * than cap_w (Net Toolbox's column cap). x, w: the edge of the texts and of the buttons; a list reaches `inset` beyond
 * them on each side (its rows draw the highlight there and their texts start at the column edge). */
struct PageColumn {
    int x = 0, w = 0;
};
PageColumn page_column(int screen_w, int inset, int min_w, int cap_w);

/* Which of `widths` (in priority order: index 0 first) fit side by side in `avail` px with `gap` between two of them.
 * The first always stays; then each in turn while it fits (a later, narrower one can still fit after a wide one did
 * not). Returns the indexes kept, in priority order. */
std::vector<int> fit_columns(const std::vector<int> &widths, int gap, int avail);

/* ------------------------------------------------------------------ the kind of a row (APP-GUIDELINES "Row kinds") */

/* What tapping or Enter on a row does, as the row shows it: Info (a read-out: no marker), Open (a page, a panel or a menu:
 * a chevron), Edit (a value changed through a choice, an editor or a switch: the value, then a chevron), Action (does
 * something right there, maybe after a question: a dot), Running (that action is in progress: the dot in blue). */
enum class Aff { None, Info, Open, Edit, Action, Running };
Aff settings_row_aff(SRowKind k);

/* ------------------------------------------------------------------ Contacts */

/* The column titles of 0.3.0, in their order: what the Sort dialog lists. */
const std::vector<SortKey> &sort_menu_keys();
const char *sort_field_name(SortKey k);                       // "Name", "Heard", "Type", "SNR", "Hops", "Distance"
std::string sort_direction_text(SortKey k, bool reverse);     // "newest first", "A to Z", "best first" ...
std::string sort_button_text(const ContactView &v);           // "Sort: Heard"
const char *type_filter_label(TypeFilter t);                  // "All types", "Chat", "Repeater", "Room", "Sensor"
const char *age_filter_label(AgeFilter a);                    // "Any time", "Last hour", "Last 24 h", "Last 7 days"
/* The line under the title: "193 contacts  ·  All types  ·  Any time", "12 of 193  ·  Repeater  ·  Last 24 h  ·  Group Friends". */
std::string contact_view_summary(size_t shown, size_t total, const ContactView &v);
/* The select mode's line: "3 contacts marked", "3 contacts marked (+1 hidden by the filters)". */
std::string contact_marked_summary(size_t marked, size_t hidden);
/* The second line of a contact row: "Repeater  ·  heard 47 min  ·  3 hops", hops left out while unknown, "  ·  SNR 5.5 dB"
 * added when with_snr and the SNR is known. */
std::string contact_row_caption(const ContactRow &r, bool with_snr);
/* The distance at the right of a contact row: "6.2 km", or an en dash when unknown. */
std::string contact_row_distance(const ContactRow &r);

/* ------------------------------------------------------------------ Nearby */

/* "5 nodes heard  ·  2 waiting" (the line under the title). */
std::string nearby_summary(size_t shown, size_t pending, bool showing_ignored);
/* The second line of a Nearby row: "Repeater  ·  SNR 5.5 dB  direct  12 s ago". */
std::string nearby_row_caption(const NearbyRow &r);

} // namespace meshhop
