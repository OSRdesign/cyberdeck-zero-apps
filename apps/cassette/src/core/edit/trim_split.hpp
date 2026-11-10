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
 *
 * FLAC and Ogg Opus takes (phase C): the results keep the source's format, the same temporaries, the same renames, the
 * same checks and the same order of operations (so every guarantee above holds); only the step that writes a result
 * differs. Positions are frames of the decoder's output (Opus: codec/decoder.hpp).
 *   FLAC: the part is decoded (libFLAC) and encoded again (libFLAC, level 5, streamable subset, 4096 bytes of padding, as
 *         a recording): lossless, sample exact at both cuts; STREAMINFO gets the part's length and MD5. (Keeping the
 *         whole frames in between would need a variable-block-size stream around the re-encoded edge frames, which
 *         libFLAC cannot write and some players handle badly; a full re-encode is simpler and still lossless.)
 *   Opus: no re-encoding at all: the packets of the part plus 320 ms of pre-roll before it are copied into new pages;
 *         the new pre-skip drops the pre-roll and the part of the first packet before IN, the last page's granule trims
 *         the end at OUT (RFC 7845 sections 4.2 to 4.6). Sample exact, no second generation of lossy coding, no codec
 *         library needed (codec/ogg_opus.hpp remux_opus_range).
 *   Both: the Vorbis comments are copied; a TITLE that named the source file names the result.
 */
#pragma once

#include "../codec/codec_libs.hpp"
#include "../codec/encoder.hpp"
#include "fileops.hpp"
#include "wav.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
    std::function<void(uint64_t done, uint64_t total)> progress; /* audio bytes copied (FLAC / Opus: frames written);
                                                                    monotonic; (0,total) first */
    const std::atomic<bool> *cancel = nullptr;
    size_t block_bytes = 256 * 1024;   /* copy block (tests use small ones); FLAC: bytes of PCM decoded per step */
    uint64_t free_margin = 64 * 1024;  /* kept free on top of the results */
    /* FLAC / Opus sources */
    const DecoderLibs *decoders = nullptr; /* the FLAC decoder (null: the real libFLAC); Opus needs none */
    /* The encoder of a FLAC result for the source's format; null: FlacEncoder level 5 on the real libFLAC. */
    std::function<std::unique_ptr<Encoder>(const AudioFormat &fmt)> make_flac_encoder;
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
enum class EditKind { Wav, Flac, OggOpus };
struct EditTakeInfo {
    bool ok = false;
    std::string error;
    EditKind kind = EditKind::Wav;     /* by the extension (by the magic when there is none) */
    AudioFormat fmt;                   /* FLAC / Opus: the decoder's output format, the one frames count in */
    uint32_t block_align = 0;
    uint64_t frames = 0;               /* whole frames of audio */
    uint64_t data_offset = 0;          /* WAV: the audio bytes; FLAC / Opus: where the audio starts after the headers */
    uint64_t size = 0;
    int64_t mtime = 0;
    BwfInfo bwf;                       /* from the source bext (defaults when it has none) */
    Tags tags;                         /* FLAC / Opus: the Vorbis comments */
    std::string vendor;
    unsigned opus_k = 1;               /* Opus: 48000 / output rate */
};
EditTakeInfo inspect_take(FileOps &ops, const std::string &path);

/* The kDataOffset header for the part [start, start+frames) of the take (exposed so the RF64 boundary can be tested with
 * mocked sizes). part is 1-based. */
std::vector<unsigned char> build_part_header(const EditTakeInfo &take, const std::string &source_name, uint64_t start_frame,
                                             uint64_t frames, int part, int parts);

EditResult trim_split(const EditRequest &req, const EditOptions &opt);

/* Removes every ".tmp-*" file of the folder (left by a killed save or peak build). Returns the number removed, or -errno. */
int cleanup_temporaries(FileOps &ops, const std::string &dir);
int cleanup_temporaries(const std::string &dir);

}  // namespace cassette
