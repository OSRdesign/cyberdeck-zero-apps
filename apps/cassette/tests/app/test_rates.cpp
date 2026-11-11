/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: a card that records at its own rate (a speech headset, mono S16 at 16000 Hz only). The controller opens
 * it at 16000 Hz and everything downstream follows: the counter, the take length, the header, the journal, the hours left,
 * playback. An end-to-end take of 30 s is recorded by a forked process that is killed mid-take; the next start repairs it.
 */
#include "helpers.hpp"

#include <algorithm>
#include <cmath>

using namespace ta;

namespace {

std::vector<FakeCard> headset() { return {fake_headset16(0)}; }

uint32_t u32le(const std::string &b, size_t o) {
    return static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8) | (static_cast<uint8_t>(b[o + 2]) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b[o + 3])) << 24);
}

/* The take file's bext coding history and time reference, straight from the bytes. */
struct Bext {
    bool ok = false;
    std::string history, time_text;
    uint64_t tref = 0;
};
Bext read_bext(const std::string &path) {
    Bext x;
    const std::string b = slurp(path);
    HeaderInfo h = parse_header(posix_file_ops(), path);
    if (!h.ok || !h.has_bext || b.size() < h.bext_offset + 603) return x;
    const size_t o = static_cast<size_t>(h.bext_offset);
    x.history = std::string(b.c_str() + o + 602);
    x.time_text = std::string(b.c_str() + o + 330, 8);
    x.tref = u32le(b, o + 338) | (static_cast<uint64_t>(u32le(b, o + 342)) << 32);
    x.ok = true;
    return x;
}

double tone_hz(const std::vector<int32_t> &s, uint32_t rate) {
    unsigned zc = 0;
    for (size_t i = 1; i < s.size(); ++i) zc += (s[i - 1] < 0) != (s[i] < 0);
    return zc / 2.0 / (static_cast<double>(s.size()) / rate);
}

/* The largest step between neighbouring samples of a 440 Hz sine of amplitude A at `rate`, plus slack. A dropped, repeated or
 * mis-sized chunk shows up as a jump far above it. */
int32_t max_step(const std::vector<int32_t> &s) {
    int32_t m = 0;
    for (size_t i = 1; i < s.size(); ++i) m = std::max(m, std::abs(s[i] - s[i - 1]));
    return m;
}

void t_record_play_at_16k() {
    Rig r(headset());
    r.cfg.writer_sync_ms = 500;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(s.card.present);
    CHECK_EQ(s.card.label, "Speech Headset");
    CHECK(s.card.has_level && s.card.has_mute && !s.card.has_agc);
    CHECK_EQ(s.take.format_text, "16k/16 WAV");
    CHECK_EQ(s.meter.channels, 1);
    /* hours left at 32,000 bytes a second (the card's own rate), not at 96,000 */
    CHECK(s.take.free_bytes > 0);
    CHECK(std::fabs(s.take.hours_left - static_cast<double>(s.take.free_bytes) / 115200000.0) < 0.01 * s.take.hours_left + 1e-6);

    CHECK(c.key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    /* the counter runs in seconds of audio: 3 s of real time is 48,000 frames, shown as 3 s (not 1 s) */
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 3.0; }, 9000));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(wall < 4.5);
    CHECK_EQ(r.snap().problems.frames_dropped, 0u);
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));

    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    CHECK_EQ(rows[0].rate, 16000u);
    CHECK_EQ(rows[0].format_text, "16k/16 WAV");
    CHECK(rows[0].frames >= 3 * 16000u && rows[0].frames < 3 * 16000u + 16000u);
    CHECK(std::fabs(rows[0].seconds - static_cast<double>(rows[0].frames) / 16000.0) < 1e-9);
    WavData w = read_wav(rows[0].path);
    CHECK(w.ok && w.consistent);
    CHECK_EQ(w.fmt.rate, 16000u);
    CHECK_EQ(w.frames(), rows[0].frames);
    CHECK(std::fabs(tone_hz(w.s, 16000) - 440.0) < 5.0);
    /* bext: the coding history names 16000 Hz; the time reference is seconds since midnight times 16000 */
    const Bext bx = read_bext(rows[0].path);
    CHECK(bx.ok);
    CHECK_EQ(bx.history, "A=PCM,F=16000,W=16,M=mono,T=Cassette\r\n");
    CHECK(bx.time_text.size() == 8);
    if (bx.time_text.size() == 8) {
        const uint64_t secs = std::stoul(bx.time_text.substr(0, 2)) * 3600 + std::stoul(bx.time_text.substr(3, 2)) * 60 +
                              std::stoul(bx.time_text.substr(6, 2));
        CHECK_EQ(bx.tref, secs * 16000u);
    }
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.rec(), file_stem(rows[0].name)), je), 0);
    CHECK_EQ(je.state, "closed");
    CHECK_EQ(je.fmt.rate, 16000u);
    CHECK_EQ(je.frames, rows[0].frames);

    /* play it back: the sink is opened for the take's rate and every sample arrives, bit exact */
    r.sink.clear_heard();
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckIdle, 8000)); /* ends by itself */
    CHECK(r.sink.heard() == w.s);
    s = r.snap();
    CHECK(std::fabs(s.time.length_s - static_cast<double>(w.frames()) / 16000.0) < 1e-9);
}

/* The forked process records and is killed with a take of at least 30 s on disk; the next start repairs it. */
void t_sigkill_repair_16k() {
    tp::TempDir base;
    const pid_t pid = fork();
    if (pid == 0) {
        Rig r(headset(), base.path);
        r.cfg.writer_sync_ms = 3000; /* a sync every 3 s: the header is up to 3 s behind when it dies */
        r.open();
        if (!r.wait_meter(5000)) _exit(2);
        r.ctl().key_event(KeyEvent::Rec);
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(pid > 0);
    if (pid <= 0) return;
    const std::string rec = base.path + "/Recordings";
    uint64_t size = 0;
    const uint64_t want = kDataOffset + 16000ull * 2 * 30; /* 30 s of 16000 Hz mono S16 */
    const bool grew = wait_until(
        [&] {
            std::vector<std::string> n;
            posix_file_ops().list_dir(rec, n);
            for (const std::string &f : n)
                if (f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0) {
                    StatInfo st;
                    if (posix_file_ops().stat(rec + "/" + f, st) == 0) size = st.size;
                }
            return size >= want;
        },
        50000);
    CHECK(grew);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));
    const uint64_t killed_size = size;

    /* what the dead process left: an open journal that names 16000 Hz, a header that lags the file */
    std::vector<std::string> names;
    posix_file_ops().list_dir(rec, names);
    std::string wav;
    for (const std::string &f : names)
        if (f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0) wav = rec + "/" + f;
    CHECK(!wav.empty());
    if (wav.empty()) return;
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(rec, file_stem(wav.substr(wav.rfind('/') + 1))), je), 0);
    CHECK_EQ(je.state, "open");
    CHECK_EQ(je.fmt.rate, 16000u);
    const HeaderInfo before = parse_header(posix_file_ops(), wav);
    CHECK(before.ok);
    CHECK(before.declared_data < before.actual_data); /* the sizes of the last sync */
    CHECK_EQ(before.fmt.rate, 16000u);

    Rig r(headset(), base.path);
    r.open();
    CHECK(r.wait_meter());
    CHECK(has_notice(r.snap(), Notice::Repaired));
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    CHECK(rows[0].repaired && !rows[0].damaged);
    CHECK_EQ(rows[0].rate, 16000u);
    CHECK(rows[0].frames >= 480000u); /* at least the 30 s that were on disk when it died ... */
    CHECK(rows[0].frames <= (killed_size - kDataOffset) / 2 + 16000u * 2); /* ... and no more than the file held */
    CHECK(rows[0].seconds >= 30.0 && rows[0].seconds < 40.0);
    CHECK(std::fabs(rows[0].seconds - static_cast<double>(rows[0].frames) / 16000.0) < 1e-9);

    /* the repaired file: a consistent 16000 Hz header, exact length, journal marked repaired */
    const HeaderInfo h = parse_header(posix_file_ops(), wav);
    CHECK(h.ok && h.consistent() && !h.suspicious());
    CHECK_EQ(h.fmt.rate, 16000u);
    CHECK_EQ(h.fmt.channels, 1u);
    CHECK_EQ(h.fmt.bits, 16u);
    CHECK_EQ(h.declared_data, rows[0].frames * 2);
    CHECK_EQ(h.actual_data, rows[0].frames * 2);
    CHECK_EQ(read_bext(wav).history, "A=PCM,F=16000,W=16,M=mono,T=Cassette\r\n");
    JournalEntry after;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(rec, file_stem(wav.substr(wav.rfind('/') + 1))), after), 0);
    CHECK_EQ(after.state, "repaired");
    CHECK_EQ(after.frames, rows[0].frames);

    /* the bytes: a clean 440 Hz sine at half scale over the whole 30 s, no gap, no repeat, no wrong-rate stretch */
    WavData w = read_wav(wav);
    CHECK(w.ok && w.consistent);
    CHECK_EQ(w.frames(), rows[0].frames);
    CHECK(std::fabs(tone_hz(w.s, 16000) - 440.0) < 2.0);
    int32_t pk = 0;
    for (int32_t v : w.s) pk = std::max(pk, std::abs(v));
    CHECK(std::abs(pk - 16384) < 500);
    /* neighbouring samples of a 440 Hz sine at 16000 Hz differ by at most 2*pi*440/16000*16384 = 2831 (plus rounding) */
    CHECK(max_step(w.s) < 2900);
    /* every 400 samples (11 whole cycles) the sine repeats: a lost or doubled chunk would break the pattern */
    size_t bad = 0;
    for (size_t i = 400 * 60; i + 400 < w.s.size(); ++i)
        if (std::abs(w.s[i + 400] - w.s[i]) > 400) ++bad; /* the DC blocker's start-up has settled by then */
    CHECK_EQ(bad, 0u);
    r.ctl().ack_notices();
}

} // namespace

/* The forked part runs before any other thread exists (see test_main.cpp). */
void test_rates_fork() { t_sigkill_repair_16k(); }

void test_rates() { t_record_play_at_16k(); }
