/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the map credit and the scale bar at the very bottom edge of the map view. The engine's widget draws both
 * itself (plates in the toolkit's caption font, the credit cut with "..." when it does not fit) and the app cannot
 * change that. So the app gives the widget a taller box than the view shows (engine_reserve(): the widget's bottom rows
 * lie below the view's edge, clipped by the map's parent) and draws its own bottom row in the smallest font the toolkit
 * has (CP0_UI_FONT_MIN_PX): the scale bar at the left, the credit at the right, flush with the bottom edge, one baseline.
 *
 * plan_credit() lays that row out: the credit whole on one line when it fits beside the widest scale bar; else the
 * standard abbreviation ("OpenStreetMap" -> "OSM") on one line; else the credit (then the abbreviation) wrapped to two
 * lines; else as many lines as it takes. The credit is never cut and never dropped. The plates never overlap each other.
 * engine_reserve() and map_metrics() repeat the engine's layout rules (cp0_map_view.c cp0_map_layout: the plate, margin
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
/* Greedy word wrap into as many lines of at most max_w px as it takes; nothing is cut (a word wider than a line is split
 * between characters). Empty text: no lines. */
std::vector<std::string> wrap_words(const std::string &text, int max_w, const MeasureFn &measure);

struct AttrInput {
    int w = 0, h = 0;      /* the map widget's size */
    int target = 44;       /* the TARGET token */
    int space = 8;         /* the SPACE_M token */
    int text_h = 12;       /* the caption font's line height (the engine's) */
};

/* The numbers the engine derives from the tokens for its overlays (cp0_map_layout): the app lays its own overlays (the
 * GPS chip, the follow button) out with the same ones. */
struct MapMetrics {
    int margin = 0;     /* the gap to the widget's edge */
    int pad = 0;        /* a plate's inner padding */
    int plate = 0;      /* a button's drawn square (the zoom buttons') */
    int target = 0;     /* the touch square, at most a third of the shorter side */
    int text_h = 0;
    int row_h = 0;      /* one row of text on a plate: text_h + 2 pad */
    int sb_w = 0, sb_h = 0, sb_y = 0;   /* the scale bar's plate (x = margin) */
};
MapMetrics map_metrics(const AttrInput &in);

/* How many pixels taller than the visible view (in.h) the widget must be so that the engine's own scale bar and credit
 * (the credit as the engine measures it: engine_credit_w px in its caption font, 0 = none) lie wholly below the view's
 * bottom edge. in.h is the VISIBLE height; the metrics are those of the taller widget (the plate sizes depend on the
 * shorter side). */
int engine_reserve(const AttrInput &visible, int engine_credit_w);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

struct CreditInput {
    int w = 0, h = 0;      /* the VISIBLE map view */
    int right = 0;         /* the credit's right edge: 0 = the view's (flush right); less when the zoom buttons come down that far */
    int text_h = 9;        /* line height of the smallest font */
    int pad = 4;           /* MapMetrics::pad (the toolkit's) */
};

struct CreditLayout {
    int hp = 0, vp = 0;            /* a plate's horizontal and vertical padding */
    int row_h = 0;                 /* one text row on a plate: text_h + 2 vp */
    int sb_bar_max = 0;            /* the longest scale bar the row has room for, px */
    int sb_label_w = 0;            /* the widest scale bar label ("5000 km"), px */
    Rect scale;                    /* the widest scale bar plate: left, flush with the bottom edge, one row high */
    std::vector<std::string> lines; /* the credit, top line first; empty: no credit */
    bool abbreviated = false;      /* "OSM" used */
    bool wrapped = false;          /* more than one line */
    Rect credit;                   /* the credit's plate: right (at CreditInput::right), flush with the bottom edge; empty: no credit */
    int text_y = 0;                /* top of the last text line: the scale bar's text sits at the same y (one baseline) */
};

CreditLayout plan_credit(const CreditInput &in, const std::string &text, const MeasureFn &measure);
/* The scale bar's plate for a bar of bar_px and a label label_w px wide: starts at x 0, same row as the credit's last line. */
Rect scale_plate(const CreditLayout &l, int bar_px, int label_w);

} // namespace atlas
