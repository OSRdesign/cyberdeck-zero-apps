/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: the recording journal, one small JSON file per take in <recordings_dir>/.journal/<take>.json,
 * replaced atomically (write a temp file, fsync, rename, fsync the directory). State: "open" while recording,
 * "closed" after a clean stop, "error" after a failed one, "repaired" after repair. LVGL-free.
 */
#pragma once

#include "fileops.hpp"
#include "frame_sink.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

struct JournalEntry {
    std::string take;
    std::string path;                /* the first file of the take */
    std::vector<std::string> parts;  /* every file of the take, in order (path is parts[0]) */
    AudioFormat fmt;
    std::string device_name;
    std::string device_id;           /* /dev/snd/by-id name */
    int64_t started_mono_ms = 0;
    int64_t started_wall = 0;        /* seconds since the epoch; 0 = the clock was not set */
    uint32_t sync_interval_ms = 10000;
    bool auto_split = true;
    std::string state = "open";
    uint64_t frames = 0;             /* all frames written (final count when closed) */
    uint64_t frames_synced = 0;      /* durable at the last sync */
    int64_t last_sync_wall = 0;
    uint64_t paused_frames = 0;      /* frames discarded while paused */
    uint64_t dropped_frames = 0;     /* frames the ring had to drop */
    uint64_t lost_frames = 0;        /* estimate of the audio lost to capture overruns (the card or the disk could not keep up) */
    std::string reason;              /* why it ended, when it did not end by the user */
};

std::string journal_dir(const std::string &recordings_dir);
std::string journal_file(const std::string &recordings_dir, const std::string &take);
std::string journal_sanitize(const std::string &take); /* '/' and control characters become '_' */

std::string journal_to_json(const JournalEntry &e);
bool journal_from_json(const std::string &text, JournalEntry &e);

/* Atomic replace. 0 or -errno. Creates .journal when needed. */
int journal_write(FileOps &ops, const std::string &recordings_dir, const JournalEntry &e);
/* 0, or -errno / -EINVAL when unreadable or not a journal. */
int journal_read(FileOps &ops, const std::string &file, JournalEntry &e);
/* Removes nothing: a closed journal stays as the take's record. */

}  // namespace cassette
