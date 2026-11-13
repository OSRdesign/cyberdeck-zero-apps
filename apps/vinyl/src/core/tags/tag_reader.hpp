/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tags: what the browser, the queue and the now-playing page show about a file (title, artist, album, year,
 * track number, duration, format) and its embedded cover picture, read by Vinyl's own small parsers, never by a decoder
 * library and never by a process per file (report 059, 14.13: ffprobe costs 0.8 s a file on the Pi Zero 2 W; a plain
 * reader 2.2 ms):
 *
 *   MP3   ID3v2.2 / 2.3 / 2.4 (text frames, TXXX for ReplayGain, APIC / PIC), ID3v1 at the end as a fall-back; the length
 *         from the Xing / Info / VBRI header of the first frame, else from the bit rate (CBR). The tag is walked frame by
 *         frame in the file (report 060 finding 3, P2a): only the frames wanted are read, a picture's body only when a
 *         picture is asked for (the cover, on demand), so a file with a 2 MB cover costs a few kB, not the whole tag (a
 *         tag unsynchronised as a whole, rare, is read up to 256 kB without a picture asked for);
 *   FLAC  STREAMINFO, VORBIS_COMMENT and PICTURE blocks (an ID3v2 tag in front is skipped);
 *   Ogg   Vorbis and Opus: the identification and comment packets (METADATA_BLOCK_PICTURE), the length from the last page's
 *         granule position;
 *   MP4   (.m4a, .m4b, .mp4): mvhd, the sample description (AAC or ALAC), the iTunes items of udta/meta/ilst, covr, the
 *         freeform ----:com.apple.iTunes:replaygain_* items;
 *   WAV   fmt and data (the length), LIST/INFO, an id3 chunk; AIFF: COMM, NAME / AUTH, an ID3 chunk.
 * Other formats (WMA, APE, WavPack...) get the codec name from the extension and no tags: the title is the file name.
 *
 * ReplayGain (core/dsp/replaygain.hpp): ID3 TXXX, Vorbis comments, Opus R128 tags and the MP4 freeform items, into
 * TrackTags::rg.
 *
 * Text is UTF-8 (ID3 Latin-1 and UTF-16 converted), so CJK titles come through (the UI draws them with the CJK fallback
 * font). Every length is bounds-checked: a damaged file gives fewer fields, never a crash. LVGL-free.
 */
#pragma once

#include "../dsp/replaygain.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {

struct Picture {
    std::string mime;              /* "image/jpeg", "image/png", or "" when the bytes must say */
    int type = 3;                  /* ID3 / FLAC picture type: 3 = front cover */
    std::vector<unsigned char> data;
};

struct TrackTags {
    std::string title, artist, album, album_artist, date, genre;
    int track = 0, disc = 0;
    uint64_t duration_ms = 0; /* 0 = unknown */
    std::string codec;        /* "MP3", "FLAC", "Vorbis", "Opus", "AAC", "ALAC", "WAV", "AIFF", "WMA" ... */
    uint32_t rate = 0;
    unsigned channels = 0, bits = 0, bitrate_kbps = 0;
    bool has_picture = false;
    bool ok = false;          /* the container was recognised */
    ReplayGainTags rg;        /* the ReplayGain tags (none: every has_* false) */
    std::string year() const; /* the first four digits of date, or "" */
};

/* Reads the tags of `path`. `pic` (may be null) receives the front cover (else the first picture) when one is embedded
 * and is at most max_picture_bytes long (a larger one is left out: has_picture stays true). false when the file cannot
 * be opened; a file of an unknown format returns true with only `codec` from the extension. bytes_read (may be null):
 * the bytes read from the file (the tests and the tag-read bench). */
bool read_tags(const std::string &path, TrackTags &out, Picture *pic = nullptr, size_t max_picture_bytes = 4u << 20,
               uint64_t *bytes_read = nullptr);
/* The ReplayGain tags of a file only (the engine, when it opens a track): read_tags without a picture. */
bool read_replaygain(const std::string &path, ReplayGainTags &out);

/* --------------------------------------------------------------------------- pieces, for the tests */

std::string latin1_to_utf8(const unsigned char *p, size_t n);
/* UTF-16 with a byte-order mark (big endian without one, or as `big_endian` says when bom is false) to UTF-8. */
std::string utf16_to_utf8(const unsigned char *p, size_t n, bool bom, bool big_endian);
/* Replaces invalid UTF-8 sequences by their Latin-1 reading and drops control characters. */
std::string clean_utf8(const std::string &s);
std::vector<unsigned char> base64_decode(const std::string &s);
/* Parses a Vorbis comment block (after its packet signature): vendor, then NAME=value pairs into the tags; a
 * METADATA_BLOCK_PICTURE (base64 FLAC picture) into pic. */
void parse_vorbis_comments(const unsigned char *p, size_t n, TrackTags &t, Picture *pic, size_t max_pic);
/* A FLAC PICTURE block body into pic; false when malformed. */
bool parse_flac_picture(const unsigned char *p, size_t n, Picture &pic, size_t max_pic);
/* An ID3v2 tag (from its "ID3" header) into the tags; returns the bytes the tag occupies (0 when there is none). */
size_t parse_id3v2(const unsigned char *p, size_t n, TrackTags &t, Picture *pic, size_t max_pic);
/* The "Title" a file shows without a title tag: its name without the extension. */
std::string title_from_path(const std::string &path);
/* "Artist, Title" as the queue rows show (the title alone without an artist). */
std::string artist_title(const TrackTags &t, const std::string &path);

}  // namespace vinyl
