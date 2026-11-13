/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette codecs: Ogg Opus without libogg or libopusfile (phase C). Cassette reads and writes the Ogg pages itself
 * (RFC 3533) and knows the Opus framing (RFC 7845 for the mapping, RFC 6716 section 3.1 for a packet's duration), so:
 *   - playback needs only libopus' decoder (opus_decoder.*): this file hands it whole packets with their exact
 *     positions on the 48 kHz granule scale, and finds a position by bisection over the pages (no index of the whole
 *     file, no full read at open: a few small reads per seek, whatever the length of the take);
 *   - trim and split need no codec library at all: whole packets are copied into new pages with a new pre-skip and an
 *     end-trimming granule, sample exact, no second generation of lossy coding (remux_opus_range below).
 * Everything goes through FileOps (pread, pwrite), so the fault-injecting test FileOps work unchanged. LVGL-free.
 *
 * Positions ("48k") are granule positions: 48 kHz samples counted from the stream's origin, the pre-skip included. A
 * page's granule is the end of the last packet that completes on it; a packet's start is its end minus its duration.
 */
#pragma once

#include "../fileops.hpp"
#include "byte_sink.hpp"
#include "encoder.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vinyl {
namespace oggopus {

/* The place of decoded frames on the granule scale, for a decoder running at 48000 / k Hz (k = 1, 2, 3, 4 or 6). Frame
 * o of the output (0 = the first sample after the pre-skip) is the decoded sample o + skip(), whose granule position is
 * start48 + (o + skip()) * k. Packet boundaries are whole multiples of 2.5 ms (120 samples) from the stream start, so they
 * fall on whole output samples at every Opus rate; a pre-skip that is not a multiple of k is rounded down (less than one
 * output sample). */
struct Timeline {
    int64_t start48 = 0;
    uint32_t preskip = 0;
    unsigned k = 1;
    uint64_t skip() const { return preskip / k; }
    int64_t pos48(uint64_t frame) const { return start48 + static_cast<int64_t>((frame + skip()) * k); }
    /* the output frame of a granule position on a packet boundary (may be negative: inside the pre-skip) */
    int64_t frame_of(int64_t pos48) const { return (pos48 - start48) / static_cast<int64_t>(k) - static_cast<int64_t>(skip()); }
    uint64_t length(int64_t end48) const {
        const int64_t n = end48 - start48 - static_cast<int64_t>(preskip);
        return n > 0 ? static_cast<uint64_t>(n) / k : 0;
    }
};

constexpr uint64_t kNoGranule = ~0ull;
constexpr uint32_t kPreroll48 = 3840; /* 80 ms: decode this much before a seek target (RFC 7845 section 4.6) */
/* A trimmed file keeps 320 ms of packets before its first sample (dropped by its pre-skip): the decoder state (CELT's
 * band energies are predicted from the frame before) has converged by then, so the first samples of the part sound as
 * they did in the take; with the minimum 80 ms they differed audibly for about 100 ms (tests/codec real-library SNR). */
constexpr uint32_t kRemuxPreroll48 = 15360;
constexpr uint32_t kMaxPacket48 = 5760; /* 120 ms, the longest Opus packet */

/* The duration of an Opus packet in 48 kHz samples from its TOC byte (and frame count byte for code 3), RFC 6716
 * section 3.1; 0 for an empty or malformed packet. */
uint32_t packet_samples48(const unsigned char *p, size_t len);

/* OpusHead (RFC 7845 section 5.1). */
struct Head {
    unsigned version = 1;
    unsigned channels = 0;
    uint32_t preskip = 0;
    uint32_t input_rate = 0; /* 0 in the file = unknown */
    int16_t gain_q8 = 0;     /* output gain, Q7.8 dB */
    unsigned mapping_family = 0;
    std::vector<unsigned char> mapping; /* family != 0: stream count, coupled count, channel mapping */
};
bool parse_head(const unsigned char *p, size_t n, Head &h, std::string *why = nullptr);
std::vector<unsigned char> build_head(const Head &h);

/* OpusTags (RFC 7845 section 5.2): vendor string and Vorbis comments. */
bool parse_tags(const unsigned char *p, size_t n, std::string &vendor, Tags &tags);
std::vector<unsigned char> build_tags(const std::string &vendor, const Tags &tags, size_t padding = 0);

/* One Ogg page with its CRC checked. */
struct Page {
    uint64_t offset = 0;
    size_t header_len = 0, body_len = 0;
    unsigned flags = 0; /* 1 continued packet, 2 beginning of stream, 4 end of stream */
    uint64_t granule = kNoGranule;
    uint32_t serial = 0, seq = 0;
    std::vector<uint8_t> lacing;
    std::vector<unsigned char> body; /* filled when asked for */
    uint64_t end() const { return offset + header_len + body_len; }
};

/* A page from whole packets (none may need more than the page's 255 lacing values together). */
std::vector<unsigned char> build_page(unsigned flags, uint64_t granule, uint32_t serial, uint32_t seq,
                                      const std::vector<std::vector<unsigned char>> &packets);

/* Reads pages through FileOps with one buffered window. */
class PageReader {
public:
    PageReader(FileOps &ops, int fd, uint64_t size) : ops_(ops), fd_(fd), size_(size) {}
    uint64_t size() const { return size_; }
    /* The page at exactly `off` when its CRC matches. */
    bool read_at(uint64_t off, Page &pg, bool want_body);
    /* The first valid page starting in [from, limit). */
    bool find_next(uint64_t from, uint64_t limit, Page &pg, bool want_body);
    int error() const { return err_; } /* the first read error (-errno), 0 when none */
    uint64_t bytes_read() const { return bytes_read_; }

private:
    const unsigned char *get(uint64_t pos, size_t n);
    FileOps &ops_;
    int fd_;
    uint64_t size_;
    std::vector<unsigned char> buf_;
    uint64_t base_ = 0;
    size_t len_ = 0;
    int err_ = 0;
    uint64_t bytes_read_ = 0;
};

/* A whole packet and its place on the granule scale. */
struct Packet {
    std::vector<unsigned char> data;
    int64_t start48 = 0, end48 = 0;
    uint64_t page_offset = 0; /* the page on which it completes */
};

/* One Ogg Opus file opened for reading packets in order, from the start or from a position. Logical streams other than
 * the first are skipped (Cassette writes one); damaged pages in the middle are skipped by resync. */
class OggOpusFile {
public:
    explicit OggOpusFile(FileOps &ops) : ops_(ops) {}
    ~OggOpusFile() { close(); }
    OggOpusFile(const OggOpusFile &) = delete;
    OggOpusFile &operator=(const OggOpusFile &) = delete;

    /* Reads OpusHead and OpusTags, finds the first audio page and the last granule. 0, -errno, or kCodecBadStream. */
    int open(const std::string &path, std::string *why = nullptr);
    void close();
    bool is_open() const { return fd_ >= 0; }

    const Head &head() const { return head_; }
    const std::string &vendor() const { return vendor_; }
    const Tags &tags() const { return tags_; }
    uint32_t serial() const { return serial_; }
    uint64_t file_size() const { return size_; }
    uint64_t audio_offset() const { return audio_off_; }
    bool has_audio() const { return has_end_; }  /* a page with a granule exists after the headers */
    bool eos() const { return eos_; }             /* the last page found carries the end-of-stream flag */
    int64_t start48() const { return start48_; }  /* the start of the first packet (0 for Cassette's takes) */
    int64_t end48() const { return end48_; }      /* the granule of the last page: the end, end trimming applied */
    /* Playable 48 kHz samples: end48 - start48 - pre-skip (0 when none). */
    uint64_t length48() const;

    /* The next packet returned is the first whole packet that starts at or before `target48` and ends after it (or an
     * earlier one; at the latest the one containing it). Positions before the start rewind. 0 or < 0. */
    int seek(int64_t target48);
    /* Back to the first audio packet. */
    int rewind();
    /* The next whole packet: 1, 0 at the end of the stream, < 0 on a read error. */
    int next(Packet &p);

    /* Counters (tests and the CPU measurement): pages read, bisection probes, bytes read. */
    struct Stats {
        uint64_t pages = 0, probes = 0, bytes = 0, seeks = 0;
    };
    Stats stats() const;

private:
    bool load_page(uint64_t off, Page &pg); /* the page at off or the next valid one of our stream */
    int start_at_page(const Page &pg, bool keep_tail_only);
    void note(const Page &pg);

    FileOps &ops_;
    int fd_ = -1;
    uint64_t size_ = 0;
    std::unique_ptr<PageReader> rd_;
    Head head_;
    std::string vendor_;
    Tags tags_;
    uint32_t serial_ = 0;
    uint64_t audio_off_ = 0;
    int64_t start48_ = 0, end48_ = 0;
    bool has_end_ = false, eos_ = false;

    /* the packet cursor */
    std::vector<Packet> ready_;   /* packets of the current page, in order */
    size_t ready_i_ = 0;
    std::vector<unsigned char> partial_;
    bool partial_ok_ = false;     /* partial_ holds a packet from its first byte */
    uint64_t next_off_ = 0;       /* where the next page should start */
    int64_t last_end48_ = 0;      /* the end of the last page read (its granule), for an end-of-stream page */
    bool have_last_end_ = false;
    bool at_end_ = false;
    std::map<int64_t, uint64_t> known_; /* granule -> page offset of pages seen (bisection hints) */
    mutable Stats st_;
};

/* What remux_opus_range() writes for the output frames [first, first + frames) at the decoder's output rate (rate_k =
 * 48000 / output rate; the mapping is OpusFileDecoder's). */
struct RemuxRequest {
    uint64_t first = 0, frames = 0;
    unsigned rate_k = 1;
    Tags tags;                /* the OpusTags comments of the result */
    std::string vendor;       /* kept from the source */
    uint32_t serial = 0;      /* the result's stream serial number */
};
struct RemuxStats {
    uint64_t packets = 0, pages = 0, bytes = 0;
    uint32_t preskip = 0;     /* the result's pre-skip */
    uint64_t granule_end = 0; /* the result's last granule */
};
/* Copies the packets that cover the range (plus kRemuxPreroll48 of pre-roll before it) into a new Ogg Opus stream on `sink`:
 * OpusHead with the new pre-skip, OpusTags, then pages of whole packets with granules counted from 0 and an
 * end-of-stream page whose granule trims the end. Sample exact at the decoder's output rate. 0, -errno, kCodecBadStream,
 * or -ECANCELED. progress(frames done, frames). */
int remux_opus_range(OggOpusFile &src, const RemuxRequest &req, ByteSink &sink, RemuxStats *stats,
                     const std::function<void(uint64_t, uint64_t)> &progress, const std::atomic<bool> *cancel);

/* The output rate Cassette decodes an Opus stream at: its input rate when that is one of Opus' own rates (8, 12, 16,
 * 24, 48 kHz), so a take plays at the rate it was recorded, else 48 kHz. */
uint32_t decode_rate(uint32_t input_rate);

}  // namespace oggopus
}  // namespace vinyl
