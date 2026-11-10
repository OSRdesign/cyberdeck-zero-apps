/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: the encoders over the REAL libFLAC and libopusenc, when they load (else "skipped"). The output
 * is checked by parsing it here: FLAC metadata blocks and STREAMINFO (RFC 9639), Ogg pages with their CRCs (RFC 3533),
 * OpusHead and OpusTags (RFC 7845). Also what a power cut would leave: a copy of the sink taken after flush().
 */
#include "check.hpp"

#include "fileops.hpp"
#include "flac_encoder.hpp"
#include "opus_encoder.hpp"
#include "siggen.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cassette;

namespace {

using Data = std::vector<unsigned char>;

uint32_t le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t le64(const unsigned char *p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

struct FlacInfo {
    bool ok = false;
    uint32_t min_block = 0, max_block = 0, rate = 0;
    unsigned channels = 0, bits = 0;
    uint64_t total = 0;
    bool md5_zero = true;
    std::vector<std::string> comments;
    uint32_t padding = 0;
    size_t audio_offset = 0;
};

FlacInfo parse_flac(const Data &d) {
    FlacInfo f;
    if (d.size() < 8 || std::memcmp(d.data(), "fLaC", 4) != 0) return f;
    size_t pos = 4;
    bool last = false, have_si = false;
    while (!last && pos + 4 <= d.size()) {
        const unsigned type = d[pos] & 0x7F;
        last = d[pos] & 0x80;
        const size_t len = (d[pos + 1] << 16) | (d[pos + 2] << 8) | d[pos + 3];
        const unsigned char *b = d.data() + pos + 4;
        if (pos + 4 + len > d.size()) return f;
        if (type == 0 && len == 34) {
            f.min_block = (b[0] << 8) | b[1];
            f.max_block = (b[2] << 8) | b[3];
            f.rate = (b[10] << 12) | (b[11] << 4) | (b[12] >> 4);
            f.channels = ((b[12] >> 1) & 7) + 1;
            f.bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
            f.total = (static_cast<uint64_t>(b[13] & 0x0F) << 32) | (static_cast<uint64_t>(b[14]) << 24) |
                      (b[15] << 16) | (b[16] << 8) | b[17];
            f.md5_zero = std::all_of(b + 18, b + 34, [](unsigned char c) { return c == 0; });
            have_si = true;
        } else if (type == 4 && len >= 8) {
            size_t q = 4 + le32(b);
            const uint32_t n = q + 4 <= len ? le32(b + q) : 0;
            q += 4;
            for (uint32_t i = 0; i < n && q + 4 <= len; ++i) {
                const uint32_t l = le32(b + q);
                q += 4;
                if (q + l > len) break;
                f.comments.emplace_back(reinterpret_cast<const char *>(b + q), l);
                q += l;
            }
        } else if (type == 1) {
            f.padding = static_cast<uint32_t>(len);
        }
        pos += 4 + len;
    }
    f.audio_offset = pos;
    f.ok = have_si && last;
    return f;
}

uint32_t ogg_crc(const unsigned char *p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
            table[i] = r;
        }
        init = true;
    }
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ table[((crc >> 24) & 0xFF) ^ p[i]];
    return crc;
}

struct OggPage {
    unsigned type = 0;
    uint64_t granule = 0;
    uint32_t serial = 0, seq = 0;
    bool crc_ok = false;
    Data body;
};

/* false when the bytes are not a sequence of whole pages */
bool parse_ogg(const Data &d, std::vector<OggPage> &pages) {
    size_t pos = 0;
    while (pos < d.size()) {
        if (pos + 27 > d.size() || std::memcmp(d.data() + pos, "OggS", 4) != 0) return false;
        const unsigned nseg = d[pos + 26];
        if (pos + 27 + nseg > d.size()) return false;
        size_t body = 0;
        for (unsigned i = 0; i < nseg; ++i) body += d[pos + 27 + i];
        const size_t len = 27 + nseg + body;
        if (pos + len > d.size()) return false;
        OggPage p;
        p.type = d[pos + 5];
        p.granule = le64(d.data() + pos + 6);
        p.serial = le32(d.data() + pos + 14);
        p.seq = le32(d.data() + pos + 18);
        Data copy(d.begin() + static_cast<long>(pos), d.begin() + static_cast<long>(pos + len));
        const uint32_t stored = le32(copy.data() + 22);
        for (size_t k = 22; k < 26; ++k) copy[k] = 0; /* the CRC is computed with its own field zeroed */
        p.crc_ok = ogg_crc(copy.data(), copy.size()) == stored;
        p.body.assign(copy.begin() + 27 + nseg, copy.end());
        pages.push_back(std::move(p));
        pos += len;
    }
    return true;
}

std::vector<std::string> opus_tags(const OggPage &p) {
    std::vector<std::string> out;
    const Data &b = p.body;
    if (b.size() < 16 || std::memcmp(b.data(), "OpusTags", 8) != 0) return out;
    size_t q = 8;
    const uint32_t vl = le32(b.data() + q);
    q += 4 + vl;
    if (q + 4 > b.size()) return out;
    const uint32_t n = le32(b.data() + q);
    q += 4;
    for (uint32_t i = 0; i < n && q + 4 <= b.size(); ++i) {
        const uint32_t l = le32(b.data() + q);
        q += 4;
        if (q + l > b.size()) break;
        out.emplace_back(reinterpret_cast<const char *>(b.data() + q), l);
        q += l;
    }
    return out;
}

bool has(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }

void feed(Encoder &e, const sig::Bytes &pcm, size_t from, size_t to, size_t block) {
    const AudioFormat &f = e.format();
    const size_t fb = f.channels * (f.bits / 8u);
    for (size_t at = from; at < to; at += block) CHECK_EQ(e.write(pcm.data() + at * fb, std::min(block, to - at)), 0);
}

void run_real_flac(const FlacLib &lib) {
    std::printf("  real FLAC: %s %s\n", lib.name.c_str(), lib.version.c_str());
    {
        /* 48 kHz mono 16-bit speech; a crash snapshot after 3 s */
        FlacConfig cfg;
        cfg.fmt = AudioFormat{48000, 1, 16};
        const size_t frames = 48000 * 5;
        const sig::Bytes pcm = sig::speech(cfg.fmt, frames);
        MemoryByteSink sink;
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {{"TITLE", "Real test"}, {"TAKE", "3"}}), 0);
        feed(e, pcm, 0, 48000 * 3, 1024);
        CHECK_EQ(e.flush(), 0);
        const Data cut = sink.data; /* what a power cut after the next fdatasync leaves */
        const EncoderStats mid = e.stats();
        CHECK_EQ(mid.frames_out % 4096, uint64_t(0));
        CHECK(mid.frames_out + 4096 > mid.frames_in);
        const FlacInfo c = parse_flac(cut);
        CHECK(c.ok);
        CHECK_EQ(c.total, uint64_t(0)); /* "unknown" until finish */
        CHECK(c.md5_zero);
        feed(e, pcm, 48000 * 3, frames, 1000);
        CHECK_EQ(e.finish(), 0);
        const FlacInfo f = parse_flac(sink.data);
        CHECK(f.ok);
        CHECK_EQ(f.total, uint64_t(frames));
        CHECK(!f.md5_zero);
        CHECK_EQ(f.rate, 48000u);
        CHECK_EQ(f.channels, 1u);
        CHECK_EQ(f.bits, 16u);
        CHECK_EQ(f.max_block, 4096u);
        CHECK(has(f.comments, "TITLE=Real test"));
        CHECK(has(f.comments, "TAKE=3"));
        CHECK_EQ(f.padding, 4096u);
        CHECK_EQ(e.stats().frames_out, uint64_t(frames));
        CHECK_EQ(sink.size(), uint64_t(sink.data.size()));
        CHECK(sink.size() < frames * 2); /* compressed */
        std::printf("  real FLAC 48k mono speech: %.1f %% of PCM\n", 100.0 * (sink.size() - f.audio_offset) / (frames * 2.0));
    }
    {
        /* 44.1 kHz stereo 24-bit music into a real file through FileOps */
        FlacConfig cfg;
        cfg.fmt = AudioFormat{44100, 2, 24};
        const size_t frames = 44100 * 4;
        const sig::Bytes pcm = sig::music(cfg.fmt, frames);
        FileOps &ops = posix_file_ops();
        const std::string path = (std::filesystem::temp_directory_path() / "cassette-codec-real.flac").string();
        int fd = ops.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        if (fd >= 0) {
            FileByteSink sink(ops, fd);
            FlacEncoder e(&lib, cfg);
            CHECK_EQ(e.start(sink, {{"TITLE", "Stereo"}}), 0);
            feed(e, pcm, 0, frames, 4800);
            CHECK_EQ(ops.fdatasync(fd), 0);
            CHECK_EQ(e.finish(), 0);
            StatInfo st;
            CHECK_EQ(ops.fstat(fd, st), 0);
            CHECK_EQ(st.size, sink.size());
            Data d(st.size);
            CHECK_EQ(ops.pread(fd, d.data(), d.size(), 0), ssize_t(d.size()));
            ops.close(fd);
            ops.unlink(path);
            const FlacInfo f = parse_flac(d);
            CHECK(f.ok);
            CHECK_EQ(f.total, uint64_t(frames));
            CHECK_EQ(f.rate, 44100u);
            CHECK_EQ(f.channels, 2u);
            CHECK_EQ(f.bits, 24u);
            CHECK(!f.md5_zero);
        }
    }
    {
        /* every level and a 16 kHz card parse; an odd rate needs no subset tricks */
        for (unsigned level : {0u, 8u}) {
            FlacConfig cfg;
            cfg.fmt = AudioFormat{16000, 1, 16};
            cfg.level = level;
            const sig::Bytes pcm = sig::speech(cfg.fmt, 16000 * 2);
            MemoryByteSink sink;
            FlacEncoder e(&lib, cfg);
            CHECK_EQ(e.start(sink, {}), 0);
            feed(e, pcm, 0, 32000, 333);
            CHECK_EQ(e.finish(), 0);
            const FlacInfo f = parse_flac(sink.data);
            CHECK(f.ok && f.total == 32000 && f.rate == 16000);
        }
    }
}

void run_real_opus(const OpusEncLib &lib) {
    std::printf("  real Opus: %s %s\n", lib.name.c_str(), lib.version.c_str());
    {
        /* 48 kHz mono speech at 24 kbps; a crash snapshot after 5 s */
        OpusConfig cfg;
        cfg.fmt = AudioFormat{48000, 1, 16};
        cfg.bitrate_bps = 24000;
        cfg.serialno = 4242;
        const size_t frames = 48000 * 8;
        const sig::Bytes pcm = sig::speech(cfg.fmt, frames);
        MemoryByteSink sink;
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {{"TITLE", "Real Opus"}, {"DATE", "2026-11-10"}}), 0);
        CHECK(e.preskip() > 0);
        feed(e, pcm, 0, 48000 * 5, 1024);
        CHECK_EQ(e.flush(), 0);
        const Data cut = sink.data;
        const EncoderStats mid = e.stats();
        std::vector<OggPage> cp;
        CHECK(parse_ogg(cut, cp));
        CHECK(cp.size() >= 3);
        CHECK(std::all_of(cp.begin(), cp.end(), [](const OggPage &p) { return p.crc_ok; }));
        CHECK(!(cp.back().type & 0x04)); /* no end-of-stream yet: a cut stream (RFC 7845: decoders cope) */
        /* the 2 s decision delay: everything older is in the sink after flush() */
        CHECK(mid.frames_out + 96000 + 2 * 960 >= mid.frames_in);
        CHECK(mid.frames_out < mid.frames_in);
        std::printf("  real Opus flush at 5 s: %.2f s in the sink, %.2f s pending\n", mid.frames_out / 48000.0,
                    mid.pending_frames() / 48000.0);
        feed(e, pcm, 48000 * 5, frames, 960);
        CHECK_EQ(e.finish(), 0);
        std::vector<OggPage> pages;
        CHECK(parse_ogg(sink.data, pages));
        CHECK(std::all_of(pages.begin(), pages.end(), [](const OggPage &p) { return p.crc_ok; }));
        CHECK(!pages.empty() && (pages.front().type & 0x02)); /* beginning of stream */
        CHECK(!pages.empty() && (pages.back().type & 0x04));  /* end of stream */
        CHECK(pages.size() >= 2 && pages[0].body.size() >= 19 && std::memcmp(pages[0].body.data(), "OpusHead", 8) == 0);
        if (pages.size() >= 2) {
            const uint32_t preskip = pages[0].body[10] | (pages[0].body[11] << 8);
            CHECK_EQ(preskip, e.preskip());
            CHECK_EQ(le32(pages[0].body.data() + 12), 48000u); /* input rate */
            CHECK_EQ(pages.back().granule - preskip, uint64_t(frames));
            const std::vector<std::string> tags = opus_tags(pages[1]);
            CHECK(has(tags, "TITLE=Real Opus"));
            CHECK(has(tags, "DATE=2026-11-10"));
            CHECK_EQ(pages[0].serial, 4242u);
        }
        CHECK_EQ(e.stats().frames_out, uint64_t(frames));
        CHECK(e.eos_written());
        std::printf("  real Opus 24 kbps speech: %llu bytes for 8 s (target model %llu)\n",
                    static_cast<unsigned long long>(sink.size()),
                    static_cast<unsigned long long>((3000 + 50 + 27) * 8 + 700));
    }
    {
        /* a 16 kHz headset, no decision delay: the sink is within one frame of the input after flush */
        OpusConfig cfg;
        cfg.fmt = AudioFormat{16000, 1, 16};
        cfg.decision_delay_ms = 0;
        const size_t frames = 16000 * 4;
        const sig::Bytes pcm = sig::speech(cfg.fmt, frames);
        MemoryByteSink sink;
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), 0);
        feed(e, pcm, 0, frames, 1600);
        CHECK_EQ(e.flush(), 0);
        CHECK(e.stats().pending_frames() <= 1600u); /* a frame plus the resampler's latency, well under 0.1 s */
        std::printf("  real Opus 16 kHz, decision delay 0: %llu frames pending after flush\n",
                    static_cast<unsigned long long>(e.stats().pending_frames()));
        CHECK_EQ(e.finish(), 0);
        std::vector<OggPage> pages;
        CHECK(parse_ogg(sink.data, pages));
        if (!pages.empty()) {
            const uint32_t preskip = pages[0].body[10] | (pages[0].body[11] << 8);
            CHECK_EQ(pages.back().granule - preskip, uint64_t(frames) * 3); /* 48 kHz granules */
            CHECK_EQ(le32(pages[0].body.data() + 12), 16000u);
        }
    }
    {
        /* 48 kHz stereo 24-bit music at 128 kbps: the size lands near the model */
        OpusConfig cfg;
        cfg.fmt = AudioFormat{48000, 2, 24};
        cfg.bitrate_bps = 128000;
        cfg.application = OpusApplication::Music;
        const size_t frames = 48000 * 10;
        const sig::Bytes pcm = sig::music(cfg.fmt, frames);
        MemoryByteSink sink;
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), 0);
        feed(e, pcm, 0, frames, 4096);
        CHECK_EQ(e.finish(), 0);
        const double model = (16000 + 100 + 27) * 10.0 + 700;
        const double ratio = sink.size() / model;
        std::printf("  real Opus 128 kbps stereo music: %llu bytes for 10 s, %.2f of the model\n",
                    static_cast<unsigned long long>(sink.size()), ratio);
        CHECK(ratio > 0.6 && ratio < 1.3);
        std::vector<OggPage> pages;
        CHECK(parse_ogg(sink.data, pages));
        CHECK(std::all_of(pages.begin(), pages.end(), [](const OggPage &p) { return p.crc_ok; }));
    }
}

}  // namespace

void test_real() {
    std::string why;
    if (const FlacLib *f = real_flac(&why)) run_real_flac(*f);
    else SKIP(("real libFLAC: " + why).c_str());
    why.clear();
    if (const OpusEncLib *o = real_opusenc(&why)) run_real_opus(*o);
    else SKIP(("real libopusenc: " + why).c_str());
    const CodecAvailability a = codec_availability_here();
    CHECK_EQ(a.flac, real_flac() != nullptr);
    CHECK_EQ(a.opus, real_opusenc() != nullptr);
}
