/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl layout: the owner-approved drafts (docs/design (the .dc.html drafts), 2026-11-12) as data, in their own pixels ("design
 * px"), and the size classes that pick one (pure, no LVGL; tests/test_layout.cpp):
 *
 *   Small     480x320 (NP-480x320 of the full-UX canvas, owner 2026-11-13: "ok for the 480x320 layout"): landscape
 *             canvases from aspect 1.2 to 1.6 that are not Medium (480x320, 1024x768)
 *   Medium    640x480 (NP-640x480, owner 2026-11-13: "that works"): the same aspects where the width is under 800 and the
 *             height 480 or more (the deck's 640x480, 720x540)
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

enum class SizeClass { Small, Medium, Portrait, Square, Large };
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
    /* the chips: information only (the format and Gapless), except the large class's output chip. Small: one plain
     * line "AAC 44.1 / 161k, gapless" (gapless_in_format); medium: [format, gapless] flat (chip_flat); square [format]
     * [] [gapless]; large [format] [output] [gapless]; portrait [format] beside the output button. No Vol chip on any
     * class (owner, 2026-11-13): the volume is the group. */
    std::vector<std::vector<int>> chip_layout; /* ids: 0 format, 2 output, 3 gapless (id 1, the old Vol chip, is gone; an
                                                  empty row keeps its place) */
    R chip0;                  /* the first chip's position; height in h */
    double chip_px = 12, chip_pad = 10, chip_gap = 6, chip_icon = 12;
    double chip_space = 8;     /* between two chips of a row */
    bool chips_in_row = false; /* portrait: side by side */
    double chip_step = 32;     /* landscape: the next row's y offset */
    int chip_rows = 2;
    bool short_format = false; /* "FLAC 44.1 / 16" instead of "FLAC 44.1 kHz / 16 bit" */
    bool format_khz = false;   /* medium: "AAC 44.1 kHz / 161k" (the kHz kept, the rest short) */
    bool gapless_in_format = false; /* small: Gapless at the end of the format line, no chip of its own */
    bool chip_flat = false;    /* medium: the format and gapless chips flat (the draft's light fill); else plain text */
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
    /* The volume group (owner, 2026-11-13): "-", the level on teal, "+", on every class (no Vol chip and no volume strip
     * any more). Square, portrait, medium: in the bottom row; small: under the output button in the info column; large:
     * the dimmed Equalizer key's place. The output is a button of its own (out_button_on: icon, the device's name, a
     * chevron; out_button is its box, in portrait x is the least, it starts right of a wider format chip) except on the
     * large class, where the output chip and the Output key stay until the Menu key (P2) exists. */
    bool vol_group = true;
    bool out_button_on = false;
    R vol_minus, vol_value, vol_plus;
    double vol_text_px = 18, vol_icon = 20; /* the level's text, the "-" / "+" icons */
    R out_button;
    double out_px = 12, out_icon = 12, out_pad = 8, out_gap = 6; /* the output button's text, icons, padding, gaps */
    double queue_row_h = 40, queue_heading_px = 11;
    std::vector<double> seams;
};

NowGeom now_geom(SizeClass c);
/* The class shows a Gapless mark (a chip, or the end of the format line). */
bool shows_gapless(const NowGeom &g);

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

/* Every touch area of now playing, in one place (design QA run 3: no two areas may overlap; the view and the tests use
 * this). The areas come in as grow_hit made them (the seek bar's the target high, its own width). Areas whose drawn
 * boxes share a height form a row; in a row, neighbours meet in the middle of the gap between their drawn boxes. The
 * rows (the output button or chip; the small class's volume group; the seek bar; the transport keys; the bottom row) are
 * then laid down the screen: each covers its drawn boxes, a row that shares x with the next one meets it at a boundary
 * in the gap between them, and the heights are the target where the screen has the room, else the largest height every
 * row can have (the rest of the gap goes to the rows that have room: owner rule R-4, the 7 mm are a recommendation; the
 * heights under the target are listed in the QA notes). A row's free side (above the first, below the last, or next to
 * a row it shares no x with) grows to the target, never above top_limit (the header) or below screen_h. */
struct NowHitSet {
    std::vector<HitArea> chips, seek, keys, bottom;
};
void place_now_hits(NowHitSet &h, double target, double top_limit, double screen_w, double screen_h);

/* The drawn boxes of now playing at a size (screen px), as the view draws them: the enabled transport keys, the enabled
 * bottom-row keys, the volume group's "-" and "+", the seek bar's touch box (the drafts' track, the knob high). The
 * output button's (or chip's) box depends on its text: the caller adds it. Areas filled with grow_hit (place_now_hits
 * does the rest). */
NowHitSet now_hit_set(const NowGeom &g, const ui::Fit &f, double target, int keys_dy = 0);
/* The smallest height place_now_hits gives a row (screen px): the target where the screen has room for it. */
double now_rows_min_height(const NowHitSet &h);
/* The few px the transport keys move down where the rows above them miss the target by a little (the coordinator's rule
 * of 2026-11-12: no overlap, the geometry fixed by 1-3 px): the shift (0..P(4)) that gives the rows the most height,
 * the keys staying P(4) clear of what is under them (the panel's edge, the up-next box, the bottom row); 0 where the
 * target fits, or where moving the keys gains nothing. `outputs`: the output button's (or chip's) drawn box. */
int now_keys_shift(const NowGeom &g, const ui::Fit &f, double target, const std::vector<R> &outputs, double top_limit,
                   double screen_w, double screen_h);

/* The queue page's four buttons' touch areas (one row of four, or two rows of two): the target wide and high, the
 * neighbours meeting in the middle of their gaps, from the strip's top down (moved up where the screen's bottom is
 * nearer: `list_bottom` then says where the list must end so its rows are not under them). */
std::vector<R> queue_touch_areas(const ListGeom &lg, int screen_h, double target, double &list_bottom);
/* The browser's mini player: its drawn boxes (the strip that opens now playing, play, next) and their touch areas, the
 * target high from the strip's top down (moved up as above), play and next meeting in the middle of their gap, the
 * open area ending where play's begins. */
struct MiniBoxes {
    R open, play, next;              /* drawn */
    R open_area, play_area, next_area;
    double list_bottom = 0;
};
MiniBoxes mini_touch_areas(const ListGeom &lg, int screen_w, int screen_h, double target);

}  // namespace layout
}  // namespace vinyl
