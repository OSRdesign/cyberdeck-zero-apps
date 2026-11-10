/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests (phase C): trim, split and the peak cache of FLAC and Ogg Opus takes, on the in-memory file system
 * with kill and failure injection (helpers.hpp), the test encoders (fixture_codecs.hpp) and the fake decoders
 * (fake_decoders.hpp), so every result is checked to the sample:
 *   - FLAC: trim, split, replace (one part over the original, several parts); the results are FLAC with the source's
 *     comments (TITLE follows the file name), STREAMINFO says their length, and they decode to exactly their slice;
 *   - Opus: the same by remux (no codec library at all), decoded back by the fake;
 *   - errors: no FLAC decoder or encoder (one message, the original untouched), not enough space (refused before
 *     anything is written);
 *   - crash safety: every file operation of a FLAC and an Opus save is made to fail (EIO, ENOSPC) and to be the point of
 *     a kill; afterwards the original is unchanged or every result is complete, never a half file under a final name,
 *     and cleanup_temporaries() leaves nothing behind;
 *   - the peak cache of FLAC and Opus takes: values against the decoded audio at every zoom, the cache reused and
 *     rebuilt when the take changes, progress, cancel, a missing decoder, and a kill at every step of a build.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "peaks.hpp"

#include "../fake_decoders.hpp"
#include "../fixture_codecs.hpp"
#include "codec/flac_decoder.hpp"
#include "codec/opus_decoder.hpp"
#include "codec/stream_scan.hpp"

#include <thread>

using namespace tst;

namespace {

const char *kDir = "/rec";

struct Fakes {
    FlacDecLib flac = fakedec::make_flac_dec_lib();
    OpusDecLib opus = fakedec::make_opus_dec_lib();
    DecoderLibs libs;
    Fakes() {
        libs.flac = &flac;
        libs.opus = &opus;
    }
};
Fakes &fakes() {
    static Fakes f;
    return f;
}

AudioFormat afmt(uint32_t rate, uint16_t ch, uint16_t bits) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

Bytes flac_bytes(const AudioFormat &f, uint64_t frames, const std::string &stem) {
    MemoryByteSink s;
    fx::VerbatimFlac e(f, 4096, 128);
    e.start(s, {{"TITLE", stem}, {"ENCODER", "Cassette"}, {"DATE", "2026-11-10"}});
    const Bytes pcm = fx::pcm(f, 0, frames);
    e.write(pcm.data(), static_cast<size_t>(frames));
    e.finish();
    return s.data;
}

Bytes opus_bytes(const AudioFormat &f, uint64_t frames, const std::string &stem, unsigned page_packets = 10) {
    MemoryByteSink s;
    fx::PlainOggOpus e(f, 70, page_packets, 312);
    e.start(s, {{"TITLE", stem}, {"ENCODER", "Cassette"}});
    const Bytes pcm(static_cast<size_t>(frames) * f.channels * 2, 0);
    e.write(pcm.data(), static_cast<size_t>(frames));
    e.finish();
    return s.data;
}

/* The result names: "<stem>-N<ext>", N never reused (the extension kept, as next_edit_names does). */
struct ExtNames {
    std::set<int> used;
    std::vector<std::string> operator()(const std::string &source, int count) {
        const size_t dot = source.rfind('.');
        const std::string stem = source.substr(0, dot), ext = source.substr(dot);
        std::vector<std::string> out;
        int n = 1;
        for (int i = 0; i < count; i++) {
            while (used.count(n)) n++;
            used.insert(n);
            out.push_back(stem + "-" + std::to_string(n) + ext);
        }
        return out;
    }
};

/* Decodes a file of an in-memory folder with the fakes (a fresh FileOps holding just that file, so a "dead" file system
 * after a kill can still be judged). Interleaved samples on the output scale; ok false when it does not open. */
struct Decoded {
    bool ok = false;
    DecodedInfo info;
    Bytes pcm;
};
Decoded decode_bytes(const std::string &name, const Bytes &file) {
    Decoded d;
    MemOps v;
    v.put(std::string(kDir) + "/" + name, file);
    int err = 0;
    std::unique_ptr<Decoder> dec = make_decoder(v, std::string(kDir) + "/" + name, fakes().libs, d.info, &err);
    if (!dec) return d;
    const size_t align = static_cast<size_t>(d.info.output.channels) * (d.info.output.bits / 8u);
    d.pcm.resize(static_cast<size_t>(d.info.total_frames) * align);
    uint64_t got = 0;
    while (got < d.info.total_frames) {
        const long n = dec->read(d.pcm.data() + got * align, 4096);
        if (n <= 0) break;
        got += static_cast<uint64_t>(n);
    }
    d.ok = got == d.info.total_frames;
    return d;
}

Bytes slice_of(const Bytes &pcm, size_t align, uint64_t a, uint64_t b) {
    return Bytes(pcm.begin() + static_cast<long>(a * align), pcm.begin() + static_cast<long>(b * align));
}

std::string tag(const Tags &t, const std::string &name) {
    for (const Tag &g : t)
        if (g.name == name) return g.value;
    return "";
}

/* ---------------------------------------------------------------- one save */

struct Scen {
    const char *name;
    bool opus;
    AudioFormat fmt;
    uint64_t frames, in, out;
    std::vector<uint64_t> splits;
    SaveMode mode;
};

struct Outcome {
    int steps = 0;
    bool ok = false;
    EditResult res;
    std::string problems;
};

std::string src_name(const Scen &sc) { return sc.opus ? "take_007_20261110-1000.opus" : "take_006_20261110-1000.flac"; }

/* Judges the folder: the original as it was, or results that are complete and exact; nothing else but temporaries
 * (when allowed) and peak caches. */
std::string judge(MemOps &m, const Scen &sc, const Bytes &original, const Bytes &ref, size_t align, bool allow_tmp) {
    const std::string src = src_name(sc);
    const bool single_over = sc.mode == SaveMode::ReplaceOriginal && sc.splits.empty();
    std::vector<uint64_t> cuts{sc.in};
    cuts.insert(cuts.end(), sc.splits.begin(), sc.splits.end());
    cuts.push_back(sc.out);
    ExtNames nm;
    const std::vector<std::string> names = single_over ? std::vector<std::string>{src} : nm(src, static_cast<int>(cuts.size() - 1));
    std::string bad;
    auto valid = [&](const std::string &n, size_t i) {
        const Decoded d = decode_bytes(n, m.get(std::string(kDir) + "/" + n));
        return d.ok && d.info.total_frames == cuts[i + 1] - cuts[i] && d.pcm == slice_of(ref, align, cuts[i], cuts[i + 1]);
    };
    std::set<std::string> present;
    for (const std::string &n : m.names(kDir)) present.insert(n);
    for (const std::string &n : present) {
        if (n.compare(0, 5, ".tmp-") == 0) {
            if (!allow_tmp) bad += "temporary left: " + n + "; ";
            continue;
        }
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".peaks") == 0) continue;
        if (n == src) continue;
        const auto it = std::find(names.begin(), names.end(), n);
        if (it == names.end()) {
            bad += "unexpected file " + n + "; ";
            continue;
        }
        if (!valid(n, static_cast<size_t>(it - names.begin()))) bad += "a result under its final name is not complete: " + n + "; ";
    }
    const bool have_src = present.count(src) != 0;
    if (single_over) {
        if (!have_src) return bad + "the original name vanished";
        if (m.get(std::string(kDir) + "/" + src) != original && !valid(src, 0))
            bad += "the original name holds neither the original nor the finished result; ";
        return bad;
    }
    if (have_src) {
        if (m.get(std::string(kDir) + "/" + src) != original) bad += "the original changed; ";
    } else if (sc.mode == SaveMode::NewFiles) {
        bad += "the original is gone in new-files mode; ";
    } else {
        for (const std::string &n : names)
            if (!present.count(n)) bad += "the original is gone but result " + n + " is missing; ";
    }
    return bad;
}

Outcome run(const Scen &sc, int kill_at, int fail_at, int fail_errno = EIO, const DecoderLibs *libs = &fakes().libs) {
    const std::string src = src_name(sc);
    const std::string stem = src.substr(0, src.rfind('.'));
    const Bytes original = sc.opus ? opus_bytes(sc.fmt, sc.frames, stem) : flac_bytes(sc.fmt, sc.frames, stem);
    MemOps m;
    const std::string path = std::string(kDir) + "/" + src;
    m.put(path, original);
    m.put(path + ".peaks", Bytes(40, 1));
    /* the reference: the source decoded */
    const Decoded refd = decode_bytes(src, original);
    const size_t align = static_cast<size_t>(refd.info.output.channels) * (refd.info.output.bits / 8u);
    m.kill_at = kill_at;
    m.fail_at = fail_at;
    m.fail_errno = fail_errno;
    ExtNames nm;
    EditOptions o;
    o.ops = &m;
    o.names = [&](const std::string &n, int c) { return nm(n, c); };
    o.decoders = libs;
    o.make_flac_encoder = [](const AudioFormat &f) { return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096, 128)); };
    o.block_bytes = 4096;
    uint64_t last = 0, total_seen = 0;
    bool mono = true, first = true;
    o.progress = [&](uint64_t d, uint64_t t) {
        if (first && d != 0) mono = false;
        first = false;
        if (d < last) mono = false;
        last = d;
        total_seen = t;
    };
    EditRequest r;
    r.source_path = path;
    r.in_frame = sc.in;
    r.out_frame = sc.out;
    r.splits = sc.splits;
    r.mode = sc.mode;
    Outcome out;
    out.res = trim_split(r, o);
    out.steps = m.step;
    out.ok = out.res.ok;
    out.problems = judge(m, sc, original, refd.pcm, align, true);
    if (!mono) out.problems += "progress went backwards; ";
    if (out.ok && kill_at < 0 && fail_at < 0 && last != total_seen) out.problems += "progress did not reach the total; ";
    m.dead = false;
    m.kill_at = -1;
    m.fail_at = -1;
    cleanup_temporaries(m, kDir);
    const std::string after = judge(m, sc, original, refd.pcm, align, false);
    if (!after.empty()) out.problems += "after cleanup: " + after;
    if (kill_at < 0 && fail_at < 0 && out.ok) {
        ExtNames n2;
        const size_t parts = sc.splits.size() + 1;
        if (!(sc.mode == SaveMode::ReplaceOriginal && parts == 1))
            for (const std::string &n : n2(src, static_cast<int>(parts)))
                if (!m.exists(std::string(kDir) + "/" + n)) out.problems += "clean run: missing " + n + "; ";
        if (sc.mode == SaveMode::ReplaceOriginal && parts > 1 && m.exists(path)) out.problems += "clean replace kept the original; ";
    }
    return out;
}

const Scen kScens[] = {
    {"FLAC trim, new file", false, afmt(48000, 1, 16), 20000, 100, 19000, {}, SaveMode::NewFiles},
    {"FLAC split 2 points, new files", false, afmt(48000, 1, 16), 20000, 0, 20000, {4096, 13001}, SaveMode::NewFiles},
    {"FLAC split + trim, stereo 24-bit, replace", false, afmt(44100, 2, 24), 9000, 7, 8990, {3000}, SaveMode::ReplaceOriginal},
    {"FLAC trim alone, replace", false, afmt(48000, 1, 16), 12000, 4095, 8193, {}, SaveMode::ReplaceOriginal},
    {"Opus trim, new file", true, afmt(48000, 1, 16), 48000 * 2, 5000, 90001, {}, SaveMode::NewFiles},
    {"Opus split 2 points, new files", true, afmt(48000, 2, 16), 48000 * 2, 0, 48000 * 2 - 312, {960, 47000}, SaveMode::NewFiles},
    {"Opus split 16 kHz, replace", true, afmt(16000, 1, 16), 16000 * 3, 1, 16000 * 3 - 5, {16000}, SaveMode::ReplaceOriginal},
    {"Opus trim alone, replace", true, afmt(48000, 1, 16), 48000 * 2, 4000, 30000, {}, SaveMode::ReplaceOriginal},
};

/* ---------------------------------------------------------------- functional */

void t_results() {
    for (const Scen &sc : kScens) {
        const Outcome o = run(sc, -1, -1);
        CHECK(o.ok);
        if (!o.ok) std::fprintf(stderr, "[%s] %s\n", sc.name, o.res.message.c_str());
        CHECK(o.problems.empty());
        if (!o.problems.empty()) std::fprintf(stderr, "[%s] %s\n", sc.name, o.problems.c_str());
        CHECK_EQ(o.res.outputs.size(), sc.splits.size() + 1);
    }
    /* comments, names, STREAMINFO */
    const Scen &sc = kScens[1];
    const std::string src = src_name(sc);
    MemOps m;
    m.put(std::string(kDir) + "/" + src, flac_bytes(sc.fmt, sc.frames, "take_006_20261110-1000"));
    ExtNames nm;
    EditOptions o;
    o.ops = &m;
    o.names = [&](const std::string &n, int c) { return nm(n, c); };
    o.decoders = &fakes().libs;
    o.make_flac_encoder = [](const AudioFormat &f) { return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096, 128)); };
    EditRequest r;
    r.source_path = std::string(kDir) + "/" + src;
    r.in_frame = 10;
    r.out_frame = 15000;
    r.splits = {5000};
    const EditTakeInfo ti = inspect_take(m, r.source_path);
    CHECK(ti.ok && ti.kind == EditKind::Flac && ti.frames == sc.frames && ti.fmt.rate == 48000 && ti.fmt.bits == 16);
    CHECK_EQ(tag(ti.tags, "TITLE"), "take_006_20261110-1000");
    const EditResult res = trim_split(r, o);
    CHECK(res.ok && res.outputs.size() == 2);
    for (size_t i = 0; i < res.outputs.size(); ++i) {
        const std::string n = res.outputs[i].substr(std::strlen(kDir) + 1);
        CHECK_EQ(n, "take_006_20261110-1000-" + std::to_string(i + 1) + ".flac");
        const Decoded d = decode_bytes(n, m.get(res.outputs[i]));
        CHECK_EQ(tag(d.info.tags, "TITLE"), "take_006_20261110-1000-" + std::to_string(i + 1)); /* the result's own name */
        CHECK_EQ(tag(d.info.tags, "ENCODER"), "Cassette");
        CHECK_EQ(tag(d.info.tags, "DATE"), "2026-11-10");
        const StreamInfo si = read_stream_info(m, res.outputs[i]);
        CHECK(si.ok && si.frames_known && si.frames == (i == 0 ? 4990u : 10000u));
    }
    /* Opus inspection */
    MemOps m2;
    const std::string op = std::string(kDir) + "/take_008_nodate.opus";
    m2.put(op, opus_bytes(afmt(16000, 2, 16), 16000 * 2, "take_008_nodate"));
    const EditTakeInfo oi = inspect_take(m2, op);
    CHECK(oi.ok && oi.kind == EditKind::OggOpus && oi.fmt.rate == 16000 && oi.fmt.channels == 2 && oi.opus_k == 3);
    CHECK_EQ(oi.frames, 16000u * 2u);
    CHECK_EQ(tag(oi.tags, "TITLE"), "take_008_nodate");
}

void t_errors() {
    /* no FLAC decoder: one message, nothing written, the original untouched */
    {
        DecoderLibs none;
        const Outcome o = run(kScens[0], -1, -1, EIO, &none);
        CHECK(!o.ok);
        CHECK(o.res.message.find("FLAC not installed (libflac14)") != std::string::npos);
        CHECK(o.problems.empty());
    }
    /* no FLAC encoder (the library is missing): the same */
    {
        const Scen &sc = kScens[0];
        MemOps m;
        const std::string path = std::string(kDir) + "/" + src_name(sc);
        const Bytes original = flac_bytes(sc.fmt, sc.frames, "x");
        m.put(path, original);
        ExtNames nm;
        EditOptions o;
        o.ops = &m;
        o.names = [&](const std::string &n, int c) { return nm(n, c); };
        o.decoders = &fakes().libs;
        o.make_flac_encoder = [](const AudioFormat &) { return std::unique_ptr<Encoder>(); };
        EditRequest r;
        r.source_path = path;
        r.in_frame = 0;
        r.out_frame = 1000;
        const EditResult res = trim_split(r, o);
        CHECK(!res.ok && res.message.find("FLAC not installed") != std::string::npos);
        CHECK(m.get(path) == original);
        CHECK_EQ(m.names(kDir).size(), 1u);
    }
    /* Opus needs no library: the cut is a copy of packets */
    {
        DecoderLibs none;
        const Outcome o = run(kScens[4], -1, -1, EIO, &none);
        CHECK(o.ok && o.problems.empty());
    }
    /* too little space: refused before anything is written */
    {
        const Scen &sc = kScens[5];
        MemOps m;
        const std::string path = std::string(kDir) + "/" + src_name(sc);
        m.put(path, opus_bytes(sc.fmt, sc.frames, "x"));
        m.free_bytes = 70000;
        ExtNames nm;
        EditOptions o;
        o.ops = &m;
        o.names = [&](const std::string &n, int c) { return nm(n, c); };
        EditRequest r;
        r.source_path = path;
        r.in_frame = 0;
        r.out_frame = sc.frames - 400;
        r.splits = {sc.frames / 2};
        const EditResult res = trim_split(r, o);
        CHECK(!res.ok && res.error == EditError::NoSpace);
        CHECK_EQ(m.names(kDir).size(), 1u);
    }
    /* positions past the end of the decoded length */
    {
        const Scen &sc = kScens[4];
        Scen bad = sc;
        bad.out = sc.frames + 1;
        const Outcome o = run(bad, -1, -1);
        CHECK(!o.ok && o.res.error == EditError::BadPositions);
    }
}

/* ---------------------------------------------------------------- crash safety */

void t_every_step() {
    for (const Scen &sc : kScens) {
        const Outcome base = run(sc, -1, -1);
        CHECK(base.ok && base.problems.empty());
        CHECK(base.steps > 8);
        int failures = 0;
        for (int n = 1; n <= base.steps + 1; n++) {
            const Outcome k = run(sc, n, -1);
            if (!k.problems.empty()) {
                ++failures;
                std::fprintf(stderr, "[%s] kill at step %d: %s\n", sc.name, n, k.problems.c_str());
            }
            for (int err : {EIO, ENOSPC}) {
                const Outcome f = run(sc, -1, n, err);
                if (!f.problems.empty()) {
                    ++failures;
                    std::fprintf(stderr, "[%s] fail at step %d (errno %d): %s\n", sc.name, n, err, f.problems.c_str());
                }
                if (!f.ok && f.res.message.empty()) { /* a failed save always says why */
                    ++failures;
                    std::fprintf(stderr, "[%s] fail at step %d: no message\n", sc.name, n);
                }
            }
        }
        CHECK_EQ(failures, 0);
        std::printf("  %s: %d steps, kill and EIO / ENOSPC at each: safe\n", sc.name, base.steps);
    }
}

/* ---------------------------------------------------------------- peaks */

struct PeakRef {
    std::vector<int> v; /* 16-bit scale envelope per frame: min and max of the channels */
    std::vector<int> lo, hi;
};

PeakRef peak_ref(const Bytes &pcm, const AudioFormat &out) {
    PeakRef r;
    const unsigned sb = out.bits / 8u, ch = out.channels;
    const size_t frames = pcm.size() / (sb * ch);
    r.lo.resize(frames);
    r.hi.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        int lo = 32767, hi = -32768;
        for (unsigned c = 0; c < ch; ++c) {
            const unsigned char *p = &pcm[(i * ch + c) * sb];
            int v;
            if (sb == 2) v = static_cast<int16_t>(p[0] | (p[1] << 8));
            else v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 24) >> 16;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        r.lo[i] = lo;
        r.hi[i] = hi;
    }
    return r;
}

/* Bars against the decoded audio: exact for fewer than 256 frames per bar and for bars aligned to the level's blocks;
 * otherwise a bar may reach into the neighbouring blocks, so it must at least hold the exact envelope. */
void check_bars(PeakPyramid &P, const PeakRef &ref, uint64_t start, uint64_t fpb, size_t bars, int &bad) {
    const std::vector<PeakPair> w = P.window(start, fpb, bars);
    const uint64_t F = ref.lo.size();
    const uint64_t bs = fpb < kPeakBlock ? 1 : (static_cast<uint64_t>(kPeakBlock) << P.level_for(fpb));
    const bool aligned = start % bs == 0 && fpb % bs == 0;
    for (size_t i = 0; i < bars; ++i) {
        const uint64_t a = start + i * fpb;
        if (a >= F) {
            if (!(w[i].min == 0 && w[i].max == 0)) ++bad;
            continue;
        }
        const uint64_t b = std::min<uint64_t>(a + fpb, F);
        int lo = 32767, hi = -32768;
        for (uint64_t k = a; k < b; ++k) {
            lo = std::min(lo, ref.lo[k]);
            hi = std::max(hi, ref.hi[k]);
        }
        if (aligned ? (w[i].min != lo || w[i].max != hi) : (w[i].min > lo || w[i].max < hi)) ++bad;
    }
}

void t_peaks() {
    struct Case {
        const char *name;
        bool opus;
        AudioFormat f;
        uint64_t frames;
    };
    const Case cases[] = {{"FLAC mono 16-bit", false, afmt(48000, 1, 16), 70000},
                          {"FLAC stereo 24-bit", false, afmt(44100, 2, 24), 30001},
                          {"Opus 48 kHz stereo", true, afmt(48000, 2, 16), 48000 * 2},
                          {"Opus 16 kHz mono", true, afmt(16000, 1, 16), 16000 * 3}};
    for (const Case &c : cases) {
        MemOps m;
        const std::string name = c.opus ? "take_020_nodate.opus" : "take_020_nodate.flac";
        const std::string path = std::string(kDir) + "/" + name;
        const Bytes file = c.opus ? opus_bytes(c.f, c.frames, "x") : flac_bytes(c.f, c.frames, "x");
        m.put(path, file);
        const Decoded d = decode_bytes(name, file);
        CHECK(d.ok);
        const PeakRef ref = peak_ref(d.pcm, d.info.output);
        std::string err;
        uint64_t last = 0, total = 0;
        bool mono = true;
        PeakBuildOptions o;
        o.decoders = &fakes().libs;
        o.progress = [&](uint64_t done, uint64_t t) {
            if (done < last) mono = false;
            last = done;
            total = t;
        };
        auto P = PeakPyramid::open_or_build(m, path, &err, o);
        CHECK(P != nullptr);
        if (!P) {
            std::fprintf(stderr, "[%s] %s\n", c.name, err.c_str());
            continue;
        }
        CHECK(P->rebuilt() && !P->in_memory());
        CHECK(mono && last == total && total == d.info.total_frames);
        CHECK_EQ(P->frames(), d.info.total_frames);
        CHECK_EQ(P->rate(), d.info.output.rate);
        CHECK(m.exists(PeakPyramid::cache_path(path)));
        int bad = 0;
        const uint64_t F = P->frames();
        check_bars(*P, ref, 0, F, 1, bad);
        check_bars(*P, ref, 0, (F + 299) / 300, 300, bad);
        check_bars(*P, ref, 0, 256, (F + 255) / 256, bad);
        check_bars(*P, ref, 1024, 1024, 20, bad);
        for (int i = 0; i < 12; ++i) {
            const uint64_t a = (static_cast<uint64_t>(i) * 7919) % (F - 300);
            check_bars(*P, ref, a, 1 + static_cast<uint64_t>(i) * 13 % 200, 25, bad); /* exact bars: decoded */
        }
        CHECK_EQ(bad, 0);
        if (bad) std::fprintf(stderr, "[%s] %d bars differ\n", c.name, bad);
        /* two threads asking for exact bars at once */
        std::thread t1([&] { for (int i = 0; i < 20; ++i) P->window(static_cast<uint64_t>(i) * 1000, 10, 50); });
        std::thread t2([&] { for (int i = 0; i < 20; ++i) P->window(F - 1 - static_cast<uint64_t>(i) * 900, 7, 40); });
        t1.join();
        t2.join();
        P.reset();
        /* the cache is reused; a changed take rebuilds it */
        P = PeakPyramid::open_or_build(m, path, &err, o);
        CHECK(P && !P->rebuilt());
        P.reset();
        m.put(path, file); /* new mtime */
        P = PeakPyramid::open_or_build(m, path, &err, o);
        CHECK(P && P->rebuilt());
    }
    /* cancel, a missing decoder */
    {
        MemOps m;
        const std::string path = std::string(kDir) + "/take_021_nodate.flac";
        m.put(path, flac_bytes(afmt(48000, 1, 16), 200000, "x"));
        std::atomic<bool> stop{true};
        PeakBuildOptions o;
        o.decoders = &fakes().libs;
        o.cancel = &stop;
        std::string err;
        CHECK(PeakPyramid::open_or_build(m, path, &err, o) == nullptr);
        CHECK_EQ(err, "cancelled");
        DecoderLibs none;
        PeakBuildOptions o2;
        o2.decoders = &none;
        CHECK(PeakPyramid::open_or_build(m, path, &err, o2) == nullptr);
        CHECK(err.find("FLAC not installed (libflac14)") != std::string::npos);
        for (const std::string &n : m.names(kDir)) CHECK(n.compare(0, 5, ".tmp-") != 0);
    }
}

/* A kill at every step of a peak build of a FLAC and an Opus take: a cache is absent or valid, the next open is right. */
void t_peaks_every_step() {
    for (int opus = 0; opus < 2; ++opus) {
        const std::string name = opus ? "take_022_nodate.opus" : "take_022_nodate.flac";
        const std::string path = std::string(kDir) + "/" + name;
        const AudioFormat f = afmt(48000, 1, 16);
        const Bytes file = opus ? opus_bytes(f, 48000, "x") : flac_bytes(f, 20000, "x");
        const Decoded d = decode_bytes(name, file);
        const PeakRef ref = peak_ref(d.pcm, d.info.output);
        int lo = 32767, hi = -32768;
        for (size_t i = 0; i < ref.lo.size(); ++i) {
            lo = std::min(lo, ref.lo[i]);
            hi = std::max(hi, ref.hi[i]);
        }
        int T = 0, bad = 0;
        PeakBuildOptions o;
        o.decoders = &fakes().libs;
        for (int n = 0; n <= 400; n++) {
            MemOps m;
            m.put(path, file);
            m.kill_at = n == 0 ? -1 : n;
            std::string err;
            { auto P = PeakPyramid::open_or_build(m, path, &err, o); }
            if (n == 0) T = m.step;
            const bool killed = m.dead;
            m.dead = false;
            m.kill_at = -1;
            cleanup_temporaries(m, kDir);
            for (const std::string &nm : m.names(kDir))
                if (nm.compare(0, 5, ".tmp-") == 0) ++bad;
            auto P = PeakPyramid::open_or_build(m, path, &err, o);
            if (!P) {
                ++bad;
                continue;
            }
            const std::vector<PeakPair> w = P->window(0, P->frames(), 1);
            if (P->frames() != ref.lo.size() || w[0].min != lo || w[0].max != hi || m.get(path) != file) ++bad;
            if (!killed && n > 0) break;
        }
        CHECK_EQ(bad, 0);
        CHECK(T > 5);
    }
}

}  // namespace

void test_codec_edit() {
    t_results();
    t_errors();
    t_every_step();
    t_peaks();
    t_peaks_every_step();
}
