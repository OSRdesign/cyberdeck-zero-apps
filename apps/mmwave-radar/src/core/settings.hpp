/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the user's settings and where the app keeps its files. Pure text in and out (no LVGL); the file
 * operations are plain POSIX and never throw.
 *
 * Files (one folder, `config_dir()`): `settings.conf` (the five settings below, key=value lines) and `remembered.conf`
 * (the port identity and speed of the last radar, Remembered::serialize). Both are written atomically (a temp file,
 * fsync, rename). The folder is $MMWAVE_CONFIG_DIR when set (tests), else $XDG_CONFIG_HOME/mmwave-radar, else
 * ~/.config/mmwave-radar.
 */
#pragma once

#include "units.hpp"

#include <cstddef>
#include <string>

namespace mmw {

/* How the plot is drawn from the sensor's coordinates. Normal: the sensor at the bottom centre, its front up (the
 * owner's mounting: the sensor on the deck facing away from the screen). Mirror: left and right swapped. Rotate180:
 * the sensor at the top centre, its front down (the screen turned, or the sensor facing the user). */
enum class Orientation { Normal, Mirror, Rotate180 };

struct Settings {
    UnitMode units = UnitMode::MetresKmh;
    Orientation orientation = Orientation::Normal;
    int range_m = 0;      /* 0 = Auto, else 2, 4 or 6 */
    bool trails = true;
    bool hide_held = false;   /* do not draw or list held (stale) targets; the status line counts them */
};

/* "units=metres\norientation=normal\nrange=auto\ntrails=on\nhide_held=off\n" */
std::string serialize_settings(const Settings &s);
/* Tolerant: unknown keys and bad values keep the defaults. */
Settings parse_settings(const std::string &text);

/* What the Sensor page shows. */
const char *units_label(UnitMode u);              /* "Metres" / "Millimetres" */
const char *orientation_label(Orientation o);     /* "Normal" / "Mirror" / "Rotate 180" */
const char *orientation_name(Orientation o);      /* "normal" / "mirror" / "rotate180" (the file) */
std::string range_label(int range_m);             /* "Auto" / "2 m" / "4 m" / "6 m" */

/* The next value of each setting (a tap or Enter on its row cycles). */
UnitMode next_units(UnitMode u);
Orientation next_orientation(Orientation o);
int next_range_setting(int range_m);              /* Auto -> 2 -> 4 -> 6 -> Auto */

/* The folder of the app's files (see above; "" when no home is known). */
std::string config_dir();
/* mkdir -p with mode 0700. */
bool make_dirs(const std::string &path);
/* The whole file, at most max_bytes (false when missing or larger). */
bool read_small_file(const std::string &path, std::string &out, std::size_t max_bytes = 64 * 1024);
/* Temp file, fsync, rename, fsync of the folder. */
bool write_file_atomic(const std::string &path, const std::string &data);

} // namespace mmw
