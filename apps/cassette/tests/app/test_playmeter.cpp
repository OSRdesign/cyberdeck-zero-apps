/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the playback meter in the snapshot (Snapshot::play_meter) and the jump flash (ScanSnap::jump_dir).
 * A generated -12 dBFS tone take (synthetic) at 16000 Hz and at 48000 Hz: while it plays the played level is within one
 * meter step of -12 dBFS, the PEAK lamp stays off, the input meter keeps running beside it; paused the level falls; stopped
 * the playback meter is empty again.
 */
#include "helpers.hpp"

#include "meter.hpp"

#include <cmath>

using namespace ta;

namespace {

const char *kTake = "take_001_20261108-1105.wav";
constexpr double kStepDb = 40.0 / 14.0; /* one LED step */

void make_tone_take(const std::string &dir, uint32_t rate, double seconds, double dbfs) {
    posix_file_ops().mkdir(dir);
    const uint64_t n = static_cast<uint64_t>(seconds * rate);
    std::vector<int32_t> s;
    s.reserve(n);
    const double a = 32767.0 * std::pow(10.0, dbfs / 20.0);
    for (uint64_t i = 0; i < n; ++i) s.push_back(static_cast<int32_t>(std::lround(a * std::sin(2.0 * kPi * 250.0 * i / rate))));
    tp::write_wav(dir + "/" + kTake, tp::afmt(rate, 1, 16), s);
}

void t_play_meter(uint32_t rate) {
    Rig r(one_card());
    make_tone_take(r.rec(), rate, 20.0, -12.0);
    r.sink.sleep_us = 8000; /* about real time or faster at both rates: 20 s of audio outlasts the test */
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    /* idle: the input meter runs, the playback meter is empty */
    Snapshot s = r.snap();
    CHECK(s.meter.channels > 0);
    CHECK_EQ(s.play_meter.channels, 0);
    CHECK_EQ(s.play_meter.ch[0].bar, 0);

    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    const int want = Meter::bar_steps(-12.0f);
    CHECK(r.wait([](const Snapshot &x) { return x.play_meter.channels == 1 && x.play_meter.ch[0].peak_db > -40.f; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s = r.snap();
    CHECK_EQ(s.play_meter.channels, 1);
    CHECK(std::fabs(s.play_meter.ch[0].peak_db + 12.0) <= kStepDb);
    CHECK(std::abs(s.play_meter.ch[0].bar - want) <= 1);
    CHECK(std::abs(s.play_meter.ch[0].hold_bar - want) <= 1);
    CHECK_EQ(s.play_meter.ch[0].zone, 1); /* -12 dBFS: in the target zone */
    CHECK(!s.play_meter.ch[0].peak_lamp);
    CHECK_EQ(s.play_meter.clips, 0u);
    CHECK(s.meter.channels > 0); /* the input meter is still there (the view shows the played level) */

    /* paused: the level falls at 20 dB/s */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlayingPaused));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const float p1 = r.snap().play_meter.ch[0].peak_db;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    s = r.snap();
    const float p2 = s.play_meter.ch[0].peak_db;
    CHECK(p1 < -12.0f - 1.0f);
    CHECK(p2 < p1 - 4.0f);
    CHECK(s.play_meter.ch[0].bar < want);
    CHECK_EQ(s.play_meter.channels, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_EQ(r.snap().play_meter.ch[0].bar, 0); /* fallen away */

    /* resumed: back at the tone's level */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait([](const Snapshot &x) { return std::fabs(x.play_meter.ch[0].peak_db + 12.0) <= kStepDb; }, 3000));

    /* stopped: the playback meter is empty, the input meter carries on */
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    CHECK(r.wait([](const Snapshot &x) { return x.play_meter.channels == 0; }, 1000));
    s = r.snap();
    CHECK_EQ(s.play_meter.ch[0].bar, 0);
    CHECK_EQ(s.play_meter.ch[0].hold_bar, 0);
    CHECK(s.meter.channels > 0);
}

/* jump mode: a 5 s jump shows its direction for kJumpFlashMs, then nothing */
void t_jump_flash() {
    Rig r(one_card());
    make_take(r.rec(), kTake, 30.0);
    r.sink.sleep_us = 8000;
    r.open();
    Controller &c = r.ctl();
    CHECK(c.set_scan_mode(ScanMode::Jump));
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK_EQ(r.snap().scan.jump_dir, 0);
    CHECK(c.key_event(KeyEvent::FwdDown));
    CHECK(c.key_event(KeyEvent::FwdUp));
    CHECK_EQ(r.snap().scan.jump_dir, 1);
    CHECK(!r.snap().scan.active);
    CHECK(r.wait([](const Snapshot &x) { return x.scan.jump_dir == 0; }, 1000));
    CHECK(c.key_event(KeyEvent::RewDown));
    CHECK(c.key_event(KeyEvent::RewUp));
    CHECK_EQ(r.snap().scan.jump_dir, -1);
    CHECK(r.wait([](const Snapshot &x) { return x.scan.jump_dir == 0; }, 1000));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
}

}  // namespace

void test_playmeter() {
    t_play_meter(16000);
    t_play_meter(48000);
    t_jump_flash();
}
