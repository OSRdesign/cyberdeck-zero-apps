/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: unit conversion for the screen. The core keeps the sensor's own units (mm, cm/s); only this helper
 * converts. Two display modes (a setting): MetresKmh (m with 2 decimals, km/h with 1) and MmCms (the raw units, as the
 * table view shows them).
 */
#pragma once

#include <string>

namespace mmw {

enum class UnitMode { MetresKmh, MmCms };

double mm_to_m(int mm);
double cm_s_to_m_s(int cm_s);
double cm_s_to_km_h(int cm_s);

/* "2.34" / "2340" (no unit text) and the unit text "m" / "mm". */
std::string format_distance(int mm, UnitMode mode);
const char *distance_unit(UnitMode mode);
/* "-1.8" / "-51" and the unit text "km/h" / "cm/s". A sign only when negative. */
std::string format_speed(int cm_s, UnitMode mode);
const char *speed_unit(UnitMode mode);
/* value + space + unit. */
std::string format_distance_unit(int mm, UnitMode mode);
std::string format_speed_unit(int cm_s, UnitMode mode);

/* Persisted name: "metres" / "raw". Unknown text gives MetresKmh. */
const char *unit_mode_name(UnitMode mode);
UnitMode unit_mode_from_name(const std::string &name);

} // namespace mmw
