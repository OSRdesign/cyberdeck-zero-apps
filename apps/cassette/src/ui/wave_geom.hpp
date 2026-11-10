/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the geometry of the trim page's waveform (pure, no LVGL; tests/ui/test_wave.cpp).
 *
 * The main waveform shows a window of the take, span_ms wide, centred on the red play marker (the marker never moves on
 * the screen; the take scrolls under it). The window maps time to x linearly over [x, x + w): the window's start at x,
 * its end at x + w, the marker at x + w / 2. A point outside the window (an IN / OUT handle, a split flag) is drawn
 * as a chip at that edge. The overview strip maps the whole take onto its width the same way.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {
namespace ui {

struct WaveWindow {
    std::int64_t start_ms = 0; /* may be negative, or end past the take, near its ends */
    std::int64_t span_ms = 1;
    double x = 0, w = 1;

    double x_of(std::int64_t t) const;
    std::int64_t t_of(double px) const;
    bool contains(std::int64_t t) const { return t >= start_ms && t <= start_ms + span_ms; }
};

/* The window of span_ms (at least 1) centred on marker_ms, over [x, x + w). */
WaveWindow wave_window(std::int64_t marker_ms, std::int64_t span_ms, double x, double w);

/* Where a point of the take is drawn: inside the window, or as a chip at the left / right edge. */
enum class WaveEdge { Inside, Left, Right };
WaveEdge wave_edge(const WaveWindow &win, std::int64_t t);

/* The overview strip: the whole take (length_ms, at least 1) over [x, x + w). */
double overview_x(std::int64_t t, std::int64_t length_ms, double x, double w);

/* The bars that fit in w px at pitch px per bar (bar + gap), at least 0. */
int bar_count(double w, double pitch);

/* Peaks resampled to n bars: each bar is the maximum of the input values its bucket covers (at least one value), so a
 * short loud click is never lost; an empty input gives n zeros. Values are clamped to 0..1. */
std::vector<float> resample_max(const std::vector<float> &in, int n);

/* The waveform's vertical scale (trim page chip: TRUE or FIT). TRUE draws the real level (0..1 of full scale). FIT multiplies every
 * bar, in the main waveform and the overview alike, by ONE gain per take: the loudest peak of the whole take then reaches
 * kFitTarget (90 %) of the panel height. The gain never changes with zoom or scroll. Gain 0 means "no gain" (a take whose peak
 * is under kFitSilent, -60 dBFS: a silent take is not inflated); the gain is capped at kFitMaxGain (x64). */
constexpr float kFitTarget = 0.9f, kFitSilent = 0.001f, kFitMaxGain = 64.0f;
float fit_gain_for(float take_peak);
/* level * gain clamped to 0..1; a gain of 0 (none) or 1 leaves the level as is */
float fit_level(float level, float gain);
/* The chip's text: "TRUE"; FIT with its gain "FIT x8" (rounded to a whole number); FIT without a gain (gain 0) "FIT" */
std::string wave_chip_text(bool fit, float gain);

/* The IN / OUT tags above the main waveform (screen px). A tag's x: an edge tag (handle outside the window) sits at its
 * edge, the IN tag at left + inset, the OUT tag right-aligned at right; an inside IN tag starts at its line, an inside OUT
 * tag ends at its line. Either way the tag is kept within [left, right]. */
struct TagSpan {
    int x1 = 0, x2 = 0; /* [x1, x2) */
    bool overlaps(const TagSpan &o, int gap) const { return x1 < o.x2 + gap && o.x1 < x2 + gap; }
};
TagSpan tag_span(WaveEdge edge, bool is_out, int line_x, int w, int left, int right, int inset);
/* The tags' rows (0 = the top row, each row one tag high below the last): the mode chip (row 0, right corner) owns its
 * place, a tag that would touch it (within gap) drops a row; the OUT tag also drops below the IN tag when the two would
 * touch. A tag never goes below max_rows - 1 (the rows that fit in the panel; at least 1). The tags do not avoid the
 * vertical lines (IN / OUT lines, the play marker, split lines): they are drawn above them (kWaveDrawOrder). */
struct TagRows {
    int in = 0, out = 0;
};
TagRows tag_rows(const TagSpan &in, const TagSpan &out, const TagSpan &chip, int gap, int max_rows);

/* The order the trim page draws its waveform in, back to front: the tags and the mode chip (opaque, each with an outline
 * in the panel colour) cover every vertical line, so a line stops short of a tag and never strikes its text; the split
 * flag boxes come last. */
enum class WaveLayer { Overview, Bars, HandleLines, SplitLines, PlayMarker, Tags, ModeChip, FlagBoxes, Ruler };
constexpr WaveLayer kWaveDrawOrder[] = {WaveLayer::Overview,   WaveLayer::Bars, WaveLayer::HandleLines,
                                        WaveLayer::SplitLines, WaveLayer::PlayMarker, WaveLayer::Tags,
                                        WaveLayer::ModeChip,   WaveLayer::FlagBoxes, WaveLayer::Ruler};

/* The split flag a tap at x hits: the nearest flag inside the window within slop_px, -1 for none. */
int flag_at(const WaveWindow &win, const std::vector<std::int64_t> &splits, double x, double slop_px);

/* The marker a sideways drag leaves: the marker at the drag's start moved by the time the finger travelled (dragging
 * the take to the left moves later), clamped to 0..length_ms. */
std::int64_t scrub_marker(const WaveWindow &win, std::int64_t marker_at_start, double dx_px, std::int64_t length_ms);

/* One POSITION step: 100 ms, or 1 s with Shift (the trim page's Left / Right and its POSITION buttons). */
std::int64_t nudge_step_ms(bool shift);

/* The time ruler under the waveform: n ticks evenly over the window (its start, ..., its end; n >= 2). */
struct RulerTick {
    double x;
    std::int64_t t;
};
std::vector<RulerTick> ruler_ticks(const WaveWindow &win, int n);

} // namespace ui
} // namespace cassette
