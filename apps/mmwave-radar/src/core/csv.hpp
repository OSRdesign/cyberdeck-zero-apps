/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: one decoded frame as a CSV row, the same columns as the study fixture ld2452-9600-walk-15s.csv:
 * frame, t_ms, targets, then for slots 1-3: x_mm, y_mm, dist_mm, angle_deg (1 decimal), speed_cms, res_mm (empty when
 * the slot is empty). LF line endings (rows end with "\n").
 */
#pragma once

#include "frame.hpp"

#include <cstdint>
#include <string>

namespace mmw {

std::string csv_header();
std::string csv_row(std::uint32_t frame_index, std::int64_t t_ms, const Frame &f);

} // namespace mmw
