/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: map geometry (see geo.hpp).
 */
#include "geo.hpp"

#include <algorithm>
#include <cmath>

namespace atlas {

namespace {
constexpr double kPi = 3.14159265358979323846;
double world_size(int zoom) { return 256.0 * std::ldexp(1.0, std::clamp(zoom, 0, 24)); }
} // namespace

void to_world_px(double lat, double lon, int zoom, double &x, double &y)
{
    const double size = world_size(zoom);
    const double la = std::clamp(lat, -kMaxLat, kMaxLat) * kPi / 180.0;
    x = (lon + 180.0) / 360.0 * size;
    y = (1.0 - std::log(std::tan(la) + 1.0 / std::cos(la)) / kPi) / 2.0 * size;
}

void from_world_px(double x, double y, int zoom, double &lat, double &lon)
{
    const double size = world_size(zoom);
    lon = x / size * 360.0 - 180.0;
    const double n = kPi * (1.0 - 2.0 * std::clamp(y, 0.0, size) / size);
    lat = std::atan(std::sinh(n)) * 180.0 / kPi;
}

Box view_box(double lat, double lon, int zoom, int w_px, int h_px, float scale)
{
    const double s = scale > 0 ? scale : 1.0;
    double cx, cy;
    to_world_px(lat, lon, zoom, cx, cy);
    const double hw = w_px / s / 2.0, hh = h_px / s / 2.0;
    Box b;
    from_world_px(cx - hw, cy + hh, zoom, b.south, b.west);
    from_world_px(cx + hw, cy - hh, zoom, b.north, b.east);
    b.west = std::max(b.west, -180.0);
    b.east = std::min(b.east, 180.0);
    return b;
}

bool box_intersects(const Box &a, const Box &b)
{
    return a.west <= b.east && b.west <= a.east && a.south <= b.north && b.south <= a.north;
}

Fit fit_box(const Box &b, int w_px, int h_px, float scale, int zmin, int zmax)
{
    Fit f;
    const double s = scale > 0 ? scale : 1.0;
    zmin = std::clamp(zmin, 0, 24);
    zmax = std::clamp(std::max(zmax, zmin), 0, 24);
    /* the centre in world pixels at zoom 0 (Mercator centre, not the mean of the latitudes) */
    double x0, y0, x1, y1;
    to_world_px(b.north, b.west, 0, x0, y0);
    to_world_px(b.south, b.east, 0, x1, y1);
    from_world_px((x0 + x1) / 2.0, (y0 + y1) / 2.0, 0, f.lat, f.lon);
    f.zoom = zmin;
    for (int z = zmax; z >= zmin; --z) {
        const double k = std::ldexp(1.0, z) * s;
        if ((x1 - x0) * k <= w_px && (y1 - y0) * k <= h_px) {
            f.zoom = z;
            break;
        }
    }
    return f;
}

Fit world_view(int w_px, int h_px, float scale)
{
    (void)h_px;
    const double s = scale > 0 ? scale : 1.0;
    Fit f;
    f.lat = 20.0;
    f.lon = 0.0;
    f.zoom = 0;
    while (f.zoom < 4 && 256.0 * std::ldexp(1.0, f.zoom) * s < w_px) ++f.zoom;
    return f;
}

bool bounds_deepest_only(bool read_by_id, bool has_zoom_extents) { return read_by_id && !has_zoom_extents; }

bool view_covered(const std::vector<Coverage> &files, const Box &view, int zoom)
{
    for (const Coverage &c : files) {
        if (c.missing) continue;
        if (c.minzoom >= 0 && zoom < c.minzoom) continue;
        if (c.maxzoom >= 0 && zoom > c.maxzoom + 1) continue;
        if (!c.has_bounds || (c.bounds_deepest_only && zoom < c.maxzoom) || box_intersects(c.bounds, view)) return true;
    }
    return false;
}

} // namespace atlas
