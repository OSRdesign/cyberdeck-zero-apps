/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core tests: a card that records at its own rate (16000 Hz, a speech headset; 22050 stereo; 96000). Everything
 * the core derives from the rate: the ring size, the writer's time-based flush and sync, the WAV and BWF header, the
 * journal and repair, the BWF time reference across parts.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "journal.hpp"
#include "repair.hpp"
#include "ring.hpp"
#include "writer.hpp"

#include <cstring>
#include <ctime>
#include <memory>

using namespace cassette;
using namespace tst;

namespace {

AudioFormat fmt_of(uint32_t rate, uint16_t ch = 1, uint16_t bits = 16) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

void ring_is_eight_seconds() {
    const uint32_t rates[] = {16000, 22050, 32000, 44100, 48000, 96000};
    for (uint32_t r : rates) {
        Ring ring(fmt_of(r)); /* the default: 8 s */
        CHECK_EQ(ring.capacity_frames(), static_cast<size_t>(r) * 8u);
    }
    Ring st(fmt_of(22050, 2, 24), 8.0);
    CHECK_EQ(st.frame_bytes(), static_cast<size_t>(6));
    CHECK_EQ(st.capacity_frames(), static_cast<size_t>(176400));
    /* 8 s at 16000 Hz fit exactly; one frame more is dropped */
    Ring r16(fmt_of(16000));
    std::vector<int16_t> a(128000, 7);
    CHECK_EQ(r16.write(a.data(), a.size()), static_cast<size_t>(128000));
    CHECK_EQ(r16.frames_dropped(), static_cast<uint64_t>(0));
    CHECK_EQ(r16.write(a.data(), 1), static_cast<size_t>(0));
    CHECK_EQ(r16.frames_dropped(), static_cast<uint64_t>(1));
}

void header_carries_the_rate() {
    const AudioFormat f = fmt_of(16000);
    BwfInfo b;
    b.description = "rate test";
    const std::vector<unsigned char> h = build_header(f, b, 32000);
    HeaderInfo i = parse_header(h.data(), h.size(), h.size() + 32000);
    CHECK(i.ok);
    CHECK_EQ(i.fmt.rate, 16000u);
    CHECK_EQ(i.fmt.channels, 1u);
    CHECK_EQ(i.fmt.bits, 16u);
    CHECK_EQ(i.block_align, 2u);
    auto g32 = [&](size_t o) {
        return static_cast<uint32_t>(h[o] | (h[o + 1] << 8) | (h[o + 2] << 16) | (static_cast<uint32_t>(h[o + 3]) << 24));
    };
    size_t fm = 12;
    while (fm + 8 < h.size() && std::memcmp(&h[fm], "fmt ", 4) != 0) fm += 8 + g32(fm + 4);
    CHECK(fm + 24 < h.size());
    CHECK_EQ(g32(fm + 12), 16000u); /* sample rate */
    CHECK_EQ(g32(fm + 16), 32000u); /* byte rate: 16000 * 2 */
    CHECK(coding_history_line(f) == "A=PCM,F=16000,W=16,M=mono,T=Cassette\r\n");
    CHECK(i.has_bext);
    const size_t o = static_cast<size_t>(i.bext_offset);
    CHECK(std::string(reinterpret_cast<const char *>(&h[o + 602])) == coding_history_line(f));
    CHECK(coding_history_line(fmt_of(22050, 2, 24)) == "A=PCM,F=22050,W=24,M=stereo,T=Cassette\r\n");
    CHECK(coding_history_line(fmt_of(96000)) == "A=PCM,F=96000,W=16,M=mono,T=Cassette\r\n");
    const std::vector<unsigned char> h2 = build_header(fmt_of(22050, 2, 24), BwfInfo{}, 0);
    HeaderInfo i2 = parse_header(h2.data(), h2.size(), h2.size());
    CHECK(i2.ok && i2.fmt.rate == 22050 && i2.fmt.channels == 2 && i2.fmt.bits == 24 && i2.block_align == 6);
}

/* 16000 Hz mono S16: 32,000 bytes a second. */
struct Rig16 {
    TempDir d;
    FakeClock clk;
    FaultOps ops{&clk};
    AudioFormat fmt = fmt_of(16000);
    Ring ring{fmt_of(16000)};
    WriterConfig cfg;
    std::unique_ptr<Writer> w;
    uint64_t fed = 0;

    Rig16() {
        cfg.fmt = fmt;
        cfg.take = "r16";
        cfg.recordings_dir = d.path;
        std::string dir = d.path;
        cfg.namer = [dir](int p) { return p == 0 ? dir + "/t.wav" : dir + "/t-" + std::to_string(p) + ".wav"; };
        cfg.min_free_bytes = 0;
        cfg.sync_interval_ms = 5000;
        cfg.block_bytes = 65536; /* two seconds at this rate */
    }
    int begin() {
        w = std::make_unique<Writer>(ring, ops, clk, cfg);
        return w->start();
    }
    void feed(size_t frames) {
        std::vector<int16_t> b = sig_block(fed, frames);
        CHECK_EQ(ring.write(b.data(), frames), frames);
        fed += frames;
    }
    void run() {
        bool busy = true;
        while (busy && w->pump(&busy)) {
        }
    }
    std::string path() const { return d.file("t.wav"); }
};

void writer_timing_is_time_based() {
    Rig16 r;
    CHECK_EQ(r.begin(), 0);
    const int w0 = r.ops.count("write");
    /* half a second of audio is 16,000 bytes: a quarter of a block. It still goes out after flush_ms, not after 2 s. */
    r.feed(8000);
    r.run();
    CHECK_EQ(r.ops.count("write"), w0);
    r.clk.advance(999);
    r.run();
    CHECK_EQ(r.ops.count("write"), w0);
    r.clk.advance(1);
    r.run();
    CHECK_EQ(r.ops.count("write"), w0 + 1);
    CHECK_EQ(r.ops.log.back().a, static_cast<uint64_t>(16000));
    /* three seconds: one full 64 KB block (2.05 s); the remainder is written at the next flush, not held for a block */
    const int w1 = r.ops.count("write");
    r.feed(48000);
    r.run();
    CHECK_EQ(r.ops.count("write"), w1 + 1);
    CHECK_EQ(r.ops.log.back().a, static_cast<uint64_t>(65536));
    r.clk.advance(1000);
    r.run();
    CHECK_EQ(r.ops.count("write"), w1 + 2);
    CHECK_EQ(r.w->status().frames_written, r.fed);

    /* a stop writes the rest at once, however little it is */
    r.feed(123);
    r.w->stop();
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.fmt.rate, 16000u);
    CHECK_EQ(h.actual_data, r.fed * 2);
    CHECK(audio_is_sig(r.path(), h.data_offset, 0, r.fed));
}

void last_audio_is_in_every_sync() {
    Rig16 r;
    CHECK_EQ(r.begin(), 0);
    /* one second in, then 0.3 s of fresh audio arrives just before the sync is due: the sync must contain it */
    r.feed(16000);
    r.clk.advance(4700);
    r.run();
    r.feed(4800);
    r.clk.advance(300); /* the sync is due now; the 0.3 s block is younger than flush_ms */
    r.run();
    CHECK_EQ(r.w->status().syncs, static_cast<uint64_t>(1));
    CHECK_EQ(r.w->status().frames_synced, r.fed);
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "r16"), je), 0);
    CHECK_EQ(je.fmt.rate, 16000u);
    CHECK_EQ(je.frames_synced, r.fed);
    HeaderInfo h = parse_header(posix_file_ops(), r.path());
    CHECK(h.consistent());
    CHECK_EQ(h.declared_data, r.fed * 2); /* the header after the sync names every frame so far */
    r.w->stop();
}

void journal_and_repair_use_the_journal_rate() {
    /* a take at 16000 Hz written by the real writer and abandoned (no stop): repair reads the rate from the journal */
    Rig16 r;
    CHECK_EQ(r.begin(), 0);
    r.feed(16000 * 3);
    r.clk.advance(1000);
    r.run();
    r.feed(16000 * 2);
    r.clk.advance(1000);
    r.run(); /* more audio went out after the last sync: the header is behind the file */
    const uint64_t on_disk = r.w->status().frames_written;
    CHECK(on_disk >= 16000u * 4u);
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.d.path, "r16"), je), 0);
    CHECK(je.state == "open");
    CHECK_EQ(je.fmt.rate, 16000u);
    CHECK_EQ(je.fmt.channels, 1u);
    /* repair a copy of the abandoned file with its journal (the writer object still holds the original) */
    TempDir copy;
    write_file(copy.file("t.wav"), read_file(r.path()));
    JournalEntry c = je;
    c.path = copy.file("t.wav");
    c.parts = {c.path};
    c.started_wall = static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(on_disk / 16000); /* "now" = file mtime */
    c.last_sync_wall = c.started_wall + static_cast<int64_t>(on_disk / 16000);
    CHECK_EQ(journal_write(posix_file_ops(), copy.path, c), 0);
    RepairReport rep = repair_all(posix_file_ops(), copy.path);
    CHECK(rep.errors.empty());
    CHECK_EQ(rep.repaired.size(), static_cast<size_t>(1));
    if (rep.repaired.size() == 1) {
        CHECK_EQ(rep.repaired[0].frames, on_disk);
        CHECK(rep.repaired[0].seconds > on_disk / 16000.0 - 1e-9 && rep.repaired[0].seconds < on_disk / 16000.0 + 1e-9);
        CHECK(rep.repaired[0].missing_seconds >= 0 && rep.repaired[0].missing_seconds < 2.5); /* the estimate divides by 16000, not 48000 */
    }
    HeaderInfo h = parse_header(posix_file_ops(), copy.file("t.wav"));
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.fmt.rate, 16000u);
    CHECK_EQ(h.declared_data, on_disk * 2);
    CHECK(audio_is_sig(copy.file("t.wav"), h.data_offset, 0, on_disk));
    r.w->stop();
}

void time_reference_continues_across_parts() {
    Rig16 r;
    r.cfg.bwf.time_reference = 10ull * 16000; /* ten seconds past midnight, in samples at 16000 Hz */
    r.cfg.split_bytes = 200000;
    r.cfg.auto_split = true;
    CHECK_EQ(r.begin(), 0);
    r.feed(16000 * 8); /* 256,000 bytes: more than one part */
    r.run();
    r.clk.advance(1000);
    r.run();
    r.w->stop();
    HeaderInfo h0 = parse_header(posix_file_ops(), r.d.file("t.wav"));
    HeaderInfo h1 = parse_header(posix_file_ops(), r.d.file("t-1.wav"));
    CHECK(h0.ok && h1.ok);
    CHECK_EQ(h1.fmt.rate, 16000u);
    auto tref = [&](const HeaderInfo &h, const std::string &p) {
        std::vector<unsigned char> b = read_file(p);
        const size_t o = static_cast<size_t>(h.bext_offset) + 338;
        return static_cast<uint64_t>(b[o]) | (static_cast<uint64_t>(b[o + 1]) << 8) | (static_cast<uint64_t>(b[o + 2]) << 16) |
               (static_cast<uint64_t>(b[o + 3]) << 24);
    };
    const uint64_t t0 = tref(h0, r.d.file("t.wav"));
    const uint64_t t1 = tref(h1, r.d.file("t-1.wav"));
    CHECK_EQ(t0, static_cast<uint64_t>(160000));
    CHECK_EQ(t1, t0 + h0.declared_data / 2); /* the next part starts where the last ended, counted in frames */
}

}  // namespace

void test_rates() {
    ring_is_eight_seconds();
    header_carries_the_rate();
    writer_timing_is_time_based();
    last_audio_is_in_every_sync();
    journal_and_repair_use_the_journal_rate();
    time_reference_continues_across_parts();
}
