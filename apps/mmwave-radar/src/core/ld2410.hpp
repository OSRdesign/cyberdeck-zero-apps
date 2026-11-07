/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the LD2410 STUB driver. The Hi-Link LD2410 presence radar sends
 * F4 F3 F2 F1 len(2) data F8 F7 F6 F5; the app recognises it (so a scan can say what the device is and leave it
 * alone) but does not read it yet: no decoder, no speeds of its own.
 */
#pragma once

#include "driver.hpp"

namespace mmw {

/* True when a complete LD2410 data frame starts at p (avail bytes from there on): header, length 4..100, data type 01
 * (engineering) or 02 (basic), the head byte AA right after it, the tail 55 00 and the footer. The LD2451 uses the same
 * header and footer (ld2451.hpp), but its data starts with a target count and an alarm byte (0 or 1), never AA, and its
 * length is 0 or 2 + 5n: so no frame is both. */
bool ld2410_frame_at(const std::uint8_t *p, std::size_t avail);

extern const SensorDriver kDriverLd2410;

} // namespace mmw
