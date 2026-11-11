/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: LOAD (Controller::load) and the list's preview (Controller::preview_take): the loaded take is the one
 * PLAY plays; loading while playing, refused while recording, deleting / renaming / replacing the loaded take, the last take
 * loaded at launch, a preview that leaves the loaded take alone. Sources are synthetic ramps.
 */
#include "helpers.hpp"

using namespace ta;

namespace {

const char *kA = "take_001_20261108-1105.wav";
const char *kB = "take_002_20261108-1106.wav";
const char *kC = "take_003_20261108-1107.wav";

bool wait_save_done(Rig &r) {
    return r.wait([](const Snapshot &x) { return x.trim.save_state == 2; }, 8000);
}
bool wait_peaks(Rig &r) {
    return r.wait([](const Snapshot &x) { return x.trim.open && x.trim.peaks_state == 2; }, 8000);
}

void t_launch_loads_last() {
    Rig r(one_card());
    make_take(r.rec(), kA, 2.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Snapshot s = r.snap();
    CHECK_EQ(s.take.name, std::string(kB)); /* the newest take is loaded at launch */
    CHECK_EQ(s.take.load_seq, 0u);          /* ... without counting a load (no animation) */
    CHECK(s.take.playing_name.empty());
    CHECK(s.deck.play_pause.enabled);
    CHECK(r.ctl().takes()[0].current);
}

void t_launch_empty() {
    Rig r(one_card());
    r.open();
    CHECK(r.snap().take.name.empty());
    CHECK(!r.snap().deck.play_pause.enabled); /* nothing in the deck */
}

void t_load_basic() {
    Rig r(one_card());
    make_take(r.rec(), kA, 2.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(!c.load("nope.wav"));
    CHECK_EQ(r.snap().take.load_seq, 0u);
    CHECK(c.load(kA));
    Snapshot s = r.snap();
    CHECK_EQ(s.take.name, std::string(kA));
    CHECK_EQ(s.take.load_seq, 1u);
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckIdle)); /* ready, NOT playing */
    CHECK_EQ(s.time.position_s, 0.0);
    CHECK(c.takes()[1].current && !c.takes()[0].current);
    /* loading the loaded take again still counts (the UI shows the cassette going in) */
    CHECK(c.load(kA));
    CHECK_EQ(r.snap().take.load_seq, 2u);
    /* PLAY plays the loaded take */
    r.sink.clear_heard();
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying) || r.snap().deck.state == kDeckIdle);
    CHECK(r.wait_state(kDeckIdle, 8000));
    CHECK_EQ(r.sink.heard().size(), 96000u);
    CHECK_EQ(r.snap().take.name, std::string(kA));
}

void t_load_while_playing() {
    Rig r(one_card());
    make_take(r.rec(), kA, 30.0);
    make_take(r.rec(), kB, 30.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(c.key_event(KeyEvent::PlayPause)); /* plays kB */
    CHECK(r.wait_state(kDeckPlaying));
    CHECK_EQ(r.snap().take.playing_name, std::string(kB));
    CHECK(c.load(kA));
    Snapshot s = r.snap();
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckIdle)); /* the playback stopped, nothing plays */
    CHECK(s.take.playing_name.empty());
    CHECK_EQ(s.take.name, std::string(kA));
    CHECK_EQ(s.time.position_s, 0.0);
    /* REW / FWD act on the loaded take */
    CHECK(c.key_event(KeyEvent::FwdDown));
    CHECK(r.wait([](const Snapshot &x) { return x.deck.state == kDeckScanningForward; }, 2000));
    CHECK_EQ(r.snap().take.playing_name, std::string(kA));
    CHECK(c.key_event(KeyEvent::FwdUp));
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    /* PLAY plays the loaded take, from its start */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK_EQ(r.snap().take.playing_name, std::string(kA));
    CHECK(r.snap().time.position_s < 2.0);
    c.key_event(KeyEvent::Stop);
}

void t_load_refused_while_recording() {
    Rig r(one_card());
    make_take(r.rec(), kA, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.key_event(KeyEvent::Rec));
    CHECK(r.wait_state(kDeckRecording));
    CHECK(!c.load(kA));
    CHECK(!c.preview_take(kA));
    CHECK_EQ(r.snap().take.load_seq, 0u);
    CHECK(c.key_event(KeyEvent::PlayPause)); /* paused recording */
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckRecordingPaused));
    CHECK(!c.load(kA));
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    /* the new take is loaded when the recording stops, without a load */
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 2u);
    CHECK(rows[0].current);
    CHECK_EQ(r.snap().take.name, rows[0].name);
    CHECK_EQ(r.snap().take.load_seq, 0u);
    /* ... and the record countdown refuses too */
    CHECK(c.set_record_timer(3));
    CHECK(c.key_event(KeyEvent::Rec));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckCountdown));
    CHECK(!c.load(kA));
    CHECK(c.key_event(KeyEvent::Stop));
}

void t_delete_loaded() {
    Rig r(one_card());
    make_take(r.rec(), kA, 2.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(c.load(kA));
    CHECK_EQ(c.delete_take(kA), 0);
    Snapshot s = r.snap();
    CHECK(s.take.name.empty()); /* the deck is empty although a take is left */
    CHECK(!s.deck.play_pause.enabled);
    CHECK_EQ(c.takes().size(), 1u);
    CHECK(!c.takes()[0].current);
    CHECK(!c.key_event(KeyEvent::PlayPause));
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckIdle));
    /* loading fills it again */
    CHECK(c.load(kB));
    CHECK_EQ(r.snap().take.name, std::string(kB));
    CHECK(r.snap().deck.play_pause.enabled);
}

void t_delete_other_keeps_loaded() {
    Rig r(one_card());
    make_take(r.rec(), kA, 2.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK_EQ(c.delete_take(kA), 0); /* kB is loaded */
    CHECK_EQ(r.snap().take.name, std::string(kB));
    CHECK_EQ(r.snap().take.load_seq, 0u);
}

void t_rename_loaded() {
    Rig r(one_card());
    make_take(r.rec(), kA, 2.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(c.load(kA));
    const unsigned seq = r.snap().take.load_seq;
    std::string nn;
    CHECK_EQ(c.rename_take(kA, "band", &nn), 0);
    Snapshot s = r.snap();
    CHECK_EQ(s.take.name, nn);      /* still loaded, under the new name */
    CHECK_EQ(s.take.load_seq, seq); /* no animation */
    CHECK(c.takes()[1].current);
    CHECK(s.deck.play_pause.enabled);
}

void t_replace_original_keeps_loaded() {
    Rig r(one_card());
    make_take(r.rec(), kA, 6.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK(c.load(kA));
    const unsigned seq = r.snap().take.load_seq;
    CHECK(c.open_trim(kA));
    CHECK(wait_peaks(r));
    c.set_position(24000);
    c.set_in();
    CHECK(c.save(SaveKind::ReplaceOriginal));
    CHECK(wait_save_done(r));
    Snapshot s = r.snap();
    CHECK(!s.take.name.empty());
    CHECK(!s.trim.save_outputs.empty());
    if (!s.trim.save_outputs.empty()) CHECK_EQ(s.take.name, s.trim.save_outputs.front()); /* still loaded */
    CHECK_EQ(s.take.load_seq, seq);
    bool found = false;
    for (const TakeRow &t : c.takes())
        if (t.current) found = t.name == s.take.name;
    CHECK(found);
    c.close_trim();
    /* a split into three parts: the first part stays loaded */
    make_take(r.rec(), kC, 6.0);
    c.refresh_takes();
    CHECK(c.load(kC));
    CHECK(c.open_trim(kC));
    CHECK(wait_peaks(r));
    c.set_position(96000);
    c.add_split();
    c.set_position(192000);
    c.add_split();
    CHECK(c.save(SaveKind::ReplaceOriginal));
    CHECK(wait_save_done(r));
    s = r.snap();
    CHECK_EQ(s.trim.save_outputs.size(), 3u);
    if (s.trim.save_outputs.size() == 3) CHECK_EQ(s.take.name, s.trim.save_outputs.front());
    c.close_trim();
}

void t_replace_other_keeps_loaded() {
    /* trimming a take that is not loaded leaves the loaded take alone */
    Rig r(one_card());
    make_take(r.rec(), kA, 6.0);
    make_take(r.rec(), kB, 2.0);
    r.open();
    Controller &c = r.ctl();
    CHECK_EQ(r.snap().take.name, std::string(kB));
    CHECK(c.open_trim(kA));
    CHECK(wait_peaks(r));
    c.set_position(24000);
    c.set_in();
    CHECK(c.save(SaveKind::ReplaceOriginal));
    CHECK(wait_save_done(r));
    CHECK_EQ(r.snap().take.name, std::string(kB));
    c.close_trim();
}

void t_preview_leaves_loaded() {
    Rig r(one_card());
    make_take(r.rec(), kA, 30.0);
    make_take(r.rec(), kB, 30.0);
    r.open();
    Controller &c = r.ctl();
    CHECK_EQ(r.snap().take.name, std::string(kB));
    CHECK(c.preview_take(kA));
    CHECK(r.wait_state(kDeckPlaying));
    Snapshot s = r.snap();
    CHECK_EQ(s.take.playing_name, std::string(kA));
    CHECK_EQ(s.take.name, std::string(kB)); /* the loaded take did not change */
    CHECK_EQ(s.take.load_seq, 0u);
    CHECK(c.takes()[0].current);
    CHECK_EQ(c.delete_take(kA), -EBUSY); /* the previewed take is in use */
    /* switching to the other take */
    CHECK(c.preview_take(kB));
    CHECK(r.wait([](const Snapshot &x) { return x.take.playing_name == kB && x.deck.state == kDeckPlaying; }, 2000));
    /* Stop rewinds: nothing plays */
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    CHECK(r.snap().take.playing_name.empty());
    CHECK_EQ(c.delete_take(kA), 0);
    /* after the preview ended PLAY plays the loaded take */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK_EQ(r.snap().take.playing_name, std::string(kB));
    c.key_event(KeyEvent::Stop);
}

void t_preview_with_empty_deck() {
    Rig r(one_card());
    make_take(r.rec(), kA, 30.0);
    make_take(r.rec(), kB, 30.0);
    r.open();
    Controller &c = r.ctl();
    CHECK_EQ(c.delete_take(kB), 0); /* the loaded one: the deck is empty */
    CHECK(r.snap().take.name.empty());
    CHECK(c.preview_take(kA));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK_EQ(r.snap().take.playing_name, std::string(kA));
    CHECK(r.snap().take.name.empty()); /* still empty */
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    CHECK(!r.snap().deck.play_pause.enabled);
}

}  // namespace

void test_load() {
    t_launch_loads_last();
    t_launch_empty();
    t_load_basic();
    t_load_while_playing();
    t_load_refused_while_recording();
    t_delete_loaded();
    t_delete_other_keeps_loaded();
    t_rename_loaded();
    t_replace_original_keeps_loaded();
    t_replace_other_keeps_loaded();
    t_preview_leaves_loaded();
    t_preview_with_empty_deck();
}
