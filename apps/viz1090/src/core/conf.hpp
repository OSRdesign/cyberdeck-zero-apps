/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the settings file ~/.config/cardputerzero/viz1090.conf (path kept from 0.1 so an upgrade keeps the
 * user's position). run_viz1090.sh reads it with the shell (`. "$CONF"`), so every line the intro writes is a plain
 * NAME=VALUE with no space or quote.
 *
 *   LAT=48.85660  LON=2.35220   the position (written by the intro)
 *   AIRPORT=CDG                 the airport the position came from (only when it came from one)
 *   RECENT1=CDG,49.00970,2.54780 ... RECENT5=,48.85660,2.35220
 *                               the last places used, most recent first (code may be empty), for the Recent pick list
 *   UISCALE=2  METRIC=1  RADIUS=500 and anything else
 *                               kept as they are, in their order (the user's own lines)
 *
 * Writing is atomic (a temporary file in the same folder, then rename). Pure C++, no LVGL: unit-tested.
 */
#pragma once

#include "place.hpp"

#include <string>
#include <vector>

namespace viz {

struct Conf {
    std::string lat, lon, airport; /* as found in the file ("" when absent) */
    std::vector<Place> recent;     /* RECENT1..RECENT5 that parse, in order */
};

/* $HOME/.config/cardputerzero/viz1090.conf, or the same under $XDG_CONFIG_HOME when it is set */
std::string conf_path();

Conf parse_conf(const std::string &text);

/* The new file: LAT, LON, AIRPORT (when place has a code), every other line of `old_text` as it was (empty lines
 * dropped, as 0.1 did), then RECENT1.. of `recent`. */
std::string render_conf(const std::string &old_text, const Place &place, const std::vector<Place> &recent);

/* "CDG,49.00970,2.54780" and back (false when the text is not one) */
std::string recent_value(const Place &place);
bool parse_recent(const std::string &value, Place &out);

/* The file's text ("" when missing). */
std::string read_text(const std::string &path);
/* Writes text atomically, making the folder (and its parent) first. "" on success, else the reason. */
std::string write_text_atomic(const std::string &path, const std::string &text);

/* load + render + write: "" on success, else the reason ("Cannot write ...: Permission denied"). */
std::string save_conf(const std::string &path, const Place &place, const std::vector<Place> &recent);

} // namespace viz
