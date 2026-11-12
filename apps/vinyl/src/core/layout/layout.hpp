/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl layout: the owner-approved drafts (docs/design (the .dc.html drafts), 2026-11-12) as data, in their own pixels ("design
 * px"), and the size classes that pick one (pure, no LVGL; tests/test_layout.cpp):
 *
 *   Small     480x320 (Main.dc.html): landscape canvases from aspect 1.2 to 1.6 (480x320, the deck's 640x480)
 *   Portrait  320x480 (NowPlaying-320x480): canvases taller than wide (aspect under 0.9: 320x480, PDA 480x800, 800x1280)
 *   Square    480x480 (NowPlaying-480x480): aspect 0.9 to 1.2 (the HackBerry's 720x720)
 *   Large     800x480 (NowPlaying-800x480): aspect 1.6 and more (800x480, 1024x600, 1280x720, 1280x800), two panes
 *
 * The top band is the launcher's (owner correction 2026-11-12): every page has the toolkit's header (cp0::ui::Page, its
 * title, its Esc, the shared status strip); only the drafts' content below it is Vinyl's. As Cassette does, the drafts'
 * y 40 sits on the toolkit header's bottom (body_y; 40 px at 480x320 and 320x480, so the panel at y 44 is where the draft
 * has it) and the content is scaled uniformly (Cassette's Fit: s = min(w / design w, body h / design body h), centred
 * across); the height a canvas has beyond the design goes to the seams listed per class, inside the cream panel. At the
 * drawn sizes nothing is stretched: the drafts' pixels. The drafts' header chips (the queue counter, the volume) are not
 * built; the volume is a chip of the info zone, on the format chip's row. Touch targets that the drawing makes smaller than the toolkit's 7 mm TARGET keep their
 * drawn look and get a larger, invisible hit box (the UI's job).
 *
 * The Browser, Queue and Output pages are drawn at 480x320 only; on the other classes they keep the same pieces at the
 * class's scale, the dark list filling the panel and the bottom strip across it (QueueButtons wrap into two rows where
 * four buttons do not fit the width).
 */
#pragma once

#include "fit.hpp"

#include <string>
#include <vector>

namespace vinyl {
namespace layout {

enum class SizeClass { Small, Portrait, Square, Large };
SizeClass size_class(int w, int h);
const char *class_name(SizeClass c);

struct R {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
};

struct Text {
    R box;
    double px = 15;
    double lh = 0;     /* line height (0: the font's) */
    int lines = 1;     /* 1: one line (scrolls when long), 2: wraps on two lines */
    bool bold = true;
};

struct Btn {
    std::string id; /* "shuffle", "prev", "play", "next", "repeat", "queue", "sleep", "browse", "output", "eq" */
    R box;
    double icon = 22;
    bool teal = false;   /* the play button */
    bool label = false;  /* a bottom-row button with its word ("Queue") */
    bool enabled = true; /* false: drawn as in the draft, dimmed, no action (a P2 feature) */
};

constexpr double kDesignBody = 40; /* the drafts' y that sits on the toolkit header's bottom */

struct NowGeom {
    SizeClass cls = SizeClass::Small;
    int dw = 480, dh = 320;
    R panel;
    R sleeve, disc;
    double label_d = 50, hole_d = 6;
    Text title, artist, album;
    /* the chips (the drafts as updated 2026-11-12): small rows [format, volume] [output]; square and large [format]
     * [output, volume] [gapless]; portrait one row [format, output, volume]. The volume chip is teal with white text. */
    std::vector<std::vector<int>> chip_layout; /* ids: 0 format, 1 volume, 2 output, 3 gapless */
    double vol_px = 12, vol_pad = 10;
    R chip0;                  /* the first chip's position; height in h */
    double chip_px = 12, chip_pad = 10, chip_gap = 6, chip_icon = 12;
    double chip_space = 8;     /* between two chips of a row */
    bool chips_in_row = false; /* portrait: side by side */
    double chip_step = 32;     /* landscape: the next row's y offset */
    int chip_rows = 2;
    bool short_format = false; /* "FLAC 44.1 / 16" instead of "FLAC 44.1 kHz / 16 bit" */
    bool quality_in_chip = true; /* "Evolve 65, music quality" */
    Text time, remain;
    R seek;
    std::vector<Btn> transport;
    R repeat_dash;
    bool has_upnext = false;
    R upnext;
    std::vector<Btn> bottom;
    bool has_queue_pane = false;
    R queue_pane;
    double queue_row_h = 40, queue_heading_px = 11;
    std::vector<double> seams;
};

NowGeom now_geom(SizeClass c);

/* The list pages at a scale: the dark panel, the list area and the bottom strip, in SCREEN px (already scaled and
 * centred), under the toolkit header that ends at body_y. */
struct ListGeom {
    double s = 1;
    R panel, heading, list, strip;
    double row_h = 36, heading_px = 12;
    /* the queue page's four buttons */
    std::vector<R> queue_buttons;
};

enum class ListPage { Browser, Queue, Output };
ListGeom list_geom(ListPage page, int screen_w, int screen_h, int body_y);

/* The Fit of the now-playing design on a screen whose toolkit header ends at body_y. */
ui::Fit now_fit(const NowGeom &g, int screen_w, int screen_h, int body_y);

/* Where a notice strip goes on the now-playing page (design px): across the seek row, its bottom on the row's bottom. */
R notice_box_now(const NowGeom &g, double height);

}  // namespace layout
}  // namespace vinyl
