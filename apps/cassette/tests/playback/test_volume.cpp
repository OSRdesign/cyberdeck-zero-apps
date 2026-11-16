/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback VOLUME (core/playback/output_volume.hpp, copied from Vinyl): the levels fitted to an output, the card's
 * playback control (the mute-flag fix, the loop-back controls left alone), the presses, the software volume, the output a take
 * plays on, the first-use rule and the Player's software gain.
 */
#include "check.hpp"
#include "fake_alsa.hpp"
#include "output_volume.hpp"
#include "player.hpp"
#include "pulse_api.hpp"
#include "support.hpp"

#include <cerrno>
#include <cmath>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

CtlInfo vol_ctl(const std::string &name, long mn, long mx, bool db = false, long dbmin = 0, long dbmax = 0) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Integer;
    c.capture = name.find("Capture") != std::string::npos;
    c.min = mn;
    c.max = mx;
    c.has_db = db;
    c.db_min_cdb = dbmin;
    c.db_max_cdb = dbmax;
    return c;
}

CtlInfo switch_ctl(const std::string &name) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Boolean;
    c.capture = name.find("Capture") != std::string::npos;
    c.max = 1;
    return c;
}

/* a USB card "A" with a playback control (the Apple adapter's figures: -60..0 dB in 0.5 dB steps) */
FakeCard dac_card() {
    FakeCard c = fake_uc02(1);
    c.info.id = "A";
    c.ctls.push_back(vol_ctl("Headset Playback Volume", 0, 120, true, -6000, 0));
    c.ctls.push_back(switch_ctl("Headset Playback Switch"));
    c.values["Headset Playback Volume"] = 80; /* -20 dB, set by something else */
    c.values["Headset Playback Switch"] = 1;
    return c;
}

void test_model() {
    /* an output whose range is not known (Bluetooth, software sinks) and the software volume: -60..0 dB, 30 levels */
    const OutputVolumeModel d = default_volume_model();
    CHECK(near(d.lo_db, -60.0));
    CHECK(near(d.hi_db, 0.0));
    CHECK_EQ(d.n, 30);
    CHECK(near(d.step_db(), 60.0 / 29));
    CHECK(near(d.db_of(1), -60.0)); /* level 1: the window's bottom */
    CHECK(near(d.db_of(30), 0.0));  /* level N: its top */
    CHECK_EQ(d.pct_of(0), 0);
    CHECK_EQ(d.pct_of(1), 3);
    CHECK_EQ(d.pct_of(15), 50);
    CHECK_EQ(d.pct_of(30), 100);
    for (int i = 1; i <= d.n; ++i) {
        CHECK_EQ(d.nearest(d.db_of(i)), i);
        CHECK_EQ(d.from_pct(d.pct_of(i)), i);
        if (i > 1) CHECK(near(d.db_of(i) - d.db_of(i - 1), d.step_db())); /* linear in dB */
    }
    CHECK_EQ(d.from_pct(0), 0);
    CHECK_EQ(d.from_pct(50), 15);
    CHECK_EQ(d.nearest(-500), 1);
    CHECK_EQ(d.nearest(12), 30);
    CHECK_EQ(d.step(30, 1), 30);
    CHECK_EQ(d.step(1, -1), 1);
    /* a range wider than 60 dB keeps its top 60 dB (the Pi jack: +4 dB is its 100 %) */
    const OutputVolumeModel wide = volume_model(-102.38, 4.0, 0.01);
    CHECK(near(wide.lo_db, -56.0));
    CHECK(near(wide.hi_db, 4.0));
    CHECK_EQ(wide.n, 30);
    /* N = ceil(window / 2 dB): steps about 2 dB, never under 1 dB nor the control's own step, at least 2 levels */
    for (double w : {60.0, 45.0, 34.0, 20.0, 10.0, 3.0, 2.0, 1.5, 1.0}) {
        const OutputVolumeModel m = volume_model(-w, 0);
        CHECK(m.n >= 2);
        CHECK_EQ(m.n, std::max(2, static_cast<int>(std::ceil(w / 2 - 1e-9))));
        CHECK(m.step_db() >= 1.0 - 1e-9);
        CHECK(m.step_db() <= 3.0 + 1e-9);
    }
    CHECK_EQ(volume_model(-45, 0).n, 23); /* the Sharkoon DAC: 23 levels, the middle level 12 = 52 % = -22.5 dB */
    CHECK_EQ(volume_model(-45, 0, 1.0).pct_of(12), 52);
    CHECK(near(volume_model(-45, 0, 1.0).db_of(12), -22.5));
    const OutputVolumeModel coarse = volume_model(-30, 0, 3.0); /* a control of 3 dB steps: its own steps */
    CHECK_EQ(coarse.n, 11);
    CHECK(near(coarse.step_db(), 3.0));
    CHECK_EQ(volume_model(-0.5, 0).n, 2);
    const OutputVolumeModel one = volume_model(0, 0);
    CHECK_EQ(one.n, 1);
    CHECK_EQ(one.pct_of(1), 100);
    /* the sound server's dB (pa_sw_volume_from_dB / to_dB) */
    CHECK_EQ(sink_volume_for_db(0), 0x10000u);
    CHECK_EQ(sink_volume_for_db(-60), 6554u);
    CHECK_EQ(sink_volume_for_db(-1e9), 0u);
    CHECK(near(sink_db_for_volume(0x10000), 0.0));
    CHECK(std::isinf(sink_db_for_volume(0)));
    for (int i = 1; i <= d.n; ++i) CHECK_EQ(sink_level_for_volume(d, sink_volume_for_db(d.db_of(i))), i);
    CHECK_EQ(sink_level_for_volume(d, 0), 0);
    CHECK(placeholder_sink("auto_null"));
    CHECK(!placeholder_sink("alsa_output.usb-x"));
    /* a sink's levels: its card's range moved under the sink's 0 dB */
    MixerChoice dac;
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, dac));
    const OutputVolumeModel sm = sink_volume_model(&dac);
    CHECK(near(sm.hi_db, 0.0));
    CHECK(near(sm.lo_db, -45.0));
    CHECK_EQ(sm.n, 23);
    CHECK_EQ(sink_volume_model(nullptr).n, 30);
    /* the texts */
    CHECK_EQ(volume_log_text(-1), std::string("-- (not known)"));
    CHECK_EQ(volume_log_text(52), std::string("52 %"));
    CHECK_EQ(volume_model_text(volume_model(-45, 0)), std::string("-45.00..0.00 dB, 23 levels of 2.05 dB"));
    CHECK_EQ(volume_set_log_text("hw:CARD=A 'PCM'", volume_model(-45, 0), 12, 23, 0, 23),
             std::string("set hw:CARD=A 'PCM' step 12/23 52 % target -22.50 dB raw=23 read back raw=23"));
    /* the ALSA card behind a sink */
    CHECK_EQ(pulse_alsa_card("1", "", ""), 1);
    CHECK_EQ(pulse_alsa_card("", "2", "hw:5"), 2);
    CHECK_EQ(pulse_alsa_card("", "", "hw:3"), 3);
    CHECK_EQ(pulse_alsa_card("", "", "front:1"), 1);
    CHECK_EQ(pulse_alsa_card("", "", "hw:CARD=2,DEV=0"), 2);
    CHECK_EQ(pulse_alsa_card("", "", "hw:CARD=Device"), -1);
    CHECK_EQ(pulse_alsa_card("", "", ""), -1);
}

void test_mixer_choice() {
    MixerChoice m;
    /* Master, PCM, Speaker, Headphone, Headset first; never a capture control */
    CHECK(choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), vol_ctl("PCM Playback Volume", 0, 45),
                                 vol_ctl("Master Playback Volume", 0, 87)},
                                m));
    CHECK_EQ(m.volume, std::string("Master Playback Volume"));
    /* another playback volume when none of those: with its switch */
    CHECK(choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), vol_ctl("Output Playback Volume", 0, 20),
                                 switch_ctl("Output Playback Switch")},
                                m));
    CHECK_EQ(m.volume, std::string("Output Playback Volume"));
    CHECK_EQ(m.sw, std::string("Output Playback Switch"));
    /* an input's loop-back into the output is not the output's level (the PCM2902's "Mic Playback Volume") */
    CHECK(!choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 16, true, 0, 2381), vol_ctl("Mic Playback Volume", 0, 16, true, -3000, 0),
                                  switch_ctl("Mic Playback Switch")},
                                 m));
    CHECK(!choose_playback_mixer({vol_ctl("Line Playback Volume", 0, 31)}, m));
    CHECK(choose_playback_mixer({vol_ctl("Mic Playback Volume", 0, 16), vol_ctl("Speaker Playback Volume", 0, 37)}, m));
    CHECK_EQ(m.volume, std::string("Speaker Playback Volume"));
    CHECK(!choose_playback_mixer({}, m));
    /* a control whose real top is above 0 dB keeps it: -87..+12 dB over 0..99 */
    CHECK(choose_playback_mixer({vol_ctl("Master Playback Volume", 0, 99, true, -8700, 1200)}, m));
    CHECK(near(m.model.hi_db, 12.0));
    CHECK(near(m.model.lo_db, -48.0));
    CHECK_EQ(mixer_raw_for_level(m, m.model.n), 99L);
    /* a headset whose driver gives no dB: its raw steps, at most 30 levels */
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 37)}, m));
    CHECK(!m.has_db);
    CHECK(!m.model.db);
    CHECK_EQ(m.model.n, 30);
    CHECK_EQ(mixer_raw_for_level(m, 30), 37L);
    for (int i = 1; i <= 30; ++i) CHECK_EQ(mixer_level_for_raw(m, mixer_raw_for_level(m, i)), i);
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 7)}, m));
    CHECK_EQ(m.model.n, 7);
    /* the Raspberry Pi jack: a dB scale with its mute flag, -10239..400 in hundredths; ALSA reports min as the mute sentinel */
    CtlInfo pi = vol_ctl("PCM Playback Volume", -10239, 400, true, kAlsaDbGainMute, 400);
    pi.db_next_known = true;
    pi.db_next_cdb = -10238;
    CHECK(choose_playback_mixer({pi}, m));
    CHECK(m.min_is_mute);
    CHECK(m.has_db);
    CHECK_EQ(m.db_min, -10239L);
    CHECK(near(m.model.hi_db, 4.0));
    CHECK(near(m.model.lo_db, -56.0));
    CHECK_EQ(m.model.n, 30);
    CHECK_EQ(mixer_level_for_raw(m, -10239), 0); /* the mute value */
    CHECK_EQ(mixer_raw_for_level(m, 0), -10239L);
    CHECK_EQ(mixer_raw_for_level(m, 30), 400L);
    CHECK_EQ(mute_by(m), MuteBy::MuteValue);
    /* the same without the dB of min + 1: no dB data, linear in steps */
    pi.db_next_known = false;
    CHECK(choose_playback_mixer({pi}, m));
    CHECK(m.min_is_mute);
    CHECK(!m.has_db);
    /* alsa_fix_mute_db leaves a normal control alone */
    CtlInfo normal = vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0);
    alsa_fix_mute_db(normal);
    CHECK(normal.has_db);
    CHECK(!normal.min_is_mute);
    CHECK_EQ(normal.db_min_cdb, -4500L);
    /* the card behind an ALSA device */
    CHECK_EQ(mixer_card_for("plughw:CARD=UC02,DEV=0"), std::string("hw:CARD=UC02"));
    CHECK_EQ(mixer_card_for("hw:CARD=Device,DEV=0"), std::string("hw:CARD=Device"));
    CHECK_EQ(mixer_card_for("default"), std::string("default"));
    CHECK_EQ(mixer_card_for("plughw:1,0"), std::string("default"));
}

void test_mixer_rw() {
    FakeAlsa a;
    a.cards_.push_back(dac_card());
    std::string chosen;
    MixerChoice c;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.has_mixer);
    CHECK_EQ(c.volume, std::string("Headset Playback Volume"));
    CHECK_EQ(lv.level, 20); /* the nearest level to -20 dB: -60 + 19 * 60 / 29 = -20.69 dB */
    CHECK_EQ(lv.pct, 67);
    CHECK(!lv.muted);
    CHECK_EQ(write_mixer_level(a, "hw:CARD=A", c, 21), 0); /* one level up: -18.62 dB */
    CHECK_EQ(a.cards_[0].values["Headset Playback Volume"], 83L);
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK_EQ(lv.level, 21);
    CHECK_EQ(lv.pct, 70);
    CHECK_EQ(write_mixer_switch(a, "hw:CARD=A", c, false), 0); /* muted by the switch, the level kept */
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.muted);
    CHECK_EQ(lv.level, 21);
    CHECK_EQ(write_mixer_level(a, "hw:CARD=A", c, 1), 0); /* a level: the switch on again */
    CHECK_EQ(a.cards_[0].values["Headset Playback Switch"], 1L);
    CHECK_EQ(a.cards_[0].values["Headset Playback Volume"], 0L);
    a.cards_[0].values["Headset Playback Volume"] = 61; /* changed elsewhere (alsamixer, the launcher's keys) */
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK_EQ(lv.level, 16);
    a.cards_[0].present = false; /* unplugged: an error, no mixer; back: found again */
    CHECK(read_mixer(a, "hw:CARD=A", chosen, c, lv) < 0);
    CHECK(!lv.has_mixer);
    a.cards_[0].present = true;
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.has_mixer);
    /* a card without a playback control (the UC02 preset): none, the software volume then; nothing written */
    FakeAlsa bare;
    bare.cards_.push_back(fake_uc02(0));
    chosen.clear();
    CHECK_EQ(read_mixer(bare, "hw:CARD=UC02", chosen, c, lv), 0);
    CHECK(!lv.has_mixer);
    CHECK_EQ(write_mixer_level(bare, "hw:CARD=UC02", c, 5), -ENOENT);
    CHECK(bare.mixer_writes.empty());
}

void test_presses() {
    MixerChoice m;
    CHECK(choose_playback_mixer({vol_ctl("Speaker Playback Volume", 0, 45, true, -4500, 0), switch_ctl("Speaker Playback Switch")}, m));
    MixerLevel lv;
    lv.has_mixer = true;
    lv.level = 5;
    VolumePress p = mixer_press(m, lv, 1);
    CHECK(p.kind == VolumePress::Kind::Set);
    CHECK_EQ(p.level, 6);
    lv.level = 1; /* "-" at the first level mutes (the switch) */
    p = mixer_press(m, lv, -1);
    CHECK(p.kind == VolumePress::Kind::Mute);
    lv.muted = true; /* muted: "-" does nothing, "+" brings the level back */
    CHECK(mixer_press(m, lv, -1).kind == VolumePress::Kind::None);
    p = mixer_press(m, lv, 1);
    CHECK(p.kind == VolumePress::Kind::Unmute);
    CHECK_EQ(p.level, 1);
    lv.muted = false;
    lv.level = m.model.n; /* at the top: nothing */
    CHECK(mixer_press(m, lv, 1).kind == VolumePress::Kind::None);
    /* a control with neither switch nor mute value: Cassette's own mute */
    MixerChoice bare;
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, bare));
    CHECK_EQ(mute_by(bare), MuteBy::Cassette);
    lv.level = 1;
    CHECK(mixer_press(bare, lv, -1).kind == VolumePress::Kind::Mute);
    MixerLevel shown = mixer_level_shown(bare, lv, true);
    CHECK(shown.muted);
    CHECK(!mixer_level_shown(m, lv, true).muted); /* a switch: Cassette's own mute is never in play */
    /* the sink */
    const OutputVolumeModel d = default_volume_model();
    p = sink_press(d, 15, false, 1);
    CHECK(p.kind == VolumePress::Kind::Set);
    CHECK_EQ(p.level, 16);
    CHECK(sink_press(d, 1, false, -1).kind == VolumePress::Kind::Mute);
    CHECK(sink_press(d, 15, true, -1).kind == VolumePress::Kind::None);
    p = sink_press(d, 15, true, 1);
    CHECK(p.kind == VolumePress::Kind::Unmute);
    CHECK_EQ(p.level, 15);
    p = sink_press(d, 0, false, 1); /* a silent sink: "+" gives the first level */
    CHECK(p.kind == VolumePress::Kind::Set);
    CHECK_EQ(p.level, 1);
    CHECK(sink_press(d, -1, false, 1).kind == VolumePress::Kind::None); /* not known */
    /* the software volume: 100 % until changed, a level a press, "-" at level 1 mutes and keeps the level */
    OwnVolume own;
    bool muted = true;
    CHECK_EQ(software_level(own, &muted), 30);
    CHECK(!muted);
    p = software_press(own, -1);
    CHECK(p.kind == VolumePress::Kind::Set);
    CHECK_EQ(own.pct, 97);
    own.pct = 3;
    p = software_press(own, -1);
    CHECK(p.kind == VolumePress::Kind::Mute);
    CHECK(own.muted);
    CHECK_EQ(own.pct, 3);
    CHECK(software_press(own, -1).kind == VolumePress::Kind::None);
    p = software_press(own, 1);
    CHECK(p.kind == VolumePress::Kind::Unmute);
    CHECK(!own.muted);
    own.pct = 0; /* an old 0: muted with no level to come back at */
    CHECK_EQ(software_level(own, &muted), 0);
    CHECK(muted);
    CHECK(near(volume_gain_for_db(0.0), 1.0, 1e-6));
    CHECK(near(volume_gain_for_db(-60.0), 0.001, 1e-6));
    CHECK(near(20.0 * std::log10(volume_gain_for_db(d.db_of(15))), d.db_of(15), 1e-4));
}

void test_target_and_first_use() {
    /* a card: its mixer */
    VolumeTarget t = volume_target_for("hw:CARD=UC02,DEV=0", false, "", false, false, "");
    CHECK(t.kind == VolumeTarget::Kind::Mixer);
    CHECK_EQ(t.card, std::string("hw:CARD=UC02"));
    CHECK_EQ(t.key, std::string("alsa:hw:CARD=UC02"));
    /* a card while a server runs: still its own mixer (Cassette opens the card itself) */
    t = volume_target_for("hw:CARD=UC02,DEV=0", false, "", true, true, "alsa_output.x");
    CHECK(t.kind == VolumeTarget::Kind::Mixer);
    /* the default output without a server: the default card's mixer */
    t = volume_target_for("default", false, "", false, false, "");
    CHECK(t.kind == VolumeTarget::Kind::Mixer);
    CHECK_EQ(t.card, std::string("default"));
    CHECK_EQ(t.key, std::string("alsa:default"));
    /* the default output through the server: its default sink; not known yet, or the placeholder: none */
    t = volume_target_for("default", false, "", true, true, "alsa_output.usb-Sharkoon");
    CHECK(t.kind == VolumeTarget::Kind::Sink);
    CHECK_EQ(t.sink, std::string("alsa_output.usb-Sharkoon"));
    CHECK_EQ(t.key, std::string("pulse:alsa_output.usb-Sharkoon"));
    CHECK(volume_target_for("default", false, "", true, true, "").kind == VolumeTarget::Kind::None);
    CHECK(volume_target_for("default", false, "", true, true, "auto_null").kind == VolumeTarget::Kind::None);
    CHECK(volume_target_for("default", false, "", false, true, "x").kind == VolumeTarget::Kind::None); /* not answering */
    /* a Bluetooth device: its sink (the key by MAC, whatever the node's name); no sink now: none, the key kept */
    t = volume_target_for("bt:70:bf:92:53:6b:8b", true, "bluez_output.70_BF_92_53_6B_8B.1", true, true, "");
    CHECK(t.kind == VolumeTarget::Kind::Sink);
    CHECK_EQ(t.key, std::string("bt:70:BF:92:53:6B:8B"));
    t = volume_target_for("bt:70:BF:92:53:6B:8B", true, "", true, true, "");
    CHECK(t.kind == VolumeTarget::Kind::None);
    CHECK_EQ(t.key, std::string("bt:70:BF:92:53:6B:8B"));
    /* the first use of a silent output: muted, level 0 or the bottom level; not known: no */
    CHECK(first_use_silent(0, false));
    CHECK(first_use_silent(1, false));
    CHECK(first_use_silent(12, true));
    CHECK(!first_use_silent(2, false));
    CHECK(!first_use_silent(-1, false));
    CHECK_EQ(default_volume_model().from_pct(kFirstUsePct), 15);
    CHECK_EQ(volume_model(-45, 0, 1.0).from_pct(kFirstUsePct), 12); /* the Sharkoon: 52 % */
}

/* The Player's software gain: 1 leaves the samples bit for bit, 0.5 halves them, 0 silences; the level tap stays before it. */
void test_player_gain(TempDir &dir) {
    const std::string path = dir.file("gain.wav");
    std::vector<int32_t> s;
    for (int i = 0; i < 4800; ++i) s.push_back((i % 200 - 100) * 200); /* a saw, -20000..19800 */
    write_wav(path, afmt(48000, 1, 16), s);
    for (float g : {1.0f, 0.5f, 0.0f}) {
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK(near(p.gain(), 1.0));
        p.set_gain(g);
        CHECK(near(p.gain(), g));
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        while (p.step()) {
        }
        const std::vector<int32_t> h = sink.heard();
        CHECK_EQ(h.size(), s.size());
        bool ok = h.size() == s.size();
        for (size_t i = 0; ok && i < h.size(); ++i) ok = h[i] == static_cast<int32_t>(std::lround(s[i] * static_cast<double>(g)));
        CHECK(ok);
        float pk[2];
        p.level(pk);
        CHECK(pk[0] > 0.5f); /* the tap: before the gain */
    }
    FakeSink sink;
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    p.set_gain(3.0f); /* never above 1 */
    CHECK(near(p.gain(), 1.0));
    p.set_gain(-1.0f);
    CHECK(near(p.gain(), 0.0));
}

}  // namespace

void test_volume() {
    test_model();
    test_mixer_choice();
    test_mixer_rw();
    test_presses();
    test_target_and_first_use();
    TempDir d;
    test_player_gain(d);
}
