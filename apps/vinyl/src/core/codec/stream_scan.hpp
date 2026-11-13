/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette codecs: reading and mending FLAC and Ogg Opus take files without a decoder (phase B). Everything goes through
 * FileOps (pread, one ftruncate, small pwrites), so the fault-injecting test FileOps work unchanged. LVGL-free.
 *
 *   read_stream_info()  quick facts for the take list: the FLAC STREAMINFO (format, total samples), or OpusHead plus
 *                       the granule position of the last page (read from the file's last 128 KB)
 *   scan_flac()         walks the frames from the first one: a frame counts when its header passes CRC-8 and its end
 *                       passes CRC-16 (RFC 9639: FLAC frames carry no length; the end is where the CRC-16 matches, at
 *                       the next frame header or at the end of the file). A damaged frame in the middle is skipped by
 *                       looking for the next valid header (resync), so only the tail is ever cut. The sample count of
 *                       the valid prefix comes from the last valid frame's number: frame number x block size + its block
 *                       size (fixed block size), or sample number + block size (variable)
 *   scan_ogg_opus()     walks the Ogg pages (RFC 3533) checking each page CRC; OpusHead (RFC 7845) gives the channels,
 *                       the pre-skip and the input rate; the duration is the granule position of the last page with one,
 *                       minus the pre-skip, at the input rate. Pages of another stream or a bad CRC in the middle are
 *                       skipped by resync
 *   fix_flac() / fix_ogg_opus()
 *                       the repair of a cut take (study 058, 4.3): cut the file after the last valid unit (one
 *                       ftruncate, first, as the WAV repair does), then
 *                         FLAC: rewrite STREAMINFO's total samples in place; its MD5 is set to zero (= unknown) when it
 *                               was set for a different length (zeros first, then the total)
 *                         Opus: set the end-of-stream flag on the last page, with its CRC, in one 21-byte pwrite
 *                       Idempotent and safe to interrupt: a second run finds nothing to do or finishes the job.
 */
#pragma once

#include "../fileops.hpp"
#include "../frame_sink.hpp"

#include <cstdint>
#include <string>

namespace vinyl {

enum class Container { Unknown, Wav, Flac, OggOpus };

/* By the first four bytes ("RIFF"/"RF64", "fLaC", "OggS"), else by the extension (.wav, .flac, .opus, either case). */
Container container_of(FileOps &ops, const std::string &path);
/* By the extension alone (no file access). */
Container container_of_name(const std::string &file_name);

struct StreamInfo {
    bool ok = false;
    std::string error;
    Container kind = Container::Unknown;
    AudioFormat fmt;           /* FLAC: STREAMINFO; Opus: OpusHead channels and input rate, 16 bits (what decodes) */
    uint64_t frames = 0;       /* at fmt.rate */
    bool frames_known = false; /* FLAC: STREAMINFO says (0 = unknown: a take being recorded or cut); Opus: a page had a granule */
    uint64_t file_size = 0;
};
StreamInfo read_stream_info(FileOps &ops, const std::string &path);

struct StreamScan {
    bool ok = false;             /* the stream headers are readable */
    std::string error;
    Container kind = Container::Unknown;
    AudioFormat fmt;
    uint64_t file_size = 0;
    uint64_t audio_offset = 0;   /* FLAC: the first frame; Ogg: the end of the header pages */
    uint64_t valid_end = 0;      /* the end of the last valid frame / page: what is kept */
    uint64_t frames = 0;         /* audio frames in the kept part, at fmt.rate */
    uint64_t units = 0;          /* valid FLAC frames / Ogg pages */
    uint64_t damaged = 0;        /* stretches skipped in the middle (resync), not cut */
    /* FLAC */
    uint64_t stored_total = 0;   /* STREAMINFO total samples */
    bool md5_set = false;
    uint64_t streaminfo_offset = 0;
    /* Ogg Opus */
    uint32_t preskip = 0;
    uint64_t last_page = 0;      /* offset of the last kept page */
    uint64_t last_granule = 0;
    bool eos = false;            /* the last kept page carries the end-of-stream flag */
    bool header_only = false;    /* no audio page: nothing after OpusHead / OpusTags */
};
StreamScan scan_flac(FileOps &ops, const std::string &path);
StreamScan scan_ogg_opus(FileOps &ops, const std::string &path);

struct StreamFix {
    bool ok = false;
    std::string error;
    bool changed = false;
    AudioFormat fmt;
    uint64_t frames = 0;
    uint64_t truncated_bytes = 0; /* the cut tail */
};
/* force: the file is known to be ours and unfinished (an open journal): fix whatever is inconsistent. Without force
 * only a file that looks cut is touched (FLAC: bytes after the last valid frame, or a total of 0 with frames;
 * Opus: bytes after the last valid page, or no end-of-stream flag). */
StreamFix fix_flac(FileOps &ops, const std::string &path, bool force);
StreamFix fix_ogg_opus(FileOps &ops, const std::string &path, bool force);

/* The checksums (exposed for the tests and the test encoders). */
uint8_t flac_crc8(const unsigned char *p, size_t n);
uint16_t flac_crc16(const unsigned char *p, size_t n, uint16_t crc = 0);
uint32_t ogg_crc32(const unsigned char *p, size_t n, uint32_t crc = 0);

}  // namespace vinyl
