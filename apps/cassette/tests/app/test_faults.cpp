/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: a take abandoned by a real SIGKILL and repaired at the next start, a take without a journal, a disk that
 * fills up and free space that runs low.
 */
#include "helpers.hpp"

#include <algorithm>

using namespace ta;

namespace {

void t_sigkill_repair() {
    tp::TempDir base; /* the folder both processes use */
    const pid_t pid = fork();
    if (pid == 0) {
        /* the child is the app: it records and never stops */
        Rig r(one_card(), base.path);
        r.cfg.writer_sync_ms = 30000; /* no sync: the header still says "empty" when it dies */
        r.open();
        if (!r.wait_meter(5000)) _exit(2);
        r.ctl().key_event(KeyEvent::Rec);
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(pid > 0);
    if (pid <= 0) return;
    const std::string rec = base.path + "/Recordings";
    uint64_t size = 0;
    const bool grew = wait_until(
        [&] {
            std::vector<std::string> n;
            posix_file_ops().list_dir(rec, n);
            for (const std::string &f : n)
                if (f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0) {
                    StatInfo st;
                    if (posix_file_ops().stat(rec + "/" + f, st) == 0) size = st.size;
                }
            return size > kDataOffset + 96000ull * 2;
        },
        8000);
    CHECK(grew);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));

    {
        HeaderInfo h;
        std::vector<std::string> n;
        posix_file_ops().list_dir(rec, n);
        for (const std::string &f : n)
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0) h = parse_header(posix_file_ops(), rec + "/" + f);
        CHECK(h.ok && !h.consistent()); /* really abandoned: the sizes say nothing */
    }

    /* the next start repairs it and says so once */
    Rig r(one_card(), base.path);
    r.open();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(has_notice(s, Notice::Repaired));
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK(!rows[0].damaged);
        CHECK(rows[0].repaired);
        CHECK(rows[0].seconds > 1.0);
        WavData w = read_wav(rows[0].path);
        CHECK(w.ok && w.consistent);
        CHECK_EQ(w.frames(), rows[0].frames);
        /* the tone survived: a clean sine at half scale */
        int32_t pk = 0;
        for (int32_t v : w.s) pk = std::max(pk, std::abs(v));
        CHECK(std::abs(pk - 16384) < 500);
    }
    r.ctl().ack_notices();
    CHECK(r.snap().notices.empty());
    r.close();
    r.open(); /* idempotent: nothing to repair the second time */
    CHECK(!has_notice(r.snap(), Notice::Repaired));
    /* a new take after the crash gets the next number */
    CHECK(r.wait_meter());
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }));
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 2u);
    if (rows.size() == 2) CHECK(rows[0].name.compare(0, 9, "take_002_") == 0);
}

void t_repair_without_journal() {
    Rig r(one_card());
    posix_file_ops().mkdir(r.rec());
    tp::write_wav(r.rec() + "/take_005_nodate.wav", mono48(), tp::ramp_samples(48000, 1), tp::HeaderKind::Placeholder);
    r.open();
    CHECK(has_notice(r.snap(), Notice::Repaired));
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) CHECK_EQ(rows[0].frames, 48000u);
}

void t_disk_full() {
    Rig r(one_card());
    r.ops.enospc_after.store(150000); /* the take files take 150 kB, then ENOSPC */
    r.open();
    CHECK(r.wait_meter());
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckIdle, 8000)); /* stopped by itself */
    Snapshot s = r.snap();
    CHECK(s.problems.disk_full);
    CHECK(s.problems.message.find("disk full") != std::string::npos);
    CHECK(!s.take.recording);
    CHECK(!s.notices.empty());
    CHECK(s.card.present); /* the meter keeps running */
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u); /* the take is kept */
    if (!rows.empty()) {
        CHECK(!rows[0].damaged);
        CHECK(rows[0].frames > 20000 && rows[0].frames < 80000);
        WavData w = read_wav(rows[0].path);
        CHECK(w.ok && w.consistent);
    }
    CHECK(r.snap().deck.play_pause.enabled);
}

void t_low_space() {
    Rig r(one_card());
    r.ops.free_override.store(100ll << 20); /* below the 200 MB reserve */
    r.open();
    CHECK(r.wait_meter());
    CHECK_EQ(r.snap().take.free_bytes, 100ull << 20);
    CHECK(r.snap().take.hours_left > 0);
    r.ctl().key_event(KeyEvent::Rec); /* refused: no take starts */
    CHECK(r.wait_state(kDeckIdle, 2000));
    Snapshot s = r.snap();
    CHECK(s.problems.disk_full);
    CHECK(!s.problems.message.empty());
    CHECK(r.wavs().empty());
    CHECK(!r.snap().deck.play_pause.enabled);
    /* space is back: a take runs, then the free space falls under the reserve: it ends cleanly and is kept */
    r.ops.free_override.store(10ll << 30);
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(!r.snap().problems.disk_full);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }));
    r.ops.free_override.store(50ll << 20);
    CHECK(r.wait_state(kDeckIdle, 4000));
    s = r.snap();
    CHECK(s.problems.disk_full);
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK(!rows[0].damaged);
        CHECK(rows[0].seconds > 0.4);
        JournalEntry je;
        CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.rec(), file_stem(rows[0].name)), je), 0);
        CHECK_EQ(je.state, "closed");
    }
}

}  // namespace

void test_faults() {
    t_sigkill_repair();
    t_repair_without_journal();
    t_disk_full();
    t_low_space();
}
