/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: follow mode (core/follow.cpp): off by default, a fix moves the view only when on, the app's own
 * recentring is told from a pan by where the view ended up, a zoom keeps follow, a pan (or anything that moves the
 * centre more than a few pixels) ends it.
 */
#include "check.hpp"
#include "follow.hpp"
#include "geo.hpp"

using namespace atlas;

namespace {

/* the point `dx`, `dy` screen px east / south of lat, lon at the zoom and display scale */
void shifted(double lat, double lon, int zoom, double scale, double dx, double dy, double &out_lat, double &out_lon)
{
    double x, y;
    to_world_px(lat, lon, zoom, x, y);
    from_world_px(x + dx / scale, y + dy / scale, zoom, out_lat, out_lon);
}

} // namespace

void test_follow()
{
    const double lat = 48.889640, lon = 2.269995; /* an invented point inside the fixture's area */
    FollowLogic f;
    CHECK(!f.on());
    CHECK(!f.on_fix(lat, lon));
    CHECK(!f.on_view_change(lat + 1, lon, 15, 2.0)); /* off: a view change means nothing */
    CHECK(!f.on());

    /* switched on at the current centre; a fix says "move there" and the view is then expected there */
    f.set(true, lat, lon);
    CHECK(f.on());
    double lat2 = 0, lon2 = 0;
    shifted(lat, lon, 15, 2.0, 120, -40, lat2, lon2); /* the fix is 120 px east, 40 px north of the centre */
    CHECK(f.on_fix(lat2, lon2));
    /* the engine reports the view where the app put it: not a pan */
    CHECK(!f.on_view_change(lat2, lon2, 15, 2.0));
    CHECK(f.on());
    /* a zoom with the buttons or keys keeps the centre: not a pan, at any zoom */
    CHECK(!f.on_view_change(lat2, lon2, 16, 2.0));
    CHECK(!f.on_view_change(lat2, lon2, 14, 2.0));
    CHECK(f.on());
    /* a pixel or two of rounding is not a pan; five pixels are */
    double a, b;
    shifted(lat2, lon2, 15, 2.0, 2, 0, a, b);
    CHECK(!f.on_view_change(a, b, 15, 2.0));
    CHECK(f.on());
    shifted(lat2, lon2, 15, 2.0, 0, 5, a, b);
    CHECK(f.on_view_change(a, b, 15, 2.0)); /* the user dragged: follow just went off */
    CHECK(!f.on());
    /* off: more fixes do not move the view, more view changes mean nothing */
    CHECK(!f.on_fix(lat2, lon2));
    CHECK(!f.on_view_change(lat, lon, 15, 2.0));

    /* the same pixels count more at the display scale: 2 px at 1x is 2 px, but at 2x the world moved only one */
    f.set(true, lat, lon);
    shifted(lat, lon, 15, 1.0, 4, 0, a, b);
    CHECK(f.on_view_change(a, b, 15, 1.0));
    f.set(true, lat, lon);
    shifted(lat, lon, 15, 2.0, 4, 0, a, b);
    CHECK(f.on_view_change(a, b, 15, 2.0)); /* 4 screen px at 2x: still a pan */
    f.set(true, lat, lon);
    shifted(lat, lon, 15, 2.0, 2, 0, a, b);
    CHECK(!f.on_view_change(a, b, 15, 2.0));

    /* a pan seen at another zoom than the expectation: compared in screen px of the zoom reported */
    f.set(true, lat, lon);
    shifted(lat, lon, 17, 1.5, 30, 0, a, b);
    CHECK(f.on_view_change(a, b, 17, 1.5));

    /* tapping the button with a fix: switch on, then recentre at once (what the app does) */
    f.set(false, lat, lon);
    CHECK(!f.on());
    f.set(true, lat, lon);
    CHECK(f.on_fix(lat2, lon2)); /* "recentre now" */
    CHECK(!f.on_view_change(lat2, lon2, 15, 2.0));
    /* switched on with no fix yet: the view stays where it is; a pan ends it */
    f.set(true, lat, lon);
    CHECK(!f.on_view_change(lat, lon, 15, 2.0));
    shifted(lat, lon, 15, 2.0, 60, 0, a, b);
    CHECK(f.on_view_change(a, b, 15, 2.0));
    CHECK(!f.on());

    /* a fix that follows a fix: every one moves the expectation */
    f.set(true, lat, lon);
    for (int k = 1; k <= 5; ++k) {
        shifted(lat, lon, 15, 2.0, 40.0 * k, 0, a, b);
        CHECK(f.on_fix(a, b));
        CHECK(!f.on_view_change(a, b, 15, 2.0));
    }
    CHECK(f.on());
}
