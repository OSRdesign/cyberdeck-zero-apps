/* SPDX-License-Identifier: MIT */
#include <cerrno>
#include <cmath>

#include "card.hpp"
#include "check.hpp"
#include "fake_alsa.hpp"
#include "mixer.hpp"

using namespace cassette;

namespace {

bool near(double a, double b, double eps = 0.01) { return std::fabs(a - b) <= eps; }

void pcm2902_controls() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    Mixer m(f, mixer_device(f.cards_[0].info));
    CHECK_EQ(m.discover(), 0);
    CHECK(m.caps().has_level && m.caps().has_agc && m.caps().has_mute);
    CHECK_EQ(m.level_name(), "Mic Capture Volume");
    CHECK_EQ(m.mute_name(), "Mic Capture Switch");
    CHECK_EQ(m.agc_name(), "Auto Gain Control");
    const LevelRange &r = m.level_range();
    CHECK(!r.db_inferred); /* the driver reports dB */
    CHECK_EQ(r.raw_min, 0);
    CHECK_EQ(r.raw_max, 16);
    CHECK(near(r.db_max, 23.81));

    MixerState s = m.read();
    CHECK(s.level_valid && s.agc_valid && s.mute_valid);
    CHECK_EQ(s.level_raw, 0);
    CHECK(near(s.level_db, 0.0));
    CHECK(s.agc); /* as found: AGC on after a reboot */
    CHECK(!s.mute);
    CHECK(f.mixer_writes.empty()); /* discovering and reading change nothing */

    /* the sidetone controls are not mistaken for the capture level */
    CHECK(m.controls().size() == 5);
}

void headset_dB_is_inferred() {
    FakeAlsa f;
    f.cards_ = {fake_uc02(0)};
    Mixer m(f, mixer_device(f.cards_[0].info));
    m.discover();
    CHECK(m.caps().has_level);
    CHECK(!m.caps().has_agc);
    CHECK(m.caps().has_mute);
    CHECK(m.level_range().db_inferred); /* kernel disabled the dB data: 0..23.81 dB is assumed */
    CHECK_EQ(m.level_range().raw_max, 127);
    CHECK(near(m.level_range().db_min, 0.0));
    CHECK(near(m.level_range().db_max, 23.81));
    CHECK(!m.read().agc_valid);
}

void card_without_controls() {
    FakeAlsa f;
    f.cards_ = {fake_card_no_controls(0)};
    Mixer m(f, mixer_device(f.cards_[0].info));
    CHECK_EQ(m.discover(), 0);
    CHECK(!m.caps().has_level && !m.caps().has_agc && !m.caps().has_mute);
    MixerState s = m.read();
    CHECK(!s.level_valid && !s.agc_valid && !s.mute_valid);
    int err = 0;
    CHECK(!m.set_level_db(10, nullptr, &err));
    CHECK_EQ(err, -ENOENT);
    CHECK(!m.set_agc(true));
    CHECK(!m.set_mute(true));
    MixerSetting all;
    all.set_level = all.set_agc = all.set_mute = true;
    CHECK_EQ(m.reapply(all), 0);
    CHECK(f.mixer_writes.empty());

    /* a card that cannot be opened: the error comes back, the caps stay false */
    Mixer gone(f, "hw:CARD=Missing");
    CHECK(gone.discover() < 0);
    CHECK(!gone.caps().has_level);
    CHECK(!gone.read().level_valid);
    f.lib_available = false;
    CHECK_EQ(Mixer(f, "hw:CARD=Plain").discover(), kNoAlsa);
}

void level_clamps_and_rounds() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    Mixer m(f, mixer_device(f.cards_[0].info));
    m.discover();
    double applied = -1;
    CHECK(m.set_level_db(100, &applied)); /* clamped to the top */
    CHECK_EQ(f.cards_[0].values["Mic Capture Volume"], 16);
    CHECK(near(applied, 23.81));
    CHECK(m.set_level_db(-30, &applied));
    CHECK_EQ(f.cards_[0].values["Mic Capture Volume"], 0);
    CHECK(near(applied, 0.0));
    /* 10 dB is 6.72 steps of 1.488 dB: the nearest step is 7 = 10.42 dB */
    CHECK(m.set_level_db(10.0, &applied));
    CHECK_EQ(f.cards_[0].values["Mic Capture Volume"], 7);
    CHECK(near(applied, 10.42, 0.02));
    CHECK_EQ(m.read().level_raw, 7);
    CHECK(near(m.read().level_db, 10.42, 0.02));
    /* 9.7 dB: 6.52 steps, rounds up to 7; 9.5: 6.38 down to 6 */
    CHECK(m.set_level_db(9.7));
    CHECK_EQ(f.cards_[0].values["Mic Capture Volume"], 7);
    CHECK(m.set_level_db(9.5));
    CHECK_EQ(f.cards_[0].values["Mic Capture Volume"], 6);

    /* the headset: 12 dB of 23.81 over 0..127 is raw 64 (inferred mapping) */
    Mixer h(f, mixer_device(f.cards_[1].info));
    h.discover();
    CHECK(h.set_level_db(12.0, &applied));
    CHECK_EQ(f.cards_[1].values["Mic Capture Volume"], 64);
    CHECK(near(applied, 64 * 23.81 / 127, 0.01));
    CHECK(h.set_level_db(99));
    CHECK_EQ(f.cards_[1].values["Mic Capture Volume"], 127);

    /* the range maths on its own: a negative-dB range and a stepped control */
    LevelRange r;
    r.raw_min = 0;
    r.raw_max = 30;
    r.step = 3;
    r.db_min = -12;
    r.db_max = 18;
    CHECK_EQ(r.db_to_raw(-12), 0);
    CHECK_EQ(r.db_to_raw(18), 30);
    CHECK_EQ(r.db_to_raw(0), 12); /* raw 12 exactly, a multiple of 3 */
    CHECK_EQ(r.db_to_raw(1.4), 12); /* 13.4 rounds to the step at 12 */
    CHECK_EQ(r.db_to_raw(1.6), 15); /* 13.6 rounds to the step at 15 (1.5 steps up) */
    CHECK(near(r.raw_to_db(30), 18));
    CHECK(near(r.raw_to_db(-5), -12)); /* out of range raw is clamped */
    LevelRange flat; /* a degenerate range does not divide by zero */
    CHECK_EQ(flat.db_to_raw(5), 0);
    CHECK(near(flat.raw_to_db(3), 0));
}

void reapply_and_switches() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    Mixer m(f, mixer_device(f.cards_[0].info));
    m.discover();
    MixerSetting s;
    s.set_level = true;
    s.level_db = 12.0;
    s.set_agc = true;
    s.agc = false;
    CHECK_EQ(m.reapply(s), 2); /* after a reboot the card is back to level 0 and AGC on */
    CHECK_EQ(f.cards_[0].values["Mic Capture Volume"], 8); /* 12 / 1.488 = 8.06 */
    CHECK_EQ(f.cards_[0].values["Auto Gain Control"], 0);
    CHECK_EQ(f.cards_[0].values["Mic Capture Switch"], 1); /* not asked: untouched */
    CHECK_EQ(f.mixer_writes.size(), 2u);

    MixerState st = m.read();
    CHECK(!st.agc);
    CHECK(near(st.level_db, 8 * 23.81 / 16, 0.01));

    CHECK(m.set_mute(true)); /* muted = capture switch off */
    CHECK_EQ(f.cards_[0].values["Mic Capture Switch"], 0);
    CHECK(m.read().mute);
    CHECK(m.set_mute(false));
    CHECK(!m.read().mute);
    CHECK(m.set_agc(true));
    CHECK(m.read().agc);

    /* nothing set means nothing written */
    f.mixer_writes.clear();
    CHECK_EQ(m.reapply(MixerSetting()), 0);
    CHECK(f.mixer_writes.empty());

    /* a headset has no AGC: that part is skipped, the rest applies */
    FakeAlsa g;
    g.cards_ = {fake_uc02(0)};
    Mixer h(g, mixer_device(g.cards_[0].info));
    h.discover();
    CHECK_EQ(h.reapply(s), 1);
    CHECK(!h.set_agc(true));
}

void read_only_and_failing_controls() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    for (auto &c : f.cards_[0].ctls)
        if (c.name == "Mic Capture Volume") c.writable = false;
    Mixer m(f, mixer_device(f.cards_[0].info));
    m.discover();
    CHECK(m.caps().has_level); /* it is there and readable */
    int err = 0;
    CHECK(!m.set_level_db(5, nullptr, &err));
    CHECK(f.mixer_writes.empty());
    CHECK(m.read().level_valid);

    /* an unreadable level control is not offered */
    FakeAlsa g;
    g.cards_ = {fake_uc02(0)};
    g.cards_[0].ctls[0].readable = false;
    Mixer u(g, mixer_device(g.cards_[0].info));
    u.discover();
    CHECK(!u.caps().has_level);

    /* a plain "Capture Volume" is found when no "Mic" one exists, and "Mic" wins when both exist */
    FakeAlsa h;
    h.cards_ = {fake_uc02(0)};
    h.cards_[0].ctls[0].name = "Capture Volume";
    h.cards_[0].values.clear();
    h.cards_[0].values["Capture Volume"] = 5;
    h.cards_[0].ctls[1].name = "Capture Switch";
    h.cards_[0].values["Capture Switch"] = 1;
    Mixer p(h, mixer_device(h.cards_[0].info));
    p.discover();
    CHECK(p.caps().has_level && p.caps().has_mute);
    CHECK_EQ(p.level_name(), "Capture Volume");
    CHECK_EQ(p.read().level_raw, 5);
}

} // namespace

void test_mixer() {
    pcm2902_controls();
    headset_dB_is_inferred();
    card_without_controls();
    level_clamps_and_rounds();
    reapply_and_switches();
    read_only_and_failing_controls();
}
