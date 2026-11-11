/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests: MERGE TAKES (src/core/edit/merge.*), on the in-memory file system with kill and failure injection
 * (helpers.hpp), the test encoders (fixture_codecs.hpp) and the fake decoders (fake_decoders.hpp). Every audio value is
 * generated here.
 *   - the compatibility rule (the matrix of codecs, rates, channels, depths; names; the reason texts);
 *   - the plan: the order chosen, at least two takes, none twice, the new name, the free space (total size + 5 %);
 *   - WAV: the takes' PCM end to end behind a fresh BWF header (bext from the request), in the order chosen; RF64 when the
 *     whole passes 4 GiB (generated multi-GiB sources, the output's bytes beyond 64 KB discarded);
 *   - FLAC: decoded and encoded again: the result decodes to exactly the takes' samples end to end;
 *   - Opus: packets copied, no encoder: the packet sequence is the takes' (less a later take's packets wholly inside its
 *     pre-skip), and the result decodes to each take's own decode at its place, with the seams as the plan says;
 *   - cancel, a take that changes during the merge, a missing FLAC decoder, delete originals (only after success, the peak
 *     caches too, the commit callback before any delete, a failed delete is a warning);
 *   - crash safety: every file operation of a WAV, FLAC and Opus merge, keeping and deleting the originals, made to fail
 *     (EIO, ENOSPC) and to be the point of a kill: afterwards every original is as it was (or deleted only when the new take
 *     is complete), never a half take under the new name, and cleanup_temporaries() leaves nothing.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "merge.hpp"
#include "peaks.hpp"

#include "../fake_decoders.hpp"
#include "../fixture_codecs.hpp"
#include "codec/flac_decoder.hpp"
#include "codec/ogg_opus.hpp"
#include "codec/stream_scan.hpp"

using namespace tst;

namespace {

const char *kDir = "/rec";

struct MFakes {
    FlacDecLib flac = fakedec::make_flac_dec_lib();
    OpusDecLib opus = fakedec::make_opus_dec_lib();
    DecoderLibs libs;
    MFakes() {
        libs.flac = &flac;
        libs.opus = &opus;
    }
};
MFakes &mfakes() {
    static MFakes f;
    return f;
}

AudioFormat af(uint32_t rate, uint16_t ch, uint16_t bits) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

std::string P(const std::string &name) { return std::string(kDir) + "/" + name; }

/* A Cassette-like WAV take: the BWF header and frames first .. first + frames - 1 of the generated signal. */
Bytes wav_take(const AudioFormat &f, uint64_t first, uint64_t frames) {
    const Bytes audio = make_audio(f, first, frames);
    BwfInfo info;
    info.description = "Cassette source";
    info.origination_date = "2026-11-08";
    info.origination_time = "11:05:00";
    Bytes file = build_header(f, info, audio.size());
    file.insert(file.end(), audio.begin(), audio.end());
    return file;
}

Bytes flac_take(const AudioFormat &f, uint64_t first, uint64_t frames, const std::string &stem) {
    MemoryByteSink s;
    fx::VerbatimFlac e(f, 4096, 128);
    e.start(s, {{"TITLE", stem}, {"ENCODER", "Cassette"}});
    const Bytes pcm = fx::pcm(f, first, frames);
    e.write(pcm.data(), static_cast<size_t>(frames));
    e.finish();
    return s.data;
}

Bytes opus_take(const AudioFormat &f, uint64_t frames, const std::string &stem, uint32_t seed, uint32_t preskip = 312,
                unsigned page_packets = 10) {
    MemoryByteSink s;
    fx::PlainOggOpus e(f, 60 + seed % 40, page_packets, preskip, seed);
    e.start(s, {{"TITLE", stem}, {"ENCODER", "Cassette"}});
    const Bytes pcm(static_cast<size_t>(frames) * f.channels * 2, 0);
    e.write(pcm.data(), static_cast<size_t>(frames));
    e.finish();
    return s.data;
}

struct Decoded {
    bool ok = false;
    DecodedInfo info;
    Bytes pcm;
};
Decoded decode_file(const std::string &name, const Bytes &file) {
    Decoded d;
    MemOps v;
    v.put(P(name), file);
    int err = 0;
    std::unique_ptr<Decoder> dec = make_decoder(v, P(name), mfakes().libs, d.info, &err);
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

std::string tagv(const Tags &t, const std::string &name) {
    for (const Tag &g : t)
        if (g.name == name) return g.value;
    return "";
}

/* The packets of an Ogg Opus file in order (independent of the engine: OggOpusFile only). */
struct OpusWalk {
    std::vector<Bytes> packets;
    std::vector<int64_t> starts, ends;
    int64_t start48 = 0, end48 = 0;
    uint32_t preskip = 0;
    unsigned k = 1;
};
OpusWalk walk_opus(const Bytes &file) {
    OpusWalk w;
    MemOps v;
    v.put(P("w.opus"), file);
    oggopus::OggOpusFile f(v);
    if (f.open(P("w.opus")) != 0) return w;
    w.start48 = f.start48();
    w.end48 = f.end48();
    w.preskip = f.head().preskip;
    w.k = 48000u / oggopus::decode_rate(f.head().input_rate);
    oggopus::Packet p;
    while (f.next(p) == 1) {
        w.starts.push_back(p.start48);
        w.ends.push_back(p.end48);
        w.packets.push_back(p.data);
    }
    return w;
}

/* ---------------------------------------------------------------- a merge on the in-memory folder */

enum class Kind { Wav, Flac, Opus };

struct MScen {
    const char *name;
    Kind kind;
    AudioFormat fmt;
    std::vector<uint64_t> frames;  /* per source */
    std::vector<int> order;        /* the order chosen (indexes into the sources) */
    bool delete_originals = false;
    std::vector<uint32_t> preskips; /* Opus: per source (empty: 312 each) */
};

std::string src_name(const MScen &sc, size_t i) {
    const char *ext = sc.kind == Kind::Wav ? ".wav" : sc.kind == Kind::Flac ? ".flac" : ".opus";
    return "take_00" + std::to_string(i + 1) + "_20261108-110" + std::to_string(i) + ext;
}
std::string out_name(const MScen &sc) {
    const char *ext = sc.kind == Kind::Wav ? ".wav" : sc.kind == Kind::Flac ? ".flac" : ".opus";
    return std::string("take_009_20261111-0400") + ext;
}

struct Built {
    std::vector<Bytes> files;      /* per source */
    std::vector<uint64_t> starts;  /* WAV / FLAC: the first frame index of the generated signal per source */
};

Built build_sources(const MScen &sc) {
    Built b;
    uint64_t at = 0;
    for (size_t i = 0; i < sc.frames.size(); ++i) {
        b.starts.push_back(at);
        const std::string stem = src_name(sc, i).substr(0, src_name(sc, i).rfind('.'));
        if (sc.kind == Kind::Wav) b.files.push_back(wav_take(sc.fmt, at, sc.frames[i]));
        else if (sc.kind == Kind::Flac) b.files.push_back(flac_take(sc.fmt, at, sc.frames[i], stem));
        else b.files.push_back(opus_take(sc.fmt, sc.frames[i], stem, static_cast<uint32_t>(7 + 13 * i),
                                         sc.preskips.empty() ? 312 : sc.preskips[i]));
        at += sc.frames[i] + 1000; /* distinct audio per source */
    }
    return b;
}

/* "" when `file` is exactly the merge the scenario asks for. */
std::string check_output(const MScen &sc, const Built &b, const Bytes &file) {
    if (sc.kind == Kind::Wav) {
        Bytes audio;
        for (int i : sc.order) {
            const Bytes a = make_audio(sc.fmt, b.starts[static_cast<size_t>(i)], sc.frames[static_cast<size_t>(i)]);
            audio.insert(audio.end(), a.begin(), a.end());
        }
        return valid_result(file, audio, sc.fmt) ? "" : "the WAV is not the takes end to end";
    }
    const Decoded d = decode_file(out_name(sc), file);
    if (!d.ok) return "the new take does not decode";
    if (sc.kind == Kind::Flac) {
        Bytes want;
        for (int i : sc.order) {
            const Decoded s = decode_file(src_name(sc, static_cast<size_t>(i)), b.files[static_cast<size_t>(i)]);
            want.insert(want.end(), s.pcm.begin(), s.pcm.end());
        }
        return d.pcm == want ? "" : "the FLAC does not decode to the takes end to end";
    }
    /* Opus: the packets in order (a later take's packets inside its pre-skip left out), and each take's own decode at its
     * place in the result */
    const OpusWalk out = walk_opus(file);
    std::vector<Bytes> want_pk;
    int64_t at48 = 0;                     /* the result's 48 kHz position of the next packet */
    std::vector<int64_t> first_sample48;  /* per take in order: where its first sample lands in the result */
    int64_t tail_last = 0;
    for (size_t o = 0; o < sc.order.size(); ++o) {
        const OpusWalk w = walk_opus(b.files[static_cast<size_t>(sc.order[o])]);
        const int64_t first = w.start48 + w.preskip;
        bool kept_any = false;
        for (size_t j = 0; j < w.packets.size(); ++j) {
            if (o > 0 && w.ends[j] <= first) continue;
            if (!kept_any) first_sample48.push_back(at48 + (first - w.starts[j]));
            kept_any = true;
            want_pk.push_back(w.packets[j]);
            at48 += w.ends[j] - w.starts[j];
        }
        tail_last = w.ends.back() - w.end48;
    }
    if (out.packets != want_pk) return "the Opus packets are not the takes' in order";
    if (out.preskip != walk_opus(b.files[static_cast<size_t>(sc.order[0])]).preskip) return "the pre-skip is not the first take's";
    if (out.end48 != at48 - tail_last) return "the end trimming is not the last take's";
    const size_t ch = sc.fmt.channels;
    for (size_t o = 0; o < sc.order.size(); ++o) {
        const Decoded s = decode_file(src_name(sc, static_cast<size_t>(sc.order[o])), b.files[static_cast<size_t>(sc.order[o])]);
        const int64_t off = (first_sample48[o] - static_cast<int64_t>(out.preskip)) / static_cast<int64_t>(out.k);
        const size_t from = static_cast<size_t>(off) * ch * 2;
        if (from + s.pcm.size() > d.pcm.size() || !std::equal(s.pcm.begin(), s.pcm.end(), d.pcm.begin() + static_cast<long>(from)))
            return "take " + std::to_string(o + 1) + " does not decode at its place in the result";
    }
    return "";
}

/* The folder after a merge (killed, failed or done): every original as it was, or (deleting) gone only when the new take is
 * complete; the new name absent or the complete merge; temporaries only when allowed. */
std::string judge(MemOps &m, const MScen &sc, const Built &b, bool allow_tmp, bool *have_out = nullptr) {
    std::string bad;
    std::set<std::string> present;
    for (const std::string &n : m.names(kDir)) present.insert(n);
    const bool out_there = present.count(out_name(sc)) != 0;
    bool out_ok = false;
    if (out_there) {
        const std::string why = check_output(sc, b, m.get(P(out_name(sc))));
        if (!why.empty()) bad += "a broken take under the new name: " + why + "; ";
        else out_ok = true;
    }
    if (have_out) *have_out = out_ok;
    for (const std::string &n : present) {
        if (n.compare(0, 5, ".tmp-") == 0) {
            if (!allow_tmp) bad += "temporary left: " + n + "; ";
            continue;
        }
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".peaks") == 0) continue;
        if (n == out_name(sc)) continue;
        bool src = false;
        for (size_t i = 0; i < b.files.size(); ++i)
            if (n == src_name(sc, i)) {
                src = true;
                if (m.get(P(n)) != b.files[i]) bad += "an original changed: " + n + "; ";
            }
        if (!src) bad += "unexpected file " + n + "; ";
    }
    for (size_t i = 0; i < b.files.size(); ++i)
        if (!present.count(src_name(sc, i)) && !(sc.delete_originals && out_ok)) bad += "an original is gone: " + src_name(sc, i) + "; ";
    return bad;
}

struct MOutcome {
    MergeResult res;
    int steps = 0;
    std::string problems;
    std::vector<std::string> events; /* "commit", "delete <name>" in order */
};

MergeRequest request_of(const MScen &sc) {
    MergeRequest r;
    for (int i : sc.order) r.sources.push_back(P(src_name(sc, static_cast<size_t>(i))));
    r.output_name = out_name(sc);
    r.delete_originals = sc.delete_originals;
    r.tags = {{"TITLE", "take_009_20261111-0400"}, {"DATE", "2026-11-11"}, {"ENCODER", "Cassette"}};
    r.bwf.description = "Cassette take_009_20261111-0400";
    r.bwf.origination_date = "2026-11-11";
    r.bwf.origination_time = "04:00:07";
    r.bwf.time_reference = 4ull * 3600 * 48000 + 7 * 48000;
    return r;
}

MergeOptions options_for(MemOps &m, MOutcome &out) {
    MergeOptions o;
    o.ops = &m;
    o.decoders = &mfakes().libs;
    o.make_flac_encoder = [](const AudioFormat &f) { return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096, 128)); };
    o.block_bytes = 4096;
    o.on_committed = [&out](const std::string &) { out.events.push_back("commit"); };
    return o;
}

MOutcome run(const MScen &sc, int kill_at, int fail_at, int fail_errno = EIO) {
    const Built b = build_sources(sc);
    MemOps m;
    for (size_t i = 0; i < b.files.size(); ++i) {
        m.put(P(src_name(sc, i)), b.files[i]);
        m.put(P(src_name(sc, i)) + ".peaks", Bytes(40, 1));
    }
    m.kill_at = kill_at;
    m.fail_at = fail_at;
    m.fail_errno = fail_errno;
    MOutcome out;
    MergeOptions o = options_for(m, out);
    uint64_t last = 0, total = 0;
    bool mono = true, first = true;
    o.progress = [&](uint64_t d, uint64_t t) {
        if (first && d != 0) mono = false;
        first = false;
        if (d < last) mono = false;
        last = d;
        total = t;
    };
    out.res = merge_takes(request_of(sc), o);
    out.steps = m.step;
    out.problems = judge(m, sc, b, true);
    if (!mono) out.problems += "progress went backwards; ";
    if (out.res.ok && kill_at < 0 && fail_at < 0 && last != total) out.problems += "progress did not reach the total; ";
    m.dead = false;
    m.kill_at = -1;
    m.fail_at = -1;
    cleanup_temporaries(m, kDir);
    const std::string after = judge(m, sc, b, false);
    if (!after.empty()) out.problems += "after cleanup: " + after;
    if (kill_at < 0 && fail_at < 0 && out.res.ok) {
        if (!m.exists(P(out_name(sc)))) out.problems += "clean run: no new take; ";
        for (size_t i = 0; i < b.files.size(); ++i) {
            const bool there = m.exists(P(src_name(sc, i)));
            if (sc.delete_originals && there) out.problems += "clean delete kept " + src_name(sc, i) + "; ";
            if (sc.delete_originals && m.exists(P(src_name(sc, i)) + ".peaks")) out.problems += "a peak cache of a deleted take stayed; ";
            if (!sc.delete_originals && !there) out.problems += "clean keep lost " + src_name(sc, i) + "; ";
        }
    }
    return out;
}

const MScen kScens[] = {
    {"WAV mono 16-bit, keep", Kind::Wav, af(48000, 1, 16), {3000, 1700, 2500}, {2, 0, 1}, false, {}},
    {"WAV stereo 24-bit, delete", Kind::Wav, af(44100, 2, 24), {1500, 2200}, {1, 0}, true, {}},
    {"FLAC mono 16-bit, keep", Kind::Flac, af(48000, 1, 16), {5000, 4096, 1200}, {0, 2, 1}, false, {}},
    {"FLAC stereo 24-bit, delete", Kind::Flac, af(44100, 2, 24), {3000, 2500}, {1, 0}, true, {}},
    {"Opus 48 kHz mono, keep", Kind::Opus, af(48000, 1, 16), {24000, 12345, 30000}, {1, 2, 0}, false, {}},
    {"Opus 16 kHz stereo, an edited take (320 ms pre-roll), delete", Kind::Opus, af(16000, 2, 16), {16000, 9000, 20000},
     {0, 1, 2}, true, {312, 15360 + 500, 312}},
};

/* ---------------------------------------------------------------- the rule and the plan */

void t_compat() {
    MergeFacts w;
    w.named = true;
    w.kind = EditKind::Wav;
    w.rate = 48000;
    w.channels = 1;
    w.bits = 16;
    CHECK_EQ(merge_join_reason(w, w), "");
    MergeFacts x = w;
    x.kind = EditKind::Flac;
    x.rate = 44100;
    CHECK_EQ(merge_join_reason(w, x), "Different format: FLAC 44.1 kHz");
    x = w;
    x.kind = EditKind::OggOpus;
    CHECK_EQ(merge_join_reason(w, x), "Different format: Opus 48 kHz");
    x = w;
    x.rate = 44100;
    CHECK_EQ(merge_join_reason(w, x), "Different rate");
    x = w;
    x.channels = 2;
    CHECK_EQ(merge_join_reason(w, x), "Different channels");
    x = w;
    x.bits = 24;
    CHECK_EQ(merge_join_reason(w, x), "Different bit depth");
    x = w;
    x.named = false;
    CHECK_EQ(merge_join_reason(w, x), "Not a Cassette take");
    CHECK_EQ(merge_take_reason(x), "Not a Cassette take");
    x = w;
    x.readable = false;
    CHECK_EQ(merge_join_reason(w, x), "Damaged take");
    /* the first difference wins: codec, then rate, channels, depth */
    x = w;
    x.rate = 16000;
    x.channels = 2;
    x.bits = 24;
    CHECK_EQ(merge_join_reason(w, x), "Different rate");
    x.rate = 48000;
    CHECK_EQ(merge_join_reason(w, x), "Different channels");
    /* Opus: the depth is not compared (always 16 on the decoder's side; the bit rate is not stored) */
    MergeFacts o = w;
    o.kind = EditKind::OggOpus;
    MergeFacts o2 = o;
    o2.bits = 24;
    CHECK_EQ(merge_join_reason(o, o2), "");
    o2 = o;
    o2.rate = 16000;
    CHECK_EQ(merge_join_reason(o, o2), "Different rate");
    /* FLAC depth */
    MergeFacts f = w;
    f.kind = EditKind::Flac;
    MergeFacts f2 = f;
    f2.bits = 24;
    CHECK_EQ(merge_join_reason(f, f2), "Different bit depth");
    CHECK_EQ(merge_format_label(EditKind::Wav, 48000), "WAV 48 kHz");
    CHECK_EQ(merge_format_label(EditKind::OggOpus, 16000), "Opus 16 kHz");
    CHECK_EQ(merge_format_label(EditKind::Flac, 22050), "FLAC 22.05 kHz");
}

void t_plan() {
    const MScen &sc = kScens[0];
    const Built b = build_sources(sc);
    MemOps m;
    for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
    MOutcome dummy;
    MergeOptions o = options_for(m, dummy);
    MergeRequest r = request_of(sc);
    MergePlan p = plan_merge(r, o);
    CHECK(p.ok);
    CHECK(p.kind == EditKind::Wav);
    CHECK_EQ(p.takes.size(), 3u);
    CHECK_EQ(p.source_frames, 3000u + 1700u + 2500u);
    CHECK_EQ(p.frames, p.source_frames);
    CHECK_EQ(p.seam_frames, 0u);
    uint64_t bytes = 0;
    for (const Bytes &f : b.files) bytes += f.size();
    CHECK_EQ(p.source_bytes, bytes);
    CHECK_EQ(p.need_bytes, bytes + (bytes * 5 + 99) / 100);
    CHECK_EQ(p.estimate_bytes, uint64_t(kDataOffset) + p.source_frames * 2u);
    /* the order chosen is kept */
    CHECK_EQ(p.takes[0].frames, 2500u);
    CHECK_EQ(p.takes[1].frames, 3000u);
    /* free space: exactly the need passes, one byte less is refused */
    m.free_bytes = p.need_bytes;
    CHECK(plan_merge(r, o).ok);
    m.free_bytes = p.need_bytes - 1;
    p = plan_merge(r, o);
    CHECK(!p.ok && p.error == MergeError::NoSpace);
    CHECK_EQ(p.message, "Not enough free space");
    m.free_bytes = 1ull << 40;
    /* fewer than two, twice, the name */
    MergeRequest one = r;
    one.sources.resize(1);
    p = plan_merge(one, o);
    CHECK(!p.ok && p.error == MergeError::BadSources);
    CHECK_EQ(p.message, "Merging needs two takes");
    MergeRequest twice = r;
    twice.sources[2] = twice.sources[0];
    CHECK(plan_merge(twice, o).error == MergeError::BadSources);
    MergeRequest name = r;
    name.output_name = "x.flac"; /* not the takes' extension */
    CHECK(plan_merge(name, o).error == MergeError::BadName);
    name.output_name = src_name(sc, 1); /* exists */
    p = plan_merge(name, o);
    CHECK(p.error == MergeError::BadName);
    name.output_name = ".tmp-x.wav";
    CHECK(plan_merge(name, o).error == MergeError::BadName);
    /* an incompatible take: the message names it and says why */
    m.put(P("take_004_20261108-1110.wav"), wav_take(af(44100, 1, 16), 0, 100));
    MergeRequest inc = r;
    inc.sources.push_back(P("take_004_20261108-1110.wav"));
    p = plan_merge(inc, o);
    CHECK(!p.ok && p.error == MergeError::Incompatible);
    CHECK_EQ(p.message, "take_004_20261108-1110.wav: Different rate");
    /* a damaged take */
    m.put(P("take_005_20261108-1111.wav"), Bytes(100, 7));
    MergeRequest dam = r;
    dam.sources.push_back(P("take_005_20261108-1111.wav"));
    CHECK(plan_merge(dam, o).error == MergeError::BadSources);
    /* the plan writes nothing */
    for (const std::string &n : m.names(kDir)) CHECK(n.compare(0, 5, ".tmp-") != 0);
}

/* ---------------------------------------------------------------- the joins */

void t_joins() {
    for (const MScen &sc : kScens) {
        const MOutcome o = run(sc, -1, -1);
        CHECK(o.res.ok);
        if (!o.res.ok) std::fprintf(stderr, "[%s] %s\n", sc.name, o.res.message.c_str());
        CHECK(o.problems.empty());
        if (!o.problems.empty()) std::fprintf(stderr, "[%s] %s\n", sc.name, o.problems.c_str());
        if (sc.delete_originals) {
            CHECK_EQ(o.res.deleted.size(), sc.order.size());
            CHECK(!o.events.empty() && o.events.front() == "commit"); /* the commit before the deletes */
        }
        uint64_t sum = 0;
        for (uint64_t f : sc.frames) sum += f;
        if (sc.kind != Kind::Opus) CHECK_EQ(o.res.frames, sum);
        else CHECK(o.res.frames == sum + o.res.seam_frames && o.res.seam_frames > 0);
    }
    /* WAV: the header is a new take's (bext from the request) */
    {
        const MScen &sc = kScens[0];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        const MergeResult r = merge_takes(request_of(sc), options_for(m, out));
        CHECK(r.ok);
        const Bytes f = m.get(P(out_name(sc)));
        const HeaderInfo h = parse_header(f.data(), f.size(), f.size());
        CHECK(h.ok && h.has_bext && !h.rf64);
        const std::string s(f.begin(), f.begin() + static_cast<long>(kDataOffset));
        CHECK(s.find("Cassette take_009_20261111-0400") != std::string::npos);
        CHECK(s.find("2026-11-1104:00:07") != std::string::npos); /* date and time fields side by side in bext */
    }
    /* FLAC and Opus: the comments are the request's */
    for (int k : {2, 4}) {
        const MScen &sc = kScens[k];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        const MergeResult r = merge_takes(request_of(sc), options_for(m, out));
        CHECK(r.ok);
        const Decoded d = decode_file(out_name(sc), m.get(P(out_name(sc))));
        CHECK(d.ok);
        CHECK_EQ(tagv(d.info.tags, "TITLE"), "take_009_20261111-0400");
        CHECK_EQ(tagv(d.info.tags, "DATE"), "2026-11-11");
        if (k == 2) {
            const StreamInfo si = read_stream_info(m, P(out_name(sc)));
            CHECK(si.ok && si.frames_known && si.frames == 5000u + 4096u + 1200u);
        }
    }
    /* Opus seams, exactly: 48 kHz takes of the test encoder (pre-skip 312, 20 ms packets) */
    {
        const MScen &sc = kScens[4];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        const MergePlan p = plan_merge(request_of(sc), options_for(m, out));
        CHECK(p.ok);
        /* take 2 (12345 frames, chosen first): 312 + 12345 = 12657 coded, 14 packets = 13440: tail 783; take 3 (30000):
         * 30312 coded, 32 packets = 30720: tail 408; take 1 (24000, last): its tail is trimmed. Each later take adds its 312. */
        CHECK_EQ(p.opus_tail48[0], 783);
        CHECK_EQ(p.opus_tail48[1], 408);
        CHECK_EQ(p.opus_lead48[1], 312);
        CHECK_EQ(p.opus_lead48[2], 312);
        CHECK_EQ(p.seam_frames, 783u + 312u + 408u + 312u);
        CHECK_EQ(p.opus_drop[1], 0u);
    }
    /* an edited later take: its pre-roll packets are left out, only the part of a packet before its first sample stays */
    {
        const MScen &sc = kScens[5];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        const MergePlan p = plan_merge(request_of(sc), options_for(m, out));
        CHECK(p.ok);
        CHECK_EQ(p.opus_drop[1], 16u);         /* 16 x 960 = 15360 of pre-roll */
        CHECK_EQ(p.opus_lead48[1], 500);
        CHECK(p.seam_frames < 2u * 960u / 3u); /* under two packets at 16 kHz */
    }
}

/* Two generated takes whose data together pass 4 GiB: the new take is RF64 (the output keeps only its first 64 KB). */
void t_rf64() {
    const AudioFormat f = af(48000, 2, 16);
    const uint64_t frames = (2600ull << 20) / 4; /* 2.54 GiB of stereo 16-bit each */
    MemOps m;
    m.log_io = false;
    for (int i = 0; i < 2; ++i) {
        BwfInfo info;
        const Bytes hdr = build_header(f, info, frames * 4);
        const unsigned char fill = static_cast<unsigned char>(0x11 * (i + 1));
        m.put_virtual(P("take_00" + std::to_string(i + 1) + "_nodate.wav"), kDataOffset + frames * 4,
                      [hdr, fill](uint64_t off, unsigned char *dst, size_t n) {
                          size_t k = 0;
                          while (k < n && off + k < hdr.size()) {
                              dst[k] = hdr[static_cast<size_t>(off + k)];
                              ++k;
                          }
                          if (k < n) std::memset(dst + k, fill, n - k);
                      });
    }
    m.discard_new = true;
    MergeRequest r;
    r.sources = {P("take_002_nodate.wav"), P("take_001_nodate.wav")};
    r.output_name = "take_003_nodate.wav";
    MergeOptions o;
    o.ops = &m;
    o.block_bytes = 8u << 20;
    const MergeResult res = merge_takes(r, o);
    CHECK(res.ok);
    if (!res.ok) std::fprintf(stderr, "rf64: %s\n", res.message.c_str());
    const auto it = m.files.find(P("take_003_nodate.wav"));
    CHECK(it != m.files.end());
    if (it == m.files.end()) return;
    const MemOps::File &out = *it->second;
    CHECK_EQ(out.size, uint64_t(kDataOffset) + 2 * frames * 4);
    const HeaderInfo h = parse_header(out.data.data(), out.data.size(), out.size);
    CHECK(h.ok && h.rf64 && h.has_ds64);
    CHECK_EQ(h.declared_data, 2 * frames * 4);
    CHECK(out.data.size() > kDataOffset && out.data[kDataOffset] == 0x22); /* the take chosen first comes first */
    CHECK_EQ(res.frames, 2 * frames);
}

/* ---------------------------------------------------------------- refusals, cancel, delete */

void t_cancel_and_changes() {
    /* cancel at about half: nothing under the new name, no temporary, the originals as they were */
    for (int k : {0, 2, 4}) {
        const MScen &sc = kScens[k];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        MergeOptions o = options_for(m, out);
        std::atomic<bool> cancel{false};
        o.cancel = &cancel;
        o.progress = [&](uint64_t d, uint64_t t) {
            if (d * 2 >= t && d > 0) cancel.store(true);
        };
        MergeRequest r = request_of(sc);
        r.delete_originals = true;
        const MergeResult res = merge_takes(r, o);
        CHECK(!res.ok && res.error == MergeError::Cancelled);
        CHECK(out.events.empty()); /* never committed */
        CHECK(judge(m, sc, b, false).empty());
        CHECK(!m.exists(P(out_name(sc))));
    }
    /* a take that changes during the merge: refused, nothing written */
    {
        const MScen &sc = kScens[0];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        MergeOptions o = options_for(m, out);
        bool touched = false;
        o.progress = [&](uint64_t d, uint64_t t) {
            if (!touched && d * 2 >= t && d > 0) {
                touched = true;
                m.files[P(src_name(sc, 1))]->mtime += 5;
            }
        };
        const MergeResult res = merge_takes(request_of(sc), o);
        CHECK(!res.ok && res.error == MergeError::SourceChanged);
        CHECK(!m.exists(P(out_name(sc))));
        for (const std::string &n : m.names(kDir)) CHECK(n.compare(0, 5, ".tmp-") != 0);
    }
    /* no FLAC decoder: one message, nothing written */
    {
        const MScen &sc = kScens[2];
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        MergeOptions o = options_for(m, out);
        DecoderLibs none;
        o.decoders = &none;
        const MergeResult res = merge_takes(request_of(sc), o);
        CHECK(!res.ok);
        CHECK(res.message.find("FLAC not installed (libflac14)") != std::string::npos);
        CHECK(judge(m, sc, b, false).empty());
        CHECK(!m.exists(P(out_name(sc))));
    }
    /* delete originals: a delete that fails is a warning; the merge stands, the rest are deleted */
    {
        MScen sc = kScens[0];
        sc.delete_originals = true;
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        MOutcome out;
        MergeOptions o = options_for(m, out);
        o.delete_original = [&](const std::string &path) {
            out.events.push_back("delete " + path.substr(std::strlen(kDir) + 1));
            if (path == P(src_name(sc, 0))) return -EIO;
            return m.unlink(path);
        };
        const MergeResult res = merge_takes(request_of(sc), o);
        CHECK(res.ok);
        CHECK_EQ(res.warning, "1 original could not be deleted");
        CHECK_EQ(res.deleted.size(), 2u);
        CHECK(m.exists(P(src_name(sc, 0))));
        CHECK(!m.exists(P(src_name(sc, 1))) && !m.exists(P(src_name(sc, 2))));
        CHECK(out.events.size() == 4u && out.events[0] == "commit");
        /* the deletes in the order chosen */
        CHECK_EQ(out.events[1], "delete " + src_name(sc, 2));
    }
    /* a failed merge never deletes */
    {
        MScen sc = kScens[0];
        sc.delete_originals = true;
        const Built b = build_sources(sc);
        MemOps m;
        for (size_t i = 0; i < b.files.size(); ++i) m.put(P(src_name(sc, i)), b.files[i]);
        m.free_bytes = 1000;
        MOutcome out;
        const MergeResult res = merge_takes(request_of(sc), options_for(m, out));
        CHECK(!res.ok && res.error == MergeError::NoSpace);
        CHECK(res.deleted.empty());
        CHECK(judge(m, sc, b, false).empty());
    }
}

/* ---------------------------------------------------------------- crash safety */

void t_crash() {
    int runs = 0;
    for (const MScen &sc : kScens) {
        const MOutcome clean = run(sc, -1, -1);
        CHECK(clean.res.ok && clean.problems.empty());
        for (int at = 1; at <= clean.steps; ++at) {
            const MOutcome k = run(sc, at, -1);
            if (!k.problems.empty()) std::fprintf(stderr, "[%s] kill at %d: %s\n", sc.name, at, k.problems.c_str());
            CHECK(k.problems.empty());
            for (int e : {EIO, ENOSPC}) {
                const MOutcome f = run(sc, -1, at, e);
                if (!f.problems.empty()) std::fprintf(stderr, "[%s] errno %d at %d: %s\n", sc.name, e, at, f.problems.c_str());
                CHECK(f.problems.empty());
                /* a failure before the commit never deletes */
                if (!f.res.ok) CHECK(f.res.deleted.empty());
            }
            runs += 3;
        }
    }
    std::printf("  merge crash sweep: %d runs\n", runs);
}

}  // namespace

void test_merge() {
    t_compat();
    t_plan();
    t_joins();
    t_rf64();
    t_cancel_and_changes();
    t_crash();
}
