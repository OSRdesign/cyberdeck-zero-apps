/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the two graphic panels above a page's roller (LVGL thread only). Not interactive: the roller under
 * them keeps every key and tap. Sizes come from the layout service (the roller page: row, gap, inset; the CAPTION
 * text size; SPACE_S, RADIUS_S); colours from the launcher's palette (APP-GUIDELINES, report 024 section 5).
 *
 *   MetersPanel   the overview's bars: one per CPU core ("Core 0", the bar, "23%"), two columns, half a row high
 *                 each, and one status line under them (the power state, a hot CPU) in the status colours.
 *   HistoryPanel  a detail page's chart: a title line (one coloured text per series, the scale and the time window
 *                 at the right) over an lv_chart line chart of the last 120 samples, newest at the right edge; optional
 *                 constant lines (the 70 / 80 °C marks).
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

class MetersPanel {
public:
    MetersPanel(lv_obj_t *parent, const Box &box, int cores, const cp0_ui_roller_page_t &layout);
    ~MetersPanel();
    MetersPanel(const MetersPanel &) = delete;
    MetersPanel &operator=(const MetersPanel &) = delete;

    /* the height the panel needs for `cores` bars and its status line */
    static int height_for(int cores, const cp0_ui_roller_page_t &layout);
    /* per core %, empty: not known yet */
    void set(const std::vector<double> &pcts);
    void set_status(const std::string &text, Tone tone);

private:
    lv_obj_t *root_ = nullptr;
    std::vector<lv_obj_t *> bars_, values_;
    std::vector<int> shown_;
    lv_obj_t *status_ = nullptr;
    std::string status_text_;
    Tone status_tone_ = Tone::Normal;
    bool status_set_ = false;
};

class HistoryPanel {
public:
    /* series: 1 or 2 data series (green, then blue); marks: constant lines drawn under them */
    HistoryPanel(lv_obj_t *parent, const Box &box, int series, int marks, const cp0_ui_roller_page_t &layout);
    ~HistoryPanel();
    HistoryPanel(const HistoryPanel &) = delete;
    HistoryPanel &operator=(const HistoryPanel &) = delete;

    /* the panel's height for a body (under the header) of body_h: two rows, up to 2/5 of the body, leaving the
     * roller at least three rows */
    static int height_for(const cp0_ui_roller_page_t &layout);

    /* titles: one per series ("" hides the second); range: "0-100%, last 2 min" (shortened to "0-100%" when the
     * line has no room) */
    void set_text(const std::string &first, std::uint32_t first_rgb, const std::string &second,
                  const std::string &range);
    void set_range(std::int32_t lo, std::int32_t hi);
    /* the ring's values x scale; the newest at the right edge, nothing left of the oldest */
    void set_series(int index, const Ring &ring, double scale);
    void set_mark(int index, std::int32_t value, std::uint32_t rgb);
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
    std::string range_text_;
    std::int32_t lo_ = 0, hi_ = 100;
};

} // namespace sm
