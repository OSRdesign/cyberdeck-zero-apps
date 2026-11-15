/*
 * SPDX-License-Identifier: MIT
 *
 * VHS (brief 3.1, 10): the part of Vinyl's state.json (state/resume.hpp, Vinyl 0.3.0, apps/vinyl at fdad829) that the output
 * volume needs, and nothing else: the software level of an output WITHOUT a playback mixer and VHS's own mute of an
 * output, both per output (output_volume_key). VHS has no queue and no resume file, so this is its own small file:
 *
 *   ~/.config/vhs/output-levels   one line per output "<percent> TAB <m|u> TAB <key>" after the line "v1" (XDG_CONFIG_HOME
 *                                 when set; VHS_OUTPUT_LEVELS names another file for tests), written atomically and only
 *                                 when a level or a mute changed, read tolerantly (a missing or damaged file is a fresh
 *                                 start: every output at 100 %, not muted). At most kMaxAppVolumes outputs are kept.
 *
 * The function names are Vinyl's (app_volume_of ...). LVGL-free.
 */
#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace vhs {

struct VolumeState {
    std::map<std::string, int> app_volumes; /* the software level (percent) of an output without a playback mixer */
    std::set<std::string> app_muted;        /* the outputs VHS muted itself, their level kept */
};

constexpr size_t kMaxAppVolumes = 32;
/* The software level of the output `key` (an ALSA card without a playback mixer): its own, else 100. */
int app_volume_of(const VolumeState &s, const std::string &key);
/* Sets it (0..100) for `key`. */
void set_app_volume(VolumeState &s, const std::string &key, int level);
/* VHS's own mute of the output `key` (app_muted). */
bool app_muted_of(const VolumeState &s, const std::string &key);
void set_app_muted(VolumeState &s, const std::string &key, bool muted);

/* $VHS_OUTPUT_LEVELS, else $XDG_CONFIG_HOME/vhs/output-levels, else ~/.config/vhs/output-levels ("" without a home). */
std::string volume_state_path();
std::string volume_state_to_text(const VolumeState &s);
/* false when the text is not a levels file (out is then empty). */
bool volume_state_from_text(const std::string &text, VolumeState &out);
/* 0 or -errno (the folder is made). */
int save_volume_state(const std::string &path, const VolumeState &s);
/* false when the file is missing or damaged (out is then empty). */
bool load_volume_state(const std::string &path, VolumeState &out);

}  // namespace vhs
