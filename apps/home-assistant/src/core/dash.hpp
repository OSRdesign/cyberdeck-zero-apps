/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: what the stored dashboard's cards need besides the model (study 053 P2), no LVGL
 * (tests/test_lovelace.cpp):
 *
 *   masonry   columns = clamp(floor(width_mm / 50), 1, 4); the cards in order, each into the column that is the
 *             shortest so far (the leftmost of equals), as Home Assistant's masonry view places them
 *   toggles   the entities a row switch and an entities card's header toggle act on: light, switch, fan, input_boolean
 *             (never a lock or a cover: those ask first, one at a time)
 *   weather   weather/subscribe_forecast's events (daily): up to five days of day name, condition, high and low; the
 *             conditions' Material Design Icons names and words (the documented weather states)
 *   history   history/history_during_period (minimal_response, no_attributes): the numeric states, averaged into
 *             equal time buckets (a point an hour, six with detail 2, at most 100), empty buckets keep the last value
 *   words     the state word of any entity row ("On", "21.5 °C", "Home", "Partly cloudy" ...)
 */
#pragma once

#include "json.hpp"
#include "model.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ha {

/* clamp(floor(width_mm / 50), 1, 4), the width from px through the density (px per mm x 100) */
int masonry_columns(int width_px, int ppmm_x100);
/* The column of each card (heights in px, in order); ys (optional) gets each card's top, with `gap` between cards. */
std::vector<int> masonry_place(const std::vector<int> &heights, int columns, int gap, std::vector<int> *ys = nullptr);

/* light, switch, fan, input_boolean: a switch on the row, part of the header toggle */
bool toggle_domain(const std::string &entity_id);

struct ForecastDay {
    std::string day;       /* "Mon" */
    std::string condition; /* "partlycloudy" */
    double high = NAN, low = NAN;
};
/* A forecast event ({"type": "daily", "forecast": [...]}); day names in the zone utc_offset_s east of UTC. False when
 * it is not one. */
bool parse_forecast(const json::Value &event, long utc_offset_s, std::vector<ForecastDay> &out, std::size_t max = 5);
/* "mdi:weather-partly-cloudy" ("" for an unknown condition) */
std::string weather_icon(const std::string &condition);
/* "Partly cloudy" (an unknown condition: its words) */
std::string weather_word(const std::string &condition);

struct HistoryPoint {
    double t = 0; /* epoch seconds */
    double v = 0;
};
/* One entity's numeric states from history_during_period's result, in time order (other states skipped). */
std::vector<HistoryPoint> parse_history(const json::Value &result, const std::string &entity);
/* Averages into `buckets` equal slices of [start, end]; a slice without a point keeps the last value, NAN before the
 * first one. */
std::vector<double> downsample(const std::vector<HistoryPoint> &points, double start, double end, int buckets);
/* hours x (1, or 6 with detail 2), at least 2, at most 100 */
int graph_points(int hours, int detail);
/* The points scaled to 0..1000 between their least and greatest (-1 for NAN; all 500 when flat). */
std::vector<int> scale_points(const std::vector<double> &values);

/* "2026-11-02T10:00:00+00:00" for an epoch (UTC) */
std::string iso_utc(std::int64_t epoch);
/* ISO 8601 with Z or an offset (fractions allowed) -> epoch seconds; false when it is not one */
bool parse_iso(const std::string &text, double &epoch);

/* The word an entity row shows for any domain: the tiles' words for the phase-1 domains ("On, 72 %", "21.5 °C"),
 * "Home" / "Away" for a person, the condition of a weather entity, a number with its unit, else the state's words.
 * temp_unit as Home Assistant gives it ("°C"). "" without a state. */
std::string entity_word(const std::string &entity_id, const State *s, int precision, const std::string &temp_unit);
/* A temperature for a card: "14.2 °C", "14 °C" (the unit's degree sign kept). */
std::string card_temperature(double t, const std::string &unit);

} // namespace ha
