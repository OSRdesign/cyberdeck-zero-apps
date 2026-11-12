/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl state: what survives a restart (report 059 P1 "resume last track and position"; with X-Background=run the
 * launcher may close Vinyl while hidden, so the state is saved as it changes, not only at exit):
 *
 *   ~/.config/vinyl/state.json   the queue (items, play order, current position, shuffle, repeat, source), the position
 *                                in the current track, the output choice, the browser's folder, the app volume.
 *
 * Written atomically (a temporary file, then rename: Cassette's atomic_write_file), at most every few seconds while
 * playing and at once on pause, track change and exit. A missing or damaged file means a fresh start. At start the
 * track is loaded paused at the saved position. LVGL-free.
 */
#pragma once

#include "../queue/queue.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {

struct ResumeState {
    std::vector<std::string> items;
    std::vector<int> order;
    int current = -1;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    std::string source;
    uint64_t position_ms = 0;
    std::string output = "auto"; /* "auto", "bt:AA:BB:..", "pulse:<sink>", "alsa:<device>" */
    std::string browse;          /* the folder the browser shows */
    int app_volume = 100;        /* 0..100, the in-app stream volume (decision D11) */
};

constexpr size_t kMaxResumeItems = 5000;

/* ~/.config/vinyl/state.json (XDG_CONFIG_HOME when set). */
std::string resume_path();

std::string resume_to_json(const ResumeState &s);
bool resume_from_json(const std::string &text, ResumeState &out);

/* 0 or -errno. */
int save_resume(const std::string &path, const ResumeState &s);
/* false when the file is missing or damaged (out is then the defaults). */
bool load_resume(const std::string &path, ResumeState &out);

void resume_from_queue(const Queue &q, ResumeState &s);
void queue_from_resume(const ResumeState &s, Queue &q);

}  // namespace vinyl
