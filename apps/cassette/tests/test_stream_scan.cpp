/*
 * SPDX-License-Identifier: MIT
 *
 * FLAC and Ogg Opus take files without a decoder (codec/stream_scan): the checksums against their published check
 * values, the quick info for the list, the frame and page scanners, and the repair of a cut take at EVERY byte offset
 * of a small file (recording state and finished state), mid-file damage that must never be cut, and the rule that a
 * file without a journal is only touched when it looks unfinished. The streams come from the test encoders
 * (fixture_codecs.hpp), so each expected value is known exactly.
 */
#include "check.hpp"
#include "fixture_codecs.hpp"
#include "helpers.hpp"
#include "journal.hpp"
#include "repair.hpp"

#include <cstring>

using namespace cassette;
using namespace tst;

namespace {

/* A memory sink that remembers where each appended write ended (the encoders write one frame / page per call). */
class RecSink final : public ByteSink {
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

using fx::samples_of;

std::vector<unsigned char> pcm_bytes(const AudioFormat &f, uint64_t first, uint64_t frames) { return fx::pcm(f, first, frames); }

std::vector<unsigned char> prefix(const std::vector<unsigned char> &b, size_t n) {
    return std::vector<unsigned char>(b.begin(), b.begin() + static_cast<long>(n));
}

uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void checksums() {
    const unsigned char t[] = "123456789";
    CHECK_EQ(static_cast<unsigned>(flac_crc8(t, 9)), 0xF4u);    /* CRC-8 poly 0x07 (CRC-8/SMBUS check value) */
    CHECK_EQ(static_cast<unsigned>(flac_crc16(t, 9)), 0xFEE8u); /* CRC-16 poly 0x8005, init 0 (CRC-16/UMTS) */
    CHECK_EQ(ogg_crc32(t, 9), 0x89A1897Fu);                     /* CRC-32/CKSUM without the final inversion */
    CHECK_EQ(flac_crc16(t + 4, 5, flac_crc16(t, 4)), flac_crc16(t, 9)); /* it chains */
}

void containers() {
    TempDir d;
    write_file(d.file("a.flac"), {'f', 'L', 'a', 'C', 0});
    write_file(d.file("b.wav"), {'O', 'g', 'g', 'S', 0});
    write_file(d.file("c.opus"), {1, 2});
    CHECK(container_of(posix_file_ops(), d.file("a.flac")) == Container::Flac);
    CHECK(container_of(posix_file_ops(), d.file("b.wav")) == Container::OggOpus); /* the magic wins */
    CHECK(container_of(posix_file_ops(), d.file("c.opus")) == Container::OggOpus); /* too short: the extension */
    CHECK(container_of(posix_file_ops(), d.file("none.FLAC")) == Container::Flac);
    CHECK(container_of_name("x/take_001_nodate.wav") == Container::Wav);
    CHECK(container_of_name("x.ogg") == Container::Unknown);
    CHECK(container_of_name("dir.flac/x") == Container::Unknown);
}

/* Builds a FLAC stream: n frames fed in chunks; finish or leave it as a recording would after a sync. */
struct Built {
    std::vector<unsigned char> bytes;
    std::vector<uint64_t> ends; /* ends[0]: the end of the header; then the end of each frame / page */
    std::vector<unsigned char> pcm;
};

Built build_flac(const AudioFormat &f, uint64_t frames, uint32_t block, bool finish, uint32_t padding = 0) {
    Built b;
    RecSink s;
    fx::VerbatimFlac e(f, block, padding);
    CHECK_EQ(e.start(s, {{"TITLE", "sweep"}}), 0);
    b.pcm = pcm_bytes(f, 0, frames);
    const size_t fb = f.channels * (f.bits / 8u);
    for (uint64_t at = 0; at < frames; at += 50) CHECK_EQ(e.write(b.pcm.data() + at * fb, std::min<uint64_t>(50, frames - at)), 0);
    CHECK_EQ(e.flush(), 0);
    if (finish) CHECK_EQ(e.finish(), 0);
    b.bytes = s.m.data;
    b.ends = s.ends;
    return b;
}

Built build_ogg(const AudioFormat &f, uint64_t frames, bool finish, unsigned packet = 20, unsigned per_page = 5) {
    Built b;
    RecSink s;
    fx::PlainOggOpus e(f, packet, per_page);
    CHECK_EQ(e.start(s, {{"TITLE", "sweep"}}), 0);
    b.pcm = pcm_bytes(f, 0, frames);
    const size_t fb = f.channels * (f.bits / 8u);
    for (uint64_t at = 0; at < frames; at += 333) CHECK_EQ(e.write(b.pcm.data() + at * fb, std::min<uint64_t>(333, frames - at)), 0);
    CHECK_EQ(e.flush(), 0);
    if (finish) CHECK_EQ(e.finish(), 0);
    b.bytes = s.m.data;
    /* the header is two pages: ends[0] becomes the end of OpusTags */
    b.ends.assign(s.ends.begin() + 1, s.ends.end());
    return b;
}

void flac_info_and_scan() {
    TempDir d;
    const AudioFormat f{44100, 2, 24};
    Built b = build_flac(f, 10000, 1152, true, 512);
    write_file(d.file("s.flac"), b.bytes);
    StreamInfo i = read_stream_info(posix_file_ops(), d.file("s.flac"));
    CHECK(i.ok && i.kind == Container::Flac);
    CHECK(i.frames_known);
    CHECK_EQ(i.frames, uint64_t(10000));
    CHECK_EQ(i.fmt.rate, 44100u);
    CHECK_EQ(i.fmt.channels, uint16_t(2));
    CHECK_EQ(i.fmt.bits, uint16_t(24));
    StreamScan s = scan_flac(posix_file_ops(), d.file("s.flac"));
    CHECK(s.ok);
    CHECK_EQ(s.frames, uint64_t(10000));
    CHECK_EQ(s.units, uint64_t(9)); /* 8 x 1152 + 784 */
    CHECK_EQ(s.valid_end, s.file_size);
    CHECK_EQ(s.audio_offset, b.ends[0]);
    CHECK_EQ(s.damaged, uint64_t(0));
    CHECK_EQ(s.stored_total, uint64_t(10000));
    CHECK(fx::decode_verbatim_flac(b.bytes) == samples_of(f, b.pcm)); /* the fixture itself is right */
    StreamFix x = fix_flac(posix_file_ops(), d.file("s.flac"), true);
    CHECK(x.ok && !x.changed);
    CHECK(read_file(d.file("s.flac")) == b.bytes);

    /* a recording in progress: STREAMINFO says 0 = unknown */
    Built r = build_flac(AudioFormat{}, 9000, 4096, false);
    write_file(d.file("r.flac"), r.bytes);
    i = read_stream_info(posix_file_ops(), d.file("r.flac"));
    CHECK(i.ok && !i.frames_known && i.frames == 0);
    s = scan_flac(posix_file_ops(), d.file("r.flac"));
    CHECK(s.ok && s.frames == 8192 && s.stored_total == 0 && s.valid_end == s.file_size);

    /* not FLAC, cut in the header, empty */
    write_file(d.file("bad.flac"), {'f', 'L', 'a', 'X', 0, 0, 0, 0});
    CHECK(!scan_flac(posix_file_ops(), d.file("bad.flac")).ok);
    CHECK(!fix_flac(posix_file_ops(), d.file("bad.flac"), true).ok);
    write_file(d.file("cut.flac"), prefix(b.bytes, 30));
    CHECK(!read_stream_info(posix_file_ops(), d.file("cut.flac")).ok);
    write_file(d.file("zero.flac"), {});
    CHECK(!fix_flac(posix_file_ops(), d.file("zero.flac"), true).ok);
    CHECK(!fix_flac(posix_file_ops(), d.file("missing.flac"), true).ok);
}

/* Cut at every byte offset; finished: also with a set MD5 that the repair must clear when the length changes. */
void flac_cut_sweep(bool finished) {
    TempDir d;
    const AudioFormat f{8000, 1, 16};
    const uint32_t block = 64;
    const uint64_t n = 10 * block + 30;
    Built b = build_flac(f, n, block, finished);
    if (finished) b.bytes[8 + 20] = 0x5A; /* a "set" MD5 (byte 2 of it) */
    const std::string path = d.file("t.flac");
    const std::vector<int32_t> want = samples_of(f, b.pcm);
    int checked = 0, fails = 0;
    for (size_t len = 0; len <= b.bytes.size(); ++len) {
        write_file(path, prefix(b.bytes, len));
        const StreamFix x = fix_flac(posix_file_ops(), path, true);
        if (len < b.ends[0]) {
            if (x.ok) fails++;
            continue;
        }
        size_t k = 0; /* whole frames inside the prefix */
        while (k + 1 < b.ends.size() && b.ends[k + 1] <= len) ++k;
        const uint64_t end = b.ends[k];
        const uint64_t frames = std::min<uint64_t>(k * block, n);
        const bool ok = x.ok && x.frames == frames && x.truncated_bytes == len - end;
        const std::vector<unsigned char> after = read_file(path);
        const StreamScan s = scan_flac(posix_file_ops(), path);
        bool good = ok && after.size() == end && s.ok && s.stored_total == frames && s.frames == frames;
        /* only STREAMINFO's total and MD5 may differ from the original bytes */
        for (size_t i = 0; good && i < after.size(); ++i)
            if ((i < 8 + 13 || i >= 8 + 34) && after[i] != b.bytes[i]) good = false;
        if (good && finished && frames != n) good = !s.md5_set; /* the MD5 was for the whole take */
        if (good && finished && frames == n) good = s.md5_set && !x.changed;
        if (good) {
            const StreamFix again = fix_flac(posix_file_ops(), path, true);
            good = again.ok && !again.changed && read_file(path) == after;
        }
        if (good) {
            const std::vector<int32_t> got = fx::decode_verbatim_flac(after);
            good = got.size() == frames && std::equal(got.begin(), got.end(), want.begin());
        }
        if (good) {
            const StreamInfo i = read_stream_info(posix_file_ops(), path);
            good = i.ok && i.frames == frames;
        }
        if (!good) {
            fails++;
            if (fails < 4) std::fprintf(stderr, "flac sweep (finished %d): cut at %zu failed\n", finished ? 1 : 0, len);
        }
        checked++;
    }
    CHECK_EQ(fails, 0);
    CHECK(checked > 1000);
    std::printf("stream scan: FLAC cut at every byte offset (%s): %d cuts repaired and checked\n",
                finished ? "finished file" : "recording state", checked);
}

void ogg_info_and_scan() {
    TempDir d;
    const AudioFormat f{48000, 2, 16};
    Built b = build_ogg(f, 48000 * 3 + 500, true, 160, 50);
    write_file(d.file("s.opus"), b.bytes);
    StreamInfo i = read_stream_info(posix_file_ops(), d.file("s.opus"));
    CHECK(i.ok && i.kind == Container::OggOpus && i.frames_known);
    CHECK_EQ(i.frames, uint64_t(48000 * 3 + 500));
    CHECK_EQ(i.fmt.channels, uint16_t(2));
    CHECK_EQ(i.fmt.rate, 48000u);
    StreamScan s = scan_ogg_opus(posix_file_ops(), d.file("s.opus"));
    CHECK(s.ok && s.eos && !s.header_only);
    CHECK_EQ(s.frames, uint64_t(48000 * 3 + 500));
    CHECK_EQ(s.valid_end, s.file_size);
    CHECK_EQ(s.preskip, 312u);
    CHECK_EQ(s.audio_offset, b.ends[0]);
    StreamFix x = fix_ogg_opus(posix_file_ops(), d.file("s.opus"), true);
    CHECK(x.ok && !x.changed);

    /* a 16 kHz recording in progress: no end of stream, the duration from the last page */
    Built r = build_ogg(AudioFormat{16000, 1, 16}, 16000 * 2 + 77, false);
    write_file(d.file("r.opus"), r.bytes);
    i = read_stream_info(posix_file_ops(), d.file("r.opus"));
    CHECK(i.ok && i.frames_known);
    /* whole 20 ms packets of the coded stream (the pre-skip plus the input), minus the pre-skip */
    CHECK_EQ(i.frames, uint64_t(((312 + (16000 * 2 + 77) * 3) / 960 * 960 - 312) / 3));
    CHECK_EQ(i.fmt.rate, 16000u);
    s = scan_ogg_opus(posix_file_ops(), d.file("r.opus"));
    CHECK(s.ok && !s.eos && s.frames == i.frames);

    /* only the header pages */
    Built h = build_ogg(AudioFormat{}, 0, false);
    write_file(d.file("h.opus"), h.bytes);
    s = scan_ogg_opus(posix_file_ops(), d.file("h.opus"));
    CHECK(s.ok && s.header_only && s.frames == 0 && !s.eos);
    x = fix_ogg_opus(posix_file_ops(), d.file("h.opus"), true);
    CHECK(x.ok && !x.changed); /* nothing to end: no audio page */

    write_file(d.file("bad.opus"), {'O', 'g', 'g', 'S', 1, 2, 3});
    CHECK(!scan_ogg_opus(posix_file_ops(), d.file("bad.opus")).ok);
    write_file(d.file("cut.opus"), prefix(b.bytes, b.ends[0] - 3)); /* cut inside OpusTags */
    CHECK(!fix_ogg_opus(posix_file_ops(), d.file("cut.opus"), true).ok);
}

void ogg_cut_sweep(bool finished) {
    TempDir d;
    const AudioFormat f{8000, 1, 16};
    const uint64_t n = 8000 + 100;
    Built b = build_ogg(f, n, finished);
    const std::string path = d.file("t.opus");
    int checked = 0, fails = 0;
    for (size_t len = 0; len <= b.bytes.size(); ++len) {
        write_file(path, prefix(b.bytes, len));
        const StreamFix x = fix_ogg_opus(posix_file_ops(), path, true);
        if (len < b.ends[0]) {
            if (x.ok) fails++;
            continue;
        }
        size_t k = 0;
        while (k + 1 < b.ends.size() && b.ends[k + 1] <= len) ++k;
        const uint64_t end = b.ends[k];
        uint64_t frames = 0, last = 0;
        if (k > 0) {
            last = b.ends[k - 1];
            const uint64_t g = le64(b.bytes.data() + last + 6);
            frames = (g - 312) * 8000 / 48000;
        }
        const std::vector<unsigned char> after = read_file(path);
        const StreamScan s = scan_ogg_opus(posix_file_ops(), path);
        bool good = x.ok && x.frames == frames && x.truncated_bytes == len - end && after.size() == end && s.ok &&
                    s.frames == frames && s.valid_end == end && s.eos == (k > 0) && s.header_only == (k == 0);
        for (size_t i = 0; good && i < after.size(); ++i) /* only the last page's flags and CRC may change */
            if (!(k > 0 && i >= last && (i == last + 5 || (i >= last + 22 && i < last + 26))) && after[i] != b.bytes[i])
                good = false;
        if (good) {
            const StreamFix again = fix_ogg_opus(posix_file_ops(), path, true);
            good = again.ok && !again.changed && read_file(path) == after;
        }
        if (good) {
            const StreamInfo i = read_stream_info(posix_file_ops(), path);
            good = i.ok && i.frames == frames;
        }
        if (!good) {
            fails++;
            if (fails < 4) std::fprintf(stderr, "ogg sweep (finished %d): cut at %zu failed\n", finished ? 1 : 0, len);
        }
        checked++;
    }
    CHECK_EQ(fails, 0);
    CHECK(checked > 1000);
    std::printf("stream scan: Ogg Opus cut at every byte offset (%s): %d cuts repaired and checked\n",
                finished ? "finished file" : "recording state", checked);
}

/* A bad frame or page in the middle is skipped, never cut: the take keeps everything after it. */
void middle_damage() {
    TempDir d;
    const AudioFormat f{8000, 1, 16};
    Built b = build_flac(f, 10 * 64, 64, true);
    std::vector<unsigned char> bad = b.bytes;
    bad[b.ends[4] + 40] ^= 0x10; /* inside frame 5 */
    write_file(d.file("m.flac"), bad);
    StreamScan s = scan_flac(posix_file_ops(), d.file("m.flac"));
    CHECK(s.ok);
    CHECK_EQ(s.units, uint64_t(9));
    CHECK_EQ(s.damaged, uint64_t(1));
    CHECK_EQ(s.valid_end, s.file_size);
    CHECK_EQ(s.frames, uint64_t(640)); /* from the last frame's number */
    StreamFix x = fix_flac(posix_file_ops(), d.file("m.flac"), true);
    CHECK(x.ok && !x.changed);
    CHECK(read_file(d.file("m.flac")) == bad);
    /* the same in a recording (total 0): the total is set, nothing is cut */
    Built r = build_flac(f, 10 * 64, 64, false);
    std::vector<unsigned char> rb = r.bytes;
    rb[r.ends[2] + 3] ^= 0xFF; /* the header of frame 3 */
    write_file(d.file("n.flac"), rb);
    x = fix_flac(posix_file_ops(), d.file("n.flac"), true);
    CHECK(x.ok && x.changed && x.frames == 640 && x.truncated_bytes == 0);
    CHECK_EQ(read_file(d.file("n.flac")).size(), rb.size());

    Built o = build_ogg(f, 8000, true);
    std::vector<unsigned char> ob = o.bytes;
    ob[o.ends[3] + 30] ^= 0x01; /* a byte of page 5 */
    write_file(d.file("m.opus"), ob);
    s = scan_ogg_opus(posix_file_ops(), d.file("m.opus"));
    CHECK(s.ok && s.eos);
    CHECK_EQ(s.damaged, uint64_t(1));
    CHECK_EQ(s.valid_end, s.file_size);
    CHECK_EQ(s.frames, uint64_t(8000));
    x = fix_ogg_opus(posix_file_ops(), d.file("m.opus"), true);
    CHECK(x.ok && !x.changed);
}

/* A power cut can leave the file longer than what reached the disk, the rest zeros: every whole frame / page stays. */
void zero_tail() {
    TempDir d;
    const AudioFormat f{8000, 1, 16};
    Built b = build_flac(f, 10 * 64, 64, false);
    std::vector<unsigned char> z = b.bytes;
    z.insert(z.end(), 3000, 0);
    write_file(d.file("z.flac"), z);
    StreamFix x = fix_flac(posix_file_ops(), d.file("z.flac"), true);
    CHECK(x.ok && x.changed);
    CHECK_EQ(x.frames, uint64_t(640));
    CHECK_EQ(x.truncated_bytes, uint64_t(3000));
    CHECK_EQ(read_file(d.file("z.flac")).size(), b.bytes.size());
    /* a cut frame and then zeros */
    z = prefix(b.bytes, b.bytes.size() - 20);
    z.insert(z.end(), 500, 0);
    write_file(d.file("y.flac"), z);
    x = fix_flac(posix_file_ops(), d.file("y.flac"), true);
    CHECK(x.ok && x.frames == 576 && x.truncated_bytes == b.bytes.size() - 20 + 500 - b.ends[9]);

    /* a last frame whose CRC-16 ends in 0x00: then the CRC also "matches" one byte early (a property of this CRC), and
     * in a zero tail at every later byte; the frame's exact length (its subframes) decides */
    int found = 0;
    for (uint64_t off = 0; off < 20000 && found < 3; ++off) {
        RecSink rs;
        fx::VerbatimFlac e(f, 64, 0);
        CHECK_EQ(e.start(rs, {}), 0);
        const std::vector<unsigned char> pcm = fx::pcm(f, off, 64);
        CHECK_EQ(e.write(pcm.data(), 64), 0);
        if (rs.m.data.back() != 0) continue;
        ++found;
        for (int garbage = 0; garbage < 2; ++garbage) {
            z = rs.m.data;
            for (int i = 0; i < 100; ++i) z.push_back(garbage ? static_cast<unsigned char>(0x5A + i) : 0);
            write_file(d.file("c0.flac"), z);
            x = fix_flac(posix_file_ops(), d.file("c0.flac"), true);
            CHECK(x.ok && x.frames == 64 && x.truncated_bytes == 100);
            CHECK_EQ(read_file(d.file("c0.flac")).size(), rs.m.data.size());
        }
    }
    CHECK(found >= 1);

    Built o = build_ogg(f, 8000, false);
    z = o.bytes;
    z.insert(z.end(), 2000, 0);
    write_file(d.file("z.opus"), z);
    x = fix_ogg_opus(posix_file_ops(), d.file("z.opus"), true);
    CHECK(x.ok && x.changed && x.frames == (48000 - 312) / 6 && x.truncated_bytes == 2000);
    CHECK(scan_ogg_opus(posix_file_ops(), d.file("z.opus")).eos);
}

/* Without a journal (force false): only unfinished files; a finished one with a foreign tail (an ID3v1 tag) stays. */
void no_journal_rules() {
    TempDir d;
    const AudioFormat f{8000, 1, 16};
    std::vector<unsigned char> tail(128, 0x20);
    tail[0] = 'T', tail[1] = 'A', tail[2] = 'G';
    Built b = build_flac(f, 500, 64, true);
    std::vector<unsigned char> tagged = b.bytes;
    tagged.insert(tagged.end(), tail.begin(), tail.end());
    write_file(d.file("song.flac"), tagged);
    StreamFix x = fix_flac(posix_file_ops(), d.file("song.flac"), false);
    CHECK(x.ok && !x.changed);
    CHECK(read_file(d.file("song.flac")) == tagged);
    Built r = build_flac(f, 500, 64, false);
    write_file(d.file("rec.flac"), prefix(r.bytes, r.bytes.size() - 7));
    x = fix_flac(posix_file_ops(), d.file("rec.flac"), false);
    CHECK(x.ok && x.changed && x.frames == 6 * 64);

    Built o = build_ogg(f, 4000, true);
    std::vector<unsigned char> otag = o.bytes;
    otag.insert(otag.end(), tail.begin(), tail.end());
    write_file(d.file("song.opus"), otag);
    x = fix_ogg_opus(posix_file_ops(), d.file("song.opus"), false);
    CHECK(x.ok && !x.changed);
    CHECK(read_file(d.file("song.opus")) == otag);
    Built q = build_ogg(f, 4000, false);
    write_file(d.file("rec.opus"), q.bytes);
    x = fix_ogg_opus(posix_file_ops(), d.file("rec.opus"), false);
    CHECK(x.ok && x.changed && x.truncated_bytes == 0);
    CHECK(scan_ogg_opus(posix_file_ops(), d.file("rec.opus")).eos);

    /* repair_all on a folder without journals: the unfinished takes are mended and reported, the rest left alone */
    write_file(d.file("take_003_nodate.flac"), prefix(r.bytes, r.bytes.size() - 7));
    write_file(d.file("take_004_nodate.opus"), q.bytes);
    RepairReport rep = repair_all(posix_file_ops(), d.path);
    CHECK(rep.errors.empty());
    size_t flac = 0, opus = 0;
    for (const RepairedTake &t : rep.repaired) {
        flac += t.take == "take_003_nodate";
        opus += t.take == "take_004_nodate";
        CHECK(!t.had_journal);
    }
    CHECK_EQ(flac, size_t(1));
    CHECK_EQ(opus, size_t(1));
    CHECK(read_file(d.file("song.flac")) == tagged);
    CHECK(read_file(d.file("song.opus")) == otag);
    rep = repair_all(posix_file_ops(), d.path);
    CHECK(rep.repaired.empty() && rep.errors.empty()); /* idempotent */
}

/* Journal-driven repair: an open journal names a cut FLAC part and a cut Opus part. */
void journal_driven() {
    TempDir d;
    const AudioFormat f{8000, 1, 16};
    Built b = build_flac(f, 700, 64, false);
    write_file(d.file("take_005_nodate.flac"), prefix(b.bytes, b.bytes.size() - 11));
    JournalEntry je;
    je.take = "take_005_nodate";
    je.path = d.file("take_005_nodate.flac");
    je.parts = {je.path};
    je.fmt = f;
    je.codec = "flac";
    je.state = "open";
    je.started_wall = 1700000000;
    je.last_sync_wall = 1700000001;
    CHECK_EQ(journal_write(posix_file_ops(), d.path, je), 0);
    RepairReport rep = repair_all(posix_file_ops(), d.path);
    CHECK(rep.errors.empty());
    CHECK_EQ(rep.repaired.size(), size_t(1));
    if (!rep.repaired.empty()) {
        CHECK(rep.repaired[0].had_journal);
        CHECK_EQ(rep.repaired[0].frames, uint64_t(9 * 64));
        CHECK(rep.repaired[0].truncated_bytes > 0);
    }
    JournalEntry after;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(d.path, "take_005_nodate"), after), 0);
    CHECK_EQ(after.state, "repaired");
    CHECK_EQ(after.codec, "flac");
    CHECK_EQ(after.frames, uint64_t(9 * 64));
    StreamScan s = scan_flac(posix_file_ops(), d.file("take_005_nodate.flac"));
    CHECK(s.ok && s.stored_total == 9 * 64 && s.valid_end == s.file_size);
}

}  // namespace

void test_stream_scan() {
    checksums();
    containers();
    flac_info_and_scan();
    flac_cut_sweep(false);
    flac_cut_sweep(true);
    ogg_info_and_scan();
    ogg_cut_sweep(false);
    ogg_cut_sweep(true);
    middle_damage();
    zero_tail();
    no_journal_rules();
    journal_driven();
}
