/*
 * SPDX-License-Identifier: MIT
 *
 * Journal: JSON round trip, atomic replace, failure leaves the old file.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "journal.hpp"

#include <cerrno>

using namespace cassette;
using namespace tst;

void test_journal() {
    TempDir d;
    FaultOps ops;
    JournalEntry e;
    e.take = "voice_007_20261108-1105";
    e.path = d.file("a.wav");
    e.parts = {e.path, d.file("a-1.wav")};
    e.fmt = AudioFormat{44100, 2, 24};
    e.device_name = "USB \"Mic\" \\ one\n";
    e.device_id = "usb-C-Media_Electronics-00";
    e.started_mono_ms = 123456;
    e.started_wall = 1700000000;
    e.sync_interval_ms = 5000;
    e.auto_split = false;
    e.frames = 99;
    e.frames_synced = 90;
    e.last_sync_wall = 1700000010;
    e.paused_frames = 5;
    e.dropped_frames = 7;
    e.lost_frames = 14400;
    CHECK_EQ(journal_write(ops, d.path, e), 0);
    const std::string f = journal_file(d.path, e.take);
    CHECK(f == d.path + "/.journal/voice_007_20261108-1105.json");
    StatInfo st;
    CHECK(ops.stat(f + ".tmp", st) < 0); /* the temp file was renamed away */
    CHECK(ops.count("fsync") >= 1 && ops.count("fsync_dir") >= 1 && ops.count("rename") == 1);

    JournalEntry r;
    CHECK_EQ(journal_read(ops, f, r), 0);
    CHECK(r.take == e.take && r.path == e.path && r.parts == e.parts);
    CHECK_EQ(r.fmt.rate, 44100u);
    CHECK_EQ(static_cast<int>(r.fmt.channels), 2);
    CHECK_EQ(static_cast<int>(r.fmt.bits), 24);
    CHECK(r.device_name == e.device_name && r.device_id == e.device_id);
    CHECK_EQ(r.started_mono_ms, e.started_mono_ms);
    CHECK_EQ(r.started_wall, e.started_wall);
    CHECK_EQ(r.sync_interval_ms, 5000u);
    CHECK(!r.auto_split);
    CHECK(r.state == "open");
    CHECK_EQ(r.frames, static_cast<uint64_t>(99));
    CHECK_EQ(r.frames_synced, static_cast<uint64_t>(90));
    CHECK_EQ(r.paused_frames, static_cast<uint64_t>(5));
    CHECK_EQ(r.dropped_frames, static_cast<uint64_t>(7));
    CHECK_EQ(r.lost_frames, static_cast<uint64_t>(14400));

    /* closing */
    e.state = "closed";
    e.frames = 123;
    CHECK_EQ(journal_write(ops, d.path, e), 0);
    CHECK_EQ(journal_read(ops, f, r), 0);
    CHECK(r.state == "closed");
    CHECK_EQ(r.frames, static_cast<uint64_t>(123));

    /* an error records its reason */
    e.state = "error";
    e.reason = "write error: Input/output error";
    CHECK_EQ(journal_write(ops, d.path, e), 0);
    CHECK_EQ(journal_read(ops, f, r), 0);
    CHECK(r.state == "error" && r.reason == e.reason);

    /* a failing rename leaves the previous journal intact and no temp file behind */
    JournalEntry e2 = e;
    e2.state = "open";
    e2.frames = 1;
    ops.rename_errno = EIO;
    CHECK_EQ(journal_write(ops, d.path, e2), -EIO);
    ops.rename_errno = 0;
    CHECK_EQ(journal_read(ops, f, r), 0);
    CHECK(r.state == "error" && r.frames == 123);
    CHECK(ops.stat(f + ".tmp", st) < 0);

    /* garbage and partial files are refused, never half-read */
    write_file(d.file("bad.json"), std::vector<unsigned char>{'{', '"', 't'});
    CHECK(journal_read(ops, d.file("bad.json"), r) < 0);
    write_file(d.file("bad2.json"), std::vector<unsigned char>{'[', ']'});
    CHECK(journal_read(ops, d.file("bad2.json"), r) < 0);
    CHECK(journal_read(ops, d.file("none.json"), r) < 0);
    CHECK(journal_sanitize("a/b") == "a_b" && journal_sanitize("..") == "take" && journal_sanitize("").size() > 0);
}
