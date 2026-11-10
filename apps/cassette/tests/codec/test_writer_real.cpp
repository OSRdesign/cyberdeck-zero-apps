/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests, phase B: the writer and the repair with the REAL libFLAC and libopusenc (skipped when they do not
 * load), checked by decoding with the real decoders, loaded here with dlopen for the test only (libFLAC's stream decoder
 * from the same libFLAC, libopusfile.so.0 when present):
 *   - FLAC takes through the writer decode sample-exact (16-bit mono speech, 24-bit stereo music), with the MD5 checked
 *   - Ogg Opus takes have exactly the recorded duration (op_pcm_total) and decode to the end
 *   - a take cut at sync points and at random points after them: repaired, it decodes with no error, its length is
 *     STREAMINFO's / the last granule's, and (FLAC) its samples are the recorded ones
 *   - the repair at every byte offset of a short real FLAC and a short real Opus stream
 *   - the test encoders' streams (fixture_codecs.hpp) are accepted by the real decoders
 *   - CPU: FLAC level 5 and Opus 128 kbps on 48 kHz stereo, the encoder alone and through the writer
 */
#include "check.hpp"

#include "../fixture_codecs.hpp"
#include "flac_encoder.hpp"
#include "journal.hpp"
#include "opus_encoder.hpp"
#include "repair.hpp"
#include "ring.hpp"
#include "siggen.hpp"
#include "stream_scan.hpp"
#include "writer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

using namespace cassette;

namespace {

using Data = std::vector<unsigned char>;

struct Tmp {
    std::string path;
    Tmp() {
        std::string t = (std::filesystem::temp_directory_path() / "cassette-codec-w-XXXXXX").string();
        std::vector<char> b(t.begin(), t.end());
        b.push_back('\0');
        if (!mkdtemp(b.data())) std::abort();
        path = b.data();
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string &n) const { return path + "/" + n; }
};

Data slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return Data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void spit(const std::string &p, const Data &d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(d.data()), static_cast<std::streamsize>(d.size()));
}

class FakeClock final : public Clock {
public:
    uint64_t now = 1000;
    int64_t wall = 1700000000;
    uint64_t mono_ms() override { return now; }
    int64_t wall_s() override { return wall; }
    void sleep_ms(uint32_t ms) override { now += ms; }
};

double thread_cpu_s() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

std::vector<int32_t> samples(const sig::Bytes &pcm, const AudioFormat &f, size_t from_frame, size_t frames) {
    std::vector<int32_t> s;
    for (size_t i = from_frame * f.channels; i < (from_frame + frames) * f.channels; ++i) s.push_back(sig::get(pcm, i, f.bits));
    return s;
}

/* ---------------------------------------------------------------- the real decoders (test only) */

struct FlacDec {
    using WriteCb = int (*)(const void *, const void *, const int32_t *const[], void *);
    using ErrorCb = void (*)(const void *, int, void *);
    void *(*make)() = nullptr;
    int (*set_md5)(void *, int) = nullptr;
    int (*init_file)(void *, const char *, WriteCb, void *, ErrorCb, void *) = nullptr;
    int (*process)(void *) = nullptr;
    int (*finish)(void *) = nullptr;
    void (*destroy)(void *) = nullptr;
    bool ok() const { return make && set_md5 && init_file && process && finish && destroy; }
};

struct Decoded {
    bool opened = false;
    std::vector<int32_t> s;
    int errors = 0;
    bool md5_ok = true;
    unsigned channels = 0;
};

Decoded flac_decode(const FlacDec &d, const std::string &path) {
    Decoded out;
    void *dec = d.make();
    if (!dec) return out;
    d.set_md5(dec, 1);
    auto wcb = [](const void *, const void *frame, const int32_t *const buf[], void *client) -> int {
        Decoded *o = static_cast<Decoded *>(client);
        const uint32_t *h = static_cast<const uint32_t *>(frame); /* FLAC__FrameHeader: blocksize, sample_rate, channels */
        o->channels = h[2];
        for (uint32_t i = 0; i < h[0]; ++i)
            for (uint32_t c = 0; c < h[2]; ++c) o->s.push_back(buf[c][i]);
        return 0;
    };
    auto ecb = [](const void *, int, void *client) { static_cast<Decoded *>(client)->errors++; };
    if (d.init_file(dec, path.c_str(), wcb, nullptr, ecb, &out) == 0) {
        out.opened = true;
        if (!d.process(dec)) out.errors++;
        out.md5_ok = d.finish(dec) != 0;
    }
    d.destroy(dec);
    return out;
}

FlacDec load_flac_decoder(const FlacLib &lib) {
    FlacDec d;
    void *h = dlopen(lib.name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return d;
    d.make = reinterpret_cast<void *(*)()>(dlsym(h, "FLAC__stream_decoder_new"));
    d.set_md5 = reinterpret_cast<int (*)(void *, int)>(dlsym(h, "FLAC__stream_decoder_set_md5_checking"));
    d.init_file = reinterpret_cast<int (*)(void *, const char *, FlacDec::WriteCb, void *, FlacDec::ErrorCb, void *)>(
        dlsym(h, "FLAC__stream_decoder_init_file"));
    d.process = reinterpret_cast<int (*)(void *)>(dlsym(h, "FLAC__stream_decoder_process_until_end_of_stream"));
    d.finish = reinterpret_cast<int (*)(void *)>(dlsym(h, "FLAC__stream_decoder_finish"));
    d.destroy = reinterpret_cast<void (*)(void *)>(dlsym(h, "FLAC__stream_decoder_delete"));
    return d; /* never closed: the process keeps libFLAC loaded anyway */
}

struct OpusFile {
    void *(*open_file)(const char *, int *) = nullptr;
    void (*free_)(void *) = nullptr;
    int64_t (*pcm_total)(const void *, int) = nullptr;
    int (*channel_count)(const void *, int) = nullptr;
    int (*read)(void *, int16_t *, int, int *) = nullptr;
    bool ok() const { return open_file && free_ && pcm_total && channel_count && read; }
};

OpusFile load_opusfile() {
    OpusFile o;
    void *h = dlopen("libopusfile.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return o;
    o.open_file = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(h, "op_open_file"));
    o.free_ = reinterpret_cast<void (*)(void *)>(dlsym(h, "op_free"));
    o.pcm_total = reinterpret_cast<int64_t (*)(const void *, int)>(dlsym(h, "op_pcm_total"));
    o.channel_count = reinterpret_cast<int (*)(const void *, int)>(dlsym(h, "op_channel_count"));
    o.read = reinterpret_cast<int (*)(void *, int16_t *, int, int *)>(dlsym(h, "op_read"));
    return o;
}

struct OpusDecoded {
    bool opened = false;
    int64_t total = -1;  /* op_pcm_total: 48 kHz samples after the pre-skip */
    int64_t read = 0;    /* samples per channel read to the end */
    int channels = 0;
    bool read_error = false;
};

OpusDecoded opus_decode(const OpusFile &of, const std::string &path, bool read_all = true) {
    OpusDecoded d;
    int err = 0;
    void *f = of.open_file(path.c_str(), &err);
    if (!f) return d;
    d.opened = true;
    d.total = of.pcm_total(f, -1);
    d.channels = of.channel_count(f, -1);
    if (read_all) {
        std::vector<int16_t> buf(5760 * 2);
        for (;;) {
            const int n = of.read(f, buf.data(), static_cast<int>(buf.size()), nullptr);
            if (n < 0) {
                d.read_error = true;
                break;
            }
            if (n == 0) break;
            d.read += n;
        }
    }
    of.free_(f);
    return d;
}

/* ---------------------------------------------------------------- the writer on the real encoders */

struct Take {
    std::vector<Data> snaps;          /* the file right after each sync */
    std::vector<uint64_t> snap_synced;
    Data journal_at_snap;
    std::string path;
    uint64_t frames = 0;
    WriterStatus st;
};

/* Records `seconds` of pcm through the writer (fake clock, sync every second, 0.25 s steps). */
Take record(const Tmp &t, const AudioFormat &f, const sig::Bytes &pcm, size_t frames, const std::string &ext,
            std::function<std::unique_ptr<Encoder>()> make, const std::string &codec) {
    Take k;
    FakeClock clk;
    Ring ring(f, 8.0);
    WriterConfig cfg;
    cfg.fmt = f;
    cfg.take = "real";
    cfg.recordings_dir = t.path;
    k.path = t.file("real" + ext);
    const std::string p = k.path;
    cfg.namer = [p](int) { return p; };
    cfg.min_free_bytes = 0;
    cfg.sync_interval_ms = 1000;
    cfg.codec = codec;
    cfg.make_encoder = [make](int) { return make(); };
    cfg.tags = [](int) { return Tags{{"TITLE", "real"}}; };
    Writer w(ring, posix_file_ops(), clk, cfg);
    CHECK_EQ(w.start(), 0);
    const size_t fb = f.channels * (f.bits / 8u);
    const size_t step = f.rate / 4;
    uint64_t syncs = 0;
    for (size_t at = 0; at < frames; at += step) {
        const size_t n = std::min(step, frames - at);
        CHECK_EQ(ring.write(pcm.data() + at * fb, n), n);
        clk.now += 250;
        bool busy = true;
        while (busy && w.pump(&busy)) {
        }
        const WriterStatus s = w.status();
        if (s.syncs != syncs) {
            syncs = s.syncs;
            k.snaps.push_back(slurp(p));
            k.snap_synced.push_back(s.frames_synced);
            k.journal_at_snap = slurp(journal_file(t.path, "real"));
        }
    }
    w.stop();
    k.st = w.status();
    k.frames = frames;
    return k;
}

/* A snapshot (what a power cut right after a sync leaves, maybe with part of what came next) repaired from its
 * journal, as the app does at launch. Returns the repaired file. */
std::string repair_copy(const Tmp &t, const std::string &name, const Data &bytes, const Data &journal) {
    const std::string file = t.file(name);
    spit(file, bytes);
    JournalEntry je;
    CHECK(journal_from_json(std::string(journal.begin(), journal.end()), je));
    je.path = file;
    je.parts = {file};
    je.state = "open";
    CHECK_EQ(journal_write(posix_file_ops(), t.path, je), 0);
    const RepairReport rep = repair_all(posix_file_ops(), t.path);
    CHECK(rep.errors.empty());
    CHECK_EQ(rep.repaired.size(), size_t(1));
    return file;
}

void flac_through_writer(const FlacLib &lib, const FlacDec &dec) {
    struct Case {
        AudioFormat f;
        bool music;
        double seconds;
    };
    const Case cases[] = {{AudioFormat{48000, 1, 16}, false, 6.0}, {AudioFormat{44100, 2, 24}, true, 4.0}};
    for (const Case &c : cases) {
        Tmp t;
        const size_t frames = static_cast<size_t>(c.f.rate * c.seconds);
        const sig::Bytes pcm = c.music ? sig::music(c.f, frames) : sig::speech(c.f, frames);
        const AudioFormat f = c.f;
        Take k = record(t, f, pcm, frames, ".flac", [&lib, f] {
            FlacConfig fc;
            fc.fmt = f;
            return std::unique_ptr<Encoder>(new FlacEncoder(&lib, fc));
        }, "flac");
        CHECK(k.st.state == WriterState::Stopped);
        CHECK_EQ(k.st.frames_written, uint64_t(frames));
        const Decoded d = flac_decode(dec, k.path);
        CHECK(d.opened && d.errors == 0 && d.md5_ok);
        CHECK_EQ(d.channels, unsigned(f.channels));
        CHECK(d.s == samples(pcm, f, 0, frames)); /* sample exact */
        const StreamScan s = scan_flac(posix_file_ops(), k.path);
        CHECK(s.ok && s.valid_end == s.file_size && s.damaged == 0);
        CHECK_EQ(s.frames, uint64_t(frames));
        CHECK_EQ(s.stored_total, uint64_t(frames));
        CHECK(s.md5_set);
        const StreamInfo i = read_stream_info(posix_file_ops(), k.path);
        CHECK(i.ok && i.frames == frames && i.fmt.bits == f.bits && i.fmt.channels == f.channels);
        /* power cuts after each sync, also cut into what followed: repaired, decoded, the recorded samples */
        uint64_t rng = 99;
        int cases_done = 0;
        for (size_t n = 0; n < k.snaps.size(); ++n) {
            for (int v = 0; v < 3; ++v) {
                Tmp p;
                Data b = k.snaps[n];
                if (v > 0) { /* the next second partly reached the card */
                    const Data full = slurp(k.path);
                    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
                    const size_t extra = static_cast<size_t>(rng % 30000);
                    if (b.size() + extra < full.size()) b.insert(b.end(), full.begin() + static_cast<long>(b.size()),
                                                                 full.begin() + static_cast<long>(b.size() + extra));
                }
                const std::string file = repair_copy(p, "real.flac", b, k.journal_at_snap);
                const Decoded r = flac_decode(dec, file);
                const StreamScan rs = scan_flac(posix_file_ops(), file);
                CHECK(r.opened && r.errors == 0);
                CHECK(rs.ok && rs.stored_total == rs.frames && rs.valid_end == rs.file_size);
                CHECK(rs.frames >= k.snap_synced[n]);
                CHECK_EQ(r.s.size(), rs.frames * f.channels);
                CHECK(r.s == samples(pcm, f, 0, static_cast<size_t>(rs.frames)));
                cases_done++;
            }
        }
        std::printf("  real FLAC writer %u Hz %u ch %u-bit: sample exact, %d power cuts repaired and decoded\n",
                    f.rate, unsigned(f.channels), unsigned(f.bits), cases_done);
    }
}

void opus_through_writer(const OpusEncLib &lib, const OpusFile *of) {
    struct Case {
        AudioFormat f;
        int bitrate;
        OpusApplication app;
        bool force_mono;
        double seconds;
    };
    const Case cases[] = {{AudioFormat{48000, 2, 16}, 128000, OpusApplication::Music, false, 5.0},
                          {AudioFormat{48000, 2, 16}, 24000, OpusApplication::Speech, true, 4.0},
                          {AudioFormat{16000, 1, 16}, 32000, OpusApplication::Speech, true, 4.0}};
    for (const Case &c : cases) {
        Tmp t;
        const size_t frames = static_cast<size_t>(c.f.rate * c.seconds);
        const sig::Bytes pcm = c.app == OpusApplication::Music ? sig::music(c.f, frames) : sig::speech(c.f, frames);
        const Case cc = c;
        Take k = record(t, c.f, pcm, frames, ".opus", [&lib, cc] {
            OpusConfig oc;
            oc.fmt = cc.f;
            oc.bitrate_bps = cc.bitrate;
            oc.application = cc.app;
            oc.force_mono = cc.force_mono;
            return std::unique_ptr<Encoder>(new OpusEncoder(&lib, oc));
        }, "opus");
        CHECK(k.st.state == WriterState::Stopped);
        const StreamScan s = scan_ogg_opus(posix_file_ops(), k.path);
        CHECK(s.ok && s.eos && s.valid_end == s.file_size && s.damaged == 0);
        CHECK_EQ(s.frames, uint64_t(frames)); /* exact: the end granule trims the last packet */
        CHECK_EQ(s.fmt.channels, uint16_t(c.f.channels));
        CHECK_EQ(s.fmt.rate, c.f.rate);
        const uint64_t total48 = static_cast<uint64_t>(frames) * 48000u / c.f.rate;
        if (of) {
            const OpusDecoded d = opus_decode(*of, k.path);
            CHECK(d.opened && !d.read_error);
            CHECK_EQ(d.total, static_cast<int64_t>(total48));
            CHECK_EQ(d.read, static_cast<int64_t>(total48));
            CHECK_EQ(d.channels, int(c.f.channels));
        }
        /* power cuts after each sync (the 2 s decision delay is not in the file yet: never counted as synced) */
        int done = 0;
        uint64_t rng = 7;
        for (size_t n = 0; n < k.snaps.size(); ++n) {
            for (int v = 0; v < 2; ++v) {
                Tmp p;
                Data b = k.snaps[n];
                if (v == 1) {
                    const Data full = slurp(k.path);
                    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
                    const size_t extra = static_cast<size_t>(rng % 9000);
                    if (b.size() + extra < full.size()) b.insert(b.end(), full.begin() + static_cast<long>(b.size()),
                                                                 full.begin() + static_cast<long>(b.size() + extra));
                }
                const std::string file = repair_copy(p, "real.opus", b, k.journal_at_snap);
                const StreamScan rs = scan_ogg_opus(posix_file_ops(), file);
                CHECK(rs.ok && rs.valid_end == rs.file_size && (rs.eos || rs.header_only));
                CHECK(rs.frames >= k.snap_synced[n]);
                if (of && !rs.header_only) {
                    const OpusDecoded d = opus_decode(*of, file);
                    CHECK(d.opened && !d.read_error);
                    CHECK_EQ(d.total, static_cast<int64_t>(rs.frames * 48000u / c.f.rate));
                    CHECK_EQ(d.read, d.total);
                }
                done++;
            }
        }
        std::printf("  real Opus writer %u Hz %u ch %d kbps%s: exact length, %d power cuts repaired%s\n", c.f.rate,
                    unsigned(c.f.channels), c.bitrate / 1000, c.force_mono ? " (one coded channel)" : "", done,
                    of ? " and decoded" : "");
    }
}

/* The repair at every byte offset of a short real stream (the state a recording leaves: not finished). */
class EndsSink final : public ByteSink {
public:
    MemoryByteSink m;
    std::vector<uint64_t> ends;
    int write(const void *p, size_t n) override {
        const bool append = m.tell() == m.size();
        const int r = m.write(p, n);
        if (r == 0 && append) ends.push_back(m.size());
        return r;
    }
    int seek(uint64_t pos) override { return m.seek(pos); }
    uint64_t tell() const override { return m.tell(); }
    uint64_t size() const override { return m.size(); }
};

void flac_sweep(const FlacLib &lib, const FlacDec &dec) {
    const AudioFormat f{8000, 1, 16};
    const size_t frames = 8000 * 2 + 1234;
    const sig::Bytes pcm = sig::speech(f, frames);
    EndsSink sink;
    FlacConfig fc;
    fc.fmt = f;
    fc.padding_bytes = 64;
    FlacEncoder e(&lib, fc);
    CHECK_EQ(e.start(sink, {}), 0);
    CHECK_EQ(e.write(pcm.data(), frames), 0);
    CHECK_EQ(e.flush(), 0);
    const Data full = sink.m.data;
    Tmp t;
    spit(t.file("full.flac"), full);
    const StreamScan s0 = scan_flac(posix_file_ops(), t.file("full.flac"));
    CHECK(s0.ok && s0.frames == 4 * 4096 && s0.valid_end == full.size());
    std::vector<uint64_t> fends;
    for (uint64_t e2 : sink.ends)
        if (e2 > s0.audio_offset) fends.push_back(e2);
    CHECK_EQ(fends.size(), size_t(4));
    int fails = 0, decoded = 0, n = 0;
    for (size_t len = static_cast<size_t>(s0.audio_offset); len <= full.size(); ++len, ++n) {
        const std::string p = t.file("cut.flac");
        spit(p, Data(full.begin(), full.begin() + static_cast<long>(len)));
        const StreamFix x = fix_flac(posix_file_ops(), p, true);
        size_t k = 0;
        while (k < fends.size() && fends[k] <= len) ++k;
        const uint64_t end = k ? fends[k - 1] : s0.audio_offset;
        bool good = x.ok && x.frames == k * 4096 && x.truncated_bytes == len - end;
        if (good && n % 37 == 0) {
            const Decoded d = flac_decode(dec, p);
            good = d.opened && d.errors == 0 && d.s == samples(pcm, f, 0, k * 4096);
            decoded++;
        }
        if (!good && ++fails < 4) std::fprintf(stderr, "  real FLAC sweep: cut at %zu failed\n", len);
    }
    CHECK_EQ(fails, 0);
    std::printf("  real FLAC repair at every byte offset of the frames: %d cuts, %d of them decoded by libFLAC\n", n,
                decoded);
}

void opus_sweep(const OpusEncLib &lib, const OpusFile *of) {
    const AudioFormat f{8000, 1, 16};
    const size_t frames = 8000 * 3;
    const sig::Bytes pcm = sig::speech(f, frames);
    EndsSink sink;
    OpusConfig oc;
    oc.fmt = f;
    oc.bitrate_bps = 24000;
    oc.decision_delay_ms = 0; /* more pages for the size */
    oc.muxing_delay_ms = 200;
    OpusEncoder e(&lib, oc);
    CHECK_EQ(e.start(sink, {}), 0);
    CHECK_EQ(e.write(pcm.data(), frames), 0);
    CHECK_EQ(e.flush(), 0);
    const Data full = sink.m.data;
    Tmp t;
    spit(t.file("full.opus"), full);
    const StreamScan s0 = scan_ogg_opus(posix_file_ops(), t.file("full.opus"));
    CHECK(s0.ok && !s0.eos && s0.valid_end == full.size() && s0.units > 6);
    std::vector<uint64_t> pends;
    for (uint64_t e2 : sink.ends)
        if (e2 > s0.audio_offset) pends.push_back(e2);
    int fails = 0, decoded = 0, n = 0;
    for (size_t len = static_cast<size_t>(s0.audio_offset); len <= full.size(); ++len, ++n) {
        const std::string p = t.file("cut.opus");
        spit(p, Data(full.begin(), full.begin() + static_cast<long>(len)));
        const StreamFix x = fix_ogg_opus(posix_file_ops(), p, true);
        size_t k = 0;
        while (k < pends.size() && pends[k] <= len) ++k;
        const uint64_t end = k ? pends[k - 1] : s0.audio_offset;
        bool good = x.ok && x.truncated_bytes == len - end;
        if (good && k > 0 && of && n % 25 == 0) {
            const OpusDecoded d = opus_decode(*of, p);
            good = d.opened && !d.read_error && d.total == static_cast<int64_t>(x.frames * 6) && d.read == d.total;
            decoded++;
        }
        if (!good && ++fails < 4) std::fprintf(stderr, "  real Opus sweep: cut at %zu failed\n", len);
    }
    CHECK_EQ(fails, 0);
    std::printf("  real Opus repair at every byte offset of the pages: %d cuts, %d of them decoded by libopusfile\n", n,
                decoded);
}

/* The test encoders write streams the real decoders accept. */
void fixtures_are_real(const FlacDec *dec, const OpusFile *of) {
    Tmp t;
    const AudioFormat f{44100, 2, 24};
    MemoryByteSink s;
    fx::VerbatimFlac e(f, 1152, 128);
    e.start(s, {{"TITLE", "fixture"}});
    const std::vector<unsigned char> pcm = fx::pcm(f, 0, 10000);
    e.write(pcm.data(), 10000);
    e.finish();
    spit(t.file("fx.flac"), s.data);
    if (dec) {
        const Decoded d = flac_decode(*dec, t.file("fx.flac"));
        CHECK(d.opened && d.errors == 0);
        CHECK(d.s == fx::samples_of(f, pcm));
    }
    if (of) {
        MemoryByteSink o;
        fx::PlainOggOpus p(AudioFormat{16000, 1, 16}, 60, 50);
        p.start(o, {});
        const std::vector<unsigned char> op = fx::pcm(AudioFormat{16000, 1, 16}, 0, 16000 * 2 + 50);
        p.write(op.data(), 16000 * 2 + 50);
        p.finish();
        spit(t.file("fx.opus"), o.data);
        const OpusDecoded d = opus_decode(*of, t.file("fx.opus"), false);
        CHECK(d.opened);
        CHECK_EQ(d.total, static_cast<int64_t>((16000 * 2 + 50) * 3));
    }
}

/* CPU of encoding 48 kHz stereo music: the encoder alone, and the whole writer path (ring, encoder, pwrite, sync). */
void cpu(const FlacLib *fl, const OpusEncLib *op) {
    const AudioFormat f{48000, 2, 16};
    const double seconds = 20.0;
    const size_t frames = static_cast<size_t>(f.rate * seconds);
    const sig::Bytes pcm = sig::music(f, frames);
    auto run_encoder = [&](Encoder &e) {
        MemoryByteSink s;
        CHECK_EQ(e.start(s, {}), 0);
        const double t0 = thread_cpu_s();
        for (size_t at = 0; at < frames; at += 4800) CHECK_EQ(e.write(pcm.data() + at * 4, std::min<size_t>(4800, frames - at)), 0);
        CHECK_EQ(e.finish(), 0);
        return thread_cpu_s() - t0;
    };
    auto run_writer = [&](std::function<std::unique_ptr<Encoder>()> make, const char *ext) {
        Tmp t;
        FakeClock clk;
        Ring ring(f, 8.0);
        WriterConfig cfg;
        cfg.fmt = f;
        cfg.take = "cpu";
        cfg.recordings_dir = t.path;
        const std::string p = t.file(std::string("cpu") + ext);
        cfg.namer = [p](int) { return p; };
        cfg.min_free_bytes = 0;
        cfg.sync_interval_ms = 10000;
        cfg.make_encoder = [make](int) { return make(); };
        Writer w(ring, posix_file_ops(), clk, cfg);
        CHECK_EQ(w.start(), 0);
        const double t0 = thread_cpu_s();
        for (size_t at = 0; at < frames; at += 4800) {
            const size_t n = std::min<size_t>(4800, frames - at);
            ring.write(pcm.data() + at * 4, n);
            clk.now += 100;
            bool busy = true;
            while (busy && w.pump(&busy)) {
            }
        }
        w.stop();
        return thread_cpu_s() - t0;
    };
    if (fl) {
        FlacConfig fc;
        fc.fmt = f;
        FlacEncoder e(fl, fc);
        const double c = run_encoder(e);
        const double cw = run_writer([fl, f] {
            FlacConfig x;
            x.fmt = f;
            return std::unique_ptr<Encoder>(new FlacEncoder(fl, x));
        }, ".flac");
        std::printf("  CPU FLAC level 5, 48 kHz stereo 16-bit music (this PC): encoder %.1f %% of one core (%.0fx real time), "
                    "writer path %.1f %%\n", 100.0 * c / seconds, seconds / c, 100.0 * cw / seconds);
    }
    if (op) {
        OpusConfig oc;
        oc.fmt = f;
        oc.bitrate_bps = 128000;
        oc.application = OpusApplication::Music;
        OpusEncoder e(op, oc);
        const double c = run_encoder(e);
        const double cw = run_writer([op, f] {
            OpusConfig x;
            x.fmt = f;
            x.bitrate_bps = 128000;
            x.application = OpusApplication::Music;
            return std::unique_ptr<Encoder>(new OpusEncoder(op, x));
        }, ".opus");
        std::printf("  CPU Opus 128 kbps music, 48 kHz stereo (this PC): encoder %.1f %% of one core (%.0fx real time), "
                    "writer path %.1f %%\n", 100.0 * c / seconds, seconds / c, 100.0 * cw / seconds);
    }
}

}  // namespace

void test_writer_real() {
    std::string why;
    const FlacLib *fl = real_flac(&why);
    if (!fl) SKIP(("real FLAC writer: " + why).c_str());
    std::string why2;
    const OpusEncLib *op = real_opusenc(&why2);
    if (!op) SKIP(("real Opus writer: " + why2).c_str());
    FlacDec dec;
    if (fl) {
        dec = load_flac_decoder(*fl);
        CHECK(dec.ok());
    }
    const OpusFile of = load_opusfile();
    if (!of.ok()) SKIP("libopusfile.so.0 not found: Opus files checked by the scanner only, not decoded");
    if (fl && dec.ok()) {
        flac_through_writer(*fl, dec);
        flac_sweep(*fl, dec);
    }
    if (op) {
        opus_through_writer(*op, of.ok() ? &of : nullptr);
        opus_sweep(*op, of.ok() ? &of : nullptr);
    }
    fixtures_are_real(fl && dec.ok() ? &dec : nullptr, of.ok() ? &of : nullptr);
    cpu(fl, op);
}
