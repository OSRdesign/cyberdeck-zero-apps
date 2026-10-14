/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the panels above a detail page's list (LVGL thread only). Not interactive: the list under them keeps
 * every key and tap. Sizes come from the layout service (the roller page: row, gap, inset; the CAPTION text size;
 * SPACE_S, RADIUS_S); colours from the launcher's palette (APP-GUIDELINES, report 024 section 5).
 *
 *   HistoryPanel  a detail page's chart: a title line (one coloured text per series, the scale and the time window
 *                 at the right) over an lv_chart line chart of the last 120 samples, newest at the right edge; optional
 *                 constant lines (the 70 / 80 °C marks).
 *   StatusLine    the Processes page's line over its list (the highlighted process): one line of CAPTION text in the
 *                 page status line's green, as wide as the list, cut with dots. It has its own band: the list starts
 *                 under it, so nothing is drawn over a row.
 */
#pragma once

#include "power.hpp"
#include "sampler.hpp"
#include "stats.hpp"

#include "cp0_ui_roller.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sm {

/* the launcher's palette */
constexpr std::uint32_t kRgbText = 0xE0E0E0;   /* secondary text */
constexpr std::uint32_t kRgbLabel = 0x9A9A9A;  /* labels */
constexpr std::uint32_t kRgbValue = 0x00CC66;  /* values (the roller's green) */
constexpr std::uint32_t kRgbBlue = 0x2D9CDB;   /* selection / check: the second series */
constexpr std::uint32_t kRgbOk = 0x66CC88;     /* status line: OK */
constexpr std::uint32_t kRgbWarn = 0xF0B400;   /* warning */
constexpr std::uint32_t kRgbError = 0xFF6666;  /* error */
constexpr std::uint32_t kRgbTrack = 0x2B2B2B;  /* the roller's bar: tracks, grid lines */
constexpr std::uint32_t kRgbBorder = 0x3A3A3A; /* fields' border */

std::uint32_t tone_rgb(Tone tone, std::uint32_t normal);
/* the CAPTION text size in the launcher's Montserrat (FreeType: the degree sign), else the builtin one */
const lv_font_t *caption_font();
int text_width(const char *text, const lv_font_t *font);

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

class HistoryPanel {
public:
    /* series: 1 or 2 data series (green, then blue); marks: constant lines drawn under them */
    HistoryPanel(lv_obj_t *parent, const Box &box, int series, int marks, const cp0_ui_roller_page_t &layout);
    ~HistoryPanel();
    HistoryPanel(const HistoryPanel &) = delete;
    HistoryPanel &operator=(const HistoryPanel &) = delete;

    /* the panel's height for a body (under the header) of body_h: two ROW tokens, up to 2/5 of the body, leaving
     * at least three ROW tokens under it (four or more rows of the compact list) */
    static int height_for(const cp0_ui_roller_page_t &layout);

    /* titles: one per series ("" hides the second); range: "0-100%, last 2 min" (shortened to "0-100%" when the
     * line has no room) */
    void set_text(const std::string &first, std::uint32_t first_rgb, const std::string &second,
                  const std::string &range);
    void set_range(std::int32_t lo, std::int32_t hi);
    /* the ring's values x scale; the newest at the right edge, nothing left of the oldest */
    void set_series(int index, const Ring &ring, double scale);
    void set_mark(int index, std::int32_t value, std::uint32_t rgb);
    /* redraws the chart only when a point or the scale changed */
    void refresh();

private:
    void layout_title();

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_[2] = {};
    lv_obj_t *range_ = nullptr;
    lv_obj_t *chart_ = nullptr;
    std::vector<lv_chart_series_t *> series_;
    std::vector<std::vector<std::int32_t>> data_;
    int marks_ = 0;
    int width_ = 0, gap_ = 0;
    std::string range_text_, shown_first_, shown_second_;
    std::uint32_t shown_rgb_ = 0xFFFFFFFFu;
    std::int32_t lo_ = 0, hi_ = 100;
    bool dirty_ = true; /* a point or the scale changed since the last refresh() */
};

class StatusLine {
public:
    StatusLine(lv_obj_t *parent, const Box &box);
    ~StatusLine();
    StatusLine(const StatusLine &) = delete;
    StatusLine &operator=(const StatusLine &) = delete;

    static int height(); /* one line of the CAPTION text */
    void set_text(const std::string &text); /* only when it changed */

private:
    lv_obj_t *label_ = nullptr;
};

} // namespace sm
