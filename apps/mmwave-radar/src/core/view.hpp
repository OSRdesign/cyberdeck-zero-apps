/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the pure geometry of the screens (no LVGL), unit-tested on the PC: the plot range (Auto with
 * hysteresis, the +/- keys), where the sensor sits and how a point in mm lands on the screen for each orientation, the
 * speed arrow's length, where a target's label goes, and the graph's scales.
 */
#pragma once

#include "settings.hpp"

#include <cstdint>

namespace mmw {

constexpr int kRangeSteps[3] = {2, 4, 6};   /* metres */
constexpr int kAutoMarginMm = 500;          /* Auto: the farthest target + this must fit */
constexpr int kAutoShrinkMm = 300;          /* ... and the range shrinks only when it fits this much inside the next
                                               smaller step (no flapping at a boundary) */
constexpr double kFovDeg = 60.0;            /* azimuth +-60 deg (LD2450 and LD2452 manuals, report 056 section 2.2) */

/* The Auto range: grows at once to the smallest step that holds max_dist_mm + kAutoMarginMm, shrinks one step at a time
 * only when that need is kAutoShrinkMm inside the smaller step. Without a target (any = false) it keeps current_m. */
int auto_range(int current_m, int max_dist_mm, bool any);

/* The +/- keys: dir > 0 a bigger range. From a fixed range the next step (past 6 m or under 2 m: Auto); from Auto the
 * step next to the range shown (effective_m). Returns the new setting (0 = Auto). */
int step_range(int setting_m, int effective_m, int dir);

/* The plot: a rectangle of the screen, the sensor's pixel and the scale. */
struct PlotGeom {
    int x = 0, y = 0, w = 0, h = 0;   /* the plot's rectangle */
    int sx = 0, sy = 0;               /* the sensor */
    double px_per_mm = 0.0;
    int xsign = 1;                    /* +1: sensor x to the right */
    int ysign = -1;                   /* -1: sensor y (its front) up */
    int range_m = 4;
};

/* Fits the +-60 deg wedge of range_m metres in the rectangle (pad px kept free on the sides and at the far edge,
 * near_pad px between the sensor and its edge): Normal and Mirror put the sensor at the bottom centre, Rotate180 at
 * the top centre. */
PlotGeom plot_geom(int x, int y, int w, int h, int range_m, Orientation o, int pad, int near_pad);
/* The screen pixel of a point in sensor mm. */
void plot_point(const PlotGeom &g, int x_mm, int y_mm, int &px, int &py);
/* True when the pixel lies inside the plot's rectangle. */
bool plot_contains(const PlotGeom &g, int px, int py);

/* The speed arrow: 0 under 5 cm/s, else from 1.5 to 4 dot diameters, growing with |speed| up to 150 cm/s. */
int arrow_length(double speed_cm_s, int dot_px);

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool overlaps(const Box &o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
};

/* A target's label of lw x lh px beside its dot (radius r) at (px, py): right of it, else left when it would leave
 * the plot; moved below (else above) a label already placed when they overlap; always inside the plot. */
Box place_label(const PlotGeom &g, int px, int py, int r, int lw, int lh, const Box *taken, int n_taken);

/* A rounded-up scale for a graph axis: the smallest of 1, 2, 5 x 10^n at least v (v <= 0: 1). */
double nice_ceiling(double v);

} // namespace mmw
