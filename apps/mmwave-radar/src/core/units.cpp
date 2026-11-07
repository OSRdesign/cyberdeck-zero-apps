/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: unit conversion (see units.hpp).
 */
#include "units.hpp"

#include <cmath>
#include <cstdio>

namespace mmw {

double mm_to_m(int mm) { return mm / 1000.0; }
double cm_s_to_m_s(int cm_s) { return cm_s / 100.0; }
double cm_s_to_km_h(int cm_s) { return cm_s * 0.036; } /* 1 cm/s = 0.036 km/h */

std::string format_distance(int mm, UnitMode mode)
{
    char buf[32];
    if (mode == UnitMode::MmCms) std::snprintf(buf, sizeof(buf), "%d", mm);
    else std::snprintf(buf, sizeof(buf), "%.2f", mm_to_m(mm));
    return buf;
}

const char *distance_unit(UnitMode mode) { return mode == UnitMode::MmCms ? "mm" : "m"; }

std::string format_speed(int cm_s, UnitMode mode)
{
    char buf[32];
    if (mode == UnitMode::MmCms) {
        std::snprintf(buf, sizeof(buf), "%d", cm_s);
    } else {
        const double v = cm_s_to_km_h(cm_s);
        /* avoid "-0.0" for a value that rounds to zero */
        std::snprintf(buf, sizeof(buf), "%.1f", std::fabs(v) < 0.05 ? 0.0 : v);
    }
    return buf;
}

const char *speed_unit(UnitMode mode) { return mode == UnitMode::MmCms ? "cm/s" : "km/h"; }

std::string format_distance_unit(int mm, UnitMode mode)
{
    return format_distance(mm, mode) + " " + distance_unit(mode);
}

std::string format_speed_unit(int cm_s, UnitMode mode)
{
    return format_speed(cm_s, mode) + " " + speed_unit(mode);
}

const char *unit_mode_name(UnitMode mode) { return mode == UnitMode::MmCms ? "raw" : "metres"; }

UnitMode unit_mode_from_name(const std::string &name)
{
    return name == "raw" ? UnitMode::MmCms : UnitMode::MetresKmh;
}

} // namespace mmw
