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

/* True when a complete LD2410 frame starts at p (avail bytes from there on): header, length 1..100, footer. */
bool ld2410_frame_at(const std::uint8_t *p, std::size_t avail);

extern const SensorDriver kDriverLd2410;

} // namespace mmw
