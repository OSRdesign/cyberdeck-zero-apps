/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: what the screen is told about the capture path (study 058): recording through the sound server, holding
 * the card directly while a sound server runs, and audio lost to capture overruns (the count, the notice once per take, the
 * journal). Fakes only; the tone is synthetic.
 */
#include "helpers.hpp"

using namespace ta;

namespace {

const char *const kServerNote = "Recording through the sound server: the card is in use";
const char *const kOtherApps = "Other apps cannot use this card while Cassette records";
const char *const kLostNote = "Some audio was lost: the card or the disk could not keep up";

size_t count_notice(const Snapshot &s, const std::string &text) {
    size_t n = 0;
    for (const Notice &x : s.notices)
        if (x.text == text) ++n;
    return n;
}

bool has_capture_notice(const Snapshot &s) {
    return count_notice(s, kServerNote) || count_notice(s, kOtherApps) || count_notice(s, kLostNote);
}

std::vector<FakeCard> busy_card() {
    std::vector<FakeCard> c = one_card();
    c[0].busy = true; /* a sound server client holds the card: the direct open fails with EBUSY */
    return c;
}

/* The journal of the only take, read from the recordings folder. */
bool read_journal(Rig &r, JournalEntry &je) {
    std::vector<std::string> names;
    posix_file_ops().list_dir(r.rec() + "/.journal", names);
    for (const std::string &n : names)
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0)
            return journal_read(posix_file_ops(), r.rec() + "/.journal/" + n, je) == 0;
    return false;
}

void t_via_server() {
    Rig r(busy_card());
    r.cfg.server_running = [] { return true; }; /* a server runs, but this take goes through it: only the server notice */
    r.open();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(s.card.present);
    CHECK(s.card.via_server);
    CHECK(!has_capture_notice(s)); /* nothing before a take starts */
    CHECK(r.ctl().key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    s = r.snap();
    CHECK(s.card.via_server);
    CHECK_EQ(count_notice(s, kServerNote), 1u);
    CHECK_EQ(count_notice(s, kOtherApps), 0u);
    CHECK_EQ(count_notice(s, kLostNote), 0u);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }, 4000));
    CHECK(r.snap().card.via_server); /* the whole take */
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK_EQ(r.wavs().size(), 1u);
}

void t_direct_with_server_running() {
    Rig r(one_card());
    r.cfg.server_running = [] { return true; };
    r.open();
    CHECK(r.wait_meter());
    CHECK(!r.snap().card.via_server);
    CHECK(!has_capture_notice(r.snap()));
    CHECK(r.ctl().key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    Snapshot s = r.snap();
    CHECK_EQ(count_notice(s, kOtherApps), 1u);
    CHECK_EQ(count_notice(s, kServerNote), 0u);
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
}

void t_direct_without_server() {
    Rig r(one_card());
    std::atomic<int> asked{0};
    r.cfg.server_running = [&asked] {
        ++asked;
        return false;
    };
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.ctl().key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }, 4000));
    CHECK(!has_capture_notice(r.snap())); /* no sound server: nothing to say */
    CHECK_EQ(asked.load(), 1);            /* asked once, when the take started */
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
}

void t_lost_audio() {
    std::vector<FakeCard> cards = one_card();
    /* two stalls 2 s and 4 s into the card's stream: 0.3 s and 0.2 s lost */
    cards[0].events = {{FakeEvent::Xrun, 96000, 0, 0.3}, {FakeEvent::Xrun, 192000, 0, 0.2}};
    Rig r(cards);
    r.cfg.server_running = [] { return false; };
    r.cfg.writer_sync_ms = 300; /* the journal is rewritten often */
    r.cfg.capture_now = [&r] { return r.alsa.clock; }; /* the fake's clock: a scripted stall is a measurable gap */
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.snap().problems.frames_lost == 0);
    CHECK(r.ctl().key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.snap().problems.lost_seconds == 0.0);
    CHECK(r.wait([](const Snapshot &x) { return x.problems.frames_lost > 0; }, 8000));
    Snapshot s = r.snap();
    CHECK(s.problems.overruns >= 1);
    CHECK(s.problems.lost_seconds > 0.25 && s.problems.lost_seconds < 0.35); /* about 0.3 s */
    CHECK_EQ(count_notice(s, kLostNote), 1u);
    r.ctl().ack_notices();
    /* the second stall: the total grows, the notice does not come again in this take */
    CHECK(r.wait([](const Snapshot &x) { return x.problems.overruns >= 2; }, 8000));
    s = r.snap();
    CHECK(s.problems.lost_seconds > 0.45 && s.problems.lost_seconds < 0.55); /* about 0.5 s */
    CHECK_EQ(count_notice(s, kLostNote), 0u);
    CHECK(s.take.recording);
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    s = r.snap();
    CHECK(s.problems.lost_seconds > 0.45 && s.problems.lost_seconds < 0.55); /* kept after the take for the report */
    CHECK_EQ(count_notice(s, kLostNote), 0u);

    /* the closing journal carries the count (about 0.5 s at 48 kHz = 24000 frames) */
    JournalEntry je;
    CHECK(read_journal(r, je));
    CHECK(je.state == "closed");
    CHECK(je.lost_frames > 22000 && je.lost_frames < 26000);
    CHECK_EQ(je.dropped_frames, 0u); /* the ring never filled: a different count */
}

void t_no_loss_no_notice() {
    Rig r(one_card());
    r.cfg.server_running = [] { return false; };
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.ctl().key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.6; }, 4000));
    Snapshot s = r.snap();
    CHECK(s.problems.lost_seconds == 0.0);
    CHECK(!has_capture_notice(s));
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    JournalEntry je;
    CHECK(read_journal(r, je));
    CHECK_EQ(je.lost_frames, 0u);
}

} // namespace

void test_notes() {
    t_via_server();
    t_direct_with_server_running();
    t_direct_without_server();
    t_lost_audio();
    t_no_loss_no_notice();
}
