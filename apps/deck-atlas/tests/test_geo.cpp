/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the map geometry of the pages: Web Mercator both ways (the fixture's tile corners), the view's
 * ground box at the display scales, the first view (a file's bounds, the world) and "No map here".
 */
#include "check.hpp"
#include "geo.hpp"

#include <cmath>

using namespace atlas;

void test_geo()
{
    /* the north-west corner of the fixture's z15 tile 16591/11269 (the centre of its 2x2 block) */
    double lat = 0, lon = 0;
    from_world_px(16591.0 * 256, 11269.0 * 256, 15, lat, lon);
    CHECK_NEAR(lon, 2.274170, 1e-5);
    CHECK_NEAR(lat, 48.886392, 1e-5);
    double x = 0, y = 0;
    to_world_px(lat, lon, 15, x, y);
    CHECK_NEAR(x, 16591.0 * 256, 1e-4);
    CHECK_NEAR(y, 11269.0 * 256, 1e-4);
    to_world_px(0, 0, 0, x, y);
    CHECK_NEAR(x, 128, 1e-9);
    CHECK_NEAR(y, 128, 1e-9);
    to_world_px(89.9, -180, 1, x, y); /* clamped to the square world */
    CHECK_NEAR(x, 0, 1e-9);
    CHECK_NEAR(y, 0, 1e-6);

    /* the view's box: 512 x 512 px at 2x shows one z15 tile: exactly the tile's box when centred on its middle */
    double clat = 0, clon = 0;
    from_world_px(16590.5 * 256, 11268.5 * 256, 15, clat, clon);
    const Box b = view_box(clat, clon, 15, 512, 512, 2.0f);
    double nlat = 0, wlon = 0, slat = 0, elon = 0;
    from_world_px(16590.0 * 256, 11268.0 * 256, 15, nlat, wlon);
    from_world_px(16591.0 * 256, 11269.0 * 256, 15, slat, elon);
    CHECK_NEAR(b.west, wlon, 1e-9);
    CHECK_NEAR(b.east, elon, 1e-9);
    CHECK_NEAR(b.north, nlat, 1e-9);
    CHECK_NEAR(b.south, slat, 1e-9);
    const Box b1 = view_box(clat, clon, 15, 512, 512, 1.0f); /* 1x: twice as much ground */
    CHECK_NEAR(b1.east - b1.west, 2 * (b.east - b.west), 1e-9);

    /* intersects */
    const Box paris{2.2, 48.8, 2.4, 49.0}, lyon{4.7, 45.6, 5.0, 45.9};
    CHECK(box_intersects(paris, b));
    CHECK(!box_intersects(paris, lyon));
    CHECK(box_intersects(paris, Box{2.4, 49.0, 2.5, 49.1})); /* touching */

    /* the first view: one z15 tile (512 px at 2x) does not fit a 640x345 map at z15, it does at z14, centred on it */
    const Box fixture{wlon, slat, elon, nlat};
    Fit f = fit_box(fixture, 640, 345, 2.0f, 13, 15);
    CHECK_EQ(f.zoom, 14);
    CHECK_NEAR(f.lon, clon, 1e-9);
    CHECK_NEAR(f.lat, clat, 1e-9);
    const Box ile_de_france{1.4, 48.1, 3.6, 49.3};
    f = fit_box(ile_de_france, 640, 345, 2.0f, 1, 17);
    CHECK_EQ(f.zoom, 7);
    f = fit_box(ile_de_france, 480, 236, 1.5f, 1, 17);
    CHECK_EQ(f.zoom, 6);
    f = fit_box(ile_de_france, 320, 396, 1.5f, 10, 17); /* never under the file's first zoom */
    CHECK_EQ(f.zoom, 10);
    f = world_view(640, 345, 2.0f);
    CHECK_EQ(f.zoom, 1);
    CHECK_NEAR(f.lat, 20.0, 1e-9);
    f = world_view(480, 236, 1.5f);
    CHECK_EQ(f.zoom, 1);
    f = world_view(1280, 700, 1.0f);
    CHECK_EQ(f.zoom, 3);

    /* No map here */
    std::vector<Coverage> files;
    CHECK(!view_covered(files, b, 15));
    Coverage c;
    c.has_bounds = true;
    c.bounds = paris;
    c.minzoom = 1;
    c.maxzoom = 17;
    files.push_back(c);
    CHECK(view_covered(files, b, 15));
    CHECK(view_covered(files, b, 18));  /* one level of over-zoom */
    CHECK(!view_covered(files, b, 19));
    CHECK(!view_covered(files, lyon, 12));
    files[0].minzoom = 10;
    CHECK(!view_covered(files, b, 9));
    files[0].missing = true;            /* the stick is out: the widget says so itself */
    CHECK(!view_covered(files, b, 15));
    /* a converted folder: its bounds are the deepest level's extent, the shallower levels are not limited by them */
    Coverage conv;
    conv.has_bounds = true;
    conv.bounds = paris;
    conv.minzoom = 1;
    conv.maxzoom = 17;
    conv.bounds_deepest_only = true;
    std::vector<Coverage> folder = {conv};
    CHECK(view_covered(folder, lyon, 12));
    CHECK(view_covered(folder, lyon, 16));
    CHECK(!view_covered(folder, lyon, 17));
    CHECK(!view_covered(folder, lyon, 18));
    CHECK(view_covered(folder, b, 17));
    CHECK(bounds_deepest_only(true, false));   /* by its id, bounds of unknown reach: the deepest level only */
    CHECK(!bounds_deepest_only(true, true));   /* by its id, a record with every zoom's rectangle: the union holds */
    CHECK(!bounds_deepest_only(false, false)); /* the every-archive lookup applies the same bounds: the plate follows */
    CHECK(!bounds_deepest_only(false, true));
    Coverage nob;                       /* a file without bounds covers everything */
    files.push_back(nob);
    CHECK(view_covered(files, lyon, 5));
}
