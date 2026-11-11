/*
 * SPDX-License-Identifier: MIT
 *
 * The writer with an encoder (phase B of FLAC/Opus) on a fake clock, with the test encoders (fixture_codecs.hpp): the
 * sync cadence and what each sync makes durable, the stop (STREAMINFO / end of stream), a power cut emulated at every
 * step of a take (a copy of the file at that moment, also cut back into the unsynced part, then repaired), fdatasync
 * failing at each sync, a full card and EIO (the file is mended at once), short writes, auto-split into whole streams,
 * pause, the free-space stop, a missing codec, and an encoder that never holds up the capture side.
 */
#include "check.hpp"
#include "fixture_codecs.hpp"
#include "helpers.hpp"
#include "repair.hpp"
#include "ring.hpp"
#include "writer.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <thread>

using namespace cassette;
using namespace tst;

namespace {

enum class Kind { Flac, Opus };

struct CRig {
    TempDir d;
    FakeClock clk;
    FaultOps ops{&clk};
    AudioFormat fmt;
    Ring ring;
    WriterConfig cfg;
    std::unique_ptr<Writer> w;
    uint64_t fed = 0;
    Kind kind;
    std::string ext;

    explicit CRig(Kind k, AudioFormat f = AudioFormat{}, double ring_s = 8.0) : fmt(f), ring(f, ring_s), kind(k) {
        ext = k == Kind::Flac ? ".flac" : ".opus";
        cfg.fmt = f;
        cfg.take = "c1";
        cfg.recordings_dir = d.path;
        const std::string dir = d.path, e = ext;
        cfg.namer = [dir, e](int p) { return p == 0 ? dir + "/c" + e : dir + "/c-" + std::to_string(p) + e; };
        cfg.min_free_bytes = 0;
        cfg.sync_interval_s = 10;
        cfg.block_bytes = 65536;
        cfg.codec = k == Kind::Flac ? "flac" : "opus";
        cfg.codec_preset = k == Kind::Opus ? "music128" : "";
        cfg.bitrate_bps = k == Kind::Opus ? 128000 : 0;
        cfg.encoder_name = "fixture";
        cfg.split_slack = 1024;
        cfg.make_encoder = [f, k](int) -> std::unique_ptr<Encoder> {
            if (k == Kind::Flac) return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096));
            return std::unique_ptr<Encoder>(new fx::PlainOggOpus(f, 320, 50));
        };
        cfg.tags = [](int part) { return Tags{{"TITLE", "c part " + std::to_string(part + 1)}}; };
    }
    int begin() {
        w = std::make_unique<Writer>(ring, ops, clk, cfg);
        return w->start();
    }
    void feed(size_t frames) {
        const std::vector<unsigned char> b = fx::pcm(fmt, fed, frames);
        const size_t acc = ring.write(b.data(), frames);
        CHECK_EQ(acc, frames);
        fed += acc;
    }
    void run() {
        bool busy = true;
        while (busy && w->pump(&busy)) {
        }
    }
    std::string path(int p = 0) const { return p == 0 ? d.file("c" + ext) : d.file("c-" + std::to_string(p) + ext); }
    JournalEntry journal() const {
        JournalEntry je;
        CHECK_EQ(journal_read(posix_file_ops(), journal_file(d.path, "c1"), je), 0);
        return je;
    }
};

bool flac_holds(const std::string &path, const AudioFormat &f, uint64_t first, uint64_t frames) {
    return fx::decode_verbatim_flac(read_file(path)) == fx::samples_of(f, fx::pcm(f, first, frames));
}

bool contains(const std::vector<unsigned char> &b, const std::string &s) {
    return std::search(b.begin(), b.end(), s.begin(), s.end()) != b.end();
}

/* The file of an encoder take is a finished, self-consistent stream of `frames` frames. */
void expect_finished(Kind k, const std::string &path, uint64_t frames) {
    if (k == Kind::Flac) {
        const StreamScan s = scan_flac(posix_file_ops(), path);
        CHECK(s.ok && s.valid_end == s.file_size && s.damaged == 0);
        CHECK_EQ(s.frames, frames);
        CHECK_EQ(s.stored_total, frames);
    } else {
        const StreamScan s = scan_ogg_opus(posix_file_ops(), path);
        CHECK(s.ok && s.valid_end == s.file_size && s.damaged == 0 && s.eos);
        CHECK_EQ(s.frames, frames);
    }
}

void cadence(Kind k) {
    CRig r(k);
    CHECK_EQ(r.begin(), 0);
    /* a valid, empty stream right after start, the journal says what it is */
    const StreamScan s0 = k == Kind::Flac ? scan_flac(posix_file_ops(), r.path()) : scan_ogg_opus(posix_file_ops(), r.path());
    CHECK(s0.ok && s0.frames == 0 && s0.valid_end == s0.file_size);
    JournalEntry je = r.journal();
    CHECK(je.state == "open");
    CHECK_EQ(je.codec, k == Kind::Flac ? std::string("flac") : std::string("opus"));
    CHECK_EQ(je.encoder, "fixture");
    CHECK_EQ(je.path, r.path());
    const int header_writes = r.ops.count("pwrite");
    const size_t log0 = r.ops.log.size();
    for (int s = 1; s <= 35; s++) {
        r.clk.advance(1000);
        r.feed(48000);
        r.run();
        if (s == 10 || s == 20) {
            const WriterStatus st = r.w->status();
            /* FLAC: whole 4096-frame blocks; Opus: whole 20 ms packets of the pre-skip (312) plus the input, the page
             * closed by the sync */
            const uint64_t expect = k == Kind::Flac ? (r.fed / 4096) * 4096 : (312 + r.fed) / 960 * 960 - 312;
            CHECK_EQ(st.frames_synced, expect);
            /* what a power cut right now keeps: exactly the synced frames, a stream that needs no cut */
            const StreamScan m = k == Kind::Flac ? scan_flac(posix_file_ops(), r.path()) : scan_ogg_opus(posix_file_ops(), r.path());
            CHECK(m.ok && m.valid_end == m.file_size);
            CHECK_EQ(m.frames, st.frames_synced);
            CHECK(!m.eos);
            CHECK_EQ(r.journal().frames_synced, st.frames_synced);
            if (k == Kind::Flac) CHECK(flac_holds(r.path(), r.fmt, 0, st.frames_synced));
        }
    }
    CHECK_EQ(r.ops.count("fdatasync"), 3); /* at 10, 20 and 30 s: the WAV cadence */
    CHECK_EQ(r.w->status().syncs, uint64_t(3));
    /* appends only while recording: no header patches */
    for (size_t i = log0; i < r.ops.log.size(); ++i)
        if (r.ops.log[i].op == "pwrite") CHECK(r.ops.log[i].a >= s0.file_size);
    CHECK(header_writes >= 1);
    r.w->stop();
    const WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Stopped);
    CHECK_EQ(st.frames_written, r.fed);
    CHECK_EQ(st.frames_synced, r.fed);
    expect_finished(k, r.path(), r.fed);
    StatInfo si;
    posix_file_ops().stat(r.path(), si);
    CHECK_EQ(st.bytes_out, si.size);
    if (k == Kind::Flac) CHECK(flac_holds(r.path(), r.fmt, 0, r.fed));
    CHECK(contains(read_file(r.path()), "TITLE=c part 1"));
    je = r.journal();
    CHECK(je.state == "closed");
    CHECK_EQ(je.frames, r.fed);
    CHECK_EQ(je.codec, k == Kind::Flac ? std::string("flac") : std::string("opus"));
    CHECK(!r.w->pump());
}

/* A power cut at every step of a take: the file as it is at that moment, and cut back to a random point between the
 * synced length and its length (the unsynced part may or may not have reached the card), repaired from its journal. */
void kill_each_step(Kind k) {
    CRig r(k);
    r.cfg.sync_interval_ms = 1000;
    CHECK_EQ(r.begin(), 0);
    struct Snap {
        std::vector<unsigned char> bytes, journal;
        uint64_t synced = 0, fed = 0, synced_size = 0;
    };
    std::vector<Snap> snaps;
    uint64_t syncs = 0, synced_size = read_file(r.path()).size();
    for (int step = 0; step < 40; ++step) {
        r.clk.advance(250);
        r.feed(12000);
        r.run();
        const WriterStatus st = r.w->status();
        Snap sn;
        sn.bytes = read_file(r.path());
        if (st.syncs != syncs) {
            syncs = st.syncs;
            synced_size = sn.bytes.size(); /* nothing is written after a sync within the step */
        }
        sn.journal = read_file(journal_file(r.d.path, "c1"));
        sn.synced = st.frames_synced;
        sn.fed = r.fed;
        sn.synced_size = synced_size;
        snaps.push_back(sn);
    }
    r.w->stop();
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    int cases = 0, fails = 0;
    uint64_t worst = 0;
    for (const Snap &sn : snaps) {
        for (int variant = 0; variant < 2; ++variant) {
            TempDir p;
            std::vector<unsigned char> b = sn.bytes;
            if (variant == 1 && b.size() > sn.synced_size) {
                rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
                b.resize(static_cast<size_t>(sn.synced_size + rng % (b.size() - sn.synced_size + 1)));
            }
            const std::string file = p.file("c" + r.ext);
            write_file(file, b);
            JournalEntry pj;
            CHECK(journal_from_json(std::string(sn.journal.begin(), sn.journal.end()), pj));
            pj.path = file;
            pj.parts = {file};
            CHECK_EQ(journal_write(posix_file_ops(), p.path, pj), 0);
            const RepairReport rep = repair_all(posix_file_ops(), p.path);
            const StreamScan s = k == Kind::Flac ? scan_flac(posix_file_ops(), file) : scan_ogg_opus(posix_file_ops(), file);
            bool good = rep.errors.empty() && rep.repaired.size() == 1 && s.ok && s.valid_end == s.file_size &&
                        s.frames >= sn.synced && s.frames <= sn.fed;
            if (good && k == Kind::Flac) good = s.stored_total == s.frames && flac_holds(file, r.fmt, 0, s.frames);
            if (good && k == Kind::Opus) good = s.eos || s.header_only;
            if (good) good = rep.repaired[0].frames == s.frames;
            if (good) {
                const RepairReport again = repair_all(posix_file_ops(), p.path);
                good = again.repaired.empty() && again.errors.empty();
            }
            if (good) worst = std::max(worst, sn.fed - s.frames);
            if (!good && ++fails < 4) std::fprintf(stderr, "kill_each_step %s: case %d failed\n", r.ext.c_str(), cases);
            cases++;
        }
    }
    CHECK_EQ(fails, 0);
    /* lost at most: one sync interval, the block the writer had not written yet, and the encoder's partial unit */
    CHECK(worst <= 48000 + 12000 + 4096);
    std::printf("codec writer: %s power cut at each of %d moments (2 variants each): %d repairs checked, worst loss %.0f ms\n",
                r.ext.c_str(), static_cast<int>(snaps.size()), cases, worst * 1000.0 / 48000);
}

/* fdatasync fails at the n-th sync: the take stops, the file is mended at once and holds a valid stream. */
void sync_failures(Kind k) {
    for (int n = 0; n < 4; ++n) {
        CRig r(k);
        r.cfg.sync_interval_ms = 1000;
        r.ops.fdatasync_errno = EIO;
        r.ops.fdatasync_fail_from = n;
        CHECK_EQ(r.begin(), 0);
        uint64_t synced_before = 0;
        for (int s = 0; s < 6 && r.w->status().state == WriterState::Recording; ++s) {
            synced_before = r.w->status().frames_synced;
            r.clk.advance(1000);
            r.feed(48000);
            r.run();
        }
        r.w->stop();
        const WriterStatus st = r.w->status();
        CHECK(st.state == WriterState::Failed);
        CHECK_EQ(st.error, EIO);
        CHECK(st.reason == StopReason::IoError);
        CHECK_EQ(r.journal().state, "error");
        const StreamScan s = k == Kind::Flac ? scan_flac(posix_file_ops(), r.path()) : scan_ogg_opus(posix_file_ops(), r.path());
        CHECK(s.ok && s.valid_end == s.file_size);
        CHECK(s.frames >= synced_before);
        if (k == Kind::Flac) {
            CHECK_EQ(s.stored_total, s.frames);
            CHECK(flac_holds(r.path(), r.fmt, 0, s.frames));
        } else {
            CHECK(s.eos);
        }
    }
}

void full_card(Kind k, int err) {
    CRig r(k);
    CHECK_EQ(r.begin(), 0);
    const uint64_t start_size = read_file(r.path()).size();
    /* FLAC (verbatim here) writes 96 KB a second; the Opus test encoder a page of about 16 KB a second: the second page
     * is the one that does not fit */
    const int64_t budget = k == Kind::Flac ? (err == ENOSPC ? 100001 : 40000) : (err == ENOSPC ? 20001 : 17000);
    r.ops.grow_budget = budget;
    r.ops.write_errno = err;
    r.feed(96000);
    r.run();
    r.clk.advance(1100); /* past flush_ms: the rest of the backlog goes to the encoder */
    r.run();
    WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Failed);
    CHECK_EQ(st.error, err);
    CHECK(st.reason == (err == ENOSPC ? StopReason::LowSpace : StopReason::IoError));
    CHECK(st.reason_text.find(err == ENOSPC ? "disk full" : "write error") != std::string::npos);
    CHECK(!r.w->pump());
    r.feed(1000); /* the producer keeps going: nothing hangs */
    r.w->stop();
    CHECK_EQ(r.journal().state, "error");
    const std::vector<unsigned char> b = read_file(r.path());
    CHECK(b.size() <= start_size + static_cast<uint64_t>(budget));
    const StreamScan s = k == Kind::Flac ? scan_flac(posix_file_ops(), r.path()) : scan_ogg_opus(posix_file_ops(), r.path());
    CHECK(s.ok && s.valid_end == s.file_size); /* the cut frame / page is gone already */
    CHECK(s.frames > 0);
    if (k == Kind::Flac) {
        CHECK_EQ(s.stored_total, s.frames);
        CHECK_EQ(s.frames % 4096, uint64_t(0));
        CHECK(flac_holds(r.path(), r.fmt, 0, s.frames));
    } else {
        CHECK(s.eos);
    }
    /* the repair at the next launch has nothing left to do (the journal says "error", not "open") */
    const RepairReport rep = repair_all(posix_file_ops(), r.d.path);
    CHECK(rep.repaired.empty() && rep.errors.empty());
}

void short_writes(Kind k) {
    CRig r(k);
    r.ops.pwrite_short_max = 777;
    CHECK_EQ(r.begin(), 0);
    r.feed(144000);
    r.run();
    r.w->stop();
    CHECK(r.w->status().state == WriterState::Stopped);
    expect_finished(k, r.path(), r.fed);
    if (k == Kind::Flac) CHECK(flac_holds(r.path(), r.fmt, 0, r.fed));
}

void auto_split(Kind k) {
    CRig r(k);
    r.cfg.split_bytes = k == Kind::Flac ? 60000 : 200000;
    r.cfg.block_bytes = 16384;
    CHECK_EQ(r.begin(), 0);
    for (int i = 0; i < 25; ++i) {
        r.feed(k == Kind::Flac ? 8000 : 48000);
        r.run();
    }
    r.w->stop();
    const WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Stopped);
    CHECK(st.part >= 2);
    uint64_t first = 0;
    std::vector<int32_t> all;
    for (int p = 0; p <= st.part; ++p) {
        StatInfo si;
        CHECK_EQ(posix_file_ops().stat(r.path(p), si), 0);
        CHECK(si.size <= r.cfg.split_bytes);
        const StreamScan s = k == Kind::Flac ? scan_flac(posix_file_ops(), r.path(p)) : scan_ogg_opus(posix_file_ops(), r.path(p));
        CHECK(s.ok && s.valid_end == s.file_size && s.frames > 0); /* every part a whole stream */
        if (!(s.ok && s.valid_end == s.file_size && s.frames > 0))
            std::fprintf(stderr, "  part %d of %s: ok %d, valid end %llu of %llu, %llu frames, %s\n", p, r.ext.c_str(),
                         s.ok ? 1 : 0, static_cast<unsigned long long>(s.valid_end),
                         static_cast<unsigned long long>(s.file_size), static_cast<unsigned long long>(s.frames),
                         s.error.c_str());
        if (k == Kind::Flac) {
            CHECK_EQ(s.stored_total, s.frames);
            const std::vector<int32_t> d = fx::decode_verbatim_flac(read_file(r.path(p)));
            all.insert(all.end(), d.begin(), d.end());
        } else {
            CHECK(s.eos);
        }
        CHECK(contains(read_file(r.path(p)), "TITLE=c part " + std::to_string(p + 1)));
        first += s.frames;
    }
    CHECK_EQ(first, r.fed); /* nothing lost or doubled at the part boundaries */
    if (k == Kind::Flac) CHECK(all == fx::samples_of(r.fmt, fx::pcm(r.fmt, 0, r.fed)));
    const JournalEntry je = r.journal();
    CHECK_EQ(je.parts.size(), static_cast<size_t>(st.part + 1));
    CHECK(je.state == "closed");
    CHECK_EQ(je.frames, r.fed);
    std::printf("codec writer: %s auto-split into %d parts of at most %llu bytes\n", r.ext.c_str(), st.part + 1,
                static_cast<unsigned long long>(r.cfg.split_bytes));

    /* without auto-split one stream holds everything */
    CRig q(k);
    q.cfg.auto_split = false;
    q.cfg.split_bytes = 10000;
    CHECK_EQ(q.begin(), 0);
    q.feed(200000);
    q.run();
    q.w->stop();
    CHECK_EQ(q.w->status().part, 0);
    expect_finished(k, q.path(), q.fed);
}

void pause_resume() {
    CRig r(Kind::Flac);
    CHECK_EQ(r.begin(), 0);
    r.feed(48000);
    r.w->pause();
    r.run();
    r.clk.advance(1000);
    r.run();
    CHECK(r.w->status().state == WriterState::Paused);
    r.feed(48000); /* discarded */
    r.run();
    r.w->resume();
    const uint64_t resume_at = r.fed;
    r.feed(48000);
    r.run();
    r.w->stop();
    CHECK_EQ(r.w->status().paused_frames, uint64_t(48000));
    expect_finished(Kind::Flac, r.path(), 96000);
    std::vector<int32_t> want = fx::samples_of(r.fmt, fx::pcm(r.fmt, 0, 48000));
    const std::vector<int32_t> second = fx::samples_of(r.fmt, fx::pcm(r.fmt, resume_at, 48000));
    want.insert(want.end(), second.begin(), second.end());
    CHECK(fx::decode_verbatim_flac(read_file(r.path())) == want);
    CHECK_EQ(r.w->status().part, 0);
}

void free_space_stop() {
    CRig r(Kind::Opus);
    uint64_t free_now = 1ull << 30;
    r.ops.free_fn = [&] { return free_now; };
    r.cfg.min_free_bytes = 100ull << 20;
    CHECK_EQ(r.begin(), 0);
    for (int s = 0; s < 3; s++) {
        r.clk.advance(1000);
        r.feed(48000);
        r.run();
    }
    free_now = 50ull << 20;
    r.clk.advance(1000);
    r.feed(48000);
    r.run();
    const WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Stopped);
    CHECK(st.reason == StopReason::LowSpace);
    CHECK_EQ(st.frames_written, r.fed);
    expect_finished(Kind::Opus, r.path(), r.fed); /* closed cleanly: the end-of-stream page is there */
    CHECK_EQ(r.journal().state, "closed");
}

/* An encoder that cannot start (the library vanished between the check and the take): no file, a clear reason. */
class DeadEncoder final : public Encoder {
public:
    int start(ByteSink &, const Tags &) override { return -100100; }
    int write(const void *, size_t) override { return -100100; }
    int flush() override { return -100100; }
    int finish() override { return -100100; }
    EncoderStats stats() const override { return EncoderStats(); }
    const AudioFormat &format() const override { return f_; }

private:
    AudioFormat f_;
};

void codec_missing() {
    CRig r(Kind::Flac);
    r.cfg.make_encoder = [](int) { return std::unique_ptr<Encoder>(new DeadEncoder()); };
    CHECK(r.begin() < 0);
    const WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Failed);
    CHECK(st.reason_text.find("codec library not installed") != std::string::npos);
    StatInfo si;
    CHECK(posix_file_ops().stat(r.path(), si) < 0); /* nothing left behind */
    CRig q(Kind::Opus);
    q.cfg.make_encoder = [](int) { return std::unique_ptr<Encoder>(); };
    CHECK(q.begin() < 0);
    CHECK(posix_file_ops().stat(q.path(), si) < 0);
}

/* A slow encoder on the real writer thread: the producer (the capture side) never waits, nothing is dropped. */
class SlowEncoder final : public Encoder {
public:
    explicit SlowEncoder(const AudioFormat &f) : inner_(f, 4096) {}
    int start(ByteSink &s, const Tags &t) override { return inner_.start(s, t); }
    int write(const void *p, size_t n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return inner_.write(p, n);
    }
    int flush() override { return inner_.flush(); }
    int finish() override { return inner_.finish(); }
    EncoderStats stats() const override { return inner_.stats(); }
    const AudioFormat &format() const override { return inner_.format(); }

private:
    fx::VerbatimFlac inner_;
};

void never_stalls_capture() {
    TempDir d;
    const AudioFormat f;
    Ring ring(f, 4.0);
    WriterConfig cfg;
    cfg.fmt = f;
    cfg.take = "slow";
    cfg.recordings_dir = d.path;
    const std::string p = d.file("slow.flac");
    cfg.namer = [p](int) { return p; };
    cfg.min_free_bytes = 0;
    cfg.sync_interval_ms = 100;
    cfg.flush_ms = 20;
    cfg.block_bytes = 4096;
    cfg.codec = "flac";
    cfg.make_encoder = [f](int) { return std::unique_ptr<Encoder>(new SlowEncoder(f)); };
    Writer w(ring, posix_file_ops(), system_clock(), cfg);
    CHECK_EQ(w.start(), 0);
    w.start_thread();
    uint64_t fed = 0;
    double worst_us = 0;
    for (int i = 0; i < 60; i++) {
        const std::vector<unsigned char> b = fx::pcm(f, fed, 960);
        const auto t0 = std::chrono::steady_clock::now();
        fed += ring.write(b.data(), 960);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        worst_us = std::max(worst_us, us);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    w.stop();
    CHECK_EQ(ring.frames_dropped(), uint64_t(0));
    CHECK(w.status().state == WriterState::Stopped);
    CHECK_EQ(w.status().frames_written, fed);
    CHECK(worst_us < 20000.0); /* a ring write never waits for the encoder (20 ms per call here) */
    expect_finished(Kind::Flac, p, fed);
    std::printf("codec writer: slow encoder (20 ms a call) on the writer thread: worst ring write %.0f us, 0 dropped\n",
                worst_us);
}

}  // namespace

void test_codec_writer() {
    cadence(Kind::Flac);
    cadence(Kind::Opus);
    kill_each_step(Kind::Flac);
    kill_each_step(Kind::Opus);
    sync_failures(Kind::Flac);
    sync_failures(Kind::Opus);
    full_card(Kind::Flac, ENOSPC);
    full_card(Kind::Opus, ENOSPC);
    full_card(Kind::Flac, EIO);
    full_card(Kind::Opus, EIO);
    short_writes(Kind::Flac);
    short_writes(Kind::Opus);
    auto_split(Kind::Flac);
    auto_split(Kind::Opus);
    pause_resume();
    free_space_stop();
    codec_missing();
    never_stalls_capture();
}
