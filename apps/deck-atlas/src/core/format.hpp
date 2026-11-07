/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the texts the pages show (sizes, counts, zoom ranges, settings values, coordinates). Pure functions, no
 * LVGL, no map engine.
 */
#pragma once

#include "settings.hpp"

#include <cstdint>
#include <string>

namespace atlas {

/* 1024-based: "0 B", "820 B", "12 KB", "3.4 MB", "276 MB", "1.2 GB". */
std::string bytes_label(std::uint64_t bytes);
/* Thousands with commas: "10,326". */
std::string count_label(std::uint64_t n);
/* "z1-17", "z14" (one level), "" when unknown (a bound < 0). */
std::string zoom_label(int zmin, int zmax);

/* The Settings values. */
const char *units_label(Units u);                   /* "km" / "mi" */
/* setting 0 (Auto) -> "Auto (1.5x)" with the scale in use (0: "Auto"); else "1x", "1.5x", "2x". */
std::string scale_label(float setting, float in_use);
/* mb 0 -> "Auto (512 MB)" with the cap in force (0: "Auto"); else "128 MB", "256 MB", "512 MB", "1 GB". */
std::string cache_label(int mb, std::uint64_t cap_bytes);
const char *on_off(bool on);                        /* "On" / "Off" */

/* "48.916 N, 2.274 E" (3 decimals). */
std::string latlon_label(double lat, double lon);
/* "48.80-48.99 N, 2.10-2.45 E"; across the equator / meridian "1.20 S-0.50 N". */
std::string bounds_label(double west, double south, double east, double north);

/* At most max_chars characters (UTF-8 aware): the start and the end kept, "..." in the middle ("/media/deck/.../a.mbtiles"). */
std::string ellipsize_middle(const std::string &s, int max_chars);
/* UTF-8 characters in s. */
int utf8_chars(const std::string &s);

/* "812 tiles/s" (0 decimals from 10 up, 1 below); bytes: "35 MB/s". */
std::string tile_rate_label(double per_s);
std::string byte_rate_label(double per_s);
/* "about 12 s", "about 3 min", "about 1 h 5 min"; "" for a negative or unknown time. */
std::string eta_label(double seconds);
/* "2026-11-07" from the record's "2026-11-07T10:20:30Z" (the text unchanged when it is not of that shape). */
std::string date_label(const std::string &iso);

} // namespace atlas
