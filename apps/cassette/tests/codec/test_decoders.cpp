/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests (phase C): the decoders on the FAKE libraries (tests/fake_decoders.hpp) and the test encoders'
 * streams (tests/fixture_codecs.hpp), so every position is checked to the sample:
 *   - Opus framing: packet durations from the TOC, OpusHead / OpusTags round trips;
 *   - FlacDecoder: linear decode, a seek sweep (frame edges, the end), a frame lost to damage (silence keeps the
 *     positions), a STREAMINFO that says "unknown", a seek that fails once, read errors;
 *   - OpusFileDecoder: linear decode against an independent reference, seek and seek_near sweeps at 48, 24 and 16 kHz
 *     output and from a 44.1 kHz input, bisection on a 20-minute stream (probes and bytes per seek), a damaged page, a
 *     packet that does not decode, a stream without its end-of-stream page;
 *   - DecodedSource: forward, reverse-scan and random read patterns, with the decode cost of each;
 *   - remux_opus_range (Opus trim): many ranges at two rates, the result read back by the scanner, the list and the
 *     decoder, sample exact;
 *   - make_decoder and the notice texts.
 */
#include "check.hpp"

#include "../fake_decoders.hpp"
#include "../fixture_codecs.hpp"
#include "decoded_source.hpp"
#include "decoder.hpp"
#include "flac_decoder.hpp"
#include "ogg_opus.hpp"
#include "opus_decoder.hpp"
#include "stream_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cassette;

namespace {

using Bytes = std::vector<unsigned char>;

struct Tmp {
    std::string dir;
    Tmp() {
        std::string t = (std::filesystem::temp_directory_path() / "cassette-dec-XXXXXX").string();
        std::vector<char> b(t.begin(), t.end());
        b.push_back('\0');
        dir = mkdtemp(b.data()) ? std::string(b.data()) : std::string("/tmp");
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string put(const std::string &name, const Bytes &b) const {
        const std::string p = dir + "/" + name;
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
        return p;
    }
};

AudioFormat fmt_of(uint32_t rate, uint16_t ch, uint16_t bits) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

Bytes flac_stream(const AudioFormat &f, uint64_t frames, uint32_t block = 4096, bool finish = true,
                  const Tags &tags = {{"TITLE", "take_001_nodate"}, {"ENCODER", "Cassette"}}) {
    MemoryByteSink s;
    fx::VerbatimFlac e(f, block, 64);
    e.start(s, tags);
    const Bytes pcm = fx::pcm(f, 0, frames);
    e.write(pcm.data(), static_cast<size_t>(frames));
    if (finish) e.finish();
    return s.data;
}

Bytes opus_stream(const AudioFormat &f, uint64_t frames, unsigned page_packets = 50, uint32_t preskip = 312,
                  unsigned packet_bytes = 80, const Tags &tags = {{"TITLE", "take_002_nodate"}, {"ENCODER", "Cassette"}}) {
    MemoryByteSink s;
    fx::PlainOggOpus e(f, packet_bytes, page_packets, preskip);
    e.start(s, tags);
    const Bytes pcm(static_cast<size_t>(48000) * f.channels * 2, 0); /* the test encoder ignores the content */
    for (uint64_t done = 0; done < frames;) {
        const uint64_t n = std::min<uint64_t>(48000, frames - done);
        e.write(pcm.data(), static_cast<size_t>(n));
        done += n;
    }
    e.finish();
    return s.data;
}

/* `frames` of a decoder's output read in chunks of `chunk` from where it stands. */
Bytes read_all(Decoder &d, uint64_t frames, size_t align, size_t chunk = 1000) {
    Bytes out(static_cast<size_t>(frames) * align);
    uint64_t got = 0;
    while (got < frames) {
        const long n = d.read(out.data() + got * align, static_cast<size_t>(std::min<uint64_t>(chunk, frames - got)));
        if (n <= 0) break;
        got += static_cast<uint64_t>(n);
    }
    out.resize(static_cast<size_t>(got) * align);
    return out;
}

Bytes s16_bytes(const std::vector<int16_t> &v) {
    Bytes b;
    b.reserve(v.size() * 2);
    for (int16_t s : v) {
        b.push_back(static_cast<unsigned char>(static_cast<uint16_t>(s) & 0xFF));
        b.push_back(static_cast<unsigned char>(static_cast<uint16_t>(s) >> 8));
    }
    return b;
}

Bytes slice(const Bytes &b, size_t align, uint64_t a, uint64_t n) {
    const size_t lo = std::min(b.size(), static_cast<size_t>(a) * align);
    const size_t hi = std::min(b.size(), static_cast<size_t>(a + n) * align);
    return Bytes(b.begin() + static_cast<long>(lo), b.begin() + static_cast<long>(hi));
}

/* The fake decoder's output frames [a, a + n) of a 48 kHz mono stream from packets read in order (no seek): the
 * reference for long files, where expected_opus_pcm() would need the whole take in memory. */
Bytes expected_range48(const std::string &path, uint64_t a, uint64_t n) {
    std::vector<int16_t> v;
    oggopus::OggOpusFile f(posix_file_ops());
    if (f.open(path) != 0) return Bytes();
    const int64_t t0 = f.start48() + f.head().preskip + static_cast<int64_t>(a);
    const int64_t t1 = t0 + static_cast<int64_t>(n);
    oggopus::Packet p;
    while (f.next(p) == 1 && p.start48 < t1) {
        if (p.end48 <= t0) continue;
        for (int64_t t = std::max(p.start48, t0); t < std::min(p.end48, t1); ++t)
            v.push_back(fakedec::fake_opus_sample(p.data.data(), p.data.size(), static_cast<uint32_t>(t - p.start48), 0));
    }
    return s16_bytes(v);
}

uint64_t mix(uint64_t x) {
    x = (x + 1) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return x;
}

/* A FileOps over the real one whose pread fails with EIO once `fail_after` reads have happened (< 0: never). */
class FlakyOps final : public FileOps {
public:
    FileOps &b = posix_file_ops();
    int fail_after = -1;
    int reads = 0;
    int open(const std::string &p, int f, int m) override { return b.open(p, f, m); }
    int close(int fd) override { return b.close(fd); }
    ssize_t write(int fd, const void *x, size_t n) override { return b.write(fd, x, n); }
    ssize_t pwrite(int fd, const void *x, size_t n, uint64_t o) override { return b.pwrite(fd, x, n, o); }
    ssize_t pread(int fd, void *x, size_t n, uint64_t o) override {
        if (fail_after >= 0 && reads++ >= fail_after) return -EIO;
        return b.pread(fd, x, n, o);
    }
    int fdatasync(int fd) override { return b.fdatasync(fd); }
    int fsync(int fd) override { return b.fsync(fd); }
    int ftruncate(int fd, uint64_t l) override { return b.ftruncate(fd, l); }
    int fstat(int fd, StatInfo &s) override { return b.fstat(fd, s); }
    int stat(const std::string &p, StatInfo &s) override { return b.stat(p, s); }
    int rename(const std::string &x, const std::string &y) override { return b.rename(x, y); }
    int unlink(const std::string &p) override { return b.unlink(p); }
    int mkdir(const std::string &p) override { return b.mkdir(p); }
    int fsync_dir(const std::string &p) override { return b.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return b.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &a) override { return b.free_space(p, a); }
};

/* ---------------------------------------------------------------- framing */

void t_packet_durations() {
    auto d = [](std::initializer_list<unsigned char> p) {
        const std::vector<unsigned char> v(p);
        return oggopus::packet_samples48(v.data(), v.size());
    };
    CHECK_EQ(d({0x00}), 480u);              /* SILK NB 10 ms */
    CHECK_EQ(d({0x08}), 960u);              /* SILK NB 20 ms */
    CHECK_EQ(d({0x10}), 1920u);             /* 40 ms */
    CHECK_EQ(d({0x18}), 2880u);             /* 60 ms */
    CHECK_EQ(d({0x60}), 480u);              /* hybrid SWB 10 ms (config 12) */
    CHECK_EQ(d({0x68}), 960u);              /* config 13 */
    CHECK_EQ(d({0x78}), 960u);              /* config 15: hybrid FB 20 ms */
    CHECK_EQ(d({0x80}), 120u);              /* CELT NB 2.5 ms (config 16) */
    CHECK_EQ(d({0x88}), 240u);              /* 5 ms */
    CHECK_EQ(d({0xF8}), 960u);              /* CELT FB 20 ms (config 31): the test encoder's packets */
    CHECK_EQ(d({0xFC}), 960u);              /* stereo flag does not change the duration */
    CHECK_EQ(d({0xF9}), 1920u);             /* code 1: two frames */
    CHECK_EQ(d({0xFA}), 1920u);             /* code 2: two frames */
    CHECK_EQ(d({0xFB, 0x03}), 2880u);       /* code 3: three frames */
    CHECK_EQ(d({0xFB, 0x06}), 5760u);       /* 120 ms, the maximum */
    CHECK_EQ(d({0xFB, 0x07}), 0u);          /* 140 ms: invalid */
    CHECK_EQ(d({0x1B, 0x03}), 0u);          /* 3 x 60 ms: invalid */
    CHECK_EQ(d({0xFB, 0x00}), 0u);          /* zero frames */
    CHECK_EQ(d({0xFB}), 0u);                /* code 3 without its count byte */
    CHECK_EQ(oggopus::packet_samples48(nullptr, 0), 0u);
    CHECK_EQ(oggopus::decode_rate(16000), 16000u);
    CHECK_EQ(oggopus::decode_rate(44100), 48000u);
    CHECK_EQ(oggopus::decode_rate(0), 48000u);
}

void t_headers() {
    oggopus::Head h;
    h.channels = 2;
    h.preskip = 3840;
    h.input_rate = 16000;
    h.gain_q8 = -256;
    const Bytes b = oggopus::build_head(h);
    CHECK_EQ(b.size(), 19u);
    oggopus::Head g;
    CHECK(oggopus::parse_head(b.data(), b.size(), g));
    CHECK(g.channels == 2 && g.preskip == 3840 && g.input_rate == 16000 && g.gain_q8 == -256 && g.mapping_family == 0);
    Bytes bad = b;
    bad[9] = 3; /* family 0 with 3 channels */
    std::string why;
    CHECK(!oggopus::parse_head(bad.data(), bad.size(), g, &why));
    CHECK(!why.empty());
    bad = b;
    bad[8] = 0x10; /* version 16: another major version */
    CHECK(!oggopus::parse_head(bad.data(), bad.size(), g));
    const Tags t = {{"TITLE", "a=b"}, {"DATE", "2026-11-10"}};
    const Bytes tb = oggopus::build_tags("Cassette vendor", t, 7);
    std::string v;
    Tags u;
    CHECK(oggopus::parse_tags(tb.data(), tb.size(), v, u));
    CHECK_EQ(v, "Cassette vendor");
    CHECK(u.size() == 2 && u[0].name == "TITLE" && u[0].value == "a=b" && u[1].value == "2026-11-10");
    CHECK(!oggopus::parse_tags(tb.data(), 20, v, u));
    /* a page round trip: CRC valid, read back */
    Tmp tmp;
    const Bytes pg = oggopus::build_page(0x02, 0, 77, 0, {b});
    const std::string p = tmp.put("p.ogg", pg);
    const int fd = posix_file_ops().open(p, O_RDONLY, 0);
    oggopus::PageReader rd(posix_file_ops(), fd, pg.size());
    oggopus::Page page;
    CHECK(rd.read_at(0, page, true));
    CHECK(page.serial == 77 && page.flags == 0x02 && page.body == b && page.end() == pg.size());
    CHECK(rd.find_next(0, pg.size(), page, false));
    CHECK(!rd.find_next(1, pg.size(), page, false));
    posix_file_ops().close(fd);
}

/* ---------------------------------------------------------------- FLAC */

void flac_case(const AudioFormat &f, uint64_t frames, uint32_t block) {
    Tmp tmp;
    const std::string path = tmp.put("take_001_nodate.flac", flac_stream(f, frames, block));
    const FlacDecLib lib = fakedec::make_flac_dec_lib();
    const size_t align = static_cast<size_t>(f.channels) * (f.bits / 8u);
    const Bytes ref = fx::pcm(f, 0, frames);
    {
        FlacDecoder d(&lib);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
        CHECK(info.kind == DecodedKind::Flac);
        CHECK_EQ(info.total_frames, frames);
        CHECK(info.frames_known && !info.damaged);
        CHECK(info.output.rate == f.rate && info.output.channels == f.channels && info.output.bits == f.bits);
        CHECK(info.tags.size() == 2 && info.tags[0].name == "TITLE" && info.tags[0].value == "take_001_nodate");
        CHECK_EQ(info.vendor, "cassette test");
        CHECK_EQ(fakedec::fake_flac_dec_state().md5, 0); /* MD5 checking off */
        const Bytes all = read_all(d, frames + 5000, align, 777);
        CHECK(all == ref);
        CHECK_EQ(d.position(), frames);
        /* seeks: the edges of frames, the start, the end, and pseudo-random positions */
        std::vector<uint64_t> at = {0, 1, block - 1, block, block + 1, 2 * block - 1, 2 * block, frames - 1, frames - 100};
        for (int i = 0; i < 40; ++i) at.push_back(mix(static_cast<uint64_t>(i) * 31 + frames) % frames);
        int bad = 0;
        for (uint64_t a : at) {
            if (d.seek(a) != 0) {
                ++bad;
                continue;
            }
            CHECK_EQ(d.position(), a);
            const Bytes got = read_all(d, 1500, align, 333);
            if (got != slice(ref, align, a, 1500)) ++bad;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(d.seek(frames), 0);
        Bytes one(align * 4);
        CHECK_EQ(d.read(one.data(), 4), 0L);
        CHECK_EQ(d.seek(frames + 10), 0);
        CHECK_EQ(d.read(one.data(), 4), 0L);
        CHECK(d.seek(5) == 0 && d.read(one.data(), 4) == 4 && one == slice(ref, align, 5, 4));
        uint64_t n = 0;
        CHECK(d.scan_length(n) == 0 && n == frames);
    }
    CHECK_EQ(fakedec::fake_flac_dec_state().news, fakedec::fake_flac_dec_state().deletes);
}

void t_flac() {
    flac_case(fmt_of(48000, 1, 16), 30001, 4096);
    flac_case(fmt_of(44100, 2, 16), 20000, 4096);
    flac_case(fmt_of(48000, 2, 24), 17000, 4096);
    flac_case(fmt_of(16000, 1, 16), 9000, 1152);
}

void t_flac_damage() {
    Tmp tmp;
    const AudioFormat f = fmt_of(48000, 1, 16);
    const uint64_t frames = 6 * 4096 + 100;
    Bytes s = flac_stream(f, frames);
    /* a byte in the middle of the third frame's samples: its CRC-16 fails */
    const StreamScan before = [&] {
        const std::string p = tmp.put("x.flac", s);
        return scan_flac(posix_file_ops(), p);
    }();
    const size_t frame_bytes = 1 + 4096 * 2 + 2 + 6; /* header (6) + subframe header (1) + samples + CRC */
    s[static_cast<size_t>(before.audio_offset) + 2 * frame_bytes + 500] ^= 0x5A;
    const std::string path = tmp.put("take_003_nodate.flac", s);
    const FlacDecLib lib = fakedec::make_flac_dec_lib();
    FlacDecoder d(&lib);
    DecodedInfo info;
    CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
    CHECK_EQ(info.total_frames, frames);
    const Bytes all = read_all(d, frames, 2);
    CHECK_EQ(all.size(), static_cast<size_t>(frames) * 2);
    const Bytes ref = fx::pcm(f, 0, frames);
    CHECK(slice(all, 2, 0, 2 * 4096) == slice(ref, 2, 0, 2 * 4096));
    CHECK(slice(all, 2, 2 * 4096, 4096) == Bytes(4096 * 2, 0)); /* the lost frame: silence */
    CHECK(slice(all, 2, 3 * 4096, frames - 3 * 4096) == slice(ref, 2, 3 * 4096, frames - 3 * 4096));
    CHECK_EQ(fakedec::fake_flac_dec_state().crc_errors, 1);
    CHECK_EQ(d.stats().gaps, 4096u);
}

void t_flac_unknown_and_errors() {
    Tmp tmp;
    const AudioFormat f = fmt_of(48000, 1, 16);
    /* never finished: STREAMINFO total 0; the scan counts the 3 whole frames */
    const std::string path = tmp.put("take_004_nodate.flac", flac_stream(f, 3 * 4096 + 50, 4096, false));
    FlacDecLib lib = fakedec::make_flac_dec_lib();
    {
        FlacDecoder d(&lib);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
        CHECK(!info.frames_known && info.damaged);
        CHECK_EQ(info.total_frames, 3u * 4096u);
        CHECK(read_all(d, 99999, 2) == fx::pcm(f, 0, 3 * 4096));
    }
    /* a seek that fails once (SEEK_ERROR): flush and the second try lands */
    {
        fakedec::fake_flac_dec_state().fail_seek_once_at = fakedec::fake_flac_dec_state().seeks;
        FlacDecoder d(&lib);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
        const int flushes = fakedec::fake_flac_dec_state().flushes;
        CHECK_EQ(d.seek(5000), 0);
        CHECK_EQ(fakedec::fake_flac_dec_state().flushes, flushes + 1);
        Bytes b(20);
        CHECK(d.read(b.data(), 10) == 10 && b == slice(fx::pcm(f, 0, 6000), 2, 5000, 10));
    }
    /* read errors: open fails or read fails, never a crash or a hang */
    for (int n = 0; n < 12; ++n) {
        FlakyOps fo;
        fo.fail_after = n;
        FlacDecoder d(&lib);
        DecodedInfo info;
        const int r = d.open(fo, path, info);
        if (r == 0) {
            Bytes b(3 * 4096 * 2);
            const long k = d.read(b.data(), 3 * 4096);
            CHECK(k < 0 || k <= 3 * 4096);
        } else {
            CHECK(r < 0);
        }
    }
    /* no library, not FLAC */
    FlacDecoder none(nullptr);
    DecodedInfo info;
    CHECK_EQ(none.open(posix_file_ops(), path, info), kCodecMissing);
    FlacDecoder d(&lib);
    CHECK_EQ(d.open(posix_file_ops(), tmp.put("x.flac", Bytes(100, 7)), info), kCodecBadStream);
}

/* ---------------------------------------------------------------- Opus */

struct OpusCase {
    const char *name;
    AudioFormat f;
    uint64_t frames;
    unsigned page_packets;
    uint32_t preskip;
};

void opus_case(const OpusCase &c) {
    Tmp tmp;
    const std::string path = tmp.put("take_002_nodate.opus", opus_stream(c.f, c.frames, c.page_packets, c.preskip));
    const OpusDecLib lib = fakedec::make_opus_dec_lib();
    const std::vector<int16_t> refv = fakedec::expected_opus_pcm(posix_file_ops(), path);
    const Bytes ref = s16_bytes(refv);
    const uint32_t rate = oggopus::decode_rate(c.f.rate);
    const size_t align = 2u * c.f.channels;
    const uint64_t total = ref.size() / align;
    {
        OpusFileDecoder d(&lib);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
        CHECK(info.kind == DecodedKind::OggOpus);
        CHECK(info.output.rate == rate && info.output.channels == c.f.channels && info.output.bits == 16);
        CHECK_EQ(info.source.rate, c.f.rate);
        CHECK_EQ(info.total_frames, total);
        CHECK(info.frames_known && !info.damaged);
        CHECK(info.tags.size() == 2 && info.tags[0].value == "take_002_nodate");
        CHECK_EQ(fakedec::fake_opus_dec_state().rate, static_cast<int32_t>(rate));
        /* the length the list shows (at the input rate) is the decoder's when the input rate is an Opus rate */
        const StreamInfo si = read_stream_info(posix_file_ops(), path);
        if (rate == c.f.rate) CHECK_EQ(si.frames, total);
        /* the encoder's input length, at the output rate */
        const uint64_t in_out = c.frames * rate / c.f.rate;
        CHECK(total + 1 >= in_out && total <= in_out + 1);
        CHECK(read_all(d, total + 100, align, 1234) == ref);
        CHECK_EQ(d.position(), total);
        const uint64_t page = 960u / (48000u / rate) * c.page_packets;
        std::vector<uint64_t> at = {0, 1, 2, page - 1, page, page + 1, 3 * page + 7, total - 1, total - 960, 3839, 3840};
        for (int i = 0; i < 60; ++i) at.push_back(mix(static_cast<uint64_t>(i) + total) % total);
        int bad = 0, bad_near = 0;
        for (uint64_t a : at) {
            if (a >= total) continue;
            if (d.seek(a) != 0 || d.position() != a) {
                ++bad;
                continue;
            }
            if (read_all(d, 2000, align, 300) != slice(ref, align, a, 2000)) ++bad;
            uint64_t landed = 0;
            if (d.seek_near(a, landed) != 0 || landed > a || d.position() != landed) {
                ++bad_near;
                continue;
            }
            if (read_all(d, a - landed + 500, align, 999) != slice(ref, align, landed, a - landed + 500)) ++bad_near;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(bad_near, 0);
        if (bad || bad_near) std::fprintf(stderr, "  opus case %s: %d seek, %d seek_near failures\n", c.name, bad, bad_near);
        CHECK(fakedec::fake_opus_dec_state().resets >= static_cast<int>(at.size()));
        CHECK_EQ(d.seek(total), 0);
        Bytes b(64);
        CHECK_EQ(d.read(b.data(), 4), 0L);
    }
    CHECK_EQ(fakedec::fake_opus_dec_state().creates, fakedec::fake_opus_dec_state().destroys);
}

void t_opus() {
    opus_case({"48k mono", fmt_of(48000, 1, 16), 48000 * 3 + 17, 50, 312});
    opus_case({"16k stereo small pages", fmt_of(16000, 2, 16), 16000 * 4 + 5, 3, 312});
    opus_case({"24k mono odd pre-skip", fmt_of(24000, 1, 16), 24000 * 2 + 1, 7, 311});
    opus_case({"44.1k input plays at 48k", fmt_of(44100, 1, 16), 44100 * 2, 10, 312});
    opus_case({"8k mono one packet per page", fmt_of(8000, 1, 16), 8000 * 2, 1, 3840});
}

/* A 20-minute stream: seeks find their page by bisection with few probes and few bytes. */
void t_opus_bisection() {
    Tmp tmp;
    const AudioFormat f = fmt_of(48000, 1, 16);
    const uint64_t frames = 48000ull * 60 * 20;
    const std::string path = tmp.put("long.opus", opus_stream(f, frames, 50, 312, 40));
    const OpusDecLib lib = fakedec::make_opus_dec_lib();
    OpusFileDecoder d(&lib);
    DecodedInfo info;
    CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
    CHECK_EQ(info.total_frames, frames);
    const oggopus::OggOpusFile::Stats s0 = d.file()->stats();
    int bad = 0;
    for (int i = 0; i < 100; ++i) {
        const uint64_t a = mix(static_cast<uint64_t>(i) * 977) % frames;
        if (d.seek(a) != 0) {
            ++bad;
            continue;
        }
        Bytes b(960 * 2);
        if (d.read(b.data(), 960) != 960) ++bad;
    }
    CHECK_EQ(bad, 0);
    const oggopus::OggOpusFile::Stats s1 = d.file()->stats();
    const double probes = static_cast<double>(s1.probes - s0.probes) / 100.0;
    const double kb = static_cast<double>(s1.bytes - s0.bytes) / 100.0 / 1024.0;
    std::printf("  Opus seek on a 20-minute take (%zu KB, 1200 pages): %.1f probes and %.0f KB read per seek\n",
                static_cast<size_t>(std::filesystem::file_size(path) / 1024), probes, kb);
    CHECK(probes < 40.0);
    CHECK(kb < 600.0);
    /* exactness on the long file: seeks against the reference read in order */
    for (uint64_t a : {uint64_t(0), uint64_t(1000), frames / 3 + 11, frames / 2 + 959, frames - 5000}) {
        CHECK_EQ(d.seek(a), 0);
        const Bytes got = read_all(d, 3000, 2);
        CHECK(got == expected_range48(path, a, 3000));
    }
}

void t_opus_damage() {
    Tmp tmp;
    const AudioFormat f = fmt_of(48000, 1, 16);
    const uint64_t frames = 48000 * 4;
    const Bytes good = opus_stream(f, frames, 10);
    const std::string gp = tmp.put("good.opus", good);
    const std::vector<int16_t> ref = fakedec::expected_opus_pcm(posix_file_ops(), gp);
    /* corrupt the 5th audio page: its 10 packets (9600 samples) become silence, everything else keeps its place */
    oggopus::OggOpusFile of(posix_file_ops());
    CHECK_EQ(of.open(gp), 0);
    const uint64_t audio = of.audio_offset();
    of.close();
    const int fd = posix_file_ops().open(gp, O_RDONLY, 0);
    oggopus::PageReader rd(posix_file_ops(), fd, good.size());
    oggopus::Page pg;
    uint64_t off = audio;
    for (int i = 0; i < 4; ++i) {
        CHECK(rd.read_at(off, pg, false));
        off = pg.end();
    }
    CHECK(rd.read_at(off, pg, false));
    posix_file_ops().close(fd);
    const uint64_t lost_first = pg.granule - 312 - 9600, lost_end = pg.granule - 312;
    Bytes bad = good;
    bad[static_cast<size_t>(off + pg.header_len + 40)] ^= 0x33;
    const std::string bp = tmp.put("bad.opus", bad);
    const OpusDecLib lib = fakedec::make_opus_dec_lib();
    OpusFileDecoder d(&lib);
    DecodedInfo info;
    CHECK_EQ(d.open(posix_file_ops(), bp, info), 0);
    CHECK_EQ(info.total_frames, ref.size());
    const Bytes all = read_all(d, frames, 2);
    const Bytes r = s16_bytes(ref);
    CHECK_EQ(all.size(), r.size());
    CHECK(slice(all, 2, 0, lost_first) == slice(r, 2, 0, lost_first));
    CHECK(slice(all, 2, lost_first, lost_end - lost_first) == Bytes(static_cast<size_t>(lost_end - lost_first) * 2, 0));
    CHECK(slice(all, 2, lost_end, frames) == slice(r, 2, lost_end, frames));
    /* a seek into the hole and past it */
    CHECK_EQ(d.seek(lost_first + 100), 0);
    CHECK(read_all(d, 200, 2) == Bytes(400, 0));
    CHECK_EQ(d.seek(lost_end + 3), 0);
    CHECK(read_all(d, 200, 2) == slice(r, 2, lost_end + 3, 200));

    /* a packet libopus refuses: concealed (the fake's PLC is silence), the rest in place */
    {
        fakedec::fake_opus_dec_state().fail_decode_at = fakedec::fake_opus_dec_state().decodes + 7;
        OpusFileDecoder e(&lib);
        CHECK_EQ(e.open(posix_file_ops(), gp, info), 0);
        const Bytes a2 = read_all(e, frames, 2);
        CHECK_EQ(e.stats().concealed, 1u);
        /* packet 7 covers 48 kHz samples [6720, 7680): output frames [6408, 7368) */
        CHECK(slice(a2, 2, 0, 6408) == slice(r, 2, 0, 6408));
        CHECK(slice(a2, 2, 6408, 960) == Bytes(960 * 2, 0));
        CHECK(slice(a2, 2, 7368, frames) == slice(r, 2, 7368, frames));
    }
    /* no end-of-stream page (a cut take before its repair): plays to the last page, marked damaged */
    {
        Bytes cut = good;
        oggopus::OggOpusFile g(posix_file_ops());
        CHECK_EQ(g.open(gp), 0);
        CHECK(g.eos());
        g.close();
        cut.resize(cut.size() - 300); /* the end-of-stream page cut */
        const std::string cp = tmp.put("cut.opus", cut);
        OpusFileDecoder e(&lib);
        CHECK_EQ(e.open(posix_file_ops(), cp, info), 0);
        CHECK(info.damaged);
        CHECK(info.total_frames > 0 && info.total_frames < frames);
        const Bytes a3 = read_all(e, frames, 2);
        CHECK(a3 == slice(r, 2, 0, info.total_frames));
    }
    /* header only, not Opus, no library */
    {
        MemoryByteSink s;
        fx::PlainOggOpus e(f);
        e.start(s, {});
        OpusFileDecoder h(&lib);
        CHECK_EQ(h.open(posix_file_ops(), tmp.put("empty.opus", s.data), info), 0);
        CHECK_EQ(info.total_frames, 0u);
        Bytes b(8);
        CHECK_EQ(h.read(b.data(), 2), 0L);
        OpusFileDecoder n(&lib);
        CHECK_EQ(n.open(posix_file_ops(), tmp.put("junk.opus", Bytes(500, 'x')), info), kCodecBadStream);
        OpusFileDecoder m(nullptr);
        CHECK_EQ(m.open(posix_file_ops(), gp, info), kCodecMissing);
    }
}

/* ---------------------------------------------------------------- DecodedSource */

struct Pattern {
    uint64_t requested = 0, decoded = 0, seeks = 0;
    int bad = 0;
};

Pattern reverse_scan(DecodedSource &s, const Bytes &ref, size_t block) {
    Pattern p;
    const size_t align = s.block_align();
    uint64_t pos = s.frames();
    Bytes buf;
    const DecodedSource::Stats a = s.stats();
    while (pos > 0) {
        const uint64_t n = std::min<uint64_t>(pos, block);
        buf.resize(static_cast<size_t>(n) * align);
        if (s.read(pos - n, static_cast<size_t>(n), buf.data()) != static_cast<long>(n) || buf != slice(ref, align, pos - n, n))
            ++p.bad;
        pos -= n;
    }
    const DecodedSource::Stats b = s.stats();
    p.requested = b.requested - a.requested;
    p.decoded = b.decoded - a.decoded;
    p.seeks = b.seeks - a.seeks;
    return p;
}

void t_decoded_source() {
    Tmp tmp;
    const FlacDecLib fl = fakedec::make_flac_dec_lib();
    const OpusDecLib ol = fakedec::make_opus_dec_lib();
    DecoderLibs libs;
    libs.flac = &fl;
    libs.opus = &ol;
    struct Src {
        const char *name;
        std::string path;
        Bytes ref;
    };
    const AudioFormat f48 = fmt_of(48000, 2, 16);
    std::vector<Src> srcs;
    srcs.push_back({"FLAC", tmp.put("a.flac", flac_stream(f48, 48000 * 6 + 3)), fx::pcm(f48, 0, 48000 * 6 + 3)});
    const std::string op = tmp.put("b.opus", opus_stream(f48, 48000 * 6, 50));
    srcs.push_back({"Opus", op, s16_bytes(fakedec::expected_opus_pcm(posix_file_ops(), op))});
    for (const Src &x : srcs) {
        DecodedSource s(posix_file_ops(), &libs);
        CHECK_EQ(s.open(x.path), 0);
        const size_t align = s.block_align();
        CHECK_EQ(s.frames() * align, x.ref.size());
        /* forward, as normal play reads: no seek, nothing decoded twice */
        Bytes buf(1024 * align);
        int bad = 0;
        for (uint64_t pos = 0; pos < s.frames(); pos += 1024) {
            const long n = s.read(pos, 1024, buf.data());
            if (n <= 0 || Bytes(buf.begin(), buf.begin() + n * static_cast<long>(align)) != slice(x.ref, align, pos, static_cast<uint64_t>(n)))
                ++bad;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(s.stats().seeks, 0u);
        CHECK_EQ(s.stats().decoded, s.frames());
        /* the reverse 5x scan: blocks of 5 periods read backwards */
        const Pattern r = reverse_scan(s, x.ref, 5 * 1024);
        CHECK_EQ(r.bad, 0);
        const double ratio = static_cast<double>(r.decoded) / static_cast<double>(r.requested);
        std::printf("  %s reverse scan (6 s, fakes): %llu seeks, %.2f frames decoded per frame read\n", x.name,
                    static_cast<unsigned long long>(r.seeks), ratio);
        CHECK(ratio < (std::string(x.name) == "Opus" ? 3.0 : 1.6));
        /* jumps and random reads */
        bad = 0;
        for (int i = 0; i < 50; ++i) {
            const uint64_t a = mix(static_cast<uint64_t>(i) * 7) % s.frames();
            const size_t n = static_cast<size_t>(mix(static_cast<uint64_t>(i)) % 9000) + 1;
            buf.resize(n * align);
            const long got = s.read(a, n, buf.data());
            const uint64_t want = std::min<uint64_t>(n, s.frames() - a);
            if (got != static_cast<long>(want) || Bytes(buf.begin(), buf.begin() + got * static_cast<long>(align)) != slice(x.ref, align, a, want))
                ++bad;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(s.read(s.frames(), 10, buf.data()), 0L);
    }
    /* WAV and missing libraries */
    DecodedSource none(posix_file_ops(), nullptr);
    DecoderLibs empty;
    DecodedSource s2(posix_file_ops(), &empty);
    CHECK_EQ(s2.open(srcs[0].path), kCodecMissing);
    CHECK(!s2.is_open());
}

/* ---------------------------------------------------------------- remux (Opus trim) */

void remux_case(const AudioFormat &f, uint64_t frames, unsigned page_packets) {
    Tmp tmp;
    const std::string src = tmp.put("take_005_nodate.opus", opus_stream(f, frames, page_packets));
    const OpusDecLib lib = fakedec::make_opus_dec_lib();
    DecoderLibs libs;
    libs.opus = &lib;
    const Bytes ref = s16_bytes(fakedec::expected_opus_pcm(posix_file_ops(), src));
    const size_t align = 2u * f.channels;
    const uint64_t total = ref.size() / align;
    const unsigned k = 48000u / oggopus::decode_rate(f.rate);
    std::vector<std::pair<uint64_t, uint64_t>> ranges = {{0, total},
                                                         {0, 1},
                                                         {1, 1},
                                                         {0, 480 / k},
                                                         {3839 / k, 1000},
                                                         {3840 / k, 1000},
                                                         {48000 / k - 1, 2},
                                                         {total - 1, 1},
                                                         {total - 5000 / k, 5000 / k},
                                                         {total / 3, total / 3}};
    for (int i = 0; i < 20; ++i) {
        const uint64_t a = mix(static_cast<uint64_t>(i) * 13 + frames) % (total - 1);
        const uint64_t n = 1 + mix(static_cast<uint64_t>(i) * 17) % (total - a);
        ranges.push_back({a, n});
    }
    int bad = 0;
    uint32_t max_preskip = 0;
    for (const auto &rg : ranges) {
        oggopus::OggOpusFile in(posix_file_ops());
        CHECK_EQ(in.open(src), 0);
        oggopus::RemuxRequest rq;
        rq.first = rg.first;
        rq.frames = rg.second;
        rq.rate_k = k;
        rq.tags = in.tags();
        rq.tags.push_back(Tag{"COMMENT", "cut"});
        rq.vendor = in.vendor();
        rq.serial = 99;
        MemoryByteSink out;
        oggopus::RemuxStats st;
        uint64_t last_progress = 0;
        bool mono = true;
        const int r = oggopus::remux_opus_range(in, rq, out, &st,
                                                [&](uint64_t d, uint64_t t) {
                                                    if (d < last_progress || t != rg.second) mono = false;
                                                    last_progress = d;
                                                },
                                                nullptr);
        CHECK_EQ(r, 0);
        CHECK(mono);
        max_preskip = std::max(max_preskip, st.preskip);
        const std::string rp = tmp.put("cut.opus", out.data);
        const StreamScan sc = scan_ogg_opus(posix_file_ops(), rp);
        if (!(sc.ok && sc.eos && sc.valid_end == sc.file_size && sc.damaged == 0)) ++bad;
        const StreamInfo si = read_stream_info(posix_file_ops(), rp);
        if (k == 48000u / f.rate && si.frames != rg.second) ++bad;
        OpusFileDecoder d(&lib);
        DecodedInfo info;
        if (d.open(posix_file_ops(), rp, info) != 0 || info.total_frames != rg.second || info.damaged) {
            ++bad;
            continue;
        }
        if (info.tags.size() != 3 || info.tags[2].value != "cut") ++bad;
        const Bytes got = read_all(d, rg.second + 10, align, 2048);
        if (got != slice(ref, align, rg.first, rg.second)) {
            ++bad;
            std::fprintf(stderr, "  remux [%llu, +%llu) at %u Hz: samples differ\n", static_cast<unsigned long long>(rg.first),
                         static_cast<unsigned long long>(rg.second), f.rate);
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(max_preskip <= oggopus::kRemuxPreroll48 + 960 + 312);
    /* a range past the end, an empty one */
    oggopus::OggOpusFile in(posix_file_ops());
    CHECK_EQ(in.open(src), 0);
    oggopus::RemuxRequest rq;
    rq.first = total - 10;
    rq.frames = 11;
    rq.rate_k = k;
    MemoryByteSink out;
    CHECK_EQ(oggopus::remux_opus_range(in, rq, out, nullptr, nullptr, nullptr), kCodecBadStream);
    rq.frames = 0;
    CHECK_EQ(oggopus::remux_opus_range(in, rq, out, nullptr, nullptr, nullptr), kCodecBadStream);
    /* a full card: the sink's error comes back */
    rq.first = 0;
    rq.frames = total;
    MemoryByteSink full;
    full.fail_at = 2000;
    CHECK_EQ(oggopus::remux_opus_range(in, rq, full, nullptr, nullptr, nullptr), -28);
    /* cancel */
    std::atomic<bool> stop{true};
    MemoryByteSink c;
    CHECK_EQ(oggopus::remux_opus_range(in, rq, c, nullptr, nullptr, &stop), -ECANCELED);
}

void t_remux() {
    remux_case(fmt_of(48000, 1, 16), 48000 * 5 + 77, 50);
    remux_case(fmt_of(16000, 2, 16), 16000 * 5, 4);
}

void t_factory() {
    Tmp tmp;
    const FlacDecLib fl = fakedec::make_flac_dec_lib();
    const OpusDecLib ol = fakedec::make_opus_dec_lib();
    DecoderLibs libs;
    libs.flac = &fl;
    libs.opus = &ol;
    const AudioFormat f = fmt_of(48000, 1, 16);
    /* by magic, not by extension */
    const std::string a = tmp.put("odd-name.opus", flac_stream(f, 5000));
    const std::string b = tmp.put("odd-name.flac", opus_stream(f, 5000));
    DecodedInfo info;
    int err = 1;
    auto d = make_decoder(posix_file_ops(), a, libs, info, &err);
    CHECK(d && err == 0 && info.kind == DecodedKind::Flac);
    d = make_decoder(posix_file_ops(), b, libs, info, &err);
    CHECK(d && err == 0 && info.kind == DecodedKind::OggOpus);
    CHECK(is_compressed_take(posix_file_ops(), a) && is_compressed_take(posix_file_ops(), b));
    const std::string w = tmp.put("w.wav", Bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'});
    d = make_decoder(posix_file_ops(), w, libs, info, &err);
    CHECK(!d && err == kCodecBadStream);
    CHECK(!is_compressed_take(posix_file_ops(), w));
    DecoderLibs none;
    d = make_decoder(posix_file_ops(), a, none, info, &err);
    CHECK(!d && err == kCodecMissing);
    d = make_decoder(posix_file_ops(), tmp.dir + "/missing.flac", libs, info, &err);
    CHECK(!d && err == -ENOENT);
    CHECK_EQ(decode_error_text(kCodecMissing, "/r/take_001_x.flac"), "FLAC not installed (libflac14)");
    CHECK_EQ(decode_error_text(kCodecMissing, "take_001_x.OPUS"), "Opus not installed (libopus0)");
    CHECK_EQ(decode_error_text(kCodecBadStream, "x.flac"), codec_error_text(kCodecBadStream));
    CHECK_EQ(decode_error_text(-ENOENT, "x.wav"), std::string(std::strerror(ENOENT)));
}

}  // namespace

void test_decoders() {
    t_packet_durations();
    t_headers();
    t_flac();
    t_flac_damage();
    t_flac_unknown_and_errors();
    t_opus();
    t_opus_bisection();
    t_opus_damage();
    t_decoded_source();
    t_remux();
    t_factory();
}
