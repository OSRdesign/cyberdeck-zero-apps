/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes tests: JSON, names, the take counter, edit-suffix numbers.
 */
#include "check.hpp"

#include "atomic_file.hpp"
#include "journal.hpp"
#include "json.hpp"
#include "naming.hpp"

#include <cerrno>

using namespace cassette;
using namespace tk;

namespace {

struct FixedClockState final : ClockState {
    bool set = true;
    bool synchronized() override { return set; }
};

struct WallClock final : Clock {
    int64_t wall = 0;
    uint64_t mono_ms() override { return 0; }
    int64_t wall_s() override { return wall; }
    void sleep_ms(uint32_t) override {}
};

TakeName tn(const std::string &prefix, uint64_t take, bool dated, uint32_t part = 1, uint32_t edit = 0) {
    TakeName t;
    t.prefix = prefix;
    t.take = take;
    t.has_date = dated;
    if (dated) t.when = DateTime{2026, 11, 8, 11, 5};
    t.part = part;
    t.edit = edit;
    return t;
}

}  // namespace

void test_json() {
    json::Value v;
    CHECK(json::parse("{\"a\": 1, \"b\": [true, false, null], \"c\": \"x\\u00e9\\n\", \"d\": -2.5e1}", v));
    CHECK(v.is_obj());
    CHECK_EQ(v.get("a")->n, 1.0);
    CHECK_EQ(v.get("b")->arr.size(), static_cast<size_t>(3));
    CHECK_EQ(v.get("c")->s, std::string("x\xC3\xA9\n"));
    CHECK_EQ(v.get("d")->n, -25.0);
    json::Value back;
    CHECK(json::parse(json::write(v), back));
    CHECK_EQ(json::write(back), json::write(v));
    CHECK_EQ(back.obj[0].first, std::string("a")); /* order kept */
    /* surrogate pair */
    CHECK(json::parse("\"\\ud83d\\ude00\"", v));
    CHECK_EQ(v.s, std::string("\xF0\x9F\x98\x80"));
    /* errors */
    CHECK(!json::parse("", v));
    CHECK(!json::parse("{", v));
    CHECK(!json::parse("{\"a\":}", v));
    CHECK(!json::parse("{\"a\":1} x", v));
    CHECK(!json::parse("[1,]", v));
    CHECK(!json::parse("\"\\q\"", v));
    CHECK(!json::parse("1e999", v));
    std::string deep(100, '[');
    CHECK(!json::parse(deep + std::string(100, ']'), v));
    /* garbage of every byte never crashes */
    std::string junk;
    for (int k = 0; k < 512; k++) junk += static_cast<char>((k * 131 + 7) & 0xFF);
    json::parse(junk, v);
    CHECK(true);
}

void test_naming() {
    /* zero padding and beyond 999 */
    CHECK_EQ(format_take_name(tn("take", 6, true)), std::string("take_006_20261108-1105.wav"));
    CHECK_EQ(format_take_name(tn("take", 999, true)), std::string("take_999_20261108-1105.wav"));
    CHECK_EQ(format_take_name(tn("take", 1000, true)), std::string("take_1000_20261108-1105.wav"));
    CHECK_EQ(format_take_name(tn("take", 123456, false)), std::string("take_123456_nodate.wav"));
    CHECK_EQ(format_take_name(tn("take", 6, false)), std::string("take_006_nodate.wav"));
    CHECK_EQ(format_take_name(tn("take", 6, true, 1, 2)), std::string("take_006_20261108-1105-2.wav"));
    CHECK_EQ(format_take_name(tn("take", 6, true, 3, 0)), std::string("take_006_20261108-1105_p3.wav"));

    /* nodate while the clock is not set */
    FixedClockState cs;
    WallClock wc;
    wc.wall = 1794135900; /* 2026-11-08 11:05:00 UTC */
    cs.set = false;
    CHECK_EQ(format_take_name(new_take_name("take", 7, cs, wc)), std::string("take_007_nodate.wav"));
    cs.set = true;
    CHECK_EQ(format_take_name(new_take_name("take", 7, cs, wc)), std::string("take_007_20261108-1105.wav"));

    /* the timesync marker */
    TempDir d;
    FileOps &ops = posix_file_ops();
    TimesyncClockState ts(ops, d.file("synchronized"));
    CHECK(!ts.synchronized());
    write_text(d.file("synchronized"), "");
    CHECK(ts.synchronized());

    /* parsing */
    TakeName t;
    CHECK(parse_take_name("take_006_20261108-1105.wav", t));
    CHECK_EQ(t.prefix, std::string("take"));
    CHECK_EQ(t.take, static_cast<uint64_t>(6));
    CHECK(t.has_date);
    CHECK(t.when == (DateTime{2026, 11, 8, 11, 5}));
    CHECK_EQ(t.edit, 0u);
    CHECK_EQ(t.part, 1u);
    CHECK_EQ(take_label(t), std::string("006"));
    CHECK(parse_take_name("take_006_20261108-1105-12.wav", t));
    CHECK_EQ(t.edit, 12u);
    CHECK_EQ(take_label(t), std::string("006-12"));
    CHECK(parse_take_name("my_band-2_1000_nodate-1.wav", t));
    CHECK_EQ(t.prefix, std::string("my_band-2"));
    CHECK_EQ(t.take, static_cast<uint64_t>(1000));
    CHECK(!t.has_date);
    CHECK_EQ(t.edit, 1u);
    CHECK_EQ(take_label(t), std::string("1000-1"));
    CHECK(parse_take_name("take_006_20261108-1105_p2-3.wav", t));
    CHECK_EQ(t.part, 2u);
    CHECK_EQ(t.edit, 3u);
    CHECK_EQ(take_label(t), std::string("006 p2-3"));
    /* a prefix that looks like a name */
    CHECK(parse_take_name("a_001_nodate_002_nodate.wav", t));
    CHECK_EQ(t.prefix, std::string("a_001_nodate"));
    CHECK_EQ(t.take, static_cast<uint64_t>(2));
    /* round trip */
    for (const TakeName &x : {tn("take", 6, true), tn("x-y_z", 1001, false, 1, 9), tn("Q", 0, true, 4, 0),
                              tn("take", 42, false, 2, 5)}) {
        TakeName y;
        CHECK(parse_take_name(format_take_name(x), y));
        CHECK_EQ(format_take_name(y), format_take_name(x));
    }
    /* not ours */
    CHECK(!parse_take_name("take_006_20261108-1105.WAV", t));
    CHECK(!parse_take_name("take_06_nodate.wav", t));
    CHECK(!parse_take_name("take_0006_nodate.wav", t));
    CHECK(!parse_take_name("take_006_20261308-1105.wav", t));
    CHECK(!parse_take_name("take_006_2026110-1105.wav", t));
    CHECK(!parse_take_name("_006_nodate.wav", t));
    CHECK(!parse_take_name("take_006_nodate-0.wav", t)); /* "-0" is not an edit number */
    CHECK(!parse_take_name("take_006_nodate_p1.wav", t)); /* part 1 has no suffix */
    CHECK(!parse_take_name("take_006_nodate.json", t));
    CHECK(!parse_take_name("recording.wav", t));
    CHECK(!parse_take_name(".x_006_nodate.wav", t));

    /* prefix rule */
    CHECK(valid_prefix("take"));
    CHECK(valid_prefix("A-b_9"));
    CHECK(valid_prefix(std::string(24, 'a')));
    CHECK(!valid_prefix(std::string(25, 'a')));
    CHECK(!valid_prefix(""));
    CHECK(!valid_prefix("../x"));
    CHECK(!valid_prefix("a/b"));
    CHECK(!valid_prefix("a\\b"));
    CHECK(!valid_prefix(".hidden"));
    CHECK(!valid_prefix(".."));
    CHECK(!valid_prefix("a b"));
    CHECK(!valid_prefix("caf\xC3\xA9"));
    CHECK(!valid_prefix(std::string("a\0b", 3)));
}

void test_counter() {
    TempDir d;
    FailOps ops;
    const std::string dir = d.file("Recordings"); /* created on first use */
    uint64_t t = 0;
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(1));
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(2));
    CHECK_EQ(read_text(take_counter_path(dir)), std::string("2\n"));

    /* deleting the newest take does not bring its number back */
    make_wav(dir + "/take_002_nodate.wav", 10);
    ops.unlink(dir + "/take_002_nodate.wav");
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(3));

    /* counter file lost: recover from file names and journals, plus one */
    make_wav(dir + "/take_005_nodate.wav", 10);
    make_wav(dir + "/other_012_20261108-1105-3.wav", 10); /* an edit result of take 12 */
    JournalEntry e;
    e.take = "take_017_nodate";
    e.path = dir + "/take_017_nodate.wav"; /* the file itself is gone */
    e.parts = {e.path};
    e.state = "closed";
    CHECK_EQ(journal_write(ops, dir, e), 0);
    ops.unlink(take_counter_path(dir));
    CHECK_EQ(highest_take_found(ops, dir), static_cast<uint64_t>(17));
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(18));

    /* a corrupt counter is ignored, the folder still wins */
    write_text(take_counter_path(dir), "garbage");
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(18)); /* 17 from the journal; counter unreadable */
    /* a counter ahead of the folder wins */
    write_text(take_counter_path(dir), "500\n");
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(501));

    /* crash safety: a failure between the temp write and the rename keeps the old value, hands out nothing */
    ops.rename_errno = EIO;
    t = 0;
    CHECK_EQ(reserve_take_number(ops, dir, t), -EIO);
    CHECK_EQ(t, static_cast<uint64_t>(0));
    CHECK_EQ(read_text(take_counter_path(dir)), std::string("501\n"));
    CHECK(!exists(take_counter_path(dir) + ".tmp"));
    ops.rename_errno = 0;
    ops.fsync_errno = EIO;
    CHECK_EQ(reserve_take_number(ops, dir, t), -EIO);
    CHECK_EQ(read_text(take_counter_path(dir)), std::string("501\n"));
    ops.fsync_errno = 0;
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(502));
}

void test_edits() {
    const std::string src = "take_006_20261108-1105.wav";
    std::vector<std::string> n = next_edit_names(src, 3, {src});
    CHECK_EQ(n.size(), static_cast<size_t>(3));
    CHECK_EQ(n[0], std::string("take_006_20261108-1105-1.wav"));
    CHECK_EQ(n[1], std::string("take_006_20261108-1105-2.wav"));
    CHECK_EQ(n[2], std::string("take_006_20261108-1105-3.wav"));
    CHECK(next_edit_names(src, 0, {}).empty());
    /* a trim alone gives -1 */
    CHECK_EQ(next_edit_names(src, 1, {src})[0], std::string("take_006_20261108-1105-1.wav"));
    /* cutting again continues; cutting an edit result continues the take's numbers */
    std::vector<std::string> have = {src, n[0], n[1], n[2]};
    CHECK_EQ(next_edit_names(src, 1, have)[0], std::string("take_006_20261108-1105-4.wav"));
    CHECK_EQ(next_edit_names(n[1], 2, have)[1], std::string("take_006_20261108-1105-5.wav"));
    /* a deleted -3 (the highest) is not reused thanks to the record */
    std::vector<std::string> after = {src, n[0], n[1]};
    CHECK_EQ(next_edit_names(src, 1, after)[0], std::string("take_006_20261108-1105-3.wav")); /* no record: reuse */
    CHECK_EQ(next_edit_names(src, 1, after, {1, 2, 3})[0], std::string("take_006_20261108-1105-4.wav"));
    /* other takes and other parts do not count; a renamed edit of the same take does */
    CHECK_EQ(next_edit_names(src, 1, {"take_007_20261108-1105-9.wav", "take_006_20261108-1105_p2-8.wav"})[0],
             std::string("take_006_20261108-1105-1.wav"));
    CHECK_EQ(next_edit_names(src, 1, {"band_006_20261108-1105-4.wav"})[0],
             std::string("take_006_20261108-1105-5.wav"));
    /* the take number is unchanged */
    TakeName t;
    CHECK(parse_take_name(n[2], t));
    CHECK_EQ(t.take, static_cast<uint64_t>(6));
    CHECK_EQ(take_label(t), std::string("006-3"));
    /* a name outside the grammar */
    CHECK_EQ(next_edit_names("interview.wav", 2, {"interview.wav", "interview-1.wav"})[1],
             std::string("interview-3.wav"));

    /* the reserving call with the record on disk: a deleted -2 is never handed out again */
    TempDir d;
    FailOps ops;
    const std::string dir = d.path;
    make_wav(dir + "/" + src, 10);
    std::vector<std::string> out;
    CHECK_EQ(reserve_edit_names(ops, dir, src, 2, out), 0);
    CHECK_EQ(out.size(), static_cast<size_t>(2));
    CHECK_EQ(out[1], std::string("take_006_20261108-1105-2.wav"));
    make_wav(dir + "/" + out[0], 10);
    make_wav(dir + "/" + out[1], 10);
    ops.unlink(dir + "/" + out[1]); /* the user deletes -2 */
    CHECK_EQ(reserve_edit_names(ops, dir, src, 1, out), 0);
    CHECK_EQ(out[0], std::string("take_006_20261108-1105-3.wav"));
    /* nothing written: the reservation still holds */
    CHECK_EQ(reserve_edit_names(ops, dir, src, 1, out), 0);
    CHECK_EQ(out[0], std::string("take_006_20261108-1105-4.wav"));
    std::vector<uint32_t> used = read_edit_record(ops, dir, src);
    CHECK_EQ(used.size(), static_cast<size_t>(4));
    CHECK_EQ(edit_record_key(src), std::string("006"));
    CHECK_EQ(edit_record_key("take_006_nodate_p2-1.wav"), std::string("006_p2"));
    /* a failed record write hands out no names */
    ops.rename_errno = EIO;
    CHECK_EQ(reserve_edit_names(ops, dir, src, 1, out), -EIO);
    CHECK(out.empty());
    ops.rename_errno = 0;
    CHECK_EQ(reserve_edit_names(ops, dir, src, 1, out), 0);
    CHECK_EQ(out[0], std::string("take_006_20261108-1105-5.wav"));
}

/* ---- the Take number setting (set_next_take_number) ---- */
void test_take_number_setting() {
    TempDir d;
    FailOps ops;
    const std::string dir = d.file("Recordings");
    uint64_t t = 0, landed = 0;

    /* set, then the counter continues from there; names pad to 3 and grow past 999 */
    CHECK_EQ(set_next_take_number(ops, dir, "take", 50, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(50));
    CHECK_EQ(read_text(take_counter_path(dir)), std::string("49\n"));
    CHECK_EQ(peek_take_number(ops, dir, "take"), static_cast<uint64_t>(50));
    CHECK_EQ(reserve_take_number(ops, dir, t, "take"), 0);
    CHECK_EQ(t, static_cast<uint64_t>(50));
    CHECK_EQ(take_number_text(t), std::string("050"));
    CHECK_EQ(reserve_take_number(ops, dir, t, "take"), 0);
    CHECK_EQ(t, static_cast<uint64_t>(51));

    /* range: 0 and 10000 are refused and change nothing; the limits are accepted */
    CHECK_EQ(set_next_take_number(ops, dir, "take", 0, landed), -EINVAL);
    CHECK_EQ(set_next_take_number(ops, dir, "take", 10000, landed), -EINVAL);
    CHECK_EQ(read_text(take_counter_path(dir)), std::string("51\n"));
    CHECK_EQ(set_next_take_number(ops, dir, "take", 1, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(1));
    CHECK_EQ(set_next_take_number(ops, dir, "take", 9999, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(9999));
    CHECK_EQ(reserve_take_number(ops, dir, t, "take"), 0);
    CHECK_EQ(t, static_cast<uint64_t>(9999));
    CHECK_EQ(reserve_take_number(ops, dir, t, "take"), 0); /* past the range the counter just keeps counting */
    CHECK_EQ(t, static_cast<uint64_t>(10000));
    TakeName big = tn("take", t, false);
    CHECK_EQ(format_take_name(big), std::string("take_10000_nodate.wav"));
    TakeName p;
    CHECK(parse_take_name(format_take_name(big), p) && p.take == 10000);

    /* collision guard: takes 001..003 exist, reset to 1 lands on 004 */
    TempDir d2;
    const std::string dir2 = d2.file("Rec");
    CHECK_EQ(make_dirs(ops, dir2), 0);
    for (int i = 1; i <= 3; ++i) make_wav(dir2 + "/take_00" + std::to_string(i) + "_20261108-110" + std::to_string(i) + ".wav", 10);
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 1, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(4));
    CHECK_EQ(peek_take_number(ops, dir2, "take"), static_cast<uint64_t>(4));
    /* an edit result and a later part count as the take number being in use */
    make_wav(dir2 + "/take_010_nodate_p2.wav", 10);
    make_wav(dir2 + "/take_011_nodate-2.wav", 10);
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 10, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(12));
    CHECK_EQ(read_text(take_counter_path(dir2)), std::string("11\n"));
    /* another prefix does not collide: the number is only unique per prefix */
    CHECK_EQ(set_next_take_number(ops, dir2, "band", 2, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(2));
    /* a take in progress (a journal) holds its number */
    JournalEntry e;
    e.take = "take_020_nodate";
    e.path = dir2 + "/take_020_nodate.wav";
    e.parts = {e.path};
    e.state = "open";
    CHECK_EQ(journal_write(ops, dir2, e), 0);
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 20, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(21));
    /* the counter lands below the highest take: reserve counts on from the set value, skipping used numbers only */
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 5, landed), 0);
    CHECK_EQ(landed, static_cast<uint64_t>(5));
    CHECK_EQ(reserve_take_number(ops, dir2, t, "take"), 0);
    CHECK_EQ(t, static_cast<uint64_t>(5));
    make_wav(dir2 + "/take_005_nodate.wav", 10);
    CHECK_EQ(reserve_take_number(ops, dir2, t, "take"), 0);
    CHECK_EQ(t, static_cast<uint64_t>(6));
    /* a take that is made later on a number that was skipped to never duplicates one that exists */
    for (int i = 6; i <= 9; ++i) make_wav(dir2 + "/take_00" + std::to_string(i) + "_nodate.wav", 10);
    CHECK_EQ(reserve_take_number(ops, dir2, t, "take"), 0);
    CHECK_EQ(t, static_cast<uint64_t>(12)); /* 7..11 used (10 and 11 as p2 / edit) */

    /* nothing existing is touched */
    CHECK(exists(dir2 + "/take_001_20261108-1101.wav"));
    CHECK(exists(dir2 + "/take_011_nodate-2.wav"));

    /* persistence: a fresh read of the folder (a restart) gives the same number; atomic write failures keep the old */
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 300, landed), 0);
    CHECK_EQ(peek_take_number(ops, dir2, "take"), static_cast<uint64_t>(300));
    ops.rename_errno = EIO;
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 400, landed), -EIO);
    CHECK_EQ(read_text(take_counter_path(dir2)), std::string("299\n"));
    CHECK(!exists(take_counter_path(dir2) + ".tmp"));
    ops.rename_errno = 0;
    ops.fsync_errno = EIO;
    CHECK_EQ(set_next_take_number(ops, dir2, "take", 400, landed), -EIO);
    CHECK_EQ(read_text(take_counter_path(dir2)), std::string("299\n"));
    ops.fsync_errno = 0;
    CHECK_EQ(peek_take_number(ops, dir2, "take"), static_cast<uint64_t>(300));

    /* a lost counter: the folder decides again */
    ops.unlink(take_counter_path(dir2));
    CHECK_EQ(peek_take_number(ops, dir2, "take"), highest_take_found(ops, dir2) + 1);

    /* interplay: auto-split parts and edit names keep the take number; the edit names ignore the counter */
    set_next_take_number(ops, dir2, "take", 60, landed);
    reserve_take_number(ops, dir2, t, "take");
    TakeName first = tn("take", t, true);
    TakeName part2 = first;
    part2.part = 2;
    CHECK_EQ(format_take_name(part2), std::string("take_060_20261108-1105_p2.wav"));
    std::vector<std::string> out = next_edit_names(format_take_name(first), 2, {format_take_name(first)});
    CHECK_EQ(out[0], std::string("take_060_20261108-1105-1.wav"));
    CHECK_EQ(out[1], std::string("take_060_20261108-1105-2.wav"));
    CHECK_EQ(peek_take_number(ops, dir2, "take"), static_cast<uint64_t>(61)); /* unchanged by the edits */
}
