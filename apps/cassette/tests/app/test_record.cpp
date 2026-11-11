/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the recording pipeline end to end (fake ALSA tone -> processing -> ring -> writer -> WAV), the meter on
 * the idle screen, playback of the result, auto-split, and a card with no level control.
 */
#include "helpers.hpp"

#include <algorithm>

using namespace ta;

namespace {

void t_tone_roundtrip() {
    Rig r(one_card());
    r.cfg.writer_sync_ms = 500;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter()); /* the meter is live before any take */
    Snapshot s = r.snap();
    CHECK(s.started);
    CHECK(s.card.present);
    CHECK(s.card.has_level && s.card.has_agc && s.card.has_mute);
    CHECK_EQ(s.card.label, "USB Audio CODEC");
    CHECK_EQ(s.meter.channels, 1);
    CHECK(s.meter.ch[0].peak_db > -7.f && s.meter.ch[0].peak_db < -5.f); /* half scale: -6.02 dBFS */
    CHECK(s.meter.ch[0].bar >= 12);
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckIdle));
    CHECK(s.deck.rec.enabled);
    CHECK(!s.deck.play_pause.enabled); /* nothing to play yet */
    CHECK_EQ(s.take.format_text, "48k/16 WAV");
    CHECK(s.take.free_bytes > 0 && s.take.hours_left > 0);
    CHECK(r.wavs().empty()); /* the idle meter writes nothing */

    /* the UI side: poll hard from another thread while everything runs (TSan) */
    std::atomic<bool> stop{false};
    std::atomic<unsigned> polls{0};
    std::thread poller([&] {
        while (!stop.load()) {
            Snapshot x = c.snapshot();
            std::vector<TakeRow> t = c.takes();
            std::vector<PeakBar> p = c.peak_window(10);
            (void)x;
            (void)t;
            (void)p;
            polls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    CHECK(c.key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.snap().deck.rec_latched);
    CHECK(r.snap().take.recording);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 3.0; }, 8000));
    s = r.snap();
    CHECK_EQ(s.problems.frames_dropped, 0u);
    CHECK(!s.deck.rew.enabled && !s.deck.fwd.enabled); /* disabled while recording */
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    stop.store(true);
    poller.join();
    CHECK(polls.load() > 10);

    /* the take: listed, a real WAV, the tone */
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    CHECK(rows[0].name.compare(0, 9, "take_001_") == 0);
    CHECK(rows[0].current);
    CHECK(!rows[0].damaged);
    CHECK(rows[0].frames >= 3 * 48000u && rows[0].frames < 3 * 48000u + 24000u);
    CHECK_EQ(rows[0].format_text, "48k/16 WAV");
    WavData w = read_wav(rows[0].path);
    CHECK(w.ok && w.consistent);
    CHECK_EQ(w.frames(), rows[0].frames);
    int32_t peak = 0;
    double sq = 0;
    for (int32_t v : w.s) {
        peak = std::max(peak, std::abs(v));
        sq += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(sq / static_cast<double>(w.s.size()));
    CHECK(std::abs(peak - 16384) < 500);
    CHECK(std::abs(rms - 11585.0) < 400);
    /* the tone has the right pitch: zero crossings (440 Hz, 3 s) */
    unsigned zc = 0;
    for (size_t i = 1; i < w.s.size(); ++i) zc += (w.s[i - 1] < 0) != (w.s[i] < 0);
    const double hz = zc / 2.0 / (static_cast<double>(w.s.size()) / 48000.0);
    CHECK(std::abs(hz - 440.0) < 5.0);
    /* the journal closed cleanly */
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.rec(), file_stem(rows[0].name)), je), 0);
    CHECK_EQ(je.state, "closed");

    /* play it back to the fake sink and compare */
    CHECK(c.snapshot().deck.play_pause.enabled);
    r.sink.clear_heard();
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying) || r.snap().deck.state == kDeckIdle);
    CHECK(r.wait_state(kDeckIdle, 8000)); /* the take ended by itself */
    const std::vector<int32_t> heard = r.sink.heard();
    CHECK_EQ(heard.size(), w.s.size());
    CHECK(heard == w.s); /* bit exact */
    CHECK_EQ(r.snap().take.name, rows[0].name);

    /* the same through play_take on a second recording: names count up */
    CHECK(c.key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.4; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    rows = c.takes();
    CHECK_EQ(rows.size(), 2u);
    CHECK(rows[0].name.compare(0, 9, "take_002_") == 0); /* newest first */
    CHECK(c.play_take(rows[1].name));
    CHECK(r.wait_state(kDeckIdle, 8000));
    CHECK(c.takes()[1].current);
}

void t_idle_meter_cost() {
    /* nothing is written and nothing queues while no take runs: the ring never exists until REC */
    Rig r(one_card());
    r.open();
    CHECK(r.wait_meter());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(r.wavs().empty());
    CHECK(r.files().size() <= 3); /* at most .journal / counter directories, never audio */
    CHECK_EQ(r.snap().problems.frames_dropped, 0u);
    CHECK_EQ(r.snap().time.elapsed_s, 0.0);
}

void t_auto_split() {
    Rig r(one_card());
    r.cfg.split_bytes = kDataOffset + 96000ull * 2; /* parts of 2 s */
    r.open();
    CHECK(r.wait_meter());
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 4.6; }, 9000));
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 3u); /* take 001, _p2, _p3 */
    if (rows.size() == 3) {
        CHECK_EQ(rows[0].label, "001");
        CHECK_EQ(rows[1].label, "001 p2");
        CHECK_EQ(rows[2].label, "001 p3");
        CHECK(rows[1].name.find("_p2.wav") != std::string::npos);
        for (const TakeRow &t : rows) CHECK(!t.damaged);
    }
}

void t_no_level_control() {
    Rig r({fake_card_no_controls(0)});
    r.open();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(s.card.present);
    CHECK(!s.card.has_level && !s.card.has_agc && !s.card.has_mute);
    CHECK(!r.ctl().set_level_db(10));
    CHECK(!r.ctl().set_auto_gain(true));
    CHECK(!r.ctl().set_mute(true));
    CHECK(r.ctl().set_soft_gain_db(6.0)); /* software gain still works */
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }));
    r.ctl().key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        WavData w = read_wav(rows[0].path);
        int32_t pk = 0;
        for (int32_t v : w.s) pk = std::max(pk, std::abs(v));
        CHECK(pk > 28000 && pk <= 32768); /* +6 dB on half scale: about full scale */
    }
}

void t_no_card() {
    Rig r({});
    r.open();
    Snapshot s = r.snap();
    CHECK(!s.card.present);
    CHECK(!s.card.message.empty());
    CHECK(r.ctl().key_event(KeyEvent::Rec)); /* the deck goes to Recording, the start fails, it goes back */
    CHECK(r.wait_state(kDeckIdle));
    CHECK(!r.snap().notices.empty());
    r.ctl().ack_notices();
    CHECK(r.snap().notices.empty());
}

}  // namespace

void test_record() {
    t_tone_roundtrip();
    t_idle_meter_cost();
    t_auto_split();
    t_no_level_control();
    t_no_card();
}
