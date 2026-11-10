/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes: the recordings folder (default ~/Recordings, D14). Lists the takes from the files themselves (the
 * WAV header is the truth: format, length, bext date and time; for FLAC the STREAMINFO, for Ogg Opus the OpusHead and
 * the last page's granule position), marks repaired and damaged takes, reports free space and the hours left at a
 * format, deletes and renames takes (of every format: a rename keeps the extension). Every file call goes through
 * FileOps. LVGL-free.
 *
 * Files next to a take: its journal <dir>/.journal/<take>.json (journal.hpp; found by the file name in the journal's
 * path/parts, so a renamed take keeps its journal) and its peak cache <take>.peaks (written by the trim page, T5b).
 */
#pragma once

#include "../fileops.hpp"
#include "../frame_sink.hpp"
#include "naming.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

constexpr const char *kDefaultRecordingsDir = "~/Recordings";

/* "~/x" -> "<home>/x"; anything else unchanged. */
std::string expand_home(const std::string &path, const std::string &home);

struct TakeInfo {
    std::string name;      /* file name */
    std::string path;      /* dir + "/" + name */
    std::string label;     /* "006", "006-1"; the stem for a file outside the naming grammar */
    bool named = false;    /* the name follows the grammar (tn is valid) */
    TakeName tn;
    RecFormat format = RecFormat::Wav; /* by the extension */
    uint64_t size = 0;     /* bytes on disk */
    int64_t mtime = 0;
    bool damaged = false;  /* the header could not be parsed (error says why) */
    std::string error;
    bool repaired = false; /* the journal says "repaired" */
    std::string journal;   /* the journal file of the take, empty when none */
    AudioFormat fmt;       /* Opus: the input rate and channels from OpusHead, 16 bits */
    uint64_t frames = 0;   /* audio frames in the file (FLAC: 0 while STREAMINFO says unknown, a take being recorded) */
    double seconds = 0;
    bool has_bext = false;
    std::string origination_date; /* bext "YYYY-MM-DD", empty when absent or not set */
    std::string origination_time; /* bext "HH:MM:SS" */
    uint64_t time_reference = 0;  /* bext samples since midnight */
};

/* Is this folder entry a take file to list? (".wav", ".flac" or ".opus" in either case, not hidden, not a temp file.) */
bool is_take_file_name(const std::string &name);

/* Reads one file. Fills damaged instead of failing. */
TakeInfo read_take_info(FileOps &ops, const std::string &dir, const std::string &name);

/* All takes of the folder, newest first: named takes by take number (descending), within a take by part then edit
 * number (ascending: the original, then -1, -2); then the files outside the grammar by mtime (newest first); ties by
 * name. 0 or -errno (a missing folder gives -ENOENT and an empty list). */
int list_takes(FileOps &ops, const std::string &dir, std::vector<TakeInfo> &out);

/* Bytes one hour of audio takes at a format (48 kHz mono S16: 345 600 000). */
uint64_t bytes_per_hour(const AudioFormat &fmt);
/* Hours of recording that fit in free_bytes at fmt (0 for an unusable format). */
double hours_left(uint64_t free_bytes, const AudioFormat &fmt);

struct SpaceInfo {
    uint64_t free_bytes = 0;
    double hours_left = 0;
};
/* free_space() of the folder (the statvfs seam). 0 or -errno. */
int folder_space(FileOps &ops, const std::string &dir, const AudioFormat &fmt, SpaceInfo &out);

/* The peak cache file name of a take file name, as the trim page writes it (PeakPyramid::cache_path):
 * "take_006_x.wav" -> "take_006_x.wav.peaks"; the same for .flac / .opus ("take_006_x.flac.peaks"). */
std::string peaks_name(const std::string &take_file_name);
/* The name older versions looked for ("take_006_x.wav" -> "take_006_x.peaks"); delete_take() removes it too. */
std::string legacy_peaks_name(const std::string &take_file_name);

/* Finds the journal whose path or parts name this file (by file name). Empty when none. */
std::string find_journal_for(FileOps &ops, const std::string &dir, const std::string &take_file_name);

/* Deletes the take file, its peak cache, and its journal (or only this file from a journal that lists more parts).
 * The edit-number record stays, so the numbers of the take are never reused. 0 or -errno (only the take file's own
 * unlink is an error; a missing journal or cache is fine). */
int delete_take(FileOps &ops, const std::string &dir, const std::string &take_file_name);

/* Why rename_take answered -EINVAL (the optional `why` out parameter; None for every other result). */
enum class RenameRefusal { None, Empty, TooLong, BadChar, NotTake };

/* Renames the prefix of a take: the take number, date/time, part and edit suffix stay. -EINVAL for an invalid prefix
 * or a name outside the grammar (`why` says which), -EEXIST when the new name exists. The file is renamed atomically (rename), then the
 * peak cache, then the journal's path/parts are updated. new_name receives the new file name. */
int rename_take(FileOps &ops, const std::string &dir, const std::string &take_file_name, const std::string &new_prefix,
                std::string &new_name, RenameRefusal *why = nullptr);

}  // namespace cassette
