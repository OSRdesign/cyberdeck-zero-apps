/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the map credit at the bottom of the map. The engine's widget draws the attribution of the map in view
 * in one row (cut with "..." when it does not fit: cp0_map_widget.c fit_text) and the app cannot change that text. When
 * the engine would cut it, the app covers the engine's box with a plate of its own that shows the whole credit: the
 * standard abbreviation first ("OpenStreetMap" -> "OSM"), then up to three lines (OpenTopoMap's credit needs three on a 320 px map). The plate sits exactly where the engine's
 * row is (above the scale bar, left of the button column), so it never overlaps the scale bar or the buttons.
 *
 * plan_attribution() repeats the engine's layout rules for that box (cp0_map_view.c cp0_map_layout: the plate, margin
 * and padding rules, the scale bar's width, "beside the scale bar or a row higher"); a change of those rules in the
 * toolkit has to be followed here (tests/test_attribution.cpp pins the numbers). Pure: no LVGL, no engine.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace atlas {

/* The credit as the widget shows it: HTML tags dropped, entities decoded (&copy; &amp; &lt; &gt; &quot; &apos; &nbsp;
 * and numeric ones), spaces collapsed, the copyright sign written "(C)" (the builtin fonts have none). */
std::string attribution_plain(const std::string &raw);
/* The standard short forms: "OpenStreetMap" -> "OSM". The text is otherwise unchanged. */
std::string attribution_abbrev(const std::string &text);

using MeasureFn = std::function<int(const std::string &)>; /* width in px of one line of text */
/* Greedy word wrap into at most max_lines lines of at most max_w px; what does not fit ends the last line with "...". */
std::vector<std::string> wrap_lines(const std::string &text, int max_w, int max_lines, const MeasureFn &measure);

constexpr int kMaxLines = 3;

struct AttrInput {
    int w = 0, h = 0;      /* the map widget's size */
    int target = 44;       /* the TARGET token */
    int space = 8;         /* the SPACE_M token */
    int text_h = 12;       /* the caption font's line height */
};

struct AttrPlan {
    bool cover = false;    /* true: the engine would cut the credit; draw `lines` on a plate at x, y, w, h (widget coordinates) */
    int x = 0, y = 0, w = 0, h = 0;
    int pad = 0;
    std::vector<std::string> lines;
};

AttrPlan plan_attribution(const AttrInput &in, const std::string &text, const MeasureFn &measure);

} // namespace atlas
