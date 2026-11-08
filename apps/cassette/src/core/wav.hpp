/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: the WAV / Broadcast Wave / RF64 header. The header Cassette writes has a fixed size (kDataOffset) so
 * the sizes and later the metadata can be rewritten in place:
 *   RIFF|RF64 <size> WAVE | JUNK (28 bytes, becomes ds64 on the switch to RF64) | fmt | bext (fixed, 858 bytes) |
 *   iXML (padded, takes the rest up to kDataOffset) | data <size> ...audio
 * A header written for zero audio is a valid, empty WAV file. LVGL-free.
 */
#pragma once

#include "fileops.hpp"
#include "frame_sink.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

constexpr uint32_t kDataOffset = 5120;        /* where the audio starts in a file Cassette wrote */
constexpr uint32_t kBextBody = 602 + 256;     /* EBU bext fields + a coding history of up to 256 bytes */
constexpr uint64_t kRiffMax = 0xFFFFFFFEull;  /* the largest RIFF size a plain WAV can state */

struct BwfInfo {
    std::string description;
    std::string originator = "Cassette";
    std::string originator_reference;
    std::string origination_date; /* "YYYY-MM-DD", empty while the clock is not set */
    std::string origination_time; /* "HH:MM:SS" */
    uint64_t time_reference = 0;  /* samples since midnight (BWF convention) */
    std::string coding_history;   /* empty: generated for the capture format */
    std::string ixml;             /* empty: a minimal iXML document is generated */
};

/* The EBU R98 coding history line for the capture format, e.g. "A=PCM,F=48000,W=16,M=mono,T=Cassette\r\n". */
std::string coding_history_line(const AudioFormat &fmt);

/* Bytes per frame, 0 when the format is not one we write (16, 24 or 32 bit PCM, 1 or more channels). */
uint32_t block_align(const AudioFormat &fmt);

/* Does a file with data_offset bytes of header and data_bytes of audio no longer fit a plain RIFF size? */
bool needs_rf64(uint64_t data_offset, uint64_t data_bytes);

/* The kDataOffset header bytes; sizes for data_bytes of audio, as RF64 when rf64 (or when the size demands it). */
std::vector<unsigned char> build_header(const AudioFormat &fmt, const BwfInfo &info, uint64_t data_bytes = 0,
                                        bool rf64 = false);

/* Where the size fields of a header are, so they can be patched. */
struct HeaderLayout {
    uint64_t data_offset = kDataOffset;
    uint32_t block_align = 2;
    bool rf64 = false;        /* the file currently starts with RF64 */
    bool has_reserve = true;  /* a JUNK or ds64 chunk of at least 28 bytes sits at offset 12 */
};

struct Patch {
    uint64_t offset = 0;
    std::vector<unsigned char> bytes;
};
struct PatchSet {
    std::vector<Patch> patches; /* apply in order, each with one pwrite */
    bool rf64 = false;          /* the result is an RF64 header */
    bool clamped = false;       /* RF64 needed but the file has no room for ds64: sizes set to 0xFFFFFFFF */
};

/* The in-place writes that make the header state data_bytes of audio. Switches to RF64 when needs_rf64() or when
 * layout.rf64 already is (it never goes back). Order: ds64, data size, RIFF tag and size last. */
PatchSet size_patches(const HeaderLayout &layout, uint64_t data_bytes, bool force_rf64 = false);

struct HeaderInfo {
    bool ok = false;
    std::string error;
    AudioFormat fmt;
    uint32_t block_align = 0;
    uint64_t file_size = 0;
    uint64_t data_offset = 0;        /* first audio byte */
    uint64_t raw_riff = 0;           /* the 32-bit RIFF size field as stored */
    uint64_t raw_data = 0;           /* the 32-bit data size field as stored */
    uint64_t declared_riff = 0;      /* RIFF size (ds64's for RF64) */
    uint64_t declared_data = 0;      /* data size (ds64's for RF64) */
    bool rf64 = false;               /* starts with RF64 */
    bool has_ds64 = false;
    bool has_reserve = false;        /* JUNK or ds64 at offset 12 with room for the ds64 body */
    bool has_bext = false;
    bool has_ixml = false;
    uint64_t bext_offset = 0, ixml_offset = 0, ixml_size = 0; /* chunk body offsets (for in-place metadata edits) */
    bool placeholder_riff = false;   /* 0x80000000 / 0x7FFFFFFF / 0xFFFFFFFF kinds (arecord, ffmpeg pipes) */
    bool placeholder_data = false;   /* the same, or 0 on a file that holds audio */
    bool oversized_riff = false;     /* declares more than the file holds */
    bool oversized_data = false;
    uint64_t actual_data = 0;        /* file_size - data_offset */
    /* Placeholder or oversized: the file was never finalised (safe to repair without a journal). */
    bool suspicious() const { return placeholder_riff || placeholder_data || oversized_riff || oversized_data; }
    /* The declared sizes equal the file length. */
    bool consistent() const;
    HeaderLayout layout() const;
};

bool is_placeholder_size(uint64_t v);

HeaderInfo parse_header(FileOps &ops, const std::string &path);
HeaderInfo parse_header(const unsigned char *buf, size_t n, uint64_t file_size); /* from the first bytes */

}  // namespace cassette
