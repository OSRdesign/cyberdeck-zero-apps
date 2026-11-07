/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: follow mode (see follow.hpp).
 */
#include "follow.hpp"

#include "geo.hpp"

#include <cmath>

namespace atlas {

void FollowLogic::set(bool on, double lat, double lon)
{
    on_ = on;
    lat_ = lat;
    lon_ = lon;
}

bool FollowLogic::on_fix(double lat, double lon)
{
    if (!on_) return false;
    lat_ = lat;
    lon_ = lon;
    return true;
}

bool FollowLogic::on_view_change(double lat, double lon, int zoom, double scale)
{
    if (!on_) return false;
    double ax, ay, bx, by;
    to_world_px(lat, lon, zoom, ax, ay);
    to_world_px(lat_, lon_, zoom, bx, by);
    const double s = scale > 0 ? scale : 1.0;
    if (std::hypot(ax - bx, ay - by) * s <= kPanSlopPx) return false;
    on_ = false;
    return true;
}

} // namespace atlas
