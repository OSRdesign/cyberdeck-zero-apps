/* SPDX-License-Identifier: MIT */
#include <cmath>

#include "card.hpp"
#include "check.hpp"
#include "fake_alsa.hpp"
#include "fake_pactl.hpp"
#include "mixer.hpp"
#include "server_gain.hpp"

using namespace cassette;

namespace {

bool near(double a, double b, double eps = 0.01) { return std::fabs(a - b) <= eps; }

void parsing_and_mapping() {
    FakeAlsa f;
    f.cards_ = {fake_alc256(0)};
    FakePactl p;
    p.alsa = &f;
    const std::vector<ServerSource> v = parse_pactl_sources(p.sources_text());
    CHECK_EQ(v.size(), size_t(4));
    CHECK(v[0].monitor && v[0].alsa_card == -1); /* a monitor: never a microphone */
    CHECK_EQ(v[1].name, "alsa_input.pci-0000_00_1f.3.analog-stereo");
    CHECK(!v[1].monitor);
    CHECK_EQ(v[1].alsa_card, 0);
    CHECK_EQ(v[1].base_raw, 6554u);
    CHECK_EQ(v[1].volume_raw, 3277u); /* the 5 % the tablet had saved */
    CHECK_EQ(v[2].alsa_card, 1);
    CHECK_EQ(v[2].base_raw, 65536u);
    CHECK_EQ(v[2].volume_raw, 40000u);
    CHECK_EQ(v[3].alsa_card, -1); /* Bluetooth: no ALSA card */
    CHECK_EQ(parse_pactl_default_source("Server Name: pulseaudio\nDefault Source: x.y\nCookie: 1\n"), "x.y");
    CHECK_EQ(parse_pactl_server_name("Server Name: PulseAudio (on PipeWire 1.4.2)\n"), "PulseAudio (on PipeWire 1.4.2)");
    CHECK_EQ(parse_pactl_default_source("nothing"), "");
    /* PipeWire's property names: api.alsa.card, else the N of api.alsa.path */
    const auto pw = parse_pactl_sources("Source #5\n\tName: alsa_input.usb-X\n\tProperties:\n\t\tapi.alsa.path = \"hw:3,0\"\n"
                                        "Source #6\n\tName: alsa_input.pci-Y\n\tProperties:\n\t\tapi.alsa.card = \"2\"\n");
    CHECK_EQ(pw.size(), size_t(2));
    CHECK_EQ(pw[0].alsa_card, 3);
    CHECK_EQ(pw[1].alsa_card, 2);
    CHECK_EQ(parse_pactl_sources("garbage\n").size(), size_t(0));

    /* the source of the card: the default when it is the card's, else the card's first source, never a monitor */
    CHECK_EQ(pick_server_source(v, p.default_source, 0)->name, "alsa_input.pci-0000_00_1f.3.analog-stereo");
    CHECK_EQ(pick_server_source(v, "other", 0)->name, "alsa_input.pci-0000_00_1f.3.analog-stereo");
    CHECK(pick_server_source(v, p.default_source, 1)->name.find("usb-C-Media") != std::string::npos);
    CHECK(pick_server_source(v, p.default_source, 7) == nullptr);
    CHECK(pick_server_source(v, p.default_source, -1) == nullptr);

    /* V = base * 10^(G/60): the tablet's base volume of 10 % (-60 dB) */
    CHECK_EQ(server_volume_for_gain(0, 6554), 6554u);
    CHECK(near(static_cast<double>(server_volume_for_gain(30, 6554)), 20726, 1.0));    /* 31.6 % */
    CHECK(near(static_cast<double>(server_volume_for_gain(-17.25, 6554)), 3380, 1.0)); /* 5.2 % */
    CHECK_EQ(server_volume_for_gain(0, 0), 65536u);                                     /* no base volume: 0 dB is 100 % */
    CHECK(near(static_cast<double>(server_volume_for_gain(30, 0)), 207243, 2.0));
    CHECK(near(server_gain_of_volume(3277, 6554), -18.06, 0.01));
    CHECK(near(server_gain_of_volume(server_volume_for_gain(12.5, 6554), 6554), 12.5, 0.01));
    CHECK(near(server_gain_of_volume(65536, 6554), 60.0, 0.01)); /* 100 % asks +60 dB: the server clamps to the card's +30 */
}

struct Rig {
    FakeAlsa f;
    FakePactl p;
    Mixer m;
    explicit Rig(FakePactl::Policy pol = FakePactl::Policy::Nearest, bool use = true, int card_no = 0) : f(), p(), m(f, "hw:CARD=PCH") {
        f.cards_ = {fake_alc256(0)};
        p.alsa = &f;
        p.policy = pol;
        if (use) m.use_server(&p, card_no);
        m.discover();
    }
};

void level_goes_through_the_server() {
    Rig r;
    CHECK(r.m.caps().has_level);
    CHECK(r.m.level_via_server());
    CHECK_EQ(r.m.server_source(), "alsa_input.pci-0000_00_1f.3.analog-stereo");
    const LevelRange &lr = r.m.level_range();
    long last = -1;
    for (int pos = 0; pos <= 47; ++pos) { /* the Settings slider */
        double applied = 0;
        CHECK(r.m.set_level_db(lr.db_min + pos, &applied));
        const MixerState s = r.m.read();
        CHECK(s.level_raw >= last);
        CHECK(near(s.level_db, applied)); /* the shown value is the card's */
        CHECK(std::fabs(s.level_db - (lr.db_min + pos)) <= 0.38 + 1e-9);
        last = s.level_raw;
    }
    CHECK_EQ(last, 63); /* the card's full range: +30 dB */
    CHECK(r.m.set_level_db(lr.db_min));
    CHECK_EQ(r.m.read().level_raw, 0);
    CHECK(r.m.set_level_db(5.25));
    CHECK_EQ(r.m.read().level_raw, 30);
    CHECK(near(r.m.read().level_db, 5.25));
    CHECK(r.f.mixer_writes.empty()); /* the card's control was never written by Cassette */
    CHECK(r.m.take_note().empty());
    /* the boost, the switch and auto gain stay on the ALSA control */
    CHECK(r.m.set_boost_index(2));
    CHECK_EQ(r.f.mixer_writes.size(), size_t(1));
    CHECK_EQ(r.f.mixer_writes[0].first, "Internal Mic Boost Volume");
    CHECK(r.m.level_via_server());
    /* reapply (the start of the app) goes through the server too */
    MixerSetting ms;
    ms.set_level = true;
    ms.level_db = 12.0;
    CHECK_EQ(r.m.reapply(ms), 1);
    CHECK_EQ(r.f.mixer_writes.size(), size_t(1));
    CHECK(near(r.m.read().level_db, 12.0, 0.38));
}

void a_server_that_rounds_down() {
    Rig r(FakePactl::Policy::RoundDown);
    const LevelRange &lr = r.m.level_range();
    for (int raw = 0; raw <= 63; ++raw) {
        CHECK(r.m.set_level_db(lr.raw_to_db(raw)));
        CHECK_EQ(r.m.read().level_raw, raw);
    }
    CHECK(r.m.level_via_server());
    CHECK(r.f.mixer_writes.empty());
}

/* Regression (draft.3): a server that lands one step short of every request used to count as "set" (within a step and a half), so the
 * card stayed a step below what the stepper asked for. Now only the wanted step counts; otherwise the card is written directly. */
void a_server_one_step_short() {
    Rig r(FakePactl::Policy::Short);
    const LevelRange &lr = r.m.level_range();
    for (int raw = 1; raw <= 63; ++raw) {
        double applied = 0;
        CHECK(r.m.set_level_db(lr.raw_to_db(raw), &applied));
        CHECK_EQ(r.m.read().level_raw, raw); /* the step asked for, not the one below */
        CHECK(near(applied, lr.raw_to_db(raw)));
    }
    CHECK(!r.m.level_via_server()); /* it never reached the wanted step: the server was given up */
    CHECK(!r.m.take_note().empty());
    CHECK(!r.f.mixer_writes.empty());
    CHECK_EQ(r.f.mixer_writes.back().second, 63);
}

void the_report_says_what_the_server_did() {
    Rig r;
    CHECK(r.m.set_level_db(5.25));
    CHECK(r.m.server_report().find("asked V") != std::string::npos);
    CHECK(r.m.server_report().find("source now V") != std::string::npos);
}

void a_server_that_does_not_take_it() {
    Rig r(FakePactl::Policy::Ignore); /* acknowledges, never writes (the control is not the server's to manage) */
    double applied = 0;
    CHECK(r.m.set_level_db(10.0, &applied));
    CHECK(!r.m.level_via_server()); /* not used again */
    const std::string note = r.m.take_note();
    CHECK(note.find("did not set the card") != std::string::npos);
    CHECK(r.m.take_note().empty());
    CHECK_EQ(r.f.mixer_writes.size(), size_t(1)); /* then the card, directly, as before */
    CHECK_EQ(r.f.mixer_writes[0].first, "Capture Volume");
    CHECK(near(r.m.read().level_db, applied));
    const int sets = r.p.sets;
    CHECK(r.m.set_level_db(20.0));
    CHECK_EQ(r.p.sets, sets); /* no more pactl calls */
    CHECK_EQ(r.f.mixer_writes.size(), size_t(2));
}

void a_server_that_refuses() {
    Rig r(FakePactl::Policy::Refuse);
    CHECK(r.m.level_via_server());
    CHECK(r.m.set_level_db(10.0));
    CHECK(!r.m.level_via_server());
    CHECK(r.m.take_note().find("pactl failed") != std::string::npos);
    CHECK_EQ(r.m.read().level_raw, r.m.level_range().db_to_raw(10.0));
}

void no_server_means_the_alsa_path() {
    { /* a console board: no socket, every pactl call fails */
        FakeAlsa f;
        f.cards_ = {fake_alc256(0)};
        FakePactl p;
        p.alsa = &f;
        p.server = false;
        Mixer m(f, "hw:CARD=PCH");
        m.use_server(&p, 0);
        CHECK_EQ(m.discover(), 0);
        CHECK(m.caps().has_level && !m.level_via_server());
        CHECK(m.server_source().empty());
        CHECK(m.set_level_db(5.25));
        CHECK_EQ(f.mixer_writes.size(), size_t(1));
        CHECK_EQ(f.mixer_writes[0].second, 30);
        CHECK_EQ(p.sets, 0);
    }
    { /* use_server never called: pactl is never asked */
        Rig r(FakePactl::Policy::Nearest, false);
        CHECK(!r.m.level_via_server());
        CHECK(r.p.calls.empty());
        CHECK(r.m.set_level_db(5.25));
        CHECK_EQ(r.f.mixer_writes.size(), size_t(1));
    }
    { /* the server has no source for this card (a card number it does not know) */
        Rig r(FakePactl::Policy::Nearest, true, 7);
        CHECK(!r.m.level_via_server());
        CHECK(r.m.set_level_db(5.25));
        CHECK_EQ(r.f.mixer_writes.size(), size_t(1));
        CHECK_EQ(r.p.sets, 0);
    }
}

} // namespace

void test_server_gain() {
    parsing_and_mapping();
    level_goes_through_the_server();
    a_server_that_rounds_down();
    a_server_one_step_short();
    the_report_says_what_the_server_did();
    a_server_that_does_not_take_it();
    a_server_that_refuses();
    no_server_means_the_alsa_path();
}
