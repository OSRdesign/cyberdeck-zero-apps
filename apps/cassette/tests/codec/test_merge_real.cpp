/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: MERGE TAKES on the REAL libraries, when they load (else "skipped").
 *   - FLAC: three takes made by libFLAC from consecutive pieces of one signal, merged through the edit engine with the real
 *     decoder and encoder: the new take decodes to exactly the three pieces end to end (16-bit mono, 24-bit stereo), its
 *     STREAMINFO says the sum and libFLAC accepts its MD5;
 *   - Opus: three takes made by libopusenc from consecutive pieces of one signal (48 kHz stereo music, 16 kHz mono speech),
 *     merged by copying packets: the length is the plan's, libopusfile (the reference) reads exactly that length and decodes to
 *     the end without an error; the first take decodes in the new take exactly as alone; each later take is compared with its
 *     own decode over its first 20 ms, 20 to 100 ms and the rest (the decoder runs on across the join with the earlier take's
 *     state), and the seams are measured (length, level). The numbers are printed: they are the root findings behind the
 *     seam text of docs/FORMATS.md.
 */
#include "check.hpp"

#include "decoder.hpp"
#include "fileops.hpp"
#include "flac_decoder.hpp"
#include "flac_encoder.hpp"
#include "ogg_opus.hpp"
#include "opus_decoder.hpp"
#include "opus_encoder.hpp"
#include "siggen.hpp"
#include "edit/merge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cassette;

namespace {

using Bytes = std::vector<unsigned char>;

struct MTmp {
    std::string dir;
    MTmp() {
        std::string t = (std::filesystem::temp_directory_path() / "cassette-merger-XXXXXX").string();
        std::vector<char> b(t.begin(), t.end());
        b.push_back('\0');
        dir = mkdtemp(b.data()) ? std::string(b.data()) : std::string("/tmp");
    }
    ~MTmp() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

AudioFormat fmt3(uint32_t rate, uint16_t ch, uint16_t bits) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

int encode_file(Encoder &e, const std::string &path, const unsigned char *pcm, uint64_t frames, const Tags &tags) {
    FileOps &ops = posix_file_ops();
    const int fd = ops.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fd;
    FileByteSink s(ops, fd, 0);
    int r = e.start(s, tags);
    const size_t fb = static_cast<size_t>(e.format().channels) * (e.format().bits / 8u);
    for (uint64_t done = 0; r == 0 && done < frames;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(4800, frames - done));
        r = e.write(pcm + done * fb, n);
        done += n;
    }
    if (r == 0) r = e.finish();
    ops.close(fd);
    return r;
}

Bytes decode_all(Decoder &d, uint64_t frames, size_t align) {
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

std::vector<int16_t> s16(const Bytes &b) {
    std::vector<int16_t> v(b.size() / 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int16_t>(b[2 * i] | (b[2 * i + 1] << 8));
    return v;
}

double snr(const int16_t *ref, const int16_t *got, size_t n) {
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

double rms_dbfs(const int16_t *p, size_t n) {
    if (n == 0) return -200.0;
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(p[i]) * p[i];
    const double r = std::sqrt(s / static_cast<double>(n)) / 32768.0;
    return r > 0 ? 20.0 * std::log10(r) : -200.0;
}

bool md5_ok(const FlacLib &lib, const std::string &path) {
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
    if (init(d, path.c_str(), w, nullptr, e, &errors) == 0) ok = process(d) && finish(d) && errors == 0;
    destroy(d);
    return ok;
}

/* libopusfile, for the test only (the reference reader) */
struct OpFile {
    void *(*open_file)(const char *, int *) = nullptr;
    void (*free_)(void *) = nullptr;
    int64_t (*pcm_total)(const void *, int) = nullptr;
    int (*read_float)(void *, float *, int, int *) = nullptr;
    bool ok() const { return open_file && free_ && pcm_total && read_float; }
};
OpFile load_op() {
    OpFile o;
    void *h = dlopen("libopusfile.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return o;
    o.open_file = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(h, "op_open_file"));
    o.free_ = reinterpret_cast<void (*)(void *)>(dlsym(h, "op_free"));
    o.pcm_total = reinterpret_cast<int64_t (*)(const void *, int)>(dlsym(h, "op_pcm_total"));
    o.read_float = reinterpret_cast<int (*)(void *, float *, int, int *)>(dlsym(h, "op_read_float"));
    return o;
}
/* true when libopusfile opens the file, says `total48` samples and decodes to the end without an error */
bool op_reads(const OpFile &of, const std::string &path, int64_t total48, unsigned ch) {
    int err = 0;
    void *f = of.open_file(path.c_str(), &err);
    if (!f) return false;
    bool ok = of.pcm_total(f, -1) == total48;
    std::vector<float> buf(5760 * 2);
    int64_t got = 0;
    for (;;) {
        const int n = of.read_float(f, buf.data(), static_cast<int>(buf.size()), nullptr);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) break;
        got += n;
    }
    of.free_(f);
    (void)ch;
    return ok && got == total48;
}

void flac_merge(const FlacLib &enc, const FlacDecLib &dec) {
    MTmp tmp;
    for (const AudioFormat f : {fmt3(48000, 1, 16), fmt3(44100, 2, 24)}) {
        const uint64_t lens[3] = {f.rate * 2ull + 77, f.rate + 4096, f.rate * 3ull / 2};
        const uint64_t total = lens[0] + lens[1] + lens[2];
        const Bytes pcm = sig::music(f, static_cast<size_t>(total));
        const size_t align = static_cast<size_t>(f.channels) * (f.bits / 8u);
        MergeRequest r;
        uint64_t at = 0;
        for (int i = 0; i < 3; ++i) {
            const std::string p = tmp.dir + "/take_" + std::to_string(200 + 10 * f.bits + i) + "_20261111-100" + std::to_string(i) + ".flac";
            FlacConfig fc;
            fc.fmt = f;
            FlacEncoder e(&enc, fc);
            CHECK_EQ(encode_file(e, p, pcm.data() + at * align, lens[i], {{"TITLE", "x"}}), 0);
            r.sources.push_back(p);
            at += lens[i];
        }
        r.output_name = "take_00" + std::to_string(f.bits == 16 ? 4 : 5) + "_20261111-1010.flac";
        r.tags = {{"TITLE", "take_004_20261111-1010"}, {"ENCODER", "Cassette"}};
        DecoderLibs libs;
        libs.flac = &dec;
        MergeOptions o;
        o.decoders = &libs; /* the encoder: FlacEncoder on the real libFLAC (no make_flac_encoder) */
        const MergeResult res = merge_takes(r, o);
        CHECK(res.ok);
        if (!res.ok) {
            std::fprintf(stderr, "  real FLAC merge: %s\n", res.message.c_str());
            continue;
        }
        FlacDecoder d(&dec);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), res.output, info), 0);
        CHECK(info.frames_known && info.total_frames == total);
        CHECK(decode_all(d, total, align) == pcm);
        CHECK(md5_ok(enc, res.output));
        std::printf("  real FLAC merge %u Hz %u-bit %s: 3 takes sample exact end to end, STREAMINFO %llu frames, MD5 accepted\n",
                    f.rate, f.bits, f.channels == 2 ? "stereo" : "mono", static_cast<unsigned long long>(total));
    }
}

void opus_merge(const OpusEncLib &enc, const OpusDecLib &dec, const OpFile &of) {
    MTmp tmp;
    struct Case {
        const char *name;
        AudioFormat f;
        int bitrate;
        bool music;
    };
    const Case cases[] = {{"48 kHz stereo 96 kbps music", fmt3(48000, 2, 16), 96000, true},
                          {"16 kHz mono 24 kbps speech", fmt3(16000, 1, 16), 24000, false},
                          {"48 kHz mono 32 kbps speech", fmt3(48000, 1, 16), 32000, false}};
    for (const Case &c : cases) {
        const AudioFormat f = c.f;
        const unsigned ch = f.channels, k = 48000u / f.rate;
        const uint64_t lens[3] = {f.rate * 5ull / 2 + 123, f.rate * 17ull / 10, f.rate * 23ull / 10 + 7};
        const uint64_t total = lens[0] + lens[1] + lens[2];
        const Bytes pcm = c.music ? sig::music(f, static_cast<size_t>(total)) : sig::speech(f, static_cast<size_t>(total));
        MergeRequest r;
        std::vector<std::vector<int16_t>> alone;
        uint64_t at = 0;
        for (int i = 0; i < 3; ++i) {
            const std::string p = tmp.dir + "/take_" + std::to_string(100 + 10 * (&c - cases) + i) + "_20261111-110" + std::to_string(i) + ".opus";
            OpusConfig oc;
            oc.fmt = f;
            oc.bitrate_bps = c.bitrate;
            oc.application = c.music ? OpusApplication::Music : OpusApplication::Speech;
            oc.serialno = 100 + i;
            OpusEncoder e(&enc, oc);
            CHECK_EQ(encode_file(e, p, pcm.data() + at * ch * 2, lens[i], {{"TITLE", "x"}}), 0);
            OpusFileDecoder d(&dec);
            DecodedInfo info;
            CHECK_EQ(d.open(posix_file_ops(), p, info), 0);
            CHECK_EQ(info.total_frames, lens[i]);
            alone.push_back(s16(decode_all(d, info.total_frames, 2u * ch)));
            r.sources.push_back(p);
            at += lens[i];
        }
        r.output_name = "take_0" + std::to_string(20 + (&c - cases)) + "_20261111-1110.opus";
        r.tags = {{"TITLE", "take_013_20261111-1110"}, {"ENCODER", "Cassette"}};
        MergeOptions o;
        DecoderLibs none; /* the join needs no codec library */
        o.decoders = &none;
        const MergePlan plan = plan_merge(r, o);
        CHECK(plan.ok);
        const MergeResult res = merge_takes(r, o);
        CHECK(res.ok);
        if (!res.ok || !plan.ok) {
            std::fprintf(stderr, "  real Opus merge: %s\n", res.message.c_str());
            continue;
        }
        CHECK_EQ(res.frames, plan.frames);
        CHECK_EQ(res.frames, total + plan.seam_frames);
        OpusFileDecoder d(&dec);
        DecodedInfo info;
        CHECK_EQ(d.open(posix_file_ops(), res.output, info), 0);
        CHECK_EQ(info.total_frames, plan.frames);
        CHECK(!info.damaged);
        const std::vector<int16_t> merged = s16(decode_all(d, info.total_frames, 2u * ch));
        CHECK_EQ(merged.size(), static_cast<size_t>(plan.frames) * ch);
        bool ref_ok = true;
        if (of.ok()) {
            ref_ok = op_reads(of, res.output, static_cast<int64_t>(plan.frames) * k, ch);
            CHECK(ref_ok);
        }
        /* where each take starts in the new take (merge.hpp: the earlier takes' packets, the later take's lead) */
        const int64_t pre0 = plan.opus_lead48[0];
        int64_t kept48 = 0;
        std::string seams, windows;
        double first_snr = 0;
        for (size_t i = 0; i < 3; ++i) {
            const int64_t start = (kept48 + plan.opus_lead48[i] - pre0) / static_cast<int64_t>(k);
            const size_t n = static_cast<size_t>(lens[i]);
            if (static_cast<size_t>(start) * ch + n * ch > merged.size()) {
                CHECK(false);
                break;
            }
            const int16_t *m = &merged[static_cast<size_t>(start) * ch];
            const int16_t *a = alone[i].data();
            if (i == 0) {
                first_snr = snr(a, m, n * ch);
                CHECK(first_snr > 90.0); /* decoded exactly as alone */
            } else {
                const size_t w20 = f.rate / 50 * ch, w100 = f.rate / 10 * ch;
                const double s1 = snr(a, m, w20), s2 = snr(a + w20, m + w20, w100 - w20), s3 = snr(a + w100, m + w100, n * ch - w100);
                CHECK(s3 > 20.0); /* the decoder has settled */
                /* against the source signal itself: the take alone and in the new take */
                uint64_t from = 0;
                for (size_t j = 0; j < i; ++j) from += lens[j];
                const std::vector<int16_t> src = s16(Bytes(pcm.begin() + static_cast<long>(from * ch * 2),
                                                           pcm.begin() + static_cast<long>((from + n) * ch * 2)));
                char b[320];
                std::snprintf(b, sizeof b,
                              "%stake %zu: %.1f / %.1f / %.1f dB (against the source signal, alone: %.1f / %.1f / %.1f, merged: %.1f / %.1f / %.1f)",
                              windows.empty() ? "" : "; ", i + 1, s1, s2, s3, snr(src.data(), a, w20),
                              snr(src.data() + w20, a + w20, w100 - w20), snr(src.data() + w100, a + w100, n * ch - w100),
                              snr(src.data(), m, w20), snr(src.data() + w20, m + w20, w100 - w20),
                              snr(src.data() + w100, m + w100, n * ch - w100));
                windows += b;
                /* the seam before this take: from the end of the earlier take to this take's first sample */
                const int64_t prev_end = (kept48 - plan.opus_tail48[i - 1] - pre0) / static_cast<int64_t>(k);
                const size_t seam = static_cast<size_t>(start - prev_end);
                const double lvl = rms_dbfs(&merged[static_cast<size_t>(prev_end) * ch], seam * ch);
                const double around = rms_dbfs(&merged[static_cast<size_t>(prev_end) * ch - std::min<size_t>(seam * ch, static_cast<size_t>(prev_end) * ch)],
                                               seam * ch);
                std::snprintf(b, sizeof b, "%s%.1f ms (%.1f + %.1f) at %.0f dBFS against %.0f before", seams.empty() ? "" : ", ",
                              1000.0 * static_cast<double>(seam) / f.rate, plan.opus_tail48[i - 1] / 48.0, plan.opus_lead48[i] / 48.0,
                              lvl, around);
                seams += b;
            }
            kept48 += plan.opus_lead48[i] + static_cast<int64_t>(lens[i]) * k + plan.opus_tail48[i];
        }
        std::printf("  real Opus merge %s: length %llu = takes %llu + seams %llu%s; take 1 as alone (%.0f dB)\n", c.name,
                    static_cast<unsigned long long>(plan.frames), static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(plan.seam_frames),
                    of.ok() ? (ref_ok ? ", libopusfile reads it to the end" : ", libopusfile REFUSES it") : "", first_snr);
        std::printf("    seams: %s\n    later takes against their own decode (first 20 ms / 20-100 ms / rest): %s\n",
                    seams.c_str(), windows.c_str());
    }
}

}  // namespace

void test_merge_real() {
    std::string why;
    const FlacLib *fenc = real_flac(&why);
    const FlacDecLib *fdec = real_flac_decoder(&why);
    if (fenc && fdec) flac_merge(*fenc, *fdec);
    else SKIP(("real FLAC merge: " + why).c_str());
    std::string owhy;
    const OpusEncLib *oenc = real_opusenc(&owhy);
    const OpusDecLib *odec = real_opus_decoder(&owhy);
    if (oenc && odec) opus_merge(*oenc, *odec, load_op());
    else SKIP(("real Opus merge: " + owhy).c_str());
}
