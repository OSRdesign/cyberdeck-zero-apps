/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the words and numbers the rows show (no LVGL; tests/test_format.cpp).
 *
 * The built-in fonts of the toolkit are ASCII, so a degree sign is left out ("21.5 C", as the design writes it).
 * "unavailable" and "unknown" read as grey words. Binary sensors take a word from their device class (door: Open /
 * Closed, motion: Detected / Clear ...). No status is told by colour alone: the words carry it.
 */
#pragma once

#include "model.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ha {

/* "°C" -> "C", "°F" -> "F", any other non-ASCII byte dropped (the fonts are ASCII). */
std::string clean_unit(const std::string &unit);
/* A sensor value: a number rounded to `precision` decimals when precision >= 0, else the text as Home Assistant sent it. */
std::string format_number(const std::string &raw, int precision);
/* A temperature with its unit ("21.5 C"); "" for NAN. keep_degree: "21.5 °C" (the entity page's face, FreeType
 * Montserrat, has the degree sign; the lists keep ASCII). */
std::string format_temperature(double t, const std::string &unit, bool keep_degree = false);

bool unavailable(const State &s); /* "unavailable" or "unknown" or empty */
bool is_on(Domain d, const State &s);
/* A cover whose class asks for a confirmation (garage, gate, door). */
bool confirm_cover(const State &s);
int brightness_pct(const State &s); /* 0..100; 0 when off or unknown */

/* The value of the row: "On", "21.5 C", "Open", "Locked", "Heat, 21.5 C", "" for a scene. temp_unit: Home Assistant's
 * temperature unit (get_config), used by climate. precision: the registry's display precision (-1 none). */
std::string state_label(Domain d, const State &s, int precision, const std::string &temp_unit);

/* The read-only rows of the entity page (key, value), after the state row. */
std::vector<std::pair<std::string, std::string>> detail_rows(Domain d, const State &s, const std::string &temp_unit,
                                                             const std::string &room, std::int64_t now_epoch);

/* The confirmation question of a lock or a garage-type cover: "Unlock Front door?". service: what will be called
 * ("unlock", "lock", "open_cover", "close_cover"). */
std::string confirm_question(const std::string &service, const std::string &name);

/* "Last changed" text: "19:58" today (within 24 h), else "2026-10-30". */
std::string changed_text(double epoch, std::int64_t now_epoch);

} // namespace ha
