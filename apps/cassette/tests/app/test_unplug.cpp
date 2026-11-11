/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the card goes away mid-take and comes back without leaving the screen; a saved card that has moved to
 * another index is still found.
 */
#include "helpers.hpp"

using namespace ta;

namespace {

void t_unplug_replug() {
    Rig r(one_card());
    r.cfg.writer_sync_ms = 300;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_level_db(12.0));
    CHECK(r.alsa.mixer_value("Device", "Mic Capture Volume") == 8); /* 12 dB of 23.81 over raw 0..16 */
    CHECK(c.set_auto_gain(false));
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 1.0; }));

    r.alsa.unplug("Device");
    CHECK(r.wait_state(kDeckIdle, 3000)); /* the take was closed by itself */
    Snapshot s = r.snap();
    CHECK(s.problems.unplugged);
    CHECK(has_notice(s, Notice::CardLost));
    CHECK(!s.card.present);
    CHECK(!s.card.message.empty());
    CHECK(!s.take.recording);
    /* the take is kept, closed, consistent */
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK(!rows[0].damaged);
        CHECK(rows[0].seconds > 0.8);
        WavData w = read_wav(rows[0].path);
        CHECK(w.ok && w.consistent);
        JournalEntry je;
        CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.rec(), file_stem(rows[0].name)), je), 0);
        CHECK_EQ(je.state, "closed");
    }
    CHECK(r.snap().deck.play_pause.enabled); /* the take can be played while the card is away */
    c.ack_notices();

    /* the card comes back: re-selected by descriptor, mixer state put back, no screen change */
    r.alsa.replug("Device");
    CHECK(r.wait([](const Snapshot &x) { return x.card.present; }, 4000));
    s = r.snap();
    CHECK(has_notice(s, Notice::CardBack));
    CHECK_EQ(s.card.label, "USB Audio CODEC");
    CHECK(r.alsa.mixer_value("Device", "Mic Capture Volume") == 8); /* the saved level is back (the fake reset it to 0) */
    CHECK(r.alsa.mixer_value("Device", "Auto Gain Control") == 0);
    CHECK(std::abs(s.card.level_db - 11.9) < 0.2);
    CHECK(r.wait_meter());
    /* and it records again */
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.4; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK_EQ(c.takes().size(), 2u);
    CHECK(!r.snap().problems.unplugged); /* the problem flags belong to the take that had them */
}

void t_unplug_idle_and_countdown() {
    Rig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_record_timer(10));
    c.key_event(KeyEvent::Rec);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckCountdown));
    r.alsa.unplug("Device"); /* during the countdown: cancelled and reported */
    CHECK(r.wait_state(kDeckIdle, 3000));
    CHECK(has_notice(r.snap(), Notice::CardLost));
    CHECK(r.wavs().empty());
    CHECK(!r.snap().card.present);
    /* a REC without a card does not start anything and does not hang */
    c.key_event(KeyEvent::Rec);
    c.key_event(KeyEvent::Esc);
    CHECK(r.wait_state(kDeckIdle, 3000));
    CHECK(r.wavs().empty());
    r.alsa.replug("Device");
    CHECK(r.wait([](const Snapshot &x) { return x.card.present; }, 4000));
}

void t_saved_card_other_index() {
    std::vector<FakeCard> cards = {fake_uc02(0), fake_pcm2902(1), fake_card_no_controls(2)};
    Rig r(cards);
    r.open();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK_EQ(s.card.labels.size(), 3u);
    CHECK_EQ(s.card.label, "UC02"); /* no saved choice: the first capture card */
    int pcm = -1;
    for (size_t i = 0; i < s.card.labels.size(); ++i)
        if (s.card.labels[i] == "USB Audio CODEC") pcm = static_cast<int>(i);
    CHECK(pcm >= 0);
    CHECK(r.ctl().select_card(pcm));
    CHECK(r.wait_meter());
    CHECK_EQ(r.snap().card.label, "USB Audio CODEC");
    CHECK(r.snap().card.has_agc); /* the PCM2902's controls, not the headset's */
    CHECK(r.ctl().set_level_db(6.0));
    CHECK(!r.ctl().select_card(7));
    r.close();
    /* the next boot: the indexes are rotated, the saved card sits somewhere else */
    r.alsa.with([](FakeAlsa &a) { a.reboot_shuffle(); });
    CHECK(r.alsa.cards()[0].name != "USB Audio CODEC");
    r.open();
    CHECK(r.wait_meter());
    s = r.snap();
    CHECK_EQ(s.card.label, "USB Audio CODEC");
    CHECK(s.card.selected >= 0 && s.card.selected < static_cast<int>(s.card.labels.size()));
    CHECK_EQ(s.card.labels[static_cast<size_t>(s.card.selected)], "USB Audio CODEC");
    CHECK(std::abs(s.card.level_db - 6.0) < 0.8);
    /* the saved card is absent at the next start: the first capture card is used and the saved choice is kept */
    r.close();
    r.alsa.with([](FakeAlsa &a) {
        for (FakeCard &c : a.cards_)
            if (c.info.id == "Device") c.present = false;
    });
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.snap().card.label != "USB Audio CODEC");
    r.close();
    r.alsa.with([](FakeAlsa &a) {
        for (FakeCard &c : a.cards_) c.present = true;
    });
    r.open();
    CHECK(r.wait_meter());
    CHECK_EQ(r.snap().card.label, "USB Audio CODEC"); /* back to the saved one */
}

}  // namespace

void test_unplug() {
    t_unplug_replug();
    t_unplug_idle_and_countdown();
    t_saved_card_other_index();
}
