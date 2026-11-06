/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the diagnostic log line (off unless $MMWAVE_LOG names a file). One line a second: link state, port,
 * speed, frame counters and the three slots as the tracker holds them, plus once the port settings and the raw bytes
 * of the first three frames that held a target. Pure text from a Link; nothing is ever sent to the sensor.
 */
#pragma once

#include "link.hpp"

#include <string>

namespace mmw {

/* The per-second line at `now` (ms, the app's clock). */
std::string diag_line(const Link &link, Ms now);
/* The extra lines to add when new (port settings once found, each of the first target frames once): `sent` counts
 * what was written before (bit 0 settings, bits 1-3 the frames); returns the lines and updates it. */
std::string diag_extra(const Link &link, unsigned &sent);

} // namespace mmw
