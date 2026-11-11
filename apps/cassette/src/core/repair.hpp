/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: repair on launch. Takes whose journal is still "open" (the app or the power died while recording)
 * and, without a journal, any .wav in the recordings folder whose header still carries placeholder or oversized
 * sizes get their header sizes rewritten from the real file length. The audio is never touched, except that a
 * partial last frame is cut off. Idempotent, and safe to interrupt: the only writes are the single pwrite of each
 * header size field (plus the one ftruncate of a partial frame, done first); running it again finishes the job.
 * FLAC and Ogg Opus takes (.flac / .opus, phase B) go through codec/stream_scan.hpp instead: the cut last frame or
 * page is removed (the one ftruncate, first), then FLAC's STREAMINFO total samples (and a stale MD5) or the Opus
 * end-of-stream flag are set in place; without a journal only a file that looks unfinished is touched (FLAC total 0,
 * Opus without end of stream). The same report, the same "repaired" journal state.
 * LVGL-free.
 */
#pragma once

#include "fileops.hpp"
#include "frame_sink.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

struct FileRepair {
    bool ok = false;
    std::string error;
    bool changed = false;      /* the file was modified (header sizes or a cut partial frame) */
    AudioFormat fmt;
    uint64_t data_bytes = 0;   /* audio bytes after the repair (WAV) */
    uint64_t frames = 0;
    uint32_t truncated_bytes = 0; /* a partial last frame that was cut (FLAC/Opus: the cut frame or page) */
    bool rf64 = false;
    bool clamped = false;      /* over 4 GiB without room for ds64: sizes left at 0xFFFFFFFF */
};

/* force: the file is known to be ours and unfinished (journal open): set the sizes from the length whatever the
 * header says. Without force the file is only touched when its header is a placeholder or oversized. */
FileRepair repair_file(FileOps &ops, const std::string &path, bool force);

struct RepairedTake {
    std::string take;          /* the journal's take name, or the file name without its extension */
    std::string path;          /* the first file */
    uint64_t frames = 0;       /* recovered, all parts */
    double seconds = 0;
    double missing_seconds = -1; /* estimate of the audio lost at the end; -1 when the journal has no start time */
    bool had_journal = false;
    uint32_t truncated_bytes = 0;
    std::string note;
};

struct RepairReport {
    std::vector<RepairedTake> repaired;
    std::vector<std::string> errors; /* things that could not be repaired, one line each */
};

RepairReport repair_all(FileOps &ops, const std::string &recordings_dir);

}  // namespace cassette
