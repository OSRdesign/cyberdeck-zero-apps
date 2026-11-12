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
    /* T3: Bluetooth sinks join the list between the cards and the default output; `bt:<MAC>` is a choice like any device */
    {
        std::vector<CardInfo> cards = {fake_pcm2902(0).info};
        BtOutput h;
        h.mac = "70:BF:92:53:6B:8B";
        h.id = "bt:70:BF:92:53:6B:8B";
        h.label = "Evolve 65";
        h.kind = "headset";
        h.rate = 48000;
        h.connected = h.usable = true;
        h.hint_profile = "music";
        BtOutput k = h;
        k.mac = "9E:0C:81:E6:5D:1A";
        k.id = "bt:9E:0C:81:E6:5D:1A";
        k.label = "XWF-M18-M28-M38";
        k.kind = "speaker";
        std::vector<BtOutput> bt = {h, k};
        OutputPlan p = choose_output(cards, "Device", "", &bt);
        CHECK_EQ(p.choices.size(), 4u);
        CHECK_EQ(p.choices[0].device, std::string("hw:CARD=Device,DEV=0"));
        CHECK_EQ(p.choices[1].device, std::string("bt:70:BF:92:53:6B:8B"));
        CHECK_EQ(p.choices[2].device, std::string("bt:9E:0C:81:E6:5D:1A"));
        CHECK_EQ(p.choices[3].device, std::string("default"));
        CHECK(p.choices[1].bluetooth && !p.choices[0].bluetooth && !p.choices[3].bluetooth);
        CHECK_EQ(p.choices[1].label, std::string("Evolve 65"));
        CHECK_EQ(p.choices[1].kind, std::string("headset"));
        CHECK_EQ(p.choices[1].rate, 48000u);
        CHECK_EQ(p.choices[1].hint_profile, std::string("music"));
        CHECK(p.choices[1].connected && p.choices[1].usable);
        CHECK_EQ(p.chosen.device, std::string("hw:CARD=Device,DEV=0")); /* automatic is unchanged */
        /* the saved Bluetooth choice wins (any case), also over the capture card */
        OutputPlan q = choose_output(cards, "Device", "bt:9e:0c:81:e6:5d:1a", &bt);
        CHECK_EQ(q.chosen.device, std::string("bt:9E:0C:81:E6:5D:1A"));
        CHECK(q.chosen.bluetooth);
        /* a headset with a sink and no card yet or a card without a sink is listed unusable */
        BtOutput off = h;
        off.usable = false;
        off.reason = "The device has no audio output now";
        std::vector<BtOutput> bt2 = {off};
        OutputPlan o = choose_output(cards, "Device", "", &bt2);
        CHECK(!o.choices[1].usable && o.choices[1].connected);
        /* the chosen device is not there: it stays as a placeholder, in the list and as the choice */
        OutputPlan r = choose_output(cards, "Device", "bt:9E:0C:81:E6:5D:1A", &bt2, "XWF-M18-M28-M38");
        CHECK_EQ(r.chosen.device, std::string("bt:9E:0C:81:E6:5D:1A"));
        CHECK(!r.chosen.connected && !r.chosen.usable && r.chosen.bluetooth);
        CHECK_EQ(r.chosen.label, std::string("XWF-M18-M28-M38"));
        CHECK_EQ(r.choices.back().device, std::string("default"));
        CHECK_EQ(r.choices[r.choices.size() - 2].device, std::string("bt:9E:0C:81:E6:5D:1A"));
        /* the server is not reachable (no list): no Bluetooth entry, except the placeholder of the earlier choice */
        OutputPlan n = choose_output(cards, "Device", "", nullptr);
        CHECK_EQ(n.choices.size(), 2u);
        OutputPlan m = choose_output(cards, "Device", "bt:9E:0C:81:E6:5D:1A", nullptr);
        CHECK_EQ(m.chosen.device, std::string("bt:9E:0C:81:E6:5D:1A"));
        CHECK(!m.chosen.connected);
        CHECK_EQ(m.chosen.label, std::string("Bluetooth device"));
        /* the only choice besides "Default output" is a Bluetooth device: the list is offered (one device: two choices) */
        const std::vector<BtOutput> one = {h};
        OutputPlan only = choose_output({}, "", "", &one);
        CHECK_EQ(only.choices.size(), 2u);
        CHECK_EQ(only.choices[0].device, std::string("bt:70:BF:92:53:6B:8B"));
        CHECK_EQ(only.chosen.device, std::string("default"));
        /* no card, both devices: both listed before "Default output", automatic still the default output */
        OutputPlan both = choose_output({}, "", "", &bt);
        CHECK_EQ(both.choices.size(), 3u);
        CHECK_EQ(both.choices[1].device, std::string("bt:9E:0C:81:E6:5D:1A"));
        CHECK_EQ(both.choices.back().device, std::string("default"));
        CHECK_EQ(both.chosen.device, std::string("default"));
        /* two devices with one alias: the end of the address tells them apart */
        BtOutput twin = h;
        twin.mac = "AA:BB:CC:DD:EE:FF";
        twin.id = "bt:AA:BB:CC:DD:EE:FF";
        std::vector<BtOutput> bt3 = {h, twin};
        OutputPlan t = choose_output({}, "", "", &bt3);
        CHECK(t.choices[0].label != t.choices[1].label);
        CHECK_EQ(t.choices[1].label, std::string("Evolve 65 (EE:FF)"));
    }
}
