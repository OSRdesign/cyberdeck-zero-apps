/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes tests: FLAC and Opus take names next to WAV (phase B). The grammar with .flac / .opus, the extension
 * kept by edits and renames, a folder that mixes the three formats (the list, the order, the format of each take, its
 * length from the stream headers), take numbers shared by all formats (never reused), delete with the peak cache and the
 * journal. The FLAC and Opus files are real streams from the test encoders.
 */
#include "check.hpp"

#include "../fixture_codecs.hpp"
#include "journal.hpp"
#include "naming.hpp"
#include "recordings.hpp"

#include <cerrno>
#include <cmath>
#include <fstream>

using namespace cassette;
using namespace tk;

namespace {

void write_bytes(const std::string &p, const std::vector<unsigned char> &b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::vector<unsigned char> flac_file(const AudioFormat &f, uint64_t frames, bool finish) {
    MemoryByteSink s;
    fx::VerbatimFlac e(f, 4096);
    e.start(s, {{"TITLE", "t"}});
    const std::vector<unsigned char> pcm = fx::pcm(f, 0, frames);
    e.write(pcm.data(), frames);
    if (finish) e.finish();
    return s.data;
}

std::vector<unsigned char> opus_file(const AudioFormat &f, uint64_t frames) {
    MemoryByteSink s;
    fx::PlainOggOpus e(f, 100, 50);
    e.start(s, {});
    const std::vector<unsigned char> pcm = fx::pcm(f, 0, frames);
    e.write(pcm.data(), frames);
    e.finish();
    return s.data;
}

std::vector<unsigned char> wav_file(const AudioFormat &f, uint64_t frames) {
    std::vector<unsigned char> b = build_header(f, BwfInfo(), frames * f.channels * (f.bits / 8u));
    const std::vector<unsigned char> pcm = fx::pcm(f, 0, frames);
    b.insert(b.end(), pcm.begin(), pcm.end());
    return b;
}

const TakeInfo *find(const std::vector<TakeInfo> &v, const std::string &name) {
    for (const TakeInfo &t : v)
        if (t.name == name) return &t;
    return nullptr;
}

void names() {
    TakeName t;
    CHECK(parse_take_name("take_007_20261110-0915.flac", t));
    CHECK(t.format == RecFormat::Flac);
    CHECK_EQ(t.take, uint64_t(7));
    CHECK_EQ(format_take_name(t), "take_007_20261110-0915.flac");
    CHECK(parse_take_name("field rec_012_nodate_p3-2.opus", t));
    CHECK(t.format == RecFormat::Opus);
    CHECK_EQ(t.prefix, "field rec");
    CHECK_EQ(t.part, 3u);
    CHECK_EQ(t.edit, 2u);
    CHECK_EQ(format_take_name(t), "field rec_012_nodate_p3-2.opus");
    CHECK_EQ(take_label(t), "012 p3-2");
    CHECK(parse_take_name("take_001_nodate.wav", t) && t.format == RecFormat::Wav);
    TakeName d; /* the default stays WAV for every older caller */
    d.prefix = "take";
    d.take = 3;
    CHECK_EQ(format_take_name(d), "take_003_nodate.wav");
    CHECK(!parse_take_name("take_001_nodate.FLAC", t)); /* the grammar is lower case, as for .wav */
    CHECK(!parse_take_name("take_001_nodate.ogg", t));
    CHECK(!parse_take_name("take_001_nodate.flac.tmp", t));
    CHECK_EQ(file_stem("take_001_nodate.flac"), "take_001_nodate");
    CHECK_EQ(file_stem("song.OPUS"), "song");
    CHECK_EQ(file_stem("x.WAV"), "x");
    CHECK_EQ(file_stem("notes.txt"), "notes.txt");
    CHECK_EQ(std::string(take_extension(RecFormat::Opus)), ".opus");
    CHECK_EQ(std::string(format_tag(RecFormat::Flac)), "FLAC");
    CHECK_EQ(std::string(format_tag(RecFormat::Wav)), "WAV");
    RecFormat f = RecFormat::Wav;
    CHECK(take_format_of("A.FLAC", f) && f == RecFormat::Flac);
    CHECK(!take_format_of("a.mp3", f));
    CHECK(is_take_file_name("take_001_nodate.flac"));
    CHECK(is_take_file_name("Song.OPUS"));
    CHECK(!is_take_file_name(".flac"));
    CHECK(!is_take_file_name(".hidden.opus"));
    CHECK(!is_take_file_name("x.flac.tmp"));
    CHECK(!is_take_file_name("x.tmp.opus"));
    /* edits keep the format; the record key is the take's, whatever the extension */
    std::vector<std::string> n = next_edit_names("take_009_nodate.flac", 2, {"take_009_nodate-1.flac"});
    CHECK_EQ(n.size(), size_t(2));
    if (n.size() == 2) {
        CHECK_EQ(n[0], "take_009_nodate-2.flac");
        CHECK_EQ(n[1], "take_009_nodate-3.flac");
    }
    n = next_edit_names("song.opus", 1, {"song-1.opus"});
    CHECK(n.size() == 1 && n[0] == "song-2.opus");
    n = next_edit_names("song.WAV", 1, {});
    CHECK(n.size() == 1 && n[0] == "song-1.wav"); /* WAV as before */
    CHECK_EQ(edit_record_key("take_009_nodate_p2.opus"), "009_p2");
    CHECK_EQ(peaks_name("take_009_nodate.flac"), "take_009_nodate.peaks");
    /* (the phase B playback gate is gone: phase C plays, previews and trims FLAC and Opus takes) */
}

/* A folder with WAV, FLAC and Opus takes: one list, one numbering. */
void mixed_folder() {
    TempDir d;
    FileOps &ops = posix_file_ops();
    const AudioFormat mono;
    const AudioFormat stereo{44100, 2, 16};
    write_bytes(d.file("take_001_nodate.wav"), wav_file(mono, 48000));
    write_bytes(d.file("take_002_nodate.flac"), flac_file(stereo, 44100 * 2, true));
    write_bytes(d.file("take_003_20261110-0915.opus"), opus_file(AudioFormat{16000, 1, 16}, 16000 * 3));
    write_bytes(d.file("take_004_nodate.flac"), flac_file(mono, 9000, false)); /* being recorded: total unknown */
    write_bytes(d.file("notes.txt"), {'x'});
    write_bytes(d.file("broken.flac"), {'n', 'o', 'p', 'e', 0, 0, 0, 0});
    /* take 005 exists only in a journal (its file was deleted): its number is still taken */
    JournalEntry je;
    je.take = "take_005_nodate";
    je.path = d.file("take_005_nodate.opus");
    je.parts = {je.path};
    je.state = "closed";
    je.codec = "opus";
    CHECK_EQ(journal_write(ops, d.path, je), 0);
    JournalEntry jr;
    jr.take = "take_002_nodate";
    jr.path = d.file("take_002_nodate.flac");
    jr.parts = {jr.path};
    jr.state = "repaired";
    jr.codec = "flac";
    CHECK_EQ(journal_write(ops, d.path, jr), 0);

    std::vector<TakeInfo> v;
    CHECK_EQ(list_takes(ops, d.path, v), 0);
    CHECK_EQ(v.size(), size_t(5)); /* the four takes and the broken FLAC; not notes.txt */
    if (v.size() == 5) {
        CHECK_EQ(v[0].name, "take_004_nodate.flac"); /* newest first, whatever the format */
        CHECK_EQ(v[1].name, "take_003_20261110-0915.opus");
        CHECK_EQ(v[2].name, "take_002_nodate.flac");
        CHECK_EQ(v[3].name, "take_001_nodate.wav");
        CHECK_EQ(v[4].name, "broken.flac");
    }
    const TakeInfo *w = find(v, "take_001_nodate.wav");
    CHECK(w && w->format == RecFormat::Wav && !w->damaged && w->frames == 48000);
    const TakeInfo *fl = find(v, "take_002_nodate.flac");
    CHECK(fl && fl->format == RecFormat::Flac && !fl->damaged);
    if (fl) {
        CHECK_EQ(fl->frames, uint64_t(88200));
        CHECK(std::fabs(fl->seconds - 2.0) < 1e-9);
        CHECK_EQ(fl->fmt.channels, uint16_t(2));
        CHECK_EQ(fl->fmt.rate, 44100u);
        CHECK(fl->repaired); /* the journal's mark, as for WAV */
        CHECK_EQ(fl->label, "002");
        CHECK(!fl->has_bext);
    }
    const TakeInfo *op = find(v, "take_003_20261110-0915.opus");
    CHECK(op && op->format == RecFormat::Opus && !op->damaged);
    if (op) {
        CHECK_EQ(op->frames, uint64_t(48000));
        CHECK_EQ(op->fmt.rate, 16000u);
        CHECK(std::fabs(op->seconds - 3.0) < 1e-9);
        CHECK(!op->repaired);
    }
    const TakeInfo *rec = find(v, "take_004_nodate.flac");
    CHECK(rec && !rec->damaged && rec->frames == 0); /* STREAMINFO says "unknown" until the take is closed */
    const TakeInfo *bad = find(v, "broken.flac");
    CHECK(bad && bad->damaged && !bad->named && bad->label == "broken");

    /* numbers: every format counts, a journal too */
    CHECK_EQ(highest_take_found(ops, d.path), uint64_t(5));
    CHECK_EQ(first_free_take_number(ops, d.path, "take", 2), uint64_t(6));
    CHECK_EQ(peek_take_number(ops, d.path, "take"), uint64_t(6));
    uint64_t got = 0;
    CHECK_EQ(reserve_take_number(ops, d.path, got, "take"), 0);
    CHECK_EQ(got, uint64_t(6));
    uint64_t landed = 0;
    CHECK_EQ(set_next_take_number(ops, d.path, "take", 2, landed), 0);
    CHECK_EQ(landed, uint64_t(6)); /* 2..5 are taken by WAV, FLAC, Opus and the journal */
    CHECK_EQ(first_free_take_number(ops, d.path, "other", 1), uint64_t(1));

    /* rename keeps the extension and follows the journal */
    std::string nn;
    RenameRefusal why = RenameRefusal::None;
    CHECK_EQ(rename_take(ops, d.path, "take_002_nodate.flac", "field rec", nn, &why), 0);
    CHECK_EQ(nn, "field rec_002_nodate.flac");
    CHECK(why == RenameRefusal::None);
    CHECK(exists(d.file("field rec_002_nodate.flac")) && !exists(d.file("take_002_nodate.flac")));
    const std::string jf = find_journal_for(ops, d.path, "field rec_002_nodate.flac");
    CHECK(!jf.empty());
    CHECK_EQ(rename_take(ops, d.path, "take_003_20261110-0915.opus", "bad/name", nn, &why), -EINVAL);
    CHECK(why == RenameRefusal::BadChar);
    CHECK_EQ(rename_take(ops, d.path, "broken.flac", "x", nn, &why), -EINVAL);
    CHECK(why == RenameRefusal::NotTake);
    write_bytes(d.file("x_003_20261110-0915.opus"), {1});
    CHECK_EQ(rename_take(ops, d.path, "take_003_20261110-0915.opus", "x", nn, &why), -EEXIST);
    std::filesystem::remove(d.file("x_003_20261110-0915.opus"));

    /* delete: the file, its peak cache, its journal */
    write_bytes(d.file("take_003_20261110-0915.peaks"), {1, 2, 3});
    JournalEntry j3;
    j3.take = "take_003_20261110-0915";
    j3.path = d.file("take_003_20261110-0915.opus");
    j3.parts = {j3.path};
    j3.state = "closed";
    CHECK_EQ(journal_write(ops, d.path, j3), 0);
    CHECK_EQ(delete_take(ops, d.path, "take_003_20261110-0915.opus"), 0);
    CHECK(!exists(d.file("take_003_20261110-0915.opus")));
    CHECK(!exists(d.file("take_003_20261110-0915.peaks")));
    CHECK(!exists(journal_file(d.path, "take_003_20261110-0915")));
    CHECK_EQ(delete_take(ops, d.path, "notes.txt"), -EINVAL);
    /* the deleted Opus take's number is not handed out again: the counter is past it */
    CHECK_EQ(peek_take_number(ops, d.path, "take"), uint64_t(6));
}

}  // namespace

void test_formats_takes() {
    names();
    mixed_folder();
}
