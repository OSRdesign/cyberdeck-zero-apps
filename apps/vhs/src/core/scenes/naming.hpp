/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: file names and the shared counter (brief section 8; Cassette's naming adapted). LVGL-free.
 *
 *   <prefix>_<NNN>_<when>[_p<N>].<ext>
 *   prefix  movies: Settings > Name prefix (default "scene", Cassette's prefix rule); photos: always "photo"
 *   NNN     decimal, zero-padded to at least 3 digits; ONE counter for movies and photos (scene_014 after photo_013)
 *   when    local YYYYMMDD-HHMMSS, or "nodate" while the clock is not synchronised (the boards have no RTC)
 *   _pN     auto-split part 2, 3 ... (part 1 has no suffix); movies only
 *   ext     mkv, mp4, mov (movies); jpg, jpeg (photos)
 * Parsed from the right, so a prefix may hold '_', '-' and inner spaces. A name outside the grammar is a foreign file:
 * listed, never renamed.
 *
 * The counter: <folder>/.counter holds the last number handed out (decimal), replaced atomically. A new number is
 * max(counter, the highest number in the folder's names and in .journal/ (recordings in progress)) + 1, so numbers never
 * repeat after deletes or a lost counter.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vhs {

class FileOps;

constexpr const char *kPhotoPrefix = "photo";
constexpr const char *kTimesyncMarker = "/run/systemd/timesync/synchronized";

enum class SceneKind { Movie, Photo };

struct SceneName {
    std::string prefix;
    int number = 0;
    std::string when;  /* "20261114-185200" or "nodate" */
    int part = 1;      /* 1: no suffix */
    std::string ext;   /* lower case, no dot */
    SceneKind kind() const;
};

/* false: a foreign name (outside the grammar). */
bool parse_scene_name(const std::string &file, SceneName &out);
std::string format_scene_name(const SceneName &n);
/* "YYYYMMDD-HHMMSS" of a wall-clock second in local time (utc: UTC), or "nodate" when the clock is not synchronised. */
std::string when_text(std::int64_t wall_s, bool synchronized, bool utc = false);
/* systemd-timesyncd's marker exists (Cassette's TimesyncClockState): the clock was synchronised since boot. */
bool clock_synchronized(const std::string &marker = kTimesyncMarker);

/* The highest number of the names that parse (0 when none). Journal names (<stem>.json) count by their stem. */
int highest_number(const std::vector<std::string> &names);

/* The next number for a new scene or photo (the header comment); the counter file is written before it returns.
 * -errno when the counter cannot be written (the number is then not handed out). */
int next_scene_number(FileOps &ops, const std::string &folder, int &number);

} // namespace vhs
