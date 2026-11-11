/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "fake_alsa.hpp"
#include "output.hpp"
#include "player.hpp"
#include "sink.hpp"
#include "support.hpp"

#include <cerrno>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

std::vector<int32_t> played16(const FakeAlsa &f) {
    std::vector<int32_t> v;
    for (size_t i = 0; i + 1 < f.played.size(); i += 2) v.push_back(decode(&f.played[i], 2));
    return v;
}

void run_to_end(Player &p) {
    int n = 100000;
    while (n-- > 0 && p.step()) {
    }
}

CardInfo card(const std::string &id, const std::string &name, bool cap, bool play, int index) {
    CardInfo c;
    c.index = index;
    c.id = id;
    c.name = name;
    c.capture = cap;
    c.playback = play;
    return c;
}

} // namespace

void test_alsa_sink() {
    TempDir d;
    const auto s = ramp_samples(10000, 1);
    const std::string path = d.file("a.wav");
    write_wav(path, afmt(48000, 1, 16), s);

    /* the PCM2902 takes mono at 48 kHz: played as is, period 1024, buffer about 500 ms */
    {
        FakeAlsa fa;
        fa.cards_.push_back(fake_pcm2902(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=Device,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(sink.opened_device(), std::string("hw:CARD=Device,DEV=0"));
        CHECK_EQ(fa.playback_requests.size(), 1u);
        CHECK_EQ(fa.playback_requests[0].period_frames, 1024u);
        CHECK_EQ(fa.playback_requests[0].buffer_frames, 24576u);
        CHECK_EQ(fa.playback_requests[0].channels, 1u);
        run_to_end(p);
        CHECK(played16(fa) == s);
        CHECK_EQ(fa.drains, 1u);
        CHECK(fa.opened.empty()); /* no capture open */
    }
    /* the UC02 headset is stereo only: the mono take is duplicated */
    {
        FakeAlsa fa;
        fa.cards_.push_back(fake_uc02(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=UC02,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(fa.playback_requests[0].channels, 2u);
        CHECK_EQ(p.sink_format().channels, 2u);
        run_to_end(p);
        std::vector<int32_t> want;
        for (int32_t v : s) {
            want.push_back(v);
            want.push_back(v);
        }
        CHECK(played16(fa) == want);
    }
    /* a rate the hw device does not take: the same card through plughw */
    {
        const std::string p44 = d.file("b.wav");
        write_wav(p44, afmt(44100, 1, 16), s);
        FakeAlsa fa;
        fa.cards_.push_back(fake_uc02(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=UC02,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(p44), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(sink.opened_device(), std::string("plughw:CARD=UC02,DEV=0"));
        CHECK_EQ(fa.playback_requests[0].rate, 44100u);
    }
    /* the default output; a server error */
    {
        FakeAlsa fa;
        AlsaPlaybackSink sink(fa, "default");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(sink.opened_device(), std::string("default"));
        run_to_end(p);
        CHECK(played16(fa) == s);

        FakeAlsa fb;
        fb.default_play_error = -EHOSTDOWN;
        AlsaPlaybackSink sb(fb, "default");
        Player q(posix_file_ops(), sb, Player::Mode::Manual);
        CHECK_EQ(q.open(path), 0);
        CHECK_EQ(q.play(), -EHOSTDOWN);
        CHECK(q.state() == PlayerState::Stopped);

        FakeAlsa fc;
        fc.lib_available = false;
        AlsaPlaybackSink sc(fc, "default");
        Player r(posix_file_ops(), sc, Player::Mode::Manual);
        CHECK_EQ(r.open(path), 0);
        CHECK_EQ(r.play(), kNoAlsa);
    }
    /* an underrun (recovered, nothing lost) and a card pulled out (paused, DeviceLost) */
    {
        FakeAlsa fa;
        fa.cards_.push_back(fake_pcm2902(0));
        FakeEvent x;
        x.kind = FakeEvent::Xrun;
        x.at_frame = 2048;
        fa.cards_[0].play_events.push_back(x);
        AlsaPlaybackSink sink(fa, "hw:CARD=Device,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        CHECK(played16(fa) == s);
        CHECK_EQ(p.underruns(), 1u);
        CHECK_EQ(fa.underruns, 1u);

        FakeAlsa fb;
        fb.cards_.push_back(fake_pcm2902(0));
        FakeEvent e;
        e.kind = FakeEvent::Error;
        e.at_frame = 4096;
        e.err = -ENODEV;
        fb.cards_[0].play_events.push_back(e);
        AlsaPlaybackSink sb(fb, "hw:CARD=Device,DEV=0");
        Player q(posix_file_ops(), sb, Player::Mode::Manual);
        CHECK_EQ(q.open(path), 0);
        CHECK_EQ(q.play(), 0);
        run_to_end(q);
        CHECK(q.state() == PlayerState::Paused);
        CHECK_EQ(q.position(), 4096u);
        PlayerEvent ev;
        CHECK(q.poll_event(ev) && ev == PlayerEvent::DeviceLost);
        CHECK_EQ(sb.opened_device(), std::string(""));
    }
    /* a card without a playback side cannot be opened for playback */
    {
        FakeAlsa fa;
        fa.cards_.push_back(fake_card_no_controls(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=Plain,DEV=0");
        SinkFormat g;
        CHECK_EQ(sink.open(afmt(48000, 1, 16), g), -ENOENT);
    }
}

void test_output() {
    /* the capture card has a playback side: it is chosen; the list has it and the default output */
    {
        std::vector<CardInfo> cards = {fake_pcm2902(0).info};
        OutputPlan p = choose_output(cards, "Device");
        CHECK_EQ(p.chosen.device, std::string("hw:CARD=Device,DEV=0"));
        CHECK_EQ(p.chosen.card_id, std::string("Device"));
        CHECK_EQ(p.choices.size(), 2u);
        CHECK_EQ(p.choices[0].device, std::string("hw:CARD=Device,DEV=0"));
        CHECK_EQ(p.choices[1].device, std::string("default"));
    }
    /* the capture card has no playback side and there is no other output: the default, no list */
    {
        std::vector<CardInfo> cards = {fake_card_no_controls(0).info};
        OutputPlan p = choose_output(cards, "Plain");
        CHECK_EQ(p.chosen.device, std::string("default"));
        CHECK(p.choices.empty());
        OutputPlan q = choose_output({}, "");
        CHECK_EQ(q.chosen.device, std::string("default"));
        CHECK(q.choices.empty());
    }
    /* two outputs: the capture card has none, another card has one: the default is chosen, the list offers both */
    {
        std::vector<CardInfo> cards = {fake_card_no_controls(0).info, fake_uc02(1).info};
        OutputPlan p = choose_output(cards, "Plain");
        CHECK_EQ(p.chosen.device, std::string("default"));
        CHECK_EQ(p.choices.size(), 2u);
        CHECK_EQ(p.choices[0].device, std::string("hw:CARD=UC02,DEV=0"));
        /* a saved choice wins while present */
        OutputPlan q = choose_output(cards, "Plain", "hw:CARD=UC02,DEV=0");
        CHECK_EQ(q.chosen.device, std::string("hw:CARD=UC02,DEV=0"));
        OutputPlan r = choose_output({fake_card_no_controls(0).info}, "Plain", "hw:CARD=UC02,DEV=0");
        CHECK_EQ(r.chosen.device, std::string("default")); /* saved card unplugged */
    }
    /* the capture card's own side comes first even when another card is listed earlier */
    {
        std::vector<CardInfo> cards = {fake_uc02(0).info, fake_pcm2902(1).info};
        OutputPlan p = choose_output(cards, "Device");
        CHECK_EQ(p.chosen.device, std::string("hw:CARD=Device,DEV=0"));
        CHECK_EQ(p.choices.size(), 3u);
        CHECK_EQ(p.choices[0].card_id, std::string("Device"));
    }
    /* HDMI is left to the default output; same names are told apart */
    {
        std::vector<CardInfo> cards = {fake_card_no_controls(0).info, card("vc4hdmi0", "vc4-hdmi-0", false, true, 1)};
        OutputPlan p = choose_output(cards, "Plain");
        CHECK_EQ(p.chosen.device, std::string("default"));
        CHECK(p.choices.empty());
        std::vector<CardInfo> twin = {card("A", "USB Audio", true, true, 0), card("B", "USB Audio", false, true, 1)};
        OutputPlan t = choose_output(twin, "A");
        CHECK_EQ(t.choices.size(), 3u);
        CHECK(t.choices[0].label != t.choices[1].label);
        CHECK_EQ(t.choices[0].label, std::string("USB Audio (A)"));
    }
}
