/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the user's settings and where the app keeps its files. Pure text in and out (no LVGL, no map engine);
 * the file operations are plain POSIX and never throw.
 *
 * settings.conf (key=value lines, the launcher's state-file style; unknown keys and bad values keep the defaults):
 *   units=km|mi             the scale bar
 *   scale=auto|1x|1.5x|2x   the map's display scale (auto: the engine's choice from the panel density)
 *   cache=auto|128|256|512|1024   the viewing cache cap in MB (auto: the smaller of 512 MB and 10 % of free space)
 *   offline=on|off          no tile request at all (the engine's network switch)
 *   source=<id>             what the map shows: "" (absent) = every imported map file, else an archive or source id
 *   view=<lat>,<lon>,<zoom> the last view (absent before the first run ends)
 *   terms=<id>,<id>         the online sources whose terms the user accepted (asked once per source)
 * API keys are never in this file: the engine keeps them in its store, mode 0600.
 *
 * The folder is $DECKATLAS_CONFIG_DIR when set (tests), else $XDG_CONFIG_HOME/deck-atlas, else ~/.config/deck-atlas.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atlas {

enum class Units { Km, Mi };

struct View {
    bool set = false;
    double lat = 0, lon = 0;
    int zoom = 0;
};

struct Settings {
    Units units = Units::Km;
    float scale = 0.0f;                 /* 0 = Auto, else 1, 1.5 or 2 */
    int cache_mb = 0;                   /* 0 = Auto, else 128, 256, 512 or 1024 */
    bool offline = false;
    std::string source;                 /* "" = every map file */
    View view;
    std::vector<std::string> terms;     /* accepted, sorted, unique */
};

std::string serialize_settings(const Settings &s);
Settings parse_settings(const std::string &text);

/* The choices of each setting, in the order a tap or Enter cycles them. */
Units next_units(Units u);
float next_scale(float s);              /* Auto -> 1x -> 1.5x -> 2x -> Auto */
int next_cache_mb(int mb);              /* Auto -> 128 -> 256 -> 512 -> 1024 -> Auto */
bool valid_cache_mb(int mb);
float snap_scale(float s);              /* 0 stays 0 (Auto); anything else to the nearest of 1, 1.5, 2 */

/* Terms: accepted once per source id (switching a source off and on again does not ask again). */
bool terms_accepted(const Settings &s, const std::string &id);
void accept_terms(Settings &s, const std::string &id);

/* A source / archive id as the engine spells them: [a-z0-9._-], 1..63 characters. */
bool valid_id(const std::string &id);

/* The folder of the app's files (see above; "" when no home is known). */
std::string config_dir();
/* mkdir -p with mode 0700. */
bool make_dirs(const std::string &path);
/* The whole file, at most max_bytes (false when missing or larger). */
bool read_small_file(const std::string &path, std::string &out, std::size_t max_bytes = 64 * 1024);
/* Temp file (mode 0600), fsync, rename, fsync of the folder. */
bool write_file_atomic(const std::string &path, const std::string &data);

} // namespace atlas
