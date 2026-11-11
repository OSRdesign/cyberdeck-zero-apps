/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: MERGE TAKES. Several takes of the same codec and format become one new take, in the order the user chose
 * them, without any loss of quality. LVGL-free; every file access goes through FileOps (the fault-injecting test FileOps
 * work unchanged), memory stays bounded whatever the length of the takes (blocks of block_bytes, one packet at a time).
 *
 * The join, per codec (docs/FORMATS.md "Merging takes"):
 *   WAV   the PCM of each take is copied as it is, one after the other, behind a fresh BWF header (wav.hpp build_header: the
 *         sizes of the whole, RF64 when the data passes 4 GiB, a bext with the caller's date, time and time reference, as
 *         a new recording has). Sample exact: the new take is exactly the takes' samples end to end.
 *   FLAC  every take is decoded (libFLAC) and the samples go through ONE encoder at the takes' own rate, channels and depth
 *         (libFLAC level 5, streamable subset, 4096 bytes of padding, as a recording): lossless and sample exact, the
 *         STREAMINFO of the new take says the sum of the lengths and its MD5 is the MD5 of the whole.
 *   Opus  no re-encoding: the packets of every take are copied into ONE Ogg Opus stream (new serial number, page sequence,
 *         CRCs; granule positions counted on from take to take). The first take keeps its pre-skip (the stream's), the last
 *         take keeps its end trimming (the last page's granule). An Ogg Opus stream can trim samples only at its start and
 *         its end, so at each join the new take holds what the separate takes drop when they play alone:
 *           - the end of the earlier take's last packet beyond its length (the encoder's padding of its last 20 ms frame,
 *             or for an edited take the sound just after its OUT), under one packet: 0 to 20 ms;
 *           - the start of the later take's first packet before its first sample (the encoder's look-ahead, 6.5 ms for a
 *             libopusenc take at 48 kHz; for an edited take the sound just before its IN). Packets that lie wholly inside a
 *             later take's pre-skip are left out (an edited take keeps 320 ms of pre-roll there: it is not played twice).
 *         So a join adds under 40 ms (typically 7 to 27 ms between takes straight from the recorder), and plan.seam_frames
 *         says exactly how much; the decoder also runs on across the join with its state from the earlier take, so the first
 *         few tens of milliseconds of a later take can sound slightly different from that take played alone. The Opus bit
 *         rate is not stored in a take and need not match: packets of any bit rate decode in one stream.
 *
 * Safety (the trim engine's rules, trim_split.hpp): the new take is written to "<dir>/.tmp-merge-<random>" in the takes'
 * folder, fdatasync'ed, closed and VERIFIED (re-read from the file: the format and the length, which must be the sum of the
 * takes' lengths, plus the seams for Opus); only then renamed to its final name and the folder fsynced. A cancel, an error or
 * a kill before that leaves the takes untouched and no take under the new name (the temporary goes at once, or by
 * cleanup_temporaries() at the next start). delete_originals removes the originals only after all of that, one by one; a
 * failure there leaves the rest in place (a warning, the merge itself succeeded).
 */
#pragma once

#include "../codec/codec_libs.hpp"
#include "../codec/encoder.hpp"
#include "fileops.hpp"
#include "trim_split.hpp"
#include "wav.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cassette {

/* What the compatibility rule needs to know of a take (from the take list, or from inspect_take()). */
struct MergeFacts {
    bool named = false;     /* the file name follows the take grammar (a Cassette take) */
    bool readable = true;   /* false: damaged, cannot be read */
    EditKind kind = EditKind::Wav;
    uint32_t rate = 0;      /* Opus: the input rate of OpusHead (the rate the take plays at) */
    uint16_t channels = 0;
    uint16_t bits = 0;      /* WAV and FLAC: bits per sample (Opus: not compared) */
};

/* "WAV 48 kHz", "FLAC 44.1 kHz", "Opus 16 kHz". */
std::string merge_format_label(EditKind kind, uint32_t rate);
/* Why a take cannot be in any merge: "Not a Cassette take", "Damaged take"; "" when it can. */
std::string merge_take_reason(const MergeFacts &t);
/* Why `other` cannot be joined losslessly to a merge whose FIRST take is `first`, "" when it can. In this order:
 * merge_take_reason(other); "Different format: FLAC 44.1 kHz" (another codec: the other take's codec and rate), "Different
 * rate", "Different channels", "Different bit depth" (WAV and FLAC only). */
std::string merge_join_reason(const MergeFacts &first, const MergeFacts &other);
/* The facts of an inspected take (named: the caller knows the name). */
MergeFacts merge_facts_of(const EditTakeInfo &t, bool named);

struct MergeRequest {
    std::vector<std::string> sources; /* full paths in the order chosen: at least 2, none twice, all in one folder */
    std::string output_name;          /* the new take's file name (no folder; it goes in the takes' folder) */
    bool delete_originals = false;
    Tags tags;                        /* FLAC / Opus: the Vorbis comments of the new take (empty: the first take's) */
    BwfInfo bwf;                      /* WAV: the bext of the new take (description, date, time, time reference) */
};

enum class MergeError { None, BadSources, Incompatible, BadName, NoSpace, Io, Cancelled, SourceChanged, Verify };

struct MergeOptions {
    FileOps *ops = nullptr;           /* null: the real file system */
    /* frames of the new take written so far, of `total`; (0, total) first, monotonic */
    std::function<void(uint64_t done, uint64_t total)> progress;
    const std::atomic<bool> *cancel = nullptr;
    size_t block_bytes = 256 * 1024;  /* the copy / decode step (tests use small ones) */
    /* free space needed on top of nothing else: the takes' total size times this (the owner: total size + 5 %) */
    unsigned space_percent = 105;
    const DecoderLibs *decoders = nullptr; /* FLAC sources: the decoder (null: the real libFLAC); Opus needs none */
    /* The encoder of a FLAC result for the takes' format; null: FlacEncoder level 5 on the real libFLAC. */
    std::function<std::unique_ptr<Encoder>(const AudioFormat &fmt)> make_flac_encoder;
    /* Called once the new take is under its final name and the folder is synced, before any original is deleted (the take
     * counter is moved on there: the number is used only by a merge that succeeded). */
    std::function<void(const std::string &output_path)> on_committed;
    /* Deletes one original (the take file, its peak cache, its journal). Null: unlink the file and its peak cache. 0 or
     * -errno. */
    std::function<int(const std::string &path)> delete_original;
};

struct MergePlan {
    bool ok = false;
    MergeError error = MergeError::None;
    std::string message;              /* the reason, for the UI */
    EditKind kind = EditKind::Wav;
    AudioFormat fmt;                  /* the format the frames count in (Opus: the decoder's output) */
    std::vector<EditTakeInfo> takes;  /* in the order chosen */
    std::string dir;                  /* the takes' folder */
    uint64_t source_frames = 0;       /* the takes' lengths added up */
    uint64_t frames = 0;              /* the new take's length: source_frames (+ seam_frames for Opus) */
    uint64_t seam_frames = 0;         /* Opus: what the joins add (see the head of this file); 0 for WAV and FLAC */
    uint64_t source_bytes = 0;        /* the takes' sizes added up */
    uint64_t need_bytes = 0;          /* the free space asked for */
    uint64_t estimate_bytes = 0;      /* about the new take's size */
    /* Opus: per take, the packets kept and the 48 kHz samples the join adds before (lead) and after (tail) the take */
    std::vector<uint64_t> opus_drop;  /* packets left out at the start (wholly inside a later take's pre-skip) */
    std::vector<int64_t> opus_lead48, opus_tail48;
};

/* Checks everything a merge needs without writing anything: at least two takes, none twice, one folder, each readable and
 * joinable to the first (merge_join_reason with named = true: the caller has checked the names), the new name free and
 * valid, the free space. */
MergePlan plan_merge(const MergeRequest &req, const MergeOptions &opt);

struct MergeResult {
    bool ok = false;
    MergeError error = MergeError::None;
    std::string message;
    std::string warning;              /* ok, but an original could not be deleted (the message says which) */
    std::string output;               /* the new take's full path */
    uint64_t frames = 0;              /* its length, verified */
    uint64_t seam_frames = 0;
    std::vector<std::string> deleted; /* the originals removed */
};

MergeResult merge_takes(const MergeRequest &req, const MergeOptions &opt);

}  // namespace cassette
