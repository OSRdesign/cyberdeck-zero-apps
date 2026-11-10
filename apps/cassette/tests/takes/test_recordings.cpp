/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes tests: listing the recordings folder, space, delete, rename.
 */
#include "check.hpp"

#include "journal.hpp"
#include "naming.hpp"
#include "recordings.hpp"

#include <cerrno>
#include <cmath>
#include <filesystem>

using namespace cassette;
using namespace tk;

namespace {

const TakeInfo *find(const std::vector<TakeInfo> &v, const std::string &name) {
    for (const TakeInfo &t : v)
        if (t.name == name) return &t;
    return nullptr;
}

JournalEntry journal_for(const std::string &dir, const std::string &name, const std::string &state) {
    JournalEntry e;
    e.take = file_stem(name);
    e.path = dir + "/" + name;
    e.parts = {e.path};
    e.state = state;
    return e;
}

}  // namespace

/* Renames of ordinary takes with '_' and '-' in the old and the new prefix, big take numbers, and spaces end to end:
 * the file, its peak cache, its journal, and the names an edit gets afterwards. */
void test_rename_prefixes() {
    TempDir d;
    FailOps ops;
    const std::string dir = d.path;
    std::string nn;
    /* (old file name, new prefix, new file name) */
    const char *cases[][3] = {
        {"take_007_20261109-1403.wav", "my_take-2", "my_take-2_007_20261109-1403.wav"},
        {"take_007_20261109-1403.wav", "a-b", "a-b_007_20261109-1403.wav"},
        {"take_007_20261109-1403.wav", "a_b", "a_b_007_20261109-1403.wav"},
        {"a_b_007_20261109-1403.wav", "c-d", "c-d_007_20261109-1403.wav"},
        {"a-b_007_20261109-1403.wav", "c_d", "c_d_007_20261109-1403.wav"},
        {"a-b_007_20261109-0903.wav", "c_d", "c_d_007_20261109-0903.wav"},
        {"x_1000_20261109-1403.wav", "y_z-1", "y_z-1_1000_20261109-1403.wav"},
        {"x_12345_nodate.wav", "y-z", "y-z_12345_nodate.wav"},
        {"x-9_001_nodate.wav", "-", "-_001_nodate.wav"},
        {"take_007_20261109-1403.wav", "my take", "my take_007_20261109-1403.wav"},
        {"my take_007_20261109-1403.wav", "your  take 2", "your  take 2_007_20261109-1403.wav"},
        {"my take_007_nodate_p2.wav", "a_b-c d", "a_b-c d_007_nodate_p2.wav"},
        {"a b_007_20261109-1403-2.wav", "q r", "q r_007_20261109-1403-2.wav"},
    };
    for (auto &c : cases) {
        make_wav(d.file(c[0]), 10);
        RenameRefusal why = RenameRefusal::BadChar;
        CHECK_EQ(rename_take(ops, dir, c[0], c[1], nn, &why), 0);
        CHECK(why == RenameRefusal::None);
        CHECK_EQ(nn, std::string(c[2]));
        CHECK(exists(d.file(c[2])));
        CHECK(!exists(d.file(c[0])));
        ops.unlink(d.file(c[2]));
    }
    /* a take with spaces: peak cache and journal follow, a rename back works, edits continue its numbering */
    const std::string a = "my band_005_20261109-1403.wav", b = "other one_005_20261109-1403.wav";
    make_wav(d.file(a), 10);
    write_text(d.file("my band_005_20261109-1403.peaks"), "pk");
    CHECK_EQ(journal_write(ops, dir, journal_for(dir, a, "closed")), 0);
    CHECK_EQ(rename_take(ops, dir, a, "other one", nn), 0);
    CHECK_EQ(nn, b);
    CHECK(exists(d.file("other one_005_20261109-1403.peaks")));
    CHECK(!exists(d.file("my band_005_20261109-1403.peaks")));
    {
        const std::string jf = find_journal_for(ops, dir, b);
        CHECK(!jf.empty());
        JournalEntry e;
        CHECK_EQ(journal_read(ops, jf, e), 0);
        CHECK_EQ(e.path, dir + "/" + b);
        CHECK_EQ(e.parts[0], dir + "/" + b);
    }
    std::vector<TakeInfo> list;
    CHECK_EQ(list_takes(ops, dir, list), 0);
    CHECK(find(list, b) != nullptr);
    CHECK_EQ(peaks_name(b), std::string("other one_005_20261109-1403.peaks"));
    std::vector<std::string> en;
    CHECK_EQ(reserve_edit_names(ops, dir, b, 2, en), 0);
    CHECK_EQ(en.size(), 2u);
    CHECK_EQ(en[0], std::string("other one_005_20261109-1403-1.wav"));
    CHECK_EQ(en[1], std::string("other one_005_20261109-1403-2.wav"));
    CHECK_EQ(delete_take(ops, dir, b), 0);
    CHECK(!exists(d.file("other one_005_20261109-1403.peaks")));
    CHECK(find_journal_for(ops, dir, b).empty());
}

void test_recordings() {
    TempDir d;
    FailOps ops;
    const std::string dir = d.path;
    CHECK_EQ(expand_home("~/Recordings", "/home/pi"), std::string("/home/pi/Recordings"));
    CHECK_EQ(expand_home("/data/rec", "/home/pi"), std::string("/data/rec"));
    CHECK_EQ(expand_home("~", "/home/pi"), std::string("/home/pi"));

    std::vector<TakeInfo> list;
    CHECK_EQ(list_takes(ops, d.file("missing"), list), -ENOENT);
    CHECK(list.empty());

    /* normal takes, one with bext date/time */
    BwfInfo info;
    info.origination_date = "2026-11-08";
    info.origination_time = "11:05:00";
    info.time_reference = 48000ull * 3600 * 11 + 48000ull * 300;
    make_wav(d.file("take_006_20261108-1105.wav"), 48000, AudioFormat(), info);
    make_wav(d.file("take_006_20261108-1105-1.wav"), 24000);
    AudioFormat st;
    st.channels = 2;
    st.bits = 24;
    st.rate = 44100;
    make_wav(d.file("take_010_nodate.wav"), 44100 * 2, st);
    make_wav(d.file("take_009_nodate.wav"), 4800);
    make_wav(d.file("take_009_nodate_p2.wav"), 4800);
    /* a repaired take */
    make_wav(d.file("take_008_nodate.wav"), 9600);
    CHECK_EQ(journal_write(ops, dir, journal_for(dir, "take_008_nodate.wav", "repaired")), 0);
    CHECK_EQ(journal_write(ops, dir, journal_for(dir, "take_006_20261108-1105.wav", "closed")), 0);
    /* a truncated header */
    {
        std::vector<unsigned char> h = build_header(AudioFormat(), BwfInfo(), 0);
        write_bytes(d.file("take_007_nodate.wav"), h.data(), 30);
    }
    /* stray files: temp, hidden, unrelated, a peak cache, a foreign wav */
    write_text(d.file("take_011_nodate.wav.tmp"), "x");
    write_text(d.file("take_012_nodate.tmp.wav"), "x");
    write_text(d.file(".hidden.wav"), "x");
    write_text(d.file("notes.txt"), "x");
    write_text(d.file("take_006_20261108-1105.peaks"), "peaks");
    make_wav(d.file("interview.wav"), 480);
    std::filesystem::create_directory(d.file("subdir"));

    CHECK_EQ(list_takes(ops, dir, list), 0);
    CHECK_EQ(list.size(), static_cast<size_t>(8));
    std::vector<std::string> order;
    for (const TakeInfo &t : list) order.push_back(t.label);
    const std::vector<std::string> want = {"010", "009", "009 p2", "008", "007", "006", "006-1", "interview"};
    CHECK(order == want);
    for (size_t k = 0; k < order.size() && k < want.size(); k++) CHECK_EQ(order[k], want[k]);

    const TakeInfo *a = find(list, "take_006_20261108-1105.wav");
    CHECK(a != nullptr);
    if (a) {
        CHECK(!a->damaged);
        CHECK(!a->repaired);
        CHECK(!a->journal.empty());
        CHECK_EQ(a->frames, static_cast<uint64_t>(48000));
        CHECK(std::fabs(a->seconds - 1.0) < 1e-9);
        CHECK_EQ(a->size, static_cast<uint64_t>(kDataOffset + 96000));
        CHECK(a->has_bext);
        CHECK_EQ(a->origination_date, std::string("2026-11-08"));
        CHECK_EQ(a->origination_time, std::string("11:05:00"));
        CHECK_EQ(a->time_reference, info.time_reference);
        CHECK(a->named);
        CHECK_EQ(a->tn.take, static_cast<uint64_t>(6));
    }
    const TakeInfo *s = find(list, "take_010_nodate.wav");
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->fmt.channels, static_cast<uint16_t>(2));
        CHECK_EQ(s->fmt.bits, static_cast<uint16_t>(24));
        CHECK_EQ(s->frames, static_cast<uint64_t>(88200));
        CHECK(std::fabs(s->seconds - 2.0) < 1e-9);
        CHECK(s->origination_date.empty());
    }
    const TakeInfo *r = find(list, "take_008_nodate.wav");
    CHECK(r && r->repaired && !r->damaged);
    const TakeInfo *bad = find(list, "take_007_nodate.wav");
    CHECK(bad && bad->damaged && !bad->error.empty());
    if (bad) CHECK_EQ(bad->frames, static_cast<uint64_t>(0));
    const TakeInfo *f = find(list, "interview.wav");
    CHECK(f && !f->named && !f->damaged);

    /* a stable order: listing again gives the same order */
    std::vector<TakeInfo> again;
    CHECK_EQ(list_takes(ops, dir, again), 0);
    CHECK_EQ(again.size(), list.size());
    bool same = again.size() == list.size();
    for (size_t k = 0; same && k < list.size(); k++) same = again[k].name == list[k].name;
    CHECK(same);

    /* free space and hours left: 345.6 MB per hour at 48 kHz mono S16 */
    CHECK_EQ(bytes_per_hour(AudioFormat()), static_cast<uint64_t>(345600000));
    CHECK_EQ(bytes_per_hour(st), static_cast<uint64_t>(44100ull * 2 * 3 * 3600));
    CHECK(std::fabs(hours_left(3456000000ull, AudioFormat()) - 10.0) < 1e-9);
    CHECK(std::fabs(hours_left(172800000ull, AudioFormat()) - 0.5) < 1e-9);
    AudioFormat none;
    none.rate = 0;
    CHECK_EQ(hours_left(1000, none), 0.0);
    ops.free_fn = [] { return static_cast<uint64_t>(6912000000ull); };
    SpaceInfo sp;
    CHECK_EQ(folder_space(ops, dir, AudioFormat(), sp), 0);
    CHECK_EQ(sp.free_bytes, static_cast<uint64_t>(6912000000ull));
    CHECK(std::fabs(sp.hours_left - 20.0) < 1e-9);
    ops.free_fn = nullptr;

    /* rename: only the prefix changes; invalid prefix; collision */
    std::string nn;
    CHECK_EQ(rename_take(ops, dir, "take_006_20261108-1105.wav", "../x", nn), -EINVAL);
    CHECK_EQ(rename_take(ops, dir, "take_006_20261108-1105.wav", "a/b", nn), -EINVAL);
    CHECK_EQ(rename_take(ops, dir, "take_006_20261108-1105.wav", "", nn), -EINVAL);
    CHECK_EQ(rename_take(ops, dir, "interview.wav", "talk", nn), -EINVAL);
    make_wav(d.file("band_006_20261108-1105.wav"), 10); /* the target exists */
    CHECK_EQ(rename_take(ops, dir, "take_006_20261108-1105.wav", "band", nn), -EEXIST);
    CHECK(exists(d.file("take_006_20261108-1105.wav")));
    CHECK(exists(d.file("band_006_20261108-1105.wav")));
    ops.unlink(d.file("band_006_20261108-1105.wav"));
    CHECK_EQ(rename_take(ops, dir, "take_006_20261108-1105.wav", "band", nn), 0);
    CHECK_EQ(nn, std::string("band_006_20261108-1105.wav"));
    CHECK(!exists(d.file("take_006_20261108-1105.wav")));
    CHECK(exists(d.file("band_006_20261108-1105.wav")));
    CHECK(exists(d.file("band_006_20261108-1105.peaks")));
    {
        const std::string jf = find_journal_for(ops, dir, nn);
        CHECK(!jf.empty());
        JournalEntry e;
        CHECK_EQ(journal_read(ops, jf, e), 0);
        CHECK_EQ(e.path, dir + "/band_006_20261108-1105.wav");
        CHECK_EQ(e.parts.size(), static_cast<size_t>(1));
        CHECK_EQ(e.parts[0], dir + "/band_006_20261108-1105.wav");
    }
    CHECK_EQ(rename_take(ops, dir, "take_006_20261108-1105-1.wav", "band", nn), 0);
    CHECK_EQ(nn, std::string("band_006_20261108-1105-1.wav")); /* the edit suffix stays */
    CHECK_EQ(rename_take(ops, dir, "take_009_nodate_p2.wav", "x", nn), 0);
    CHECK_EQ(nn, std::string("x_009_nodate_p2.wav"));
    CHECK_EQ(rename_take(ops, dir, "gone_099_nodate.wav", "x", nn), -ENOENT);

    /* refusal causes: the answer stays -EINVAL, `why` says which */
    {
        RenameRefusal why = RenameRefusal::BadChar;
        const std::string tk = "take_006_20261108-1105.wav";
        CHECK_EQ(rename_take(ops, dir, tk, "", nn, &why), -EINVAL);
        CHECK(why == RenameRefusal::Empty);
        CHECK_EQ(rename_take(ops, dir, tk, std::string(25, 'a'), nn, &why), -EINVAL);
        CHECK(why == RenameRefusal::TooLong);
        CHECK_EQ(rename_take(ops, dir, tk, "a/b", nn, &why), -EINVAL);
        CHECK(why == RenameRefusal::BadChar);
        CHECK_EQ(rename_take(ops, dir, tk, " lead", nn, &why), -EINVAL);
        CHECK(why == RenameRefusal::BadChar);
        CHECK_EQ(rename_take(ops, dir, "interview.wav", "talk", nn, &why), -EINVAL);
        CHECK(why == RenameRefusal::NotTake);
        CHECK_EQ(rename_take(ops, dir, "interview.wav", "", nn, &why), -EINVAL);
        CHECK(why == RenameRefusal::NotTake); /* the file decides first */
        CHECK_EQ(rename_take(ops, dir, "gone_099_nodate.wav", "x", nn, &why), -ENOENT);
        CHECK(why == RenameRefusal::None);
    }

    /* delete: the file, its journal and its peak cache */
    CHECK_EQ(delete_take(ops, dir, "band_006_20261108-1105.wav"), 0);
    CHECK(!exists(d.file("band_006_20261108-1105.wav")));
    CHECK(!exists(d.file("band_006_20261108-1105.peaks")));
    CHECK(find_journal_for(ops, dir, "band_006_20261108-1105.wav").empty());
    CHECK(!exists(journal_file(dir, "take_006_20261108-1105")));
    CHECK_EQ(delete_take(ops, dir, "take_008_nodate.wav"), 0);
    CHECK(!exists(journal_file(dir, "take_008_nodate")));
    CHECK_EQ(delete_take(ops, dir, "take_008_nodate.wav"), -ENOENT);
    CHECK_EQ(delete_take(ops, dir, "../etc.txt"), -EINVAL);
    CHECK(exists(d.file("notes.txt")));

    /* delete one part of a two-part take: the journal keeps the other part */
    {
        JournalEntry e = journal_for(dir, "take_009_nodate.wav", "closed");
        e.parts.push_back(dir + "/x_009_nodate_p2.wav");
        CHECK_EQ(journal_write(ops, dir, e), 0);
        CHECK_EQ(delete_take(ops, dir, "take_009_nodate.wav"), 0);
        JournalEntry k;
        CHECK_EQ(journal_read(ops, journal_file(dir, "take_009_nodate"), k), 0);
        CHECK_EQ(k.parts.size(), static_cast<size_t>(1));
        CHECK_EQ(k.path, dir + "/x_009_nodate_p2.wav");
    }

    /* a deleted take never comes back as a number */
    uint64_t t = 0;
    CHECK_EQ(reserve_take_number(ops, dir, t), 0);
    CHECK_EQ(t, static_cast<uint64_t>(11));

    /* takes at a card's own rate: the length in the list is frames / rate, the hours left count the real byte rate */
    {
        TempDir r;
        AudioFormat f16;
        f16.rate = 16000;
        make_wav(r.file("take_001_nodate.wav"), 16000 * 90, f16);   /* 90 s */
        AudioFormat f22;
        f22.rate = 22050;
        f22.channels = 2;
        make_wav(r.file("take_002_nodate.wav"), 22050 * 3, f22);    /* 3 s stereo */
        std::vector<TakeInfo> v;
        CHECK_EQ(list_takes(ops, r.path, v), 0);
        CHECK_EQ(v.size(), static_cast<size_t>(2));
        const TakeInfo *a16 = find(v, "take_001_nodate.wav");
        CHECK(a16 != nullptr);
        if (a16) {
            CHECK_EQ(a16->fmt.rate, 16000u);
            CHECK_EQ(a16->frames, static_cast<uint64_t>(16000 * 90));
            CHECK(std::fabs(a16->seconds - 90.0) < 1e-9);
        }
        const TakeInfo *a22 = find(v, "take_002_nodate.wav");
        CHECK(a22 != nullptr);
        if (a22) CHECK(std::fabs(a22->seconds - 3.0) < 1e-9);
        CHECK_EQ(bytes_per_hour(f16), static_cast<uint64_t>(115200000)); /* 32,000 bytes a second */
        CHECK(std::fabs(hours_left(1152000000ull, f16) - 10.0) < 1e-9);
    }
}
