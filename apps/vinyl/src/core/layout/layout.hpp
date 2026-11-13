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

#include <cstdint>
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
    std::vector<std::vector<int>> chip_layout; /* ids: 0 format, 1 volume, 2 output, 3 gapless (vol_group classes: no 1 and no
                                                  2, the output is out_button; an empty row keeps its place) */
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
    /* The volume group (owner, 2026-11-13): "-", the level on teal, "+" in the bottom row; no Vol chip and no volume
     * strip where it is. Square and portrait (NowPlaying-480x480 / -320x480): it takes the Output key's place and the
     * output chip becomes a button of its own (out_button_on: icon, the device's name, a chevron; out_button is its box,
     * in portrait x is the least, it starts right of a wider format chip). Large (NP-800x480 of the full-UX canvas): it
     * takes the dimmed Equalizer key's place, the Output key stays until the Menu key (P2) exists, the output chip stays
     * a chip. The small class keeps the Vol chip and its strip. */
    bool vol_group = false;
    bool out_button_on = false;
    R vol_minus, vol_value, vol_plus;
    R out_button;
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

/* The time row's texts (board screenshot, 2026-11-13: "-3..." on a one-hour mixtape): h:mm:ss for a track of an hour
 * or more, elapsed and remaining alike ("0:01:14", "1:01:16"), mm:ss otherwise ("01:14" with lead_zero, "3:51"). */
std::string track_time_text(uint64_t ms, bool hours, bool lead_zero);
/* The widest text of each label for a format (the labels are sized for it, so a long track is never cut). */
const char *track_time_widest(bool hours, bool remaining);

/* The chip rows for the chips' natural widths (screen px; 0 = no chip): when the volume chip (id 1) shares a row with a
 * chip that would end with dots, it moves to the end of another row where it fits whole (the first such row), so the
 * other chip gets the whole row (board screenshot 2026-11-13: "USB Audio..." beside "Vol 70" at 720x720). A row that
 * fits, or a volume chip that fits nowhere else, stays as it is. */
std::vector<std::vector<int>> flow_chip_rows(const std::vector<std::vector<int>> &rows, const int natural[4], int avail,
                                             int space);

/* The touch areas of a page (screen px). A control drawn smaller than the toolkit's TARGET gets a larger invisible area
 * (grow_hit: the drawn box grown to min_w x min_h around its centre). split_pair keeps two neighbours' areas apart
 * (design QA D-33, 2026-11-12: the mini player's play and next keys) without making either smaller than `min` (the
 * harness's size check): the areas meet at the middle of the gap between the drawn boxes, along the axis where the
 * drawn boxes are farther apart, and an area that became smaller than min there grows away from the other one again.
 * Areas that do not overlap, and drawn boxes that overlap each other, are left as they are. */
struct HitArea {
    R drawn, area;
};
R grow_hit(const R &drawn, double min_w, double min_h);
void split_pair(HitArea &a, HitArea &b, double min);
bool overlaps(const R &a, const R &b);

/* The touch areas of now playing while the volume strip is open (design QA run 2, D-33): no strip area overlaps another
 * area. The strip's "-" and "+" take the band between the interactive chips above them and the transport keys below
 * them (the target high where the band allows it, else the whole band). A chip whose area reaches into a strip area
 * stops at it and grows upwards instead (a chip it then reaches moves up the same way, never under top_limit, the
 * header); a transport key's area starts below the strip areas it reaches and grows downwards, and a bottom-row key's
 * starts below the transport keys' areas it reaches and grows downwards (never past screen_h). The areas come in as
 * grow_hit gives them, the chips already split_pair'ed. A row that has no room for its keys at the target width keeps
 * its own overlaps (the six transport keys at 480x480); this only keeps the strip and its neighbours apart. */
struct NowHitSet {
    std::vector<HitArea> chips, strip, keys, bottom;
};
void place_strip_hits(NowHitSet &h, double target, double top_limit, double screen_h);

}  // namespace layout
}  // namespace vinyl
