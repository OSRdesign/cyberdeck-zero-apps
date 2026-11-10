/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests (phase C) on the REAL libraries, when they load (else "skipped"): libFLAC for FLAC, libopus for
 * Opus (the files are made with the real encoders, so libopusenc must load too); libopusfile, loaded here for the test
 * only, is the independent reference for Opus.
 *   - FLAC: decode sample exact (16 and 24 bits, mono and stereo); a seek sweep sample exact; the reverse-scan pattern;
 *     trim and split through the edit engine with the real encoder: every result decodes to exactly its slice of the
 *     source, STREAMINFO says its length and libFLAC accepts its MD5;
 *   - Opus: the length exact; the linear decode equals libopusfile's within 1 LSB (the pages, the pre-skip and the end
 *     trimming are read the same way); a seek sweep against the linear decode (lag 0, SNR); trim and split by remux:
 *     libopusfile reads exactly the requested length and decodes to the end without an error, our decoder agrees with
 *     the source's decode of the same range;
 *   - CPU: decode speed, the 5x scan both ways, the cost of a seek (this PC; the Pi Zero 2 W estimate is in FORMATS.md).
 */
#include "check.hpp"

#include "decoded_source.hpp"
#include "decoder.hpp"
#include "fileops.hpp"
#include "flac_decoder.hpp"
#include "flac_encoder.hpp"
#include "ogg_opus.hpp"
#include "opus_decoder.hpp"
#include "opus_encoder.hpp"
#include "siggen.hpp"
#include "stream_scan.hpp"
#include "edit/trim_split.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
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
        std::string t = (std::filesystem::temp_directory_path() / "cassette-decr-XXXXXX").string();
        std::vector<char> b(t.begin(), t.end());
        b.push_back('\0');
        dir = mkdtemp(b.data()) ? std::string(b.data()) : std::string("/tmp");
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

AudioFormat fmt_of(uint32_t rate, uint16_t ch, uint16_t bits) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

uint64_t mix(uint64_t x) {
    x = (x + 1) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return x;
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Encodes `pcm` into `path` with an encoder; frames given. 0 or the encoder's error. */
int encode_to(Encoder &e, const std::string &path, const Bytes &pcm, uint64_t frames, const Tags &tags) {
    FileOps &ops = posix_file_ops();
    const int fd = ops.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fd;
    FileByteSink s(ops, fd, 0);
    int r = e.start(s, tags);
    const size_t fb = static_cast<size_t>(e.format().channels) * (e.format().bits / 8u);
    for (uint64_t done = 0; r == 0 && done < frames;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(4800, frames - done));
        r = e.write(pcm.data() + done * fb, n);
        done += n;
    }
    if (r == 0) r = e.finish();
    ops.close(fd);
    return r;
}

Bytes read_all(Decoder &d, uint64_t frames, size_t align) {
    Bytes out(static_cast<size_t>(frames) * align);
    uint64_t got = 0;
    while (got < frames) {
        const long n = d.read(out.data() + got * align, static_cast<size_t>(std::min<uint64_t>(4096, frames - got)));
        if (n <= 0) break;
        got += static_cast<uint64_t>(n);
    }
    out.resize(static_cast<size_t>(got) * align);
    return out;
}

std::vector<int16_t> as_s16(const Bytes &b) {
    std::vector<int16_t> v(b.size() / 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int16_t>(b[2 * i] | (b[2 * i + 1] << 8));
    return v;
}

/* SNR of `got` against `ref` (dB); 200 for identical. */
double snr_db(const int16_t *ref, const int16_t *got, size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; ++i) {
        s += static_cast<double>(ref[i]) * ref[i];
        const double d = static_cast<double>(ref[i]) - got[i];
        e += d * d;
    }
    if (e == 0) return 200.0;
    if (s == 0) return -200.0;
    return 10.0 * std::log10(s / e);
}

/* libopusfile, for the test only */
struct OpusFileLib {
    void *(*open_file)(const char *, int *) = nullptr;
    void (*free_)(void *) = nullptr;
    int64_t (*pcm_total)(const void *, int) = nullptr;
    int (*read_float)(void *, float *, int, int *) = nullptr;
    bool ok() const { return open_file && free_ && pcm_total && read_float; }
};
OpusFileLib load_opusfile() {
    OpusFileLib o;
    void *h = dlopen("libopusfile.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return o;
    o.open_file = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(h, "op_open_file"));
    o.free_ = reinterpret_cast<void (*)(void *)>(dlsym(h, "op_free"));
    o.pcm_total = reinterpret_cast<int64_t (*)(const void *, int)>(dlsym(h, "op_pcm_total"));
    o.read_float = reinterpret_cast<int (*)(void *, float *, int, int *)>(dlsym(h, "op_read_float"));
    return o;
}
struct OpusRef {
    bool opened = false, error = false;
    int64_t total = -1;
    std::vector<float> pcm; /* interleaved, 48 kHz */
};
OpusRef opusfile_decode(const OpusFileLib &of, const std::string &path, unsigned channels) {
    OpusRef r;
    int err = 0;
    void *f = of.open_file(path.c_str(), &err);
    if (!f) return r;
    r.opened = true;
    r.total = of.pcm_total(f, -1);
    std::vector<float> buf(5760 * 2);
    for (;;) {
        const int n = of.read_float(f, buf.data(), static_cast<int>(buf.size()), nullptr);
        if (n < 0) {
            r.error = true;
            break;
        }
        if (n == 0) break;
        r.pcm.insert(r.pcm.end(), buf.begin(), buf.begin() + static_cast<long>(n) * channels);
    }
    of.free_(f);
    return r;
}

/* libFLAC with MD5 checking, for the test only: true when the stream decodes and its MD5 matches */
bool flac_md5_ok(const FlacLib &lib, const std::string &path) {
    void *h = dlopen(lib.name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    using WriteCb = int (*)(const void *, const void *, const int32_t *const[], void *);
    using ErrorCb = void (*)(const void *, int, void *);
    auto make = reinterpret_cast<void *(*)()>(dlsym(h, "FLAC__stream_decoder_new"));
    auto md5 = reinterpret_cast<int (*)(void *, int)>(dlsym(h, "FLAC__stream_decoder_set_md5_checking"));
    auto init = reinterpret_cast<int (*)(void *, const char *, WriteCb, void *, ErrorCb, void *)>(
        dlsym(h, "FLAC__stream_decoder_init_file"));
    auto process = reinterpret_cast<int (*)(void *)>(dlsym(h, "FLAC__stream_decoder_process_until_end_of_stream"));
    auto finish = reinterpret_cast<int (*)(void *)>(dlsym(h, "FLAC__stream_decoder_finish"));
    auto destroy = reinterpret_cast<void (*)(void *)>(dlsym(h, "FLAC__stream_decoder_delete"));
    if (!make || !md5 || !init || !process || !finish || !destroy) return false;
    void *d = make();
    md5(d, 1);
    int errors = 0;
    WriteCb w = [](const void *, const void *, const int32_t *const[], void *) -> int { return 0; };
    ErrorCb e = [](const void *, int, void *c) { ++*static_cast<int *>(c); };
    bool ok = false;
    if (init(d, path.c_str(), w, nullptr, e, &errors) == 0) {
        const int p = process(d);
        ok = p && finish(d) && errors == 0;
    }
    destroy(d);
    return ok;
}

/* ---------------------------------------------------------------- FLAC */

void flac_exact(const FlacLib &enc, const FlacDecLib &dec) {
    struct Case {
        const char *name;
        AudioFormat f;
        bool music;
        uint64_t frames;
    };
    const Case cases[] = {{"48k mono 16-bit speech", fmt_of(48000, 1, 16), false, 48000 * 8 + 11},
                          {"44.1k stereo 16-bit music", fmt_of(44100, 2, 16), true, 44100 * 6},
                          {"48k stereo 24-bit music", fmt_of(48000, 2, 24), true, 48000 * 5 + 1}};
    Tmp tmp;
    DecoderLibs libs;
    libs.flac = &dec;
    for (const Case &c : cases) {
        const Bytes pcm = c.music ? sig::music(c.f, static_cast<size_t>(c.frames)) : sig::speech(c.f, static_cast<size_t>(c.frames));
        const std::string path = tmp.dir + "/take_010_nodate.flac";
        FlacConfig fc;
        fc.fmt = c.f;
        FlacEncoder e(&enc, fc);
        CHECK_EQ(encode_to(e, path, pcm, c.frames, {{"TITLE", "take_010_nodate"}}), 0);
        const size_t align = static_cast<size_t>(c.f.channels) * (c.f.bits / 8u);
        FlacDecoder d(&dec);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), path, info), 0);
        CHECK_EQ(info.total_frames, c.frames);
        CHECK(info.output.bits == c.f.bits && info.output.channels == c.f.channels && info.output.rate == c.f.rate);
        const Bytes all = read_all(d, c.frames, align);
        CHECK(all == pcm);
        int bad = 0;
        std::vector<uint64_t> at = {0, 4095, 4096, 4097, 8191, c.frames - 1, c.frames - 4096};
        for (int i = 0; i < 100; ++i) at.push_back(mix(static_cast<uint64_t>(i) + c.frames) % c.frames);
        for (uint64_t a : at) {
            if (d.seek(a) != 0) {
                ++bad;
                continue;
            }
            const uint64_t n = std::min<uint64_t>(3000, c.frames - a);
            const Bytes got = read_all(d, n, align);
            if (got != Bytes(pcm.begin() + static_cast<long>(a * align), pcm.begin() + static_cast<long>((a + n) * align))) ++bad;
        }
        CHECK_EQ(bad, 0);
        /* the reverse 5x scan pattern through DecodedSource */
        DecodedSource s(posix_file_ops(), &libs);
        CHECK_EQ(s.open(path), 0);
        bad = 0;
        Bytes buf;
        for (uint64_t pos = c.frames; pos > 0;) {
            const uint64_t n = std::min<uint64_t>(pos, 5120);
            buf.resize(static_cast<size_t>(n) * align);
            if (s.read(pos - n, static_cast<size_t>(n), buf.data()) != static_cast<long>(n) ||
                buf != Bytes(pcm.begin() + static_cast<long>((pos - n) * align), pcm.begin() + static_cast<long>(pos * align)))
                ++bad;
            pos -= n;
        }
        CHECK_EQ(bad, 0);
        std::printf("  real FLAC %s: decode, %zu seeks and the reverse scan sample exact\n", c.name, at.size());
    }
}

void flac_trim(const FlacLib &enc, const FlacDecLib &dec) {
    Tmp tmp;
    const AudioFormat f = fmt_of(48000, 2, 16);
    const uint64_t frames = 48000 * 9 + 123;
    const Bytes pcm = sig::music(f, static_cast<size_t>(frames));
    const std::string src = tmp.dir + "/take_011_20261110-1200.flac";
    FlacConfig fc;
    fc.fmt = f;
    FlacEncoder e(&enc, fc);
    CHECK_EQ(encode_to(e, src, pcm, frames, {{"TITLE", "take_011_20261110-1200"}, {"ENCODER", "Cassette"}}), 0);
    DecoderLibs libs;
    libs.flac = &dec;
    EditRequest req;
    req.source_path = src;
    req.in_frame = 1234;
    req.splits = {100000, 250001};
    req.out_frame = frames - 777;
    EditOptions o;
    o.decoders = &libs;
    int n = 0;
    o.names = [&n](const std::string &, int count) {
        std::vector<std::string> v;
        for (int i = 0; i < count; ++i) v.push_back("take_011_20261110-1200-" + std::to_string(++n) + ".flac");
        return v;
    };
    const EditResult r = trim_split(req, o);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  real FLAC trim: %s\n", r.message.c_str());
    CHECK_EQ(r.outputs.size(), 3u);
    const uint64_t cuts[] = {1234, 100000, 250001, frames - 777};
    const size_t align = 4;
    int bad = 0;
    for (size_t i = 0; i < r.outputs.size() && i < 3; ++i) {
        const uint64_t a = cuts[i], b = cuts[i + 1];
        FlacDecoder d(&dec);
        DecodedInfo info;
        if (d.open(posix_file_ops(), r.outputs[i], info) != 0 || info.total_frames != b - a || !info.frames_known) {
            ++bad;
            continue;
        }
        const Bytes got = read_all(d, b - a, align);
        if (got != Bytes(pcm.begin() + static_cast<long>(a * align), pcm.begin() + static_cast<long>(b * align))) ++bad;
        if (!flac_md5_ok(enc, r.outputs[i])) ++bad;
        bool title = false;
        for (const Tag &t : info.tags) title = title || (t.name == "TITLE" && t.value == "take_011_20261110-1200-" + std::to_string(i + 1));
        if (!title) ++bad;
    }
    CHECK_EQ(bad, 0);
    std::printf("  real FLAC trim and split: 3 parts sample exact, MD5 accepted by libFLAC\n");
    /* replace the original with a trim */
    EditRequest rq2;
    rq2.source_path = src;
    rq2.in_frame = 48000;
    rq2.out_frame = 96000;
    rq2.mode = SaveMode::ReplaceOriginal;
    const EditResult r2 = trim_split(rq2, o);
    CHECK(r2.ok && r2.original_removed && r2.outputs.size() == 1 && r2.outputs[0] == src);
    FlacDecoder d(&dec);
    DecodedInfo info;
    CHECK_EQ(d.open(posix_file_ops(), src, info), 0);
    CHECK_EQ(info.total_frames, 48000u);
    CHECK(read_all(d, 48000, align) == Bytes(pcm.begin() + 48000 * 4, pcm.begin() + 96000 * 4));
}

/* ---------------------------------------------------------------- Opus */

struct OpusTake {
    std::string path;
    AudioFormat f;
    uint64_t frames = 0;
    std::vector<int16_t> lin; /* our linear decode */
};

OpusTake make_opus(const OpusEncLib &enc, const OpusDecLib &dec, const std::string &dir, const AudioFormat &f, int bitrate,
                   bool music, uint64_t frames, const std::string &name) {
    OpusTake t;
    t.path = dir + "/" + name;
    t.f = f;
    const Bytes pcm = music ? sig::music(f, static_cast<size_t>(frames)) : sig::speech(f, static_cast<size_t>(frames));
    OpusConfig oc;
    oc.fmt = f;
    oc.bitrate_bps = bitrate;
    oc.application = music ? OpusApplication::Music : OpusApplication::Speech;
    oc.serialno = 777;
    OpusEncoder e(&enc, oc);
    if (encode_to(e, t.path, pcm, frames, {{"TITLE", name.substr(0, name.size() - 5)}, {"ENCODER", "Cassette"}}) != 0) return t;
    t.frames = frames;
    OpusFileDecoder d(&dec);
    DecodedInfo info;
    if (d.open(posix_file_ops(), t.path, info) != 0) return t;
    t.lin = as_s16(read_all(d, info.total_frames, 2u * f.channels));
    return t;
}

void opus_exact(const OpusEncLib &enc, const OpusDecLib &dec, const OpusFileLib &of) {
    Tmp tmp;
    struct Case {
        const char *name;
        AudioFormat f;
        int bitrate;
        bool music;
    };
    const Case cases[] = {{"48k stereo 128 kbps music", fmt_of(48000, 2, 16), 128000, true},
                          {"16k mono 24 kbps speech", fmt_of(16000, 1, 16), 24000, false},
                          {"48k mono 32 kbps speech", fmt_of(48000, 1, 16), 32000, false}};
    for (const Case &c : cases) {
        const uint64_t frames = c.f.rate * 8ull + 321;
        const OpusTake t = make_opus(enc, dec, tmp.dir, c.f, c.bitrate, c.music, frames, "take_012_nodate.opus");
        CHECK_EQ(t.frames, frames);
        CHECK_EQ(t.lin.size(), static_cast<size_t>(frames) * c.f.channels); /* the exact recorded length */
        if (of.ok() && c.f.rate == 48000) {
            const OpusRef ref = opusfile_decode(of, t.path, c.f.channels);
            CHECK(ref.opened && !ref.error);
            CHECK_EQ(ref.total, static_cast<int64_t>(frames));
            CHECK_EQ(ref.pcm.size(), t.lin.size());
            int worst = 0;
            for (size_t i = 0; i < std::min(ref.pcm.size(), t.lin.size()); ++i) {
                const long r = std::lround(std::max(-1.0f, std::min(1.0f, ref.pcm[i])) * 32768.0f);
                worst = std::max(worst, static_cast<int>(std::labs(r - t.lin[i])));
            }
            CHECK(worst <= 1);
            std::printf("  real Opus %s: length exact, linear decode within %d LSB of libopusfile\n", c.name, worst);
        } else {
            std::printf("  real Opus %s: length exact (%llu frames)%s\n", c.name, static_cast<unsigned long long>(frames),
                        of.ok() ? "" : " (libopusfile not found: no reference decode)");
        }
        /* seeks against the linear decode: lag 0 is the best fit, and the SNR is high */
        OpusFileDecoder d(&dec);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), t.path, info), 0);
        const unsigned ch = c.f.channels;
        double worst_snr = 1e9;
        int lag_bad = 0;
        for (int i = 0; i < 60; ++i) {
            const uint64_t a = 4 + mix(static_cast<uint64_t>(i) * 3 + frames) % (frames - 2000);
            CHECK_EQ(d.seek(a), 0);
            const std::vector<int16_t> got = as_s16(read_all(d, 1500, 2u * ch));
            if (got.size() != 1500u * ch) {
                ++lag_bad;
                continue;
            }
            const double s0 = snr_db(&t.lin[a * ch], got.data(), got.size());
            worst_snr = std::min(worst_snr, s0);
            for (int lag : {-3, -2, -1, 1, 2, 3}) {
                const double s = snr_db(&t.lin[(a + static_cast<uint64_t>(4 + lag) - 4) * ch], got.data(), got.size());
                if (s >= s0 && s0 < 200.0) ++lag_bad;
            }
        }
        CHECK_EQ(lag_bad, 0);
        CHECK(worst_snr > 30.0);
        std::printf("  real Opus %s: 60 seeks land on the sample (lag 0 best), worst SNR against the linear decode %.1f dB\n",
                    c.name, worst_snr);
    }
}

void opus_trim(const OpusEncLib &enc, const OpusDecLib &dec, const OpusFileLib &of) {
    Tmp tmp;
    for (int pass = 0; pass < 2; ++pass) {
        const AudioFormat f = pass == 0 ? fmt_of(48000, 2, 16) : fmt_of(16000, 1, 16);
        const uint64_t frames = f.rate * 10ull;
        const OpusTake t = make_opus(enc, dec, tmp.dir, f, pass == 0 ? 96000 : 24000, pass == 0, frames,
                                     pass == 0 ? "take_013_20261110-1300.opus" : "take_014_20261110-1300.opus");
        CHECK_EQ(t.lin.size(), static_cast<size_t>(frames) * f.channels);
        const unsigned ch = f.channels;
        const uint64_t k = 48000 / f.rate;
        std::vector<std::pair<uint64_t, uint64_t>> ranges = {{0, frames},     {1, frames - 1}, {0, 480 / k},
                                                             {frames - 1, 1}, {12345 / k, 54321 / k}};
        for (int i = 0; i < 8; ++i) {
            const uint64_t a = mix(static_cast<uint64_t>(i) * 5) % (frames - 10);
            ranges.push_back({a, 1 + mix(static_cast<uint64_t>(i) * 11) % (frames - a)});
        }
        int bad = 0;
        double worst_snr = 1e9;
        for (const auto &rg : ranges) {
            oggopus::OggOpusFile in(posix_file_ops());
            CHECK_EQ(in.open(t.path), 0);
            oggopus::RemuxRequest rq;
            rq.first = rg.first;
            rq.frames = rg.second;
            rq.rate_k = static_cast<unsigned>(k);
            rq.tags = in.tags();
            rq.vendor = in.vendor();
            rq.serial = 4321;
            MemoryByteSink out;
            if (oggopus::remux_opus_range(in, rq, out, nullptr, nullptr, nullptr) != 0) {
                ++bad;
                continue;
            }
            const std::string rp = tmp.dir + "/cut.opus";
            {
                std::ofstream o(rp, std::ios::binary | std::ios::trunc);
                o.write(reinterpret_cast<const char *>(out.data.data()), static_cast<std::streamsize>(out.data.size()));
            }
            if (of.ok()) { /* libopusfile: exactly the length asked for (at 48 kHz) and a clean decode to the end */
                const OpusRef ref = opusfile_decode(of, rp, ch);
                if (!ref.opened || ref.error || ref.total != static_cast<int64_t>(rg.second * k) ||
                    ref.pcm.size() != rg.second * k * ch)
                    ++bad;
            }
            OpusFileDecoder d(&dec);
            DecodedInfo info;
            if (d.open(posix_file_ops(), rp, info) != 0 || info.total_frames != rg.second || info.damaged) {
                ++bad;
                continue;
            }
            const std::vector<int16_t> got = as_s16(read_all(d, rg.second, 2u * ch));
            if (got.size() != rg.second * ch) {
                ++bad;
                continue;
            }
            /* the same audio as the source's decode of that range (the pre-roll is decoded before the first sample) */
            const double s = snr_db(&t.lin[rg.first * ch], got.data(), got.size());
            worst_snr = std::min(worst_snr, s);
            const char *dbg = std::getenv("CASSETTE_DEBUG_SNR"); /* per range: the first 20 ms against the rest */
            if (dbg && *dbg) {
                const size_t head = std::min<size_t>(got.size(), 960u / k * ch);
                std::printf("    [%llu +%llu] snr %.1f, first 20 ms %.1f, rest %.1f\n", (unsigned long long)rg.first,
                            (unsigned long long)rg.second, s, snr_db(&t.lin[rg.first * ch], got.data(), head),
                            got.size() > head ? snr_db(&t.lin[rg.first * ch + head], got.data() + head, got.size() - head) : 0.0);
            }
        }
        CHECK_EQ(bad, 0);
        CHECK(worst_snr > 30.0);
        std::printf("  real Opus trim by remux at %u Hz: %zu ranges exact in length%s, worst SNR against the source %.1f dB\n",
                    f.rate, ranges.size(), of.ok() ? " (libopusfile agrees, clean decode)" : "", worst_snr);
        /* through the edit engine: a split into two parts, no codec library needed for the cut */
        EditRequest req;
        req.source_path = t.path;
        req.in_frame = f.rate / 2;
        req.splits = {f.rate * 4};
        req.out_frame = frames - f.rate / 3;
        EditOptions o;
        DecoderLibs none;
        o.decoders = &none;
        int n = 0;
        o.names = [&n, pass](const std::string &, int count) {
            std::vector<std::string> v;
            for (int i = 0; i < count; ++i) v.push_back(std::string(pass == 0 ? "take_013" : "take_014") + "_20261110-1300-" + std::to_string(++n) + ".opus");
            return v;
        };
        const EditResult r = trim_split(req, o);
        CHECK(r.ok && r.outputs.size() == 2);
        if (!r.ok) std::fprintf(stderr, "  real Opus split: %s\n", r.message.c_str());
        if (r.ok && r.outputs.size() == 2) {
            const uint64_t cuts[] = {req.in_frame, req.splits[0], req.out_frame};
            for (size_t i = 0; i < 2; ++i) {
                OpusFileDecoder d(&dec);
                DecodedInfo info;
                CHECK_EQ(d.open(posix_file_ops(), r.outputs[i], info), 0);
                CHECK_EQ(info.total_frames, cuts[i + 1] - cuts[i]);
                if (of.ok()) {
                    const OpusRef ref = opusfile_decode(of, r.outputs[i], ch);
                    CHECK(ref.opened && !ref.error && ref.total == static_cast<int64_t>((cuts[i + 1] - cuts[i]) * k));
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- CPU */

void cpu(const FlacLib &fenc, const FlacDecLib &fdec, const OpusEncLib *oenc, const OpusDecLib *odec) {
    Tmp tmp;
    const AudioFormat f = fmt_of(48000, 2, 16);
    const uint64_t frames = 48000 * 60;
    const Bytes pcm = sig::music(f, static_cast<size_t>(frames));
    struct Row {
        std::string name;
        double fwd = 0, rev = 0, ratio = 0, seek_ms = 0;
    };
    std::vector<Row> rows;
    auto measure = [&](const std::string &name, const std::string &path, const DecoderLibs &libs, double secs) {
        Row row;
        row.name = name;
        /* forward: the decoder alone, the whole take */
        DecodedInfo info;
        int err = 0;
        std::unique_ptr<Decoder> d = make_decoder(posix_file_ops(), path, libs, info, &err);
        if (!d) return;
        const size_t align = static_cast<size_t>(info.output.channels) * (info.output.bits / 8u);
        double t0 = now_s();
        read_all(*d, info.total_frames, align);
        row.fwd = (now_s() - t0) / secs;
        /* seeks */
        t0 = now_s();
        Bytes b(64 * align);
        for (int i = 0; i < 50; ++i) {
            d->seek(mix(static_cast<uint64_t>(i)) % info.total_frames);
            d->read(b.data(), 64);
        }
        row.seek_ms = (now_s() - t0) / 50 * 1000.0;
        /* the reverse 5x scan through DecodedSource */
        DecodedSource s(posix_file_ops(), &libs);
        s.open(path);
        t0 = now_s();
        Bytes buf;
        for (uint64_t pos = s.frames(); pos > 0;) {
            const uint64_t n = std::min<uint64_t>(pos, 5120);
            buf.resize(static_cast<size_t>(n) * align);
            s.read(pos - n, static_cast<size_t>(n), buf.data());
            pos -= n;
        }
        row.rev = (now_s() - t0) / secs;
        row.ratio = static_cast<double>(s.stats().decoded) / static_cast<double>(std::max<uint64_t>(1, s.stats().requested));
        rows.push_back(row);
    };
    {
        const std::string p = tmp.dir + "/cpu.flac";
        FlacConfig fc;
        fc.fmt = f;
        FlacEncoder e(&fenc, fc);
        encode_to(e, p, pcm, frames, {});
        DecoderLibs l;
        l.flac = &fdec;
        measure("FLAC 48 kHz stereo 16-bit", p, l, 60.0);
    }
    if (oenc && odec) {
        for (int b : {128000, 32000}) {
            const std::string p = tmp.dir + "/cpu.opus";
            OpusConfig oc;
            oc.fmt = f;
            oc.bitrate_bps = b;
            oc.application = b > 64000 ? OpusApplication::Music : OpusApplication::Speech;
            oc.force_mono = b < 64000;
            OpusEncoder e(oenc, oc);
            encode_to(e, p, pcm, frames, {});
            DecoderLibs l;
            l.opus = odec;
            measure("Opus " + std::to_string(b / 1000) + " kbps 48 kHz stereo", p, l, 60.0);
        }
    }
    std::printf("  CPU of decoding on this PC (one x86 core, 60 s of music): share of a core at 1x / 5x forward / 5x reverse\n");
    for (const Row &r : rows)
        std::printf("    %-30s %5.2f %% / %5.2f %% / %5.2f %%  (%.0fx real time; reverse decodes %.2f frames per frame; "
                    "a seek %.2f ms)\n",
                    r.name.c_str(), 100 * r.fwd, 500 * r.fwd, 500 * r.rev, r.fwd > 0 ? 1.0 / r.fwd : 0.0, r.ratio, r.seek_ms);
}

}  // namespace

void test_decoders_real() {
    std::string why;
    const FlacLib *fenc = real_flac(&why);
    const FlacDecLib *fdec = real_flac_decoder(&why);
    if (fenc && fdec) {
        std::printf("  real FLAC decoder: %s %s\n", fdec->name.c_str(), fdec->version.c_str());
        flac_exact(*fenc, *fdec);
        flac_trim(*fenc, *fdec);
    } else {
        SKIP(("real FLAC decoder: " + why).c_str());
    }
    std::string owhy;
    const OpusEncLib *oenc = real_opusenc(&owhy);
    const OpusDecLib *odec = real_opus_decoder(&owhy);
    const OpusFileLib of = load_opusfile();
    if (oenc && odec) {
        std::printf("  real Opus decoder: %s %s%s\n", odec->name.c_str(), odec->version.c_str(),
                    of.ok() ? " (libopusfile as the reference)" : "");
        opus_exact(*oenc, *odec, of);
        opus_trim(*oenc, *odec, of);
    } else {
        SKIP(("real Opus decoder: " + owhy).c_str());
    }
    if (fenc && fdec) cpu(*fenc, *fdec, oenc, odec);
}
