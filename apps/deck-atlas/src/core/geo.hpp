/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the little map geometry the pages need on top of the engine's widget (Web Mercator, 256 px tiles at a
 * display scale): where the first view goes (a file's bounds, or the world), which ground box the map shows, and
 * whether an imported file covers it ("No map here"). Pure, no LVGL, no engine.
 */
#pragma once

#include <string>
#include <vector>

namespace atlas {

struct Box {
    double west = 0, south = 0, east = 0, north = 0;
};

struct Fit {
    double lat = 0, lon = 0;
    int zoom = 0;
};

constexpr double kMaxLat = 85.05112877980659;

/* lat/lon <-> world pixels of 256 px tiles at zoom (y from the north). */
void to_world_px(double lat, double lon, int zoom, double &x, double &y);
void from_world_px(double x, double y, int zoom, double &lat, double &lon);

/* The point dy_px screen pixels below (dy > 0) or above lat/lon at zoom and display scale: the view's centre is the
 * widget's, the visible centre of a map that is taller than its view lies half the extra height above it. */
void shift_down_px(double lat, double lon, int zoom, float scale, double dy_px, double &out_lat, double &out_lon);

/* The ground box a w x h px view shows centred on lat/lon at zoom and display scale (1, 1.5, 2). */
Box view_box(double lat, double lon, int zoom, int w_px, int h_px, float scale);
bool box_intersects(const Box &a, const Box &b);

/* The deepest zoom in zmin..zmax at which the whole box fits the view, centred on the box. */
Fit fit_box(const Box &b, int w_px, int h_px, float scale, int zmin, int zmax);
/* The world view of the first run with no map: the smallest zoom whose world fills the view's width. */
Fit world_view(int w_px, int h_px, float scale);

/* One imported file as the coverage test needs it. */
struct Coverage {
    bool has_bounds = false;
    Box bounds;
    int minzoom = -1, maxzoom = -1;
    bool missing = false;                /* an in-place file whose stick is out */
    /* the bounds hold only at the deepest level (and the over-zoom one); see bounds_deepest_only() */
    bool bounds_deepest_only = false;
};
/* Whether a file's bounds can be trusted at its deepest level only. A file read by its own id (shown by itself) is
 * read at every level whatever its bounds say (a copied MBTiles made elsewhere may declare as little), so its bounds
 * count only at the deepest level, unless its record keeps the tile rectangle of every zoom (has_zoom_extents: a
 * folder converted by the engine since 2026-11-07, whose bounds are the union of all levels). The every-archive
 * lookup tests the declared bounds at every level itself, so the plate follows it: false. */
bool bounds_deepest_only(bool read_by_id, bool has_zoom_extents);
/* Some file has tiles in the view at this zoom (its bounds meet the view, the zoom within its levels plus the one
 * level of over-zoom the widget allows). A file without bounds, or whose bounds do not hold at this zoom, counts as
 * covering everything. */
bool view_covered(const std::vector<Coverage> &files, const Box &view, int zoom);

} // namespace atlas
