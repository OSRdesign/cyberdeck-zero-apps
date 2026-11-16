/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the playback VOLUME end to end (Controller::volume_step, Snapshot::volume, the owner's request of
 * 2026-11-16 with Vinyl's model). On the fakes: a card without a playback control (Cassette's software volume, the Player's
 * gain heard in the samples, Cassette's own mute), a card with one (its control set in the output's own levels, the
 * first-use rule, the switch's mute, a level changed elsewhere followed), a Bluetooth speaker through the sound server (the
 * sink's own volume set through the worker, the first-use rule). CARD and SOFT never move. All audio is synthetic.
 */
#include "bt_rig.hpp"

#include "output_volume.hpp"
#include "settings.hpp"

#include <cmath>

using namespace ta;
using namespace tbt;

namespace {

const char *kTake = "take_001_20261108-1105.wav";
const std::string kSpk = "9E:0C:81:E6:5D:1A";
const std::string kSpkId = "bt:9E:0C:81:E6:5D:1A";

/* a take whose every sample is `value` (mono, 48 kHz): the Player's gain shows exactly in what the sink hears */
void make_const_take(const std::string &dir, double seconds, int32_t value) {
    posix_file_ops().mkdir(dir);
    tp::write_wav(dir + "/" + kTake, mono48(), std::vector<int32_t>(static_cast<size_t>(seconds * 48000), value));
}

CtlInfo ctl_int(const std::string &name, long mn, long mx, long dbmin, long dbmax) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Integer;
    c.min = mn;
    c.max = mx;
    c.has_db = true;
    c.db_min_cdb = dbmin;
    c.db_max_cdb = dbmax;
    return c;
}
CtlInfo ctl_bool(const std::string &name) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Boolean;
    c.max = 1;
    return c;
}

/* the PCM2902 preset with an output level: "Speaker Playback Volume" -37..0 dB in 1 dB steps and its switch */
FakeCard card_with_output(long start) {
    FakeCard c = fake_pcm2902(0);
    c.ctls.push_back(ctl_int("Speaker Playback Volume", 0, 37, -3700, 0));
    c.ctls.push_back(ctl_bool("Speaker Playback Switch"));
    c.values["Speaker Playback Volume"] = start;
    c.values["Speaker Playback Switch"] = 1;
    return c;
}
MixerChoice speaker_choice() {
    MixerChoice m;
    choose_playback_mixer(card_with_output(0).ctls, m);
    return m;
}

bool all_near(const std::vector<int32_t> &h, long want, long tol = 1) {
    if (h.empty()) return false;
    for (int32_t v : h)
        if (std::labs(static_cast<long>(v) - want) > tol) return false;
    return true;
}

PlaybackVolume entry_of(const std::string &home, const std::string &key, bool *found) {
    SettingsStore s(posix_file_ops(), default_settings_path(home));
    s.load();
    auto it = s.values().playback_volume.find(key);
    *found = it != s.values().playback_volume.end();
    return *found ? it->second : PlaybackVolume();
}

/* plays a moment from where the take is paused, pauses again, and returns what the sink heard in that moment */
std::vector<int32_t> hear_a_moment(Rig &r) {
    r.sink.clear_heard();
    const double p0 = r.snap().time.position_s;
    r.ctl().key_event(KeyEvent::PlayPause);
    r.wait([p0](const Snapshot &x) { return x.time.position_s > p0 + 0.3; }, 3000);
    r.ctl().key_event(KeyEvent::PlayPause);
    r.wait_state(kDeckPlayingPaused);
    return r.sink.heard();
}

/* a card without a playback control (the PCM2902 preset: its "Mic Playback Volume" is the microphone's loop-back, not the
 * output): Cassette's software volume, 100 % until changed, in 30 levels over -60..0 dB */
void t_software() {
    Rig r(one_card());
    make_const_take(r.rec(), 60.0, 16000);
    r.sink.sleep_us = 8000;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    const long cap_before = r.alsa.mixer_value("Device", "Mic Capture Volume");
    const double soft_before = r.snap().card.soft_gain_db;
    /* idle: nothing to set */
    CHECK(!r.snap().volume.active);
    CHECK(!c.volume_step(1));
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(r.wait([](const Snapshot &x) { return x.volume.active && x.volume.pct == 100; }, 2000));
    Snapshot s = r.snap();
    CHECK(s.volume.software);
    CHECK(!s.volume.muted);
    CHECK_EQ(s.volume.output, "alsa:hw:CARD=Device");
    CHECK(s.volume.can_down);
    CHECK(!s.volume.can_up); /* the top */
    CHECK(!c.volume_step(1));
    /* paused: still VOLUME; 15 levels down = level 15 of 30 = 50 % (-31.03 dB) */
    CHECK(c.key_event(KeyEvent::PlayPause));
    CHECK(r.wait_state(kDeckPlayingPaused));
    CHECK(r.snap().volume.active);
    for (int i = 0; i < 15; ++i) CHECK(c.volume_step(-1));
    s = r.snap();
    CHECK_EQ(s.volume.pct, 50);
    CHECK(s.volume.can_up);
    const OutputVolumeModel m = default_volume_model();
    const long want = std::lround(16000.0 * volume_gain_for_db(m.db_of(15)));
    CHECK(all_near(hear_a_moment(r), want));
    /* CARD and SOFT are not touched by VOLUME */
    CHECK_EQ(r.alsa.mixer_value("Device", "Mic Capture Volume"), cap_before);
    CHECK(r.snap().card.soft_gain_db == soft_before);
    /* down to level 1, then "-" mutes (Cassette's own mute: the Player silent, the level kept) */
    for (int i = 0; i < 14; ++i) CHECK(c.volume_step(-1));
    CHECK_EQ(r.snap().volume.pct, 3);
    CHECK(c.volume_step(-1));
    s = r.snap();
    CHECK(s.volume.muted);
    CHECK_EQ(s.volume.pct, 3);
    CHECK(!s.volume.can_down);
    CHECK(!c.volume_step(-1)); /* muted: "-" does nothing */
    CHECK(all_near(hear_a_moment(r), 0, 0));
    CHECK(c.volume_step(1)); /* "+": back at the level it had */
    s = r.snap();
    CHECK(!s.volume.muted);
    CHECK_EQ(s.volume.pct, 3);
    CHECK(c.volume_step(1));
    CHECK_EQ(r.snap().volume.pct, 7);
    /* STOP: no VOLUME any more (the recorder shows CARD and SOFT) and the keys do nothing */
    CHECK(c.key_event(KeyEvent::Stop));
    CHECK(r.wait_state(kDeckIdle));
    CHECK(!r.snap().volume.active);
    CHECK(!c.volume_step(1));
    /* remembered for that output (written within a second, and at the end) */
    r.close();
    bool found = false;
    const PlaybackVolume e = entry_of(r.home, "alsa:hw:CARD=Device", &found);
    CHECK(found);
    CHECK_EQ(e.pct, 7);
    CHECK(!e.muted);
    /* the next run plays at that level from the start */
    r.open();
    CHECK(r.wait_meter());
    CHECK(r.ctl().key_event(KeyEvent::PlayPause));
    CHECK(r.wait([](const Snapshot &x) { return x.volume.active && x.volume.pct == 7; }, 2000));
    r.ctl().key_event(KeyEvent::PlayPause);
    CHECK(r.wait_state(kDeckPlayingPaused));
    CHECK(all_near(hear_a_moment(r), std::lround(16000.0 * volume_gain_for_db(m.db_of(2)))));
}

/* a card with a playback control: its own levels (-37..0 dB, 19 levels), the first-use rule, the switch, a level changed
 * elsewhere (the launcher's keys, alsamixer) followed within a few ticks; the samples go out untouched */
void t_mixer() {
    tp::TempDir home;
    const MixerChoice mc = speaker_choice();
    CHECK_EQ(mc.model.n, 19);
    {
        Rig r({card_with_output(0)}, home.path); /* at its bottom: silent at first use */
        make_const_take(r.rec(), 60.0, 16000);
        r.sink.sleep_us = 8000;
        r.open();
        Controller &c = r.ctl();
        CHECK(r.wait_meter());
        const long cap_before = r.alsa.mixer_value("Device", "Mic Capture Volume");
        CHECK(c.key_event(KeyEvent::PlayPause));
        CHECK(r.wait_state(kDeckPlaying));
        /* the first use of a silent output: the level nearest 50 % (level 10 of 19, 53 %) */
        const int first = mc.model.from_pct(kFirstUsePct);
        CHECK(r.wait([&](const Snapshot &x) { return x.volume.active && x.volume.pct == mc.model.pct_of(first); }, 2000));
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Volume"), mixer_raw_for_level(mc, first));
        Snapshot s = r.snap();
        CHECK(!s.volume.software);
        CHECK_EQ(s.volume.output, "alsa:hw:CARD=Device");
        CHECK(c.key_event(KeyEvent::PlayPause));
        CHECK(r.wait_state(kDeckPlayingPaused));
        CHECK(all_near(hear_a_moment(r), 16000, 0)); /* the card's level does the work: the samples untouched */
        CHECK(c.volume_step(1));
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Volume"), mixer_raw_for_level(mc, first + 1));
        CHECK_EQ(r.snap().volume.pct, mc.model.pct_of(first + 1));
        /* changed elsewhere: the stepper follows */
        r.alsa.with([&](FakeAlsa &a) { a.find_id("Device")->values["Speaker Playback Volume"] = mixer_raw_for_level(mc, 4); });
        CHECK(r.wait([&](const Snapshot &x) { return x.volume.pct == mc.model.pct_of(4); }, 2000));
        /* down to level 1, then "-": the switch mutes, the level kept */
        for (int i = 0; i < 3; ++i) CHECK(c.volume_step(-1));
        CHECK_EQ(r.snap().volume.pct, mc.model.pct_of(1));
        CHECK(c.volume_step(-1));
        CHECK(r.wait([](const Snapshot &x) { return x.volume.muted; }, 1000));
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Switch"), 0L);
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Volume"), mixer_raw_for_level(mc, 1));
        CHECK(c.volume_step(1));
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Switch"), 1L);
        CHECK(!r.snap().volume.muted);
        CHECK_EQ(r.alsa.mixer_value("Device", "Mic Capture Volume"), cap_before);
        CHECK(c.key_event(KeyEvent::Stop));
        CHECK(r.wait_state(kDeckIdle));
        CHECK(!r.snap().volume.active);
    }
    bool found = false;
    CHECK_EQ(entry_of(home.path, "alsa:hw:CARD=Device", &found).pct, mc.model.pct_of(1));
    CHECK(found);
    {   /* the next run: the card at its bottom again (a reboot), the rule has run for it: it stays where it is */
        Rig r({card_with_output(0)}, home.path);
        r.sink.sleep_us = 8000;
        r.open();
        CHECK(r.wait_meter());
        CHECK(r.ctl().key_event(KeyEvent::PlayPause));
        CHECK(r.wait([&](const Snapshot &x) { return x.volume.active && x.volume.pct == mc.model.pct_of(1); }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Volume"), 0L);
        r.ctl().key_event(KeyEvent::Stop);
        CHECK(r.wait_state(kDeckIdle));
    }
    {   /* a card that is not silent at first use keeps its level */
        tp::TempDir h2;
        Rig r({card_with_output(30)}, h2.path);
        make_const_take(r.rec(), 30.0, 16000);
        r.sink.sleep_us = 8000;
        r.open();
        CHECK(r.wait_meter());
        CHECK(r.ctl().key_event(KeyEvent::PlayPause));
        const int lv = mixer_level_for_raw(mc, 30);
        CHECK(r.wait([&](const Snapshot &x) { return x.volume.active && x.volume.pct == mc.model.pct_of(lv); }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK_EQ(r.alsa.mixer_value("Device", "Speaker Playback Volume"), 30L);
        r.ctl().key_event(KeyEvent::Stop);
        CHECK(r.wait_state(kDeckIdle));
        r.close();
        bool f2 = false;
        CHECK_EQ(entry_of(h2.path, "alsa:hw:CARD=Device", &f2).pct, mc.model.pct_of(lv)); /* the rule ran: the entry is there */
        CHECK(f2);
    }
}

/* a Bluetooth speaker through the sound server: the sink's own volume (-60..0 dB, no card behind it), set on the worker */
void t_sink(int64_t start_raw) {
    BtRig b(one_card());
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        PulseCard pc = fake_bt_card(kSpk, 71, true, "a2dp-sink", false);
        pc.description = "XWF-M18-M28-M38";
        b.pulse.cards.push_back(pc);
    }
    PulseDevice spk = fake_bt_sink(kSpk, 71, "speaker");
    spk.volume_raw = start_raw;
    b.pulse.add_sink(spk);
    make_const_take(b.r.rec(), 30.0, 16000);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    CHECK(b.r.wait([](const Snapshot &s) {
        for (const OutputSnap &o : s.card.outputs)
            if (o.id == kSpkId) return true;
        return false;
    }, 4000));
    CHECK(c.select_output(kSpkId));
    c.key_event(KeyEvent::PlayPause);
    CHECK(b.r.wait_state(kDeckPlaying));
    const OutputVolumeModel m = default_volume_model();
    const bool silent = start_raw == 0;
    const int start = silent ? m.from_pct(kFirstUsePct) : sink_level_for_volume(m, static_cast<uint32_t>(start_raw));
    CHECK(b.r.wait([&](const Snapshot &x) { return x.volume.active && x.volume.pct == m.pct_of(start); }, 3000));
    CHECK_EQ(b.r.snap().volume.output, kSpkId);
    CHECK(!b.r.snap().volume.software);
    auto sink_raw = [&] {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        for (const PulseDevice &d : b.pulse.sinks)
            if (d.name == spk.name) return d.volume_raw;
        return int64_t{-2};
    };
    if (silent) { /* the first use of a silent output: set near 50 % through the worker */
        CHECK(b.r.wait([&](const Snapshot &) { return sink_raw() == static_cast<int64_t>(sink_volume_for_db(m.db_of(start))); }, 2000));
    } else {
        CHECK(sink_raw() == start_raw); /* not silent: left as it is */
    }
    CHECK(c.volume_step(1));
    CHECK(b.r.wait([&](const Snapshot &) { return sink_raw() == static_cast<int64_t>(sink_volume_for_db(m.db_of(start + 1))); }, 2000));
    CHECK(b.r.wait([&](const Snapshot &x) { return x.volume.pct == m.pct_of(start + 1); }, 2000));
    /* the server reports another level (the headset's buttons, the launcher's keys on the default sink): followed */
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        for (PulseDevice &d : b.pulse.sinks)
            if (d.name == spk.name) d.volume_raw = sink_volume_for_db(m.db_of(1));
    }
    b.pulse.add_card(fake_bt_card("00:11:22:33:44:55", 90, true, "a2dp-sink")); /* any server event: a re-listing */
    CHECK(b.r.wait([&](const Snapshot &x) { return x.volume.pct == m.pct_of(1); }, 3000));
    auto sink_muted = [&] {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        for (const PulseDevice &d : b.pulse.sinks)
            if (d.name == spk.name) return d.muted;
        return false;
    };
    CHECK(b.r.wait([](const Snapshot &x) { return x.volume.pct == default_volume_model().pct_of(1) && !x.volume.muted; }, 2000));
    CHECK(c.volume_step(-1)); /* level 1: "-" mutes the sink, the level kept */
    CHECK(b.r.wait([&](const Snapshot &x) { return x.volume.muted && sink_muted(); }, 2000));
    CHECK(sink_raw() == static_cast<int64_t>(sink_volume_for_db(m.db_of(1))));
    CHECK(c.volume_step(1));
    CHECK(b.r.wait([&](const Snapshot &x) { return !x.volume.muted && !sink_muted(); }, 2000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK(!b.r.snap().volume.active);
}

}  // namespace

void test_volume_app() {
    t_software();
    t_mixer();
    t_sink(0x8000);                     /* not silent: -18 dB, its own level kept */
    t_sink(0);                          /* silent: the first use sets it near 50 % */
}
