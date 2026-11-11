/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the texts the screens show, built from the view model (pure, no LVGL; tests/ui/test_format.cpp).
 *
 * Times (ms, negative = 0):
 *   counter     "00:42", "59:59"; from one hour "1:02:03" (the counter then draws smaller digits)
 *   mmss        "05:44" (truncated to the second), "1:02:03" from one hour
 *   tenths      "02:58.7" (padded minutes, truncated to the tenth), "1:02:58.7" from one hour
 *   short       "2:59" (rounded to the second, minutes not padded), "1:02:59" from one hour
 *   ruler       "5:14" (truncated, minutes not padded), "1:02:03" from one hour
 *   zoom        "60 s", "2 s", "1.5 s" (under 10 s with a tenth), "12 min" from 100 s
 * Levels: db "+13.0 dB" (decimal) or "+13 dB"; 0 is "0.0 dB" / "0 dB".
 */
#pragma once

#include "view_model.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cassette {
namespace ui {

std::string fmt_counter(std::int64_t ms);
bool counter_long(std::int64_t ms); /* the counter shows H:MM:SS */
std::string fmt_mmss(std::int64_t ms);
std::string fmt_tenths(std::int64_t ms);
std::string fmt_tenths_short(std::int64_t ms); /* "5:44.2": minutes not padded (the narrow trim page) */
std::string fmt_short(std::int64_t ms);
std::string fmt_ruler(std::int64_t ms);
std::string fmt_zoom(std::int64_t span_ms);
std::string fmt_db(double db, bool decimal);

/* The name box's remaining-characters counter (owner, 2026-11-10): "<used>/<limit>" under the entry, right aligned. Muted
 * under 20 used, amber for the last four before full (20 to 23 of 24), red when full (used >= limit). used counts bytes (the name is ASCII). */
constexpr int kNameLimit = 24;
enum class CountTone { Muted, Amber, Red };
std::string name_count_text(std::size_t used, std::size_t limit = kNameLimit);
CountTone name_count_tone(std::size_t used, std::size_t limit = kNameLimit);
std::uint32_t name_count_color(CountTone t); /* theme: kHint, kAmber, kRingRed */

/* The recordings list's row geometry. A name that wraps to more than one line makes its row taller (no ellipsis): the name
 * box grows to the wrapped text, the detail line sits under it, the row is the standard height plus the growth. A name of
 * one line leaves the standard row exactly as it was (the name's one-line box name_box1, the detail's detail_box). All
 * sizes in pixels. name_text_h is the wrapped name's height, name_line_h one line of the name's font. */
struct ListRowGeom {
    int h = 0;          /* the row's height (border included) */
    int name_box = 0;   /* the height reserved for the name */
    int name_y = 0;     /* the name label's top inside the row (inside the border) */
    int detail_y = 0;   /* the top of the detail's box (the line is centred in detail_box there) */
};
ListRowGeom list_row_geom(int std_h, int name_box1, int detail_box, int name_text_h, int name_line_h, int detail_text_h = 0);
/* detail_text_h: the detail's wrapped height when it is taller than its one-line box (merge mode: a dimmed row's reason that
 * does not fit one line at the list's detail size wraps); the row grows by the difference, the name stays on top. */

/* The recordings list's row keys in board px (the page's 480 x 320 landscape / 320 x 480 portrait board): `count` keys side by
 * side over the span of the trim page's five keys (landscape x 16 to 464 at y 222, 56 high; portrait x 16 to 304 at y 396, 64
 * high; 7 px apart). Five keys (LOAD, RENAME, EDIT, MERGE, DELETE) are exactly the trim page's (84 x 56 landscape, 52 x 64
 * portrait); the two keys of merge mode (CANCEL, MERGE) share the same span. */
struct KeyBox {
    double x = 0, y = 0, w = 0, h = 0;
};
KeyBox list_key_box(int index, int count, bool portrait);

/* The LED bar: lit steps for a sample peak in dBFS (the core meter's rule: step i, 0..14, lit when db >= -40 + i*40/14),
 * and the colour band of step i: 0..9 green, 10..12 amber, 13..14 red. */
int bar_steps(float db);
enum class LedBand { Green, Amber, Red };
LedBand led_band(int step);

/* The PLAY / PAUSE key shows the CURRENT state, latched like REC while recording (owner, 2026-11-09). One key, one glyph;
 * what a press does is unchanged (it plays, pauses and resumes). The same at every screen size:
 *   transport                          glyph   latched
 *   Idle (stopped, nothing runs)       PLAY    no
 *   Countdown (REC pressed, timer)     PLAY    no      (the key is dimmed; nothing records or plays yet)
 *   Playing, ScanForward, ScanReverse  PLAY    yes     (normal speed, or while REW / FWD is held)
 *   PlayingPaused                      PAUSE   yes
 *   Recording                          PAUSE   no      (REC is latched; PAUSE is what a press would do)
 *   RecordingPaused                    PAUSE   yes
 * The UI takes the glyph from here and the latch from KeyState::latched (the binding and the demo fill it from here). */
struct PlayKeyLook {
    bool pause_glyph = false;
    bool latched = false;
};
PlayKeyLook play_key_look(Transport t);

/* The cassette hubs (owner, 2026-11-09): both hubs turn the same way at the same speed (a real cassette), in degrees per
 * second, counterclockwise positive:
 *   Recording, Playing                 +kHubPlayDps   (one turn in 2.4 s)
 *   ScanForward, a forward 5 s jump    +3 x kHubPlayDps
 *   ScanReverse, a backward 5 s jump   -3 x kHubPlayDps (clockwise)
 *   anything else                      0 (still)
 * jump_dir: -1 / +1 while a 5 s jump flashes (about 300 ms, core kJumpFlashMs), 0 otherwise; it wins over the transport. */
constexpr double kHubPlayDps = 150.0;
double hub_speed_dps(Transport t, int jump_dir = 0);

/* The recorder's info strip (landscape, one line) and the info box beside the counter (portrait, square: codec over detail;
 * the take's name is on the cassette label, not in the box):
 *   recording / idle   "TAKE 007 · 48k/16 WAV · 12 h 40 left"          box "WAV 48k" / "12 h 40 left"
 *   playing            "PLAYING TAKE 006 · 05:44 / 12:08"               box "WAV 48k" / "05:44 / 12:08"
 *   paused             "PAUSED TAKE 006 · 05:44 / 12:08"                (the strip only; the box is the same)
 *   scanning           "PLAYING TAKE 006 · 05:44 / 12:08 · REW 5x"      (FWD 5x forward)
 * Empty parts are left out with their separator. A notice replaces the line (and the box's detail). */
std::string info_line(const RecorderVm &rec);
/* The box's first line: the codec and rate of the take being recorded, idle the Settings choice ("WAV 48k", "FLAC 44.1k",
 * "OPUS 48k · 32 kbps": Opus adds its bit rate); while a take plays, the codec and rate of the take in the deck (no bit
 * rate: it is not stored with a take). Never the take's name. */
std::string info_codec(const RecorderVm &rec);
/* The box's second line: what the strip shows after the take and format: the time left ("12 h 40 left"), the position over
 * the length while playing, the lost audio, the notice. */
std::string info_detail(const RecorderVm &rec);
/* The largest size in [min_px, max_px] at which width_at(px) is at most max_w (the font ladder walked from the top: the text
 * stays one line); min_px when none fits. */
int largest_fit_px(int max_px, int min_px, int max_w, const std::function<int(int)> &width_at);
/* A cassette-label name set in at most max_lines lines no wider than max_w (width = the text's width in px at the font in
 * use). One line when it fits; else split at a space, the split that makes the longer line shortest; a word too wide for a
 * line is broken inside the word. fits = false: it does not fit (the last line then ends with "..." and fits). The spaces at
 * a split are not in the lines. */
struct NameWrap {
    std::vector<std::string> lines;
    bool fits = true;
};
NameWrap wrap_name(const std::string &name, int max_w, int max_lines, const std::function<int(const std::string &)> &width);
/* The label's name: the largest rung (rungs largest first, none above wanted) in which the name fits the writing area
 * (max_w x max_h px) in at most two lines of that rung (line pitch = rung + 1, at most the font's line height); the smallest
 * rung (lines then ending with "...") when none does. */
struct NameLayout {
    int rung = 0;
    NameWrap wrap;
    int pitch = 0;  /* px between the lines' tops */
    int text_h = 0; /* the whole text's height: (lines - 1) * pitch + the font's line height */
};
NameLayout layout_name_text(const std::string &name, const std::vector<int> &rungs, int wanted, int max_w, int max_h,
                            const std::function<int(int, const std::string &)> &width, const std::function<int(int)> &line_h);
/* The state lamp at the strip's right end: "REC" (recording, paused, countdown), "PLAY" (playing, scanning), "" none. */
std::string info_state(const RecorderVm &rec);
/* The lost-audio part of the strip: "lost 0.3 s" (one decimal; "lost <0.1 s" under 50 ms) while a take records and audio was
 * lost, else "". The strip shows it last, in red; the portrait box shows it alone as its detail.
 * Server marks: recording through the sound server "TAKE 007 · 48k/16 WAV (server) · 12 h 40 left"; idle through the server
 * "CARD LABEL (server) · TAKE 008 · 12 h 40 left" (no format: the line is short); with lost audio the time left gives way:
 * "TAKE 007 · 48k/16 WAV · lost 0.3 s"; the portrait detail "(server) 12 h 40". */
std::string info_lost(const RecorderVm &rec);

/* The recordings strip: landscape "12 TAKES · ~/Recordings" | "41 GB FREE · 117 h at 48k/16",
 * portrait "12 TAKES" | "41 GB FREE · 117 h". "1 TAKE" for one. */
std::string list_strip_left(const ListVm &l, bool narrow);
std::string list_strip_right(const ListVm &l, bool narrow);
/* The hint under the keys (landscape): "Take 006 selected. Esc returns to the recorder."; in merge mode "Choose the takes in
 * order, then MERGE. Esc cancels." */
std::string list_hint(const ListVm &l);

/* The trim page's strips:
 *   landscape top   "IN 02:58.7 · OUT 10:28.7 · LEN 07:30.0 · 2 splits"
 *   portrait top    "2 splits · LEN 7:30"
 *   portrait bottom "IN 2:59 · OUT 10:29" | "SPLIT 2 SELECTED" ("" when none is selected)
 * "1 split", "no splits". */
std::string splits_text(std::size_t n);
std::string trim_strip_wide(const TrimVm &t);
std::string trim_strip_narrow_top(const TrimVm &t);
std::string trim_strip_narrow_left(const TrimVm &t);
std::string trim_strip_narrow_right(const TrimVm &t);

/* The save box's line: "Keep 02:58.7 to 10:28.7, cut at 2 splits: 3 files." (narrow: "Keep 2:59 to 10:29, ...");
 * without a split "Keep 02:58.7 to 10:28.7: 1 file." */
std::string save_summary(const TrimVm &t, std::size_t files, bool narrow);
/* A file name without its ".wav" (the narrow save box). */
std::string strip_wav(const std::string &name);

/* "more below: REW / FWD, Channels, Low cut" for the labels of the rows under the visible part ("" for none). */
std::string more_below(const std::vector<std::string> &labels);
/* The same, with as many names as `fits` accepts, then ", ..." for the rest (at least "more below: ..."). */
std::string more_below_fit(const std::vector<std::string> &labels, const std::function<bool(const std::string &)> &fits);

} // namespace ui
} // namespace cassette
