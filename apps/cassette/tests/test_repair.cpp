/*
 * SPDX-License-Identifier: MIT
 *
 * Repair: hand-built truncated files (arecord placeholder header, oversized header), journal-driven repair, idempotence,
 * the report, the RF64 case on a mocked size.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "journal.hpp"
#include "repair.hpp"

#include <cstring>

using namespace cassette;
using namespace tst;

static std::vector<unsigned char> arecord_file(size_t audio_bytes) {
    std::vector<unsigned char> h(44, 0);
    auto p32 = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; i++) h[o + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
    };
    std::memcpy(&h[0], "RIFF", 4);
    p32(4, 0x80000024u);
    std::memcpy(&h[8], "WAVEfmt ", 8);
    p32(16, 16);
    h[20] = 1;
    h[22] = 1;
    p32(24, 48000);
    p32(28, 96000);
    h[32] = 2;
    h[34] = 16;
    std::memcpy(&h[36], "data", 4);
    p32(40, 0x80000000u);
    for (size_t k = 0; k < audio_bytes; k++) {
        int16_t s = sig(k / 2);
        h.push_back(static_cast<unsigned char>(k % 2 ? (s >> 8) : (s & 0xFF)));
    }
    return h;
}

static std::vector<unsigned char> cassette_file(uint64_t declared, size_t audio_frames) {
    std::vector<unsigned char> h = build_header(AudioFormat{}, BwfInfo{}, declared);
    for (size_t k = 0; k < audio_frames; k++) {
        int16_t s = sig(k);
        h.push_back(static_cast<unsigned char>(s & 0xFF));
        h.push_back(static_cast<unsigned char>(s >> 8));
    }
    return h;
}

static void arecord_truncated() {
    TempDir d;
    write_file(d.file("p0.wav"), arecord_file(48000 * 2 * 3 + 1)); /* 3 s and one stray byte (a cut frame) */
    FaultOps ops;
    RepairReport r = repair_all(ops, d.path);
    CHECK(r.errors.empty());
    CHECK_EQ(r.repaired.size(), static_cast<size_t>(1));
    if (r.repaired.size() != 1) return;
    CHECK(r.repaired[0].take == "p0" && !r.repaired[0].had_journal);
    CHECK_EQ(r.repaired[0].frames, static_cast<uint64_t>(144000));
    CHECK_EQ(r.repaired[0].truncated_bytes, 1u);
    CHECK(r.repaired[0].seconds > 2.99 && r.repaired[0].seconds < 3.01);
    CHECK(r.repaired[0].missing_seconds < 0); /* no journal: no start time */
    HeaderInfo h = parse_header(posix_file_ops(), d.file("p0.wav"));
    CHECK(h.ok && h.consistent() && !h.suspicious());
    CHECK_EQ(h.declared_data, static_cast<uint64_t>(288000));
    CHECK_EQ(h.data_offset, static_cast<uint64_t>(44));
    CHECK(audio_is_sig(d.file("p0.wav"), 44, 0, 144000)); /* the audio is untouched */
    /* only the header fields and the one partial byte changed: ftruncate first, then the two size fields */
    CHECK_EQ(ops.count("ftruncate"), 1);
    int pw = 0;
    for (const FaultOps::Ev &e : ops.log)
        if (e.op == "pwrite") {
            pw++;
            CHECK(e.a == 40 || e.a == 4);
            CHECK_EQ(e.b, static_cast<uint64_t>(4));
        }
    CHECK_EQ(pw, 2);

    /* idempotent */
    std::vector<unsigned char> once = read_file(d.file("p0.wav"));
    FaultOps ops2;
    RepairReport r2 = repair_all(ops2, d.path);
    CHECK(r2.repaired.empty() && r2.errors.empty());
    CHECK(read_file(d.file("p0.wav")) == once);
    CHECK_EQ(ops2.count("pwrite") + ops2.count("ftruncate"), 0);
}

static void oversized_header() {
    TempDir d;
    write_file(d.file("big.wav"), cassette_file(10000000, 2000)); /* declares 10 MB, holds 4000 bytes */
    FaultOps ops;
    RepairReport r = repair_all(ops, d.path);
    CHECK_EQ(r.repaired.size(), static_cast<size_t>(1));
    HeaderInfo h = parse_header(posix_file_ops(), d.file("big.wav"));
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.declared_data, static_cast<uint64_t>(4000));
    CHECK(audio_is_sig(d.file("big.wav"), h.data_offset, 0, 2000));
    CHECK(repair_all(ops, d.path).repaired.empty());
}

static void healthy_and_foreign_untouched() {
    TempDir d;
    write_file(d.file("ok.wav"), cassette_file(4000, 2000));  /* a finished take */
    write_file(d.file("short.wav"), cassette_file(100, 2000)); /* declares less than it holds: could have chunks after */
    write_file(d.file("notwav.wav"), std::vector<unsigned char>{1, 2, 3, 4});
    write_file(d.file("notes.txt"), std::vector<unsigned char>{'x'});
    auto a = read_file(d.file("ok.wav")), b = read_file(d.file("short.wav"));
    FaultOps ops;
    RepairReport r = repair_all(ops, d.path);
    CHECK(r.repaired.empty());
    CHECK_EQ(r.errors.size(), static_cast<size_t>(1)); /* only notwav.wav is reported */
    CHECK(read_file(d.file("ok.wav")) == a && read_file(d.file("short.wav")) == b);
    CHECK_EQ(ops.count("pwrite"), 0);
}

static JournalEntry open_journal(const TempDir &d, const std::string &take, const std::string &file) {
    JournalEntry je;
    je.take = take;
    je.path = d.file(file);
    je.parts = {je.path};
    je.started_wall = 1000;
    je.last_sync_wall = 1010;
    je.state = "open";
    return je;
}

static void journal_driven() {
    TempDir d;
    FaultOps ops;
    /* the header carries the sizes of the last sync (undersized) and more audio followed: only a journal makes it repairable */
    write_file(d.file("a.wav"), cassette_file(1000, 480000 + 1)); /* 10 s of frames, one extra frame */
    write_file(d.file("nj.wav"), cassette_file(1000, 5000));       /* the same without a journal */
    JournalEntry je = open_journal(d, "a", "a.wav");
    CHECK_EQ(journal_write(ops, d.path, je), 0);
    ops.stat_mtime = 1013; /* the file was last written 13 s after the start */
    RepairReport r = repair_all(ops, d.path);
    ops.stat_mtime = -1;
    CHECK_EQ(r.repaired.size(), static_cast<size_t>(1)); /* nj.wav is left alone: undersized, not suspicious */
    if (r.repaired.size() == 1) {
        CHECK(r.repaired[0].take == "a" && r.repaired[0].had_journal);
        CHECK_EQ(r.repaired[0].frames, static_cast<uint64_t>(480001));
        CHECK(r.repaired[0].missing_seconds > 2.98 && r.repaired[0].missing_seconds < 3.02); /* 13 s - 10 s */
    }
    HeaderInfo h = parse_header(posix_file_ops(), d.file("a.wav"));
    CHECK(h.ok && h.consistent());
    CHECK_EQ(h.actual_data, static_cast<uint64_t>(480001 * 2));
    JournalEntry after;
    CHECK_EQ(journal_read(ops, journal_file(d.path, "a"), after), 0);
    CHECK(after.state == "repaired");
    CHECK_EQ(after.frames, static_cast<uint64_t>(480001));
    HeaderInfo nj = parse_header(posix_file_ops(), d.file("nj.wav"));
    CHECK_EQ(nj.declared_data, static_cast<uint64_t>(1000));
    /* a second run does nothing */
    std::vector<unsigned char> snap = read_file(d.file("a.wav"));
    FaultOps ops2;
    CHECK(repair_all(ops2, d.path).repaired.empty());
    CHECK(read_file(d.file("a.wav")) == snap);
    CHECK_EQ(ops2.count("pwrite"), 0);

    /* closed and error journals are not touched, even when their file looks unfinished */
    TempDir e;
    write_file(e.file("c.wav"), arecord_file(2000));
    JournalEntry cj = open_journal(e, "c", "c.wav");
    cj.state = "closed";
    CHECK_EQ(journal_write(ops, e.path, cj), 0);
    RepairReport rc = repair_all(ops, e.path);
    CHECK(rc.repaired.empty());
    CHECK(read_file(e.file("c.wav")) == arecord_file(2000));

    /* an open journal whose file never got created */
    TempDir g;
    JournalEntry gj = open_journal(g, "ghost", "ghost.wav");
    CHECK_EQ(journal_write(ops, g.path, gj), 0);
    RepairReport rg = repair_all(ops, g.path);
    CHECK(rg.repaired.empty() && rg.errors.size() == 1);
    CHECK_EQ(journal_read(ops, journal_file(g.path, "ghost"), after), 0);
    CHECK(after.state == "abandoned");
    CHECK(repair_all(ops, g.path).errors.empty()); /* and it is not reported again */

    /* an open journal with a file shorter than its header: reported, not guessed */
    TempDir h2;
    write_file(h2.file("cut.wav"), std::vector<unsigned char>(100, 0));
    CHECK_EQ(journal_write(ops, h2.path, open_journal(h2, "cut", "cut.wav")), 0);
    RepairReport rh = repair_all(ops, h2.path);
    CHECK(rh.repaired.empty() && !rh.errors.empty());
}

static void multi_part_journal() {
    TempDir d;
    FaultOps ops;
    write_file(d.file("m.wav"), cassette_file(0, 3000));
    write_file(d.file("m-1.wav"), cassette_file(0, 1000));
    JournalEntry je = open_journal(d, "m", "m.wav");
    je.parts.push_back(d.file("m-1.wav"));
    CHECK_EQ(journal_write(ops, d.path, je), 0);
    RepairReport r = repair_all(ops, d.path);
    CHECK_EQ(r.repaired.size(), static_cast<size_t>(1));
    if (r.repaired.size() == 1) CHECK_EQ(r.repaired[0].frames, static_cast<uint64_t>(4000));
    CHECK(parse_header(posix_file_ops(), d.file("m.wav")).consistent());
    CHECK(parse_header(posix_file_ops(), d.file("m-1.wav")).consistent());
}

static void rf64_repair_mocked() {
    TempDir d;
    FaultOps ops;
    const uint64_t big = 5ull << 30; /* 5 GiB of audio, never written: only stat() is told so */
    write_file(d.file("r.wav"), cassette_file(0, 100));
    ops.stat_size = static_cast<int64_t>(kDataOffset + big);
    FileRepair fr = repair_file(ops, d.file("r.wav"), true);
    CHECK(fr.ok && fr.changed && fr.rf64 && !fr.clamped);
    CHECK_EQ(fr.data_bytes, big);
    HeaderInfo h = parse_header(ops, d.file("r.wav"));
    CHECK(h.ok && h.rf64 && h.has_ds64 && h.consistent());
    CHECK_EQ(h.declared_data, big);
    FileRepair again = repair_file(ops, d.file("r.wav"), true); /* idempotent */
    CHECK(again.ok && !again.changed);
    /* a foreign file (arecord header) over 4 GiB cannot become RF64: clamped once, then left alone */
    write_file(d.file("f.wav"), arecord_file(100));
    ops.stat_size = static_cast<int64_t>(44 + big);
    FileRepair f1 = repair_file(ops, d.file("f.wav"), false);
    CHECK(f1.ok && f1.changed && f1.clamped);
    FileRepair f2 = repair_file(ops, d.file("f.wav"), false);
    CHECK(f2.ok && !f2.changed && f2.clamped);
}

/* A crash in the middle of a repair: each step is one write; running repair again completes it. */
static void interrupted_repair() {
    TempDir d;
    write_file(d.file("p.wav"), arecord_file(2001));
    std::vector<unsigned char> orig = read_file(d.file("p.wav"));
    /* state A: only the partial byte cut */
    std::vector<unsigned char> a = orig;
    a.pop_back();
    write_file(d.file("p.wav"), a);
    FaultOps ops;
    CHECK_EQ(repair_all(ops, d.path).repaired.size(), static_cast<size_t>(1));
    CHECK(audio_is_sig(d.file("p.wav"), 44, 0, 1000));
    /* state B: the data size done, the RIFF size not */
    write_file(d.file("q.wav"), a);
    {
        std::vector<unsigned char> b = a;
        uint32_t v = 2000;
        for (int i = 0; i < 4; i++) b[40 + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
        write_file(d.file("q.wav"), b);
    }
    RepairReport r = repair_all(ops, d.path);
    CHECK_EQ(r.repaired.size(), static_cast<size_t>(1)); /* q.wav, still suspicious by its RIFF size */
    HeaderInfo h = parse_header(posix_file_ops(), d.file("q.wav"));
    CHECK(h.consistent());
    CHECK(audio_is_sig(d.file("q.wav"), 44, 0, 1000));
}

void test_repair() {
    arecord_truncated();
    oversized_header();
    healthy_and_foreign_untouched();
    journal_driven();
    multi_part_journal();
    rf64_repair_mocked();
    interrupted_repair();
}
