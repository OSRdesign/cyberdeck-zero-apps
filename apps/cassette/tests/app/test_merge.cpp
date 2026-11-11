/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: MERGE TAKES through the controller (plan_merge, start_merge, cancel_merge) on the fakes. Synthetic takes
 * only (the ramp of tests/playback, the test encoders).
 *   - the plan: the new take's name (the next take number, the first take's prefix, the date, the extension), its length and
 *     size; the refusals with their notices (one take, a playing take, a recording, a missing FLAC library, another rate, a
 *     full card);
 *   - a WAV merge in the order chosen: the new take holds the takes end to end, the list shows it, the take counter moved on;
 *   - cancel: nothing written, the counter where it was (the number is used only by a merge that succeeded);
 *   - delete originals: the takes, their peak caches and journals go; a loaded original leaves the new take loaded;
 *   - FLAC through the configured encoder (the fixture), decoded back with the fake decoder.
 */
#include "helpers.hpp"

#include "../fake_decoders.hpp"
#include "../fixture_codecs.hpp"
#include "codec/stream_scan.hpp"
#include "naming.hpp"
#include "recordings.hpp"

using namespace ta;

namespace {

bool notice_text(const Snapshot &s, const std::string &text) {
    for (const Notice &n : s.notices)
        if (n.text.find(text) != std::string::npos) return true;
    return false;
}

bool starts(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string &s, const std::string &t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

bool wait_merge(Rig &r, int state, unsigned seq) {
    return r.wait([state, seq](const Snapshot &x) { return x.merge.seq == seq && x.merge.state == state; }, 10000);
}

std::vector<int32_t> samples(const std::string &path) { return read_wav(path).s; }

/* Three WAV takes 001 .. 003 of different lengths, plus a 44.1 kHz one and a file outside the take grammar. */
void three_takes(Rig &r) {
    make_take(r.rec(), "take_001_20261108-1100.wav", 0.50);
    make_take(r.rec(), "take_002_20261108-1101.wav", 0.25);
    make_take(r.rec(), "take_003_20261108-1102.wav", 0.75);
    tp::write_wav(r.rec() + "/take_004_20261108-1103.wav", tp::afmt(44100, 1, 16), tp::ramp_samples(4410, 1));
    tp::write_wav(r.rec() + "/field notes.wav", mono48(), tp::ramp_samples(4800, 1));
}

void t_plan_and_refusals() {
    Rig r(one_card());
    three_takes(r);
    r.open();
    Controller &c = r.ctl();
    const std::vector<std::string> pick = {"take_003_20261108-1102.wav", "take_001_20261108-1100.wav"};
    MergeInfo m = c.plan_merge(pick);
    CHECK(m.ok);
    CHECK_EQ(m.count, 2);
    CHECK(starts(m.name, "take_005_") && ends(m.name, ".wav") && m.name != "take_005_nodate.wav"); /* the clock is set */
    CHECK(std::fabs(m.seconds - 1.25) < 0.001);
    CHECK_EQ(m.bytes, uint64_t(kDataOffset) + 60000u * 2u);
    /* the prefix of the FIRST take chosen */
    CHECK_EQ(c.rename_take("take_003_20261108-1102.wav", "my band"), 0);
    m = c.plan_merge({"my band_003_20261108-1102.wav", "take_001_20261108-1100.wav"});
    CHECK(m.ok && starts(m.name, "my band_"));
    CHECK_EQ(c.rename_take("my band_003_20261108-1102.wav", "take"), 0);
    /* refusals */
    m = c.plan_merge({"take_001_20261108-1100.wav"});
    CHECK(!m.ok);
    CHECK_EQ(m.message, "Merging needs two takes");
    m = c.plan_merge({"take_001_20261108-1100.wav", "take_004_20261108-1103.wav"});
    CHECK(!m.ok);
    CHECK_EQ(m.message, "Cannot merge: take_004_20261108-1103.wav: Different rate");
    m = c.plan_merge({"take_001_20261108-1100.wav", "field notes.wav"});
    CHECK(!m.ok);
    CHECK_EQ(m.message, "Not available: Not a Cassette take");
    r.ops.free_override.store(1000);
    m = c.plan_merge(pick);
    CHECK(!m.ok);
    CHECK_EQ(m.message, "Not enough free space");
    CHECK(!c.start_merge(pick, false));
    CHECK(r.wait([](const Snapshot &x) { return notice_text(x, "Not enough free space"); }));
    r.ops.free_override.store(-1);
    c.ack_notices();
    /* a playing take is not merged (stop first) */
    CHECK(c.play_take("take_001_20261108-1100.wav"));
    m = c.plan_merge(pick);
    CHECK(!m.ok);
    CHECK_EQ(m.message, "Not available: Playing: stop it first");
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    /* while recording */
    CHECK(r.wait_meter());
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    m = c.plan_merge(pick);
    CHECK(!m.ok);
    CHECK_EQ(m.message, "Stop the recording to merge takes");
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    /* nothing was written by the plans and refusals: no temporary, no take 006+ but the recording (005) */
    for (const std::string &n : r.files()) CHECK(!starts(n, ".tmp-"));
}

void t_merge_wav() {
    Rig r(one_card());
    three_takes(r);
    r.open();
    Controller &c = r.ctl();
    const std::vector<std::string> pick = {"take_003_20261108-1102.wav", "take_001_20261108-1100.wav",
                                           "take_002_20261108-1101.wav"};
    const MergeInfo m = c.plan_merge(pick);
    CHECK(m.ok);
    CHECK(c.start_merge(pick, false));
    CHECK(wait_merge(r, 2, 1));
    const Snapshot s = r.snap();
    CHECK_EQ(s.merge.sources, 3);
    CHECK_EQ(s.merge.output, m.name);
    CHECK(s.merge.message.empty());
    CHECK(!s.merge.originals_deleted);
    /* the takes end to end, in the order chosen */
    std::vector<int32_t> want;
    for (const std::string &n : pick) {
        const std::vector<int32_t> a = samples(r.rec() + "/" + n);
        want.insert(want.end(), a.begin(), a.end());
    }
    const WavData out = read_wav(r.rec() + "/" + m.name);
    CHECK(out.ok && out.consistent);
    CHECK(out.s == want);
    /* in the list, newest first; the originals stay */
    const std::vector<TakeRow> rows = c.takes();
    CHECK(!rows.empty() && rows.front().name == m.name && rows.front().named);
    CHECK_EQ(rows.size(), 6u);
    /* the counter moved on: the next take is 006 */
    CHECK_EQ(peek_take_number(posix_file_ops(), r.rec(), "take"), 6u);
    /* a second merge counts its own sequence */
    CHECK(c.start_merge({"take_001_20261108-1100.wav", "take_002_20261108-1101.wav"}, false));
    CHECK(wait_merge(r, 2, 2));
    CHECK(starts(r.snap().merge.output, "take_006_"));
}

void t_cancel_keeps_counter() {
    Rig r(one_card());
    make_take(r.rec(), "take_001_20261108-1100.wav", 4.0);
    make_take(r.rec(), "take_002_20261108-1101.wav", 4.0);
    r.cfg.save_block_bytes = 4096;
    r.open();
    Controller &c = r.ctl();
    r.ops.tmp_delay_us.store(5000); /* each block of the new take takes 5 ms: about 1 s for the whole */
    const std::vector<std::string> pick = {"take_002_20261108-1101.wav", "take_001_20261108-1100.wav"};
    CHECK(c.start_merge(pick, true));
    CHECK(r.wait([](const Snapshot &x) { return x.merge.state == 1 && x.merge.progress > 0.f; }));
    /* while it runs: its takes are in use, the take number and the folder cannot change, REC waits */
    CHECK_EQ(c.delete_take("take_001_20261108-1100.wav"), -EBUSY);
    uint64_t landed = 0;
    CHECK_EQ(c.set_next_take_number(50, &landed), -EBUSY);
    CHECK(!c.start_merge(pick, false));
    c.cancel_merge();
    CHECK(wait_merge(r, 4, 1));
    CHECK_EQ(r.snap().merge.message, "cancelled");
    /* nothing written, nothing deleted, the counter untouched */
    const std::vector<std::string> f = r.files();
    for (const std::string &n : f) CHECK(!starts(n, ".tmp-") && !starts(n, "take_003_"));
    CHECK(std::find(f.begin(), f.end(), "take_001_20261108-1100.wav") != f.end());
    CHECK(std::find(f.begin(), f.end(), "take_002_20261108-1101.wav") != f.end());
    CHECK_EQ(peek_take_number(posix_file_ops(), r.rec(), "take"), 3u);
    r.ops.tmp_delay_us.store(0);
}

void t_delete_originals() {
    Rig r(one_card());
    three_takes(r);
    /* a peak cache next to take 002 */
    FILE *p = std::fopen((r.rec() + "/take_002_20261108-1101.wav.peaks").c_str(), "wb");
    if (p) std::fclose(p);
    r.open();
    Controller &c = r.ctl();
    CHECK(c.load("take_002_20261108-1101.wav"));
    const std::vector<std::string> pick = {"take_002_20261108-1101.wav", "take_001_20261108-1100.wav"};
    CHECK(c.start_merge(pick, true));
    CHECK(wait_merge(r, 2, 1));
    const Snapshot s = r.snap();
    CHECK(s.merge.originals_deleted);
    const std::vector<std::string> f = r.files();
    CHECK(std::find(f.begin(), f.end(), "take_001_20261108-1100.wav") == f.end());
    CHECK(std::find(f.begin(), f.end(), "take_002_20261108-1101.wav") == f.end());
    CHECK(std::find(f.begin(), f.end(), "take_002_20261108-1101.wav.peaks") == f.end());
    CHECK(std::find(f.begin(), f.end(), s.merge.output) != f.end());
    /* the loaded original went: the new take is in the deck, without a load animation */
    CHECK_EQ(s.take.name, s.merge.output);
    CHECK_EQ(s.take.load_seq, 1u);
    CHECK_EQ(read_wav(r.rec() + "/" + s.merge.output).frames(), 36000u);
}

void t_merge_flac() {
    Rig r(one_card());
    /* FLAC takes from the fixture encoder (VERBATIM frames) */
    posix_file_ops().mkdir(r.rec());
    const AudioFormat f = mono48();
    std::vector<std::vector<unsigned char>> pcm;
    for (int i = 0; i < 2; ++i) {
        MemoryByteSink s;
        fx::VerbatimFlac e(f, 4096, 128);
        e.start(s, {{"TITLE", "x"}});
        const std::vector<unsigned char> p = fx::pcm(f, static_cast<uint64_t>(i) * 100000u, 30000u + 5000u * i);
        e.write(p.data(), p.size() / 2);
        e.finish();
        pcm.push_back(p);
        FILE *o = std::fopen((r.rec() + "/take_00" + std::to_string(i + 1) + "_20261108-110" + std::to_string(i) + ".flac").c_str(), "wb");
        if (o) {
            std::fwrite(s.data.data(), 1, s.data.size(), o);
            std::fclose(o);
        }
    }
    static const FlacDecLib dec = fakedec::make_flac_dec_lib();
    r.cfg.decoders = [] {
        DecoderLibs d;
        d.flac = &dec;
        return d;
    };
    r.cfg.make_encoder = [](const FormatChoice &, const AudioFormat &af) -> std::unique_ptr<Encoder> {
        return std::unique_ptr<Encoder>(new fx::VerbatimFlac(af, 4096));
    };
    r.open();
    Controller &c = r.ctl();
    const std::vector<std::string> pick = {"take_002_20261108-1101.flac", "take_001_20261108-1100.flac"};
    const MergeInfo m = c.plan_merge(pick);
    CHECK(m.ok && ends(m.name, ".flac") && starts(m.name, "take_003_"));
    CHECK(c.start_merge(pick, false));
    CHECK(wait_merge(r, 2, 1));
    const std::string out = r.rec() + "/" + r.snap().merge.output;
    const StreamInfo si = read_stream_info(posix_file_ops(), out);
    CHECK(si.ok && si.frames_known && si.frames == 65000u);
    const std::string bytes = slurp(out);
    std::vector<int32_t> got = fx::decode_verbatim_flac(std::vector<unsigned char>(bytes.begin(), bytes.end()));
    std::vector<int32_t> want = fx::samples_of(f, pcm[1]);
    const std::vector<int32_t> b = fx::samples_of(f, pcm[0]);
    want.insert(want.end(), b.begin(), b.end());
    CHECK(got == want);
    /* without libFLAC: refused with the one notice */
    Rig r2(one_card());
    posix_file_ops().mkdir(r2.rec());
    for (const char *n : {"take_001_20261108-1100.flac", "take_002_20261108-1101.flac"}) {
        const std::string data = slurp(r.rec() + "/" + n);
        FILE *o = std::fopen((r2.rec() + "/" + n).c_str(), "wb");
        if (o) {
            std::fwrite(data.data(), 1, data.size(), o);
            std::fclose(o);
        }
    }
    r2.cfg.decoders = [] { return DecoderLibs(); };
    r2.open();
    const MergeInfo m2 = r2.ctl().plan_merge({"take_001_20261108-1100.flac", "take_002_20261108-1101.flac"});
    CHECK(!m2.ok);
    CHECK_EQ(m2.message, "This take cannot be merged: FLAC not installed (libflac14)");
}

}  // namespace

void test_merge_app() {
    t_plan_and_refusals();
    t_merge_wav();
    t_cancel_keeps_counter();
    t_delete_originals();
    t_merge_flac();
}
