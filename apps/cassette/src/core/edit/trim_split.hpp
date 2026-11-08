/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: lossless trim and split of a WAV take. LVGL-free; every file access goes through FileOps.
 *
 * The request is the source take, an IN and an OUT frame and any number of split frames; the result is one file per part,
 * [IN, split1), [split1, split2) ... [splitN, OUT). OUT is EXCLUSIVE (OUT == frames in the take means "to the end").
 * The audio bytes are copied as they are (no decoding, no conversion); each result gets a fresh header from wav.hpp with the
 * right sizes (RF64 when needed), a bext chunk copied from the source (time reference advanced by the part's start frame,
 * description and coding history extended) and a regenerated iXML chunk (same reserved size: the header is kDataOffset).
 *
 * Safety: every result is written to "<dir>/.tmp-<random>" in the take's folder, fdatasync'ed, then renamed to its final name;
 * the folder is fsynced. Nothing is renamed into place before ALL results are complete. A cancel, an error or a kill leaves the
 * original untouched (the temporaries are removed at once, or by cleanup_temporaries() after a kill).
 * ReplaceOriginal: the original is removed only after every result is renamed into place and the folder is synced. A trim
 * alone (one part) with ReplaceOriginal takes the original's exact name by an atomic rename over it (no moment without it).
 * A kill between the renames of a split can leave some COMPLETE results next to the untouched original (never a half file).
 */
#pragma once

#include "fileops.hpp"
#include "wav.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cassette {

/* The result names (file names only, no folder) for `count` results of the take named `source` (a file name). The real one
 * is next_edit_names() of the naming unit; the engine only needs this shape. */
using NameProvider = std::function<std::vector<std::string>(const std::string &source, int count)>;

enum class SaveMode { NewFiles, ReplaceOriginal };

enum class EditError {
    None,
    BadSource,     /* unreadable, not PCM WAV, or the header declares more audio than the file holds */
    BadPositions,  /* outside the take, IN >= OUT, splits not strictly increasing inside (IN, OUT) */
    BadNames,      /* the provider gave the wrong number of names, a bad name or one that exists */
    NoSpace,       /* not enough free space for the results (checked before anything is written) */
    Io,            /* a file operation failed (the message says which) */
    Cancelled,
    SourceChanged, /* the take changed while it was being copied */
};

struct EditRequest {
    std::string source_path;
    uint64_t in_frame = 0;
    uint64_t out_frame = 0;            /* exclusive */
    std::vector<uint64_t> splits;      /* strictly increasing, inside (in, out) */
    SaveMode mode = SaveMode::NewFiles;
};

struct EditOptions {
    FileOps *ops = nullptr;            /* null: the real file system */
    NameProvider names;                /* required unless mode is ReplaceOriginal with a single part */
    std::function<void(const std::string &original_path)> on_original_deleted; /* journal, other caches; after the removal */
    std::function<void(uint64_t done, uint64_t total)> progress; /* audio bytes copied; monotonic; (0,total) first */
    const std::atomic<bool> *cancel = nullptr;
    size_t block_bytes = 256 * 1024;   /* copy block (tests use small ones) */
    uint64_t free_margin = 64 * 1024;  /* kept free on top of the results */
};

struct EditResult {
    bool ok = false;
    EditError error = EditError::None;
    std::string message;               /* the reason, for the UI */
    std::string warning;               /* ok, but something is left to look at (the original could not be removed) */
    std::vector<std::string> outputs;  /* full paths, in part order */
    bool original_removed = false;     /* ReplaceOriginal: the original no longer exists under its name with old content */
};

/* What the engine knows about a take. */
struct TakeInfo {
    bool ok = false;
    std::string error;
    AudioFormat fmt;
    uint32_t block_align = 0;
    uint64_t frames = 0;               /* whole frames of audio */
    uint64_t data_offset = 0;
    uint64_t size = 0;
    int64_t mtime = 0;
    BwfInfo bwf;                       /* from the source bext (defaults when it has none) */
};
TakeInfo inspect_take(FileOps &ops, const std::string &path);

/* The kDataOffset header for the part [start, start+frames) of the take (exposed so the RF64 boundary can be tested with
 * mocked sizes). part is 1-based. */
std::vector<unsigned char> build_part_header(const TakeInfo &take, const std::string &source_name, uint64_t start_frame,
                                             uint64_t frames, int part, int parts);

EditResult trim_split(const EditRequest &req, const EditOptions &opt);

/* Removes every ".tmp-*" file of the folder (left by a killed save or peak build). Returns the number removed, or -errno. */
int cleanup_temporaries(FileOps &ops, const std::string &dir);
int cleanup_temporaries(const std::string &dir);

}  // namespace cassette
