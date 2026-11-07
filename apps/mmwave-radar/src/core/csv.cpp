/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: CSV rows (see csv.hpp).
 */
#include "csv.hpp"

#include <cstdio>

namespace mmw {

std::string csv_header()
{
    std::string h = "frame,t_ms,targets";
    for (int s = 1; s <= 3; ++s) {
        const std::string p = ",s" + std::to_string(s) + "_";
        h += p + "x_mm" + p + "y_mm" + p + "dist_mm" + p + "angle_deg" + p + "speed_cms" + p + "res_mm";
    }
    return h + "\n";
}

std::string csv_row(std::uint32_t frame_index, std::int64_t t_ms, const Frame &f)
{
    std::string row = std::to_string(frame_index) + "," + std::to_string(t_ms) + "," + std::to_string(f.present_count());
    for (const Target &t : f.target) {
        if (!t.present) {
            row += ",,,,,,";
            continue;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), ",%d,%d,%d,%.1f,%d,%d", t.x_mm, t.y_mm, t.distance_mm, t.angle_deg,
                      t.speed_cm_s, t.resolution_mm);
        row += buf;
    }
    return row + "\n";
}

} // namespace mmw
