/*
 * SPDX-License-Identifier: MIT
 *
 * The position of the node (D6, the Position box): the latitude and longitude the user types, checked before anything is sent with
 * SET_ADVERT_LATLON, how a stored position is put back into the fields, and whether the box offers the board GPS switch. The board
 * keeps the position and puts it into its adverts (when its advert location policy shares it): setting it sends no advert. No LVGL, no I/O.
 */

#pragma once

#include "features.hpp"

#include <string>

namespace meshzero {

/* One typed coordinate in decimal degrees: an optional sign, digits, '.' or ',' as the decimal separator; spaces around are ignored.
 * latitude: -90..90, else longitude: -180..180. Returns "" and sets `out` when valid, else the reason for the box. */
std::string parse_coordinate(const std::string &text, bool latitude, double &out);
/* Both fields: the first reason, "" when both are valid (lat / lon set only then). */
std::string parse_position(const std::string &lat_text, const std::string &lon_text, double &lat, double &lon);
/* The range check of an already parsed pair (the client refuses to send anything else). "" when valid. */
std::string check_position(double lat, double lon);
/* The firmware and the app read 0, 0 as "no position". */
inline bool position_is_set(double lat, double lon) { return !(lat == 0 && lon == 0); }
/* A coordinate for a text field: up to 6 decimals (the microdegrees of the wire), no trailing zeros: "48.8566", "-0.5", "2". */
std::string fmt_coordinate(double v);

/* The Position box shows the board GPS switch only for a board that lists the custom variable "gps" (BoardGps available); a board
 * without GPS, or a firmware without custom variables, shows none. */
bool position_box_has_gps(const DeviceInfo *dev, const BoardCaps &caps);

} // namespace meshzero
