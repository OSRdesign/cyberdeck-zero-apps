/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the diagnostic log line (off unless $MMWAVE_LOG names a file or the marker file <config dir>/diag
 * exists). One line a second: link state, port, speed, frame counters, the three slots as the tracker holds them, held=N (targets reported unchanged for 3 s) and
 * the empty / non-empty / identical frame counts, plus once the port settings and the raw bytes of the first three
 * frames that held a target, and every 10 s the raw bytes of the latest one. Pure text from a Link;
 * nothing is ever sent to the sensor.
 */
#pragma once

#include "link.hpp"

#include <cstddef>
#include <string>

namespace mmw {

/* The per-second line at `now` (ms, the app's clock). */
std::string diag_line(const Link &link, Ms now);
/* The extra lines to add when new (port settings once found, each of the first target frames once): `sent` counts
 * what was written before (bit 0 settings, bits 1-3 the frames); returns the lines and updates it. */
std::string diag_extra(const Link &link, unsigned &sent);
/* The frame counters to add to the per-second line (the Link counts them while link.set_diag_counting(true)):
 * " empty=N nonempty=N same_as_prev=N total_empty=N total_nonempty=N total_same_as_prev=N", the frames since the last
 * call and since start() (same_as_prev: non-empty frames whose 24 target bytes equal the previous non-empty frame's);
 * starts a new window. Every 10 s from start(), when a non-empty frame came in those 10 s, *hex_line (when given)
 * gets "last-target-frame <30 bytes hex>\n", the latest one; else it is left empty. */
std::string diag_frames(Link &link, Ms now, std::string *hex_line);

/* The marker log: kept to about this many bytes (cut at the start and whenever it grew to twice as much). */
constexpr std::size_t kDiagKeepBytes = 200 * 1024;

/* Where the log goes: env_log ($MMWAVE_LOG) when it is set and not empty; else <config_dir>/mmwave.log when the
 * marker file <config_dir>/diag exists (the launcher starts the app, so the owner cannot set a variable there); else ""
 * (off: no file is written). *from_marker (when given) says the marker turned it on. */
std::string diag_log_path(const char *env_log, const std::string &config_dir, bool *from_marker = nullptr);

/* Keeps a log bounded: when the regular file at path is larger than keep_bytes, only its last keep_bytes remain
 * (from the first whole line in them), written atomically. True when it was cut; a missing, small or non-regular file
 * is left alone. */
bool diag_trim(const std::string &path, std::size_t keep_bytes);

} // namespace mmw
