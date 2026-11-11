/*
 * SPDX-License-Identifier: MIT
 *
 * Writer on a fake clock: block and sync cadence, header rewrites, flush, pause/resume, ENOSPC / EIO / short writes,
 * free-space stop, auto-split, RF64 switch, journal, and the real thread. Files stay repairable after every failure.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "repair.hpp"
#include "ring.hpp"
#include "writer.hpp"

#include <cerrno>
#include <memory>

using namespace cassette;
using namespace tst;

namespace {

struct Rig {
    TempDir d;
    FakeClock clk;
    FaultOps ops{&clk};
    AudioFormat fmt;
    Ring ring;
    WriterConfig cfg;
    std::unique_ptr<Writer> w;
    uint64_t fed = 0; /* frames offered to the ring (all accepted in these tests) */

    explicit Rig(double ring_s = 8.0) : ring(AudioFormat{}, ring_s) {
        cfg.fmt = fmt;
        cfg.take = "t1";
        cfg.recordings_dir = d.path;
        std::string dir = d.path;
        cfg.namer = [dir](int p) { return p == 0 ? dir + "/t.wav" : dir + "/t-" + std::to_string(p) + ".wav"; };
        cfg.min_free_bytes = 0;
        cfg.sync_interval_s = 10;
        cfg.block_bytes = 65536;
    }
    int begin() {
        w = std::make_unique<Writer>(ring, ops, clk, cfg);
        return w->start();
    }
    void feed(size_t frames) {
        std::vector<int16_t> b = sig_block(fed, frames);
        size_t acc = ring.write(b.data(), frames);
        CHECK_EQ(acc, frames);
        fed += acc;
    }
    void run() {
        bool busy = true;
        while (busy && w->pump(&busy)) {
        }
    }
    std::string path(int p = 0) const { return p == 0 ? d.file("t.wav") : d.file("t-" + std::to_string(p) + ".wav"); }
};

void cadence() {
    Rig r;
    CHECK_EQ(r.begin(), 0);
    CHECK_EQ(r.ops.count("fdatasync"), 0);
    /* the file exists with a valid empty header right after start */
    HeaderInfo h0 = parse_header(posix_file_ops(), r.path());
    CHECK(h0.ok && h0.consistent() && h0.actual_data == 0);
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "t1"), je), 0);
    CHECK(je.state == "open");
    CHECK(je.take == "t1" && je.path == r.path());
    CHECK_EQ(je.fmt.rate, 48000u);

    uint64_t at15 = 0;
    for (int s = 1; s <= 35; s++) {
        r.clk.advance(1000);
        r.feed(48000);
        r.run();
        if (s == 15) {
            at15 = r.w->status().frames_synced;
            JournalEntry mid;
            CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "t1"), mid), 0);
            CHECK(mid.state == "open");
            CHECK_EQ(mid.frames_synced, at15);
            CHECK(at15 > 0 && at15 <= r.fed);
        }
    }
    CHECK_EQ(r.ops.count("fdatasync"), 3); /* at 10 s, 20 s and 30 s */
    CHECK_EQ(r.w->status().syncs, static_cast<uint64_t>(3));
    /* the cadence: 10 s apart, and the header sizes (data size, then RIFF size) rewritten right after each sync */
    std::vector<uint64_t> times;
    for (size_t k = 0; k < r.ops.log.size(); k++) {
        if (r.ops.log[k].op != "fdatasync") continue;
        times.push_back(r.ops.log[k].at);
        CHECK(k + 2 < r.ops.log.size());
        if (k + 2 < r.ops.log.size()) {
            CHECK(r.ops.log[k + 1].op == "pwrite" && r.ops.log[k + 1].a == kDataOffset - 4);
            CHECK(r.ops.log[k + 2].op == "pwrite" && r.ops.log[k + 2].a == 4);
        }
    }
    CHECK_EQ(times.size(), static_cast<size_t>(3));
    if (times.size() == 3) {
        CHECK_EQ(times[1] - times[0], static_cast<uint64_t>(10000));
        CHECK_EQ(times[2] - times[1], static_cast<uint64_t>(10000));
    }
    /* blocks: never above 64 KB, whole frames, and full 64 KB blocks while there is a backlog */
    size_t maxw = 0, full = 0;
    bool even = true;
    for (const FaultOps::Ev &e : r.ops.log) {
        if (e.op != "write") continue;
        maxw = std::max<size_t>(maxw, static_cast<size_t>(e.a));
        full += e.a == 65536;
        even = even && e.a % 2 == 0;
    }
    CHECK_EQ(maxw, static_cast<size_t>(65536));
    CHECK(full >= 1 && even);

    r.w->stop();
    WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Stopped);
    CHECK_EQ(st.frames_written, r.fed);
    CHECK_EQ(st.frames_synced, r.fed);
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.declared_data, r.fed * 2);
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, r.fed));
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "t1"), je), 0);
    CHECK(je.state == "closed");
    CHECK_EQ(je.frames, r.fed);
    CHECK(!r.w->pump()); /* stopped */
}

void flush_timing() {
    Rig r;
    CHECK_EQ(r.begin(), 0);
    const int w0 = r.ops.count("write"); /* the header */
    r.feed(500);
    r.run();
    CHECK_EQ(r.ops.count("write"), w0); /* a partial block waits ... */
    r.clk.advance(999);
    r.run();
    CHECK_EQ(r.ops.count("write"), w0);
    r.clk.advance(1);
    r.run();
    CHECK_EQ(r.ops.count("write"), w0 + 1); /* ... for flush_ms */
    CHECK_EQ(r.ops.log.back().a, static_cast<uint64_t>(1000));
    /* a backlog goes out in whole 64 KB blocks, the remainder waits */
    const int w1 = r.ops.count("write");
    r.feed(100000); /* 200,000 bytes: three full blocks and 3,392 bytes */
    r.run();
    CHECK_EQ(r.ops.count("write"), w1 + 3);
    for (size_t k = r.ops.log.size() - 3; k < r.ops.log.size(); k++) CHECK_EQ(r.ops.log[k].a, static_cast<uint64_t>(65536));
    r.feed(100);
    r.w->stop(); /* a stop writes the rest at once */
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.consistent());
    CHECK_EQ(h.actual_data, static_cast<uint64_t>((500 + 100000 + 100) * 2));
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, 500 + 100000 + 100));
}

void pause_resume() {
    Rig r;
    CHECK_EQ(r.begin(), 0);
    r.feed(48000);
    r.w->pause();
    r.run();
    r.clk.advance(1000);
    r.run();
    CHECK(r.w->status().state == WriterState::Paused);
    const uint64_t skipFrom = r.fed;
    r.feed(48000); /* offered while paused: discarded */
    r.run();
    r.clk.advance(1000);
    r.run();
    r.w->resume();
    const uint64_t resumeAt = r.fed;
    r.feed(48000);
    r.run();
    r.clk.advance(1000);
    r.run();
    CHECK(r.w->status().state == WriterState::Recording);
    CHECK_EQ(r.w->status().paused_frames, static_cast<uint64_t>(48000));
    r.w->stop();
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.actual_data, static_cast<uint64_t>(96000 * 2));
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, 48000));
    CHECK(audio_is_sig(r.path(), h.data_offset + 48000 * 2, resumeAt, 48000));
    CHECK_EQ(skipFrom, static_cast<uint64_t>(48000));
    /* the file stayed open: one file only, and the pause did not create a part */
    CHECK_EQ(r.w->status().part, 0);
    /* a pause with the data written before it synced: the unsynced bytes were flushed by the sync timer rule */
}

void expect_repairable(Rig &r, uint64_t expect_frames, uint32_t expect_cut) {
    FileRepair fr = repair_file(posix_file_ops(), r.path(), true);
    CHECK(fr.ok);
    CHECK_EQ(fr.frames, expect_frames);
    CHECK_EQ(fr.truncated_bytes, expect_cut);
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.consistent());
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, expect_frames));
}

void enospc() {
    Rig r;
    CHECK_EQ(r.begin(), 0);
    r.ops.write_budget = 100001; /* an odd number: a partial frame stays in the file */
    r.ops.write_errno = ENOSPC;
    r.feed(96000);
    r.run();
    WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Failed);
    CHECK(st.reason == StopReason::LowSpace);
    CHECK_EQ(st.error, ENOSPC);
    CHECK(!st.reason_text.empty());
    CHECK(!r.w->pump());
    r.feed(1000); /* the producer keeps going: no crash, nothing hangs */
    r.w->stop();
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "t1"), je), 0);
    CHECK(je.state == "error" && !je.reason.empty());
    expect_repairable(r, 50000, 1);
}

void eio() {
    Rig r;
    CHECK_EQ(r.begin(), 0);
    r.ops.write_budget = 40000;
    r.ops.write_errno = EIO;
    r.feed(96000);
    r.run();
    WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Failed);
    CHECK(st.reason == StopReason::IoError);
    CHECK_EQ(st.error, EIO);
    CHECK(st.reason_text.find("write error") != std::string::npos);
    r.w->stop();
    expect_repairable(r, 20000, 0);

    /* EIO from fdatasync at the first sync */
    Rig q;
    q.ops.fdatasync_errno = EIO;
    CHECK_EQ(q.begin(), 0);
    for (int s = 0; s < 12; s++) {
        q.clk.advance(1000);
        q.feed(4800);
        q.run();
    }
    CHECK(q.w->status().state == WriterState::Failed);
    CHECK_EQ(q.w->status().error, EIO);
    q.w->stop();
    expect_repairable(q, q.w->status().frames_written, 0);
}

void short_writes() {
    Rig r;
    r.ops.short_max = 1000;
    CHECK_EQ(r.begin(), 0);
    r.feed(144000);
    r.run();
    r.w->stop();
    CHECK(r.w->status().state == WriterState::Stopped);
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.actual_data, r.fed * 2);
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, r.fed));
    CHECK(r.ops.count("write") > 280);

    Rig q; /* one byte at a time: odd splits of a frame, still all there */
    q.ops.short_max = 1;
    CHECK_EQ(q.begin(), 0);
    q.feed(300);
    q.w->stop();
    h = parse_header(posix_file_ops(), q.path());
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.actual_data, static_cast<uint64_t>(600));
    CHECK(audio_is_sig(q.path(), h.data_offset, 0, 300));
}

void free_space() {
    Rig r;
    uint64_t freeNow = 1ull << 30;
    r.ops.free_fn = [&] { return freeNow; };
    r.cfg.min_free_bytes = 100ull << 20;
    CHECK_EQ(r.begin(), 0);
    for (int s = 0; s < 3; s++) {
        r.clk.advance(1000);
        r.feed(48000);
        r.run();
    }
    CHECK(r.w->status().state == WriterState::Recording);
    freeNow = 50ull << 20;
    r.clk.advance(1000);
    r.feed(48000);
    r.run();
    WriterStatus st = r.w->status();
    CHECK(st.state == WriterState::Stopped);
    CHECK(st.reason == StopReason::LowSpace);
    CHECK(!st.reason_text.empty());
    CHECK_EQ(st.frames_written, r.fed); /* everything captured up to the stop is in the file */
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.consistent());
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, r.fed));
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "t1"), je), 0);
    CHECK(je.state == "closed" && !je.reason.empty());

    /* too little space at the start: refuse, create nothing */
    Rig q;
    q.ops.free_fn = [] { return static_cast<uint64_t>(10) << 20; };
    q.cfg.min_free_bytes = 100ull << 20;
    CHECK_EQ(q.begin(), -ENOSPC);
    StatInfo si;
    CHECK(posix_file_ops().stat(q.path(), si) < 0);
    CHECK(q.w->status().state == WriterState::Failed);
}

void auto_split() {
    Rig r;
    r.cfg.split_bytes = kDataOffset + 10000; /* 5000 frames per part */
    r.cfg.bwf.time_reference = 100;
    CHECK_EQ(r.begin(), 0);
    r.feed(22000);
    r.run();
    r.w->stop();
    CHECK_EQ(r.w->status().part, 4);
    uint64_t expectFrames[5] = {5000, 5000, 5000, 5000, 2000};
    uint64_t first = 0;
    for (int p = 0; p < 5; p++) {
        HeaderInfo h = parse_header(posix_file_ops(), r.path(p));
        CHECK(h.ok && h.consistent());
        CHECK_EQ(h.actual_data / 2, expectFrames[p]);
        StatInfo si;
        posix_file_ops().stat(r.path(p), si);
        CHECK(si.size <= r.cfg.split_bytes);
        CHECK(audio_is_sig(r.path(p), h.data_offset, first, expectFrames[p]));
        std::vector<unsigned char> b = read_file(r.path(p));
        uint64_t tr = static_cast<uint64_t>(b[h.bext_offset + 338]) | static_cast<uint64_t>(b[h.bext_offset + 339]) << 8;
        CHECK_EQ(tr, 100 + first); /* each part continues the time reference */
        first += expectFrames[p];
    }
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "t1"), je), 0);
    CHECK_EQ(je.parts.size(), static_cast<size_t>(5));
    CHECK(je.state == "closed");
    CHECK_EQ(je.frames, static_cast<uint64_t>(22000));
}

void rf64_switch() {
    Rig r;
    r.cfg.auto_split = false;
    r.cfg.riff_limit = kDataOffset - 8 + 20000; /* the writer switches to RF64 past 20,000 audio bytes */
    r.cfg.block_bytes = 4096;
    r.cfg.sync_interval_ms = 50;
    CHECK_EQ(r.begin(), 0);
    for (int i = 0; i < 2; i++) {
        r.clk.advance(100);
        r.feed(4000); /* 8000 bytes each */
        r.run();
    }
    HeaderInfo before = parse_header(posix_file_ops(), r.path());
    CHECK(before.ok && !before.rf64); /* 16,000 bytes: still a plain RIFF */
    for (int i = 0; i < 4; i++) {
        r.clk.advance(100);
        r.feed(4000);
        r.run();
    }
    HeaderInfo mid = parse_header(posix_file_ops(), r.path());
    CHECK(mid.ok && mid.rf64 && mid.has_ds64); /* crossed 20,000: switched and synced in place */
    r.w->stop();
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.rf64 && h.has_ds64 && h.consistent());
    CHECK_EQ(h.declared_data, static_cast<uint64_t>(48000));
    CHECK_EQ(h.raw_data, 0xFFFFFFFFull);
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, 24000));
    CHECK_EQ(r.w->status().part, 0);
}

void real_thread() {
    TempDir d;
    AudioFormat f;
    Ring ring(f, 4.0);
    WriterConfig cfg;
    cfg.fmt = f;
    cfg.take = "th";
    cfg.recordings_dir = d.path;
    std::string p = d.file("th.wav");
    cfg.namer = [p](int) { return p; };
    cfg.min_free_bytes = 0;
    cfg.sync_interval_ms = 40;
    cfg.flush_ms = 20;
    Writer w(ring, posix_file_ops(), system_clock(), cfg);
    CHECK_EQ(w.start(), 0);
    w.start_thread();
    uint64_t fed = 0;
    for (int i = 0; i < 30; i++) {
        std::vector<int16_t> b = sig_block(fed, 960);
        fed += ring.write(b.data(), 960);
        system_clock().sleep_ms(10);
    }
    w.stop();
    CHECK(w.status().state == WriterState::Stopped);
    CHECK_EQ(w.status().frames_written, fed);
    CHECK(w.status().syncs >= 3);
    HeaderInfo h = parse_header(posix_file_ops(), p);
    CHECK(h.ok && h.consistent() && h.actual_data == fed * 2);
    CHECK(audio_is_sig(p, h.data_offset, 0, fed));
}

}  // namespace

void test_writer() {
    cadence();
    flush_timing();
    pause_resume();
    enospc();
    eio();
    short_writes();
    free_space();
    auto_split();
    rf64_switch();
    real_thread();
}
