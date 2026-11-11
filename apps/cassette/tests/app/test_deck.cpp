/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the deck through the controller: record timer, pause and resume of a take, REW/FWD scan and jump.
 */
#include "deck.hpp"
#include "helpers.hpp"

using namespace ta;

namespace {

const char *kTake = "take_001_20261108-1105.wav";

void t_timer() {
    Rig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(!c.set_record_timer(5));
    CHECK(c.set_record_timer(3));
    CHECK_EQ(r.snap().settings.record_timer_s, 3);

    /* STOP cancels: nothing written */
    CHECK(c.key_event(KeyEvent::Rec));
    Snapshot s = r.snap();
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckCountdown));
    CHECK_EQ(s.time.countdown_seconds, 3);
    CHECK(s.time.countdown_fraction > 0.9f);
    CHECK(!s.take.recording);
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckIdle));
    CHECK_EQ(r.snap().time.countdown_seconds, 0);
    /* ESC cancels, and tells the caller it was handled */
    CHECK(c.key_event(KeyEvent::Rec));
    CHECK(c.key_event(KeyEvent::Esc));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckIdle));
    /* ESC with nothing to cancel is the UI's back */
    CHECK(!c.key_event(KeyEvent::Esc));
    /* a tap on the overlay cancels too */
    CHECK(c.key_event(KeyEvent::Rec));
    CHECK(c.key_event(KeyEvent::TapOverlay));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckIdle));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(r.wavs().empty());

    /* run it out: fires exactly once */
    CHECK(c.key_event(KeyEvent::Rec));
    r.clock.advance(1000);
    CHECK(r.wait([](const Snapshot &x) { return x.time.countdown_seconds == 2; }, 1000));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckCountdown));
    CHECK(r.snap().time.countdown_fraction < 0.7f && r.snap().time.countdown_fraction > 0.3f);
    r.clock.advance(2100);
    CHECK(r.wait_state(kDeckRecording, 1000));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.4; }));
    CHECK_EQ(r.wavs().size(), 1u);
    r.clock.advance(5000); /* the countdown is over: pushing time on does not start another take */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(r.wavs().size(), 1u);
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    CHECK_EQ(c.takes().size(), 1u);
}

void t_pause_resume() {
    Rig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.6; }));
    CHECK(c.key_event(KeyEvent::PlayPause)); /* pauses the recording */
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckRecordingPaused));
    CHECK(r.snap().deck.rec_latched);
    const double frozen = r.snap().time.elapsed_s;
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(std::abs(r.snap().time.elapsed_s - frozen) < 0.001);
    CHECK_EQ(r.wavs().size(), 1u);
    CHECK(c.key_event(KeyEvent::PlayPause)); /* resumes: the same file */
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckRecording));
    CHECK(r.wait([frozen](const Snapshot &x) { return x.time.elapsed_s >= frozen + 0.6; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK_EQ(r.wavs().size(), 1u);
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        /* about 1.2 s of audio in a take that took 2 s of wall time: the paused part is not in the file */
        CHECK(rows[0].seconds > 1.0 && rows[0].seconds < 1.65);
        CHECK(!rows[0].damaged);
    }
}

void t_scan_and_jump() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 60.0);
    r.sink.sleep_us = 5000; /* 1024 frames per 5 ms: ten times real time, so a minute plays in six seconds */
    r.open();
    Controller &c = r.ctl();
    CHECK_EQ(c.takes().size(), 1u);
    CHECK(r.snap().deck.play_pause.enabled && r.snap().deck.rew.enabled && r.snap().deck.fwd.enabled);

    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(r.snap().deck.pause_icon);
    CHECK(r.wait([](const Snapshot &x) { return x.time.position_s > 10.0; }));
    CHECK(r.snap().time.length_s > 59.9);

    /* REW held: reverse scan at 5x, then back to play */
    const double p0 = r.snap().time.position_s;
    CHECK(c.key_event(KeyEvent::RewDown));
    Snapshot s = r.snap();
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckScanningReverse));
    CHECK(s.scan.active && s.scan.direction == -1);
    CHECK_EQ(s.scan.speed_text, "5x");
    CHECK(s.deck.rew.active);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const double p1 = r.snap().time.position_s;
    CHECK(p1 < p0 - 0.5);
    CHECK(c.key_event(KeyEvent::RewUp));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(!r.snap().scan.active);
    const double p2 = r.snap().time.position_s;
    CHECK(r.wait([p2](const Snapshot &x) { return x.time.position_s > p2 + 0.5; }));
    /* FWD held: forward scan */
    CHECK(c.key_event(KeyEvent::FwdDown));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckScanningForward));
    CHECK(r.snap().scan.direction == 1);
    CHECK(c.key_event(KeyEvent::FwdUp));
    CHECK(r.wait_state(kDeckPlaying) || r.snap().deck.state == kDeckIdle);
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));

    /* jump mode: each tap moves 5 s (paused, so the numbers are exact) */
    CHECK(c.set_scan_mode(ScanMode::Jump));
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlayingPaused));
    const double q0 = r.snap().time.position_s;
    CHECK(c.key_event(KeyEvent::FwdDown));
    CHECK(c.key_event(KeyEvent::FwdUp));
    CHECK(std::abs(r.snap().time.position_s - (q0 + 5.0)) < 0.01);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlayingPaused));
    CHECK(!r.snap().scan.active);
    CHECK(c.key_event(KeyEvent::RewDown));
    CHECK(c.key_event(KeyEvent::RewUp));
    CHECK(std::abs(r.snap().time.position_s - q0) < 0.01);
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
}

/* The deck looks stopped: no scan, REW / FWD and PLAY/PAUSE not latched, no jump flash. */
bool looks_idle(const Snapshot &x) {
    return x.deck.state == kDeckIdle && !x.scan.active && x.scan.jump_dir == 0 && !x.deck.rew.active &&
           !x.deck.fwd.active && !x.deck.pause_icon;
}

/* REW reaching the start ends the scan like FWD reaching the end: Idle, the take still loaded; the same at once when the
 * scan starts at its end; a jump that cannot move does not flash the hubs. */
void t_scan_ends() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 20.0);
    r.sink.sleep_us = 5000;
    r.open();
    Controller &c = r.ctl();

    /* REW held while playing: runs into the start and stops there, the key still down */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(r.wait([](const Snapshot &x) { return x.time.position_s > 2.0; }));
    CHECK(c.key_event(KeyEvent::RewDown));
    CHECK(r.wait(looks_idle, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(looks_idle(r.snap())); /* stays stopped */
    CHECK_EQ(r.snap().take.name, kTake);
    c.key_event(KeyEvent::RewUp); /* released at 0: nothing happens */
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(looks_idle(r.snap()));
    /* PLAY afterwards plays the take from the start */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(r.snap().time.position_s < 1.0);

    /* REW held while paused: the same */
    CHECK(r.wait([](const Snapshot &x) { return x.time.position_s > 1.0; }));
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlayingPaused));
    CHECK(c.key_event(KeyEvent::RewDown));
    CHECK(r.wait(looks_idle, 3000));
    c.key_event(KeyEvent::RewUp);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(looks_idle(r.snap()));

    /* REW pressed with the take at 0 (Idle: cued at its start): stops at once */
    CHECK(c.key_event(KeyEvent::RewDown));
    CHECK(r.wait(looks_idle, 200));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(looks_idle(r.snap()));
    c.key_event(KeyEvent::RewUp);
    CHECK(looks_idle(r.snap()));

    /* FWD held: runs into the end, Idle (the same end) */
    CHECK(c.key_event(KeyEvent::FwdDown));
    CHECK(r.wait(looks_idle, 3000));
    c.key_event(KeyEvent::FwdUp);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(looks_idle(r.snap()));

    /* jump mode: REW at 0 cannot move, so it does not flash the hubs, held or not */
    CHECK(c.set_scan_mode(ScanMode::Jump));
    CHECK(c.key_event(KeyEvent::RewDown)); /* Idle: cued at 0, paused */
    Snapshot s = r.snap();
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckPlayingPaused));
    CHECK_EQ(s.scan.jump_dir, 0);
    CHECK(s.time.position_s < 0.001);
    r.clock.advance(cassette::kJumpRepeatMs + 10); /* held: the repeat cannot move either */
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(r.snap().scan.jump_dir, 0);
    c.key_event(KeyEvent::RewUp);
    /* FWD jumps while paused until the end: the last jump that moves flashes, the one at the end does not */
    for (int i = 0; i < 4; ++i) {
        c.key_event(KeyEvent::FwdDown);
        c.key_event(KeyEvent::FwdUp);
    }
    CHECK(std::abs(r.snap().time.position_s - 20.0) < 0.01);
    CHECK_EQ(r.snap().scan.jump_dir, 1);
    r.clock.advance(kJumpFlashMs + 10);
    c.key_event(KeyEvent::FwdDown);
    c.key_event(KeyEvent::FwdUp);
    CHECK_EQ(r.snap().scan.jump_dir, 0);
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
}

}  // namespace

void test_deck() {
    t_timer();
    t_pause_resume();
    t_scan_and_jump();
    t_scan_ends();
}
