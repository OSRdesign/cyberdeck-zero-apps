/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound-server tests: the Bluetooth OUTPUT model (bt_output.*, task T3): which sinks are outputs, the label, kind,
 * rate, the profile hint and whether a music profile is available, per device, on lists shaped like the HackBerry's.
 */
#include "bt_output.hpp"
#include "check.hpp"
#include "fake_pulse.hpp"

#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";
const std::string kSpeaker = "9E:0C:81:E6:5D:1A";

std::vector<BtOutput> list_of(FakePulse &f) {
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cds;
    f.connect("t", 100);
    f.list_sources(src);
    f.list_sinks(snk);
    f.list_cards(cds);
    return bt_outputs(pulse_probe(src, snk, cds));
}

}  // namespace

void test_bt_output() {
    /* a headset in music mode: a 48 kHz stereo sink on a2dp-sink, music available, the microphone profile offered */
    {
        FakePulse f;
        f.sinks.push_back(fake_bt_sink(kEvolve, 70));
        f.cards.push_back(fake_bt_card(kEvolve, 70, true, "a2dp-sink"));
        f.sinks.push_back(fake_usb_sink("card", 50));
        f.sinks.push_back(fake_bt_monitor(kEvolve, 70)); /* monitors are never outputs */
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK_EQ(l[0].mac, kEvolve);
        CHECK_EQ(l[0].id, "bt:70:BF:92:53:6B:8B");
        CHECK_EQ(l[0].label, "Evolve 65");
        CHECK_EQ(l[0].kind, "headset");
        CHECK_EQ(l[0].sink, "bluez_output.70_BF_92_53_6B_8B.1");
        CHECK_EQ(l[0].card, "bluez_card.70_BF_92_53_6B_8B");
        CHECK_EQ(l[0].rate, 48000u);
        CHECK_EQ(l[0].channels, 2u);
        CHECK(l[0].connected && l[0].usable);
        CHECK_EQ(l[0].hint_profile, "music");
        CHECK(l[0].music_offered && l[0].music_available && l[0].call_offered);
        CHECK(l[0].reason.empty());
        CHECK(bt_output_find(l, "70:bf:92:53:6b:8b") == &l[0]); /* found in any case */
        CHECK(bt_output_find(l, kSpeaker) == nullptr);
    }
    /* the same headset in call mode: the sink is the 16 kHz mono one, the hint says call */
    {
        FakePulse f;
        PulseDevice call_sink = fake_bt_sink(kEvolve, 70);
        call_sink.rate = 16000;
        call_sink.channels = 1;
        call_sink.bt_profile = "headset-head-unit";
        call_sink.bt_codec = "msbc";
        f.sinks.push_back(call_sink);
        f.cards.push_back(fake_bt_card(kEvolve, 70, true, "headset-head-unit"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK_EQ(l[0].sink, "bluez_output.70_BF_92_53_6B_8B.1"); /* the same name in both modes */
        CHECK_EQ(l[0].rate, 16000u);
        CHECK_EQ(l[0].channels, 1u);
        CHECK_EQ(l[0].hint_profile, "call");
        CHECK(l[0].music_available); /* music is possible: it is only the profile now */
    }
    /* a speaker: A2DP only, no call profile; two devices keep the server's order */
    {
        FakePulse f;
        f.sinks.push_back(fake_bt_sink(kSpeaker, 71, "speaker"));
        f.cards.push_back(fake_bt_card(kSpeaker, 71, true, "a2dp-sink", false));
        f.sinks.push_back(fake_bt_sink(kEvolve, 70));
        f.cards.push_back(fake_bt_card(kEvolve, 70, true, "a2dp-sink"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 2u);
        if (l.size() != 2) return;
        CHECK_EQ(l[0].mac, kSpeaker);
        CHECK_EQ(l[0].label, "XWF-M18-M28-M38");
        CHECK_EQ(l[0].kind, "speaker");
        CHECK(!l[0].call_offered && l[0].music_offered);
        CHECK_EQ(l[0].hint_profile, "music");
        CHECK_EQ(l[1].mac, kEvolve);
    }
    /* another A2DP device holds the link (P6): the headset has call profiles only, music is not available */
    {
        FakePulse f;
        PulseDevice call_sink = fake_bt_sink(kEvolve, 70);
        call_sink.rate = 16000;
        call_sink.channels = 1;
        call_sink.bt_profile = "headset-head-unit";
        f.sinks.push_back(call_sink);
        f.cards.push_back(fake_bt_card(kEvolve, 70, false, "headset-head-unit"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK(l[0].music_offered && !l[0].music_available && l[0].call_offered);
        CHECK_EQ(l[0].hint_profile, "call");
    }
    /* a card with its profile off has no sink: connected but not usable, with a reason */
    {
        FakePulse f;
        f.cards.push_back(fake_bt_card(kEvolve, 70, true, "off"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK(l[0].connected && !l[0].usable);
        CHECK(l[0].sink.empty());
        CHECK(!l[0].reason.empty());
        CHECK_EQ(l[0].hint_profile, "unknown");
        CHECK_EQ(l[0].label, "Evolve 65");
    }
    /* T9: the node and the card re-created with NEW indexes (a reconnect, a profile switch): the entry is found by MAC, the sink by
     * its name, which does not change */
    {
        FakePulse f;
        PulseDevice s = fake_bt_sink(kEvolve, 1305);
        s.index = 1271;
        s.rate = 16000;
        s.channels = 1;
        s.bt_profile = "headset-head-unit";
        f.sinks.push_back(s);
        f.cards.push_back(fake_bt_card(kEvolve, 1305, true, "headset-head-unit"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK_EQ(l[0].id, "bt:70:BF:92:53:6B:8B");
        CHECK_EQ(l[0].sink, "bluez_output.70_BF_92_53_6B_8B.1");
        CHECK_EQ(l[0].card, "bluez_card.70_BF_92_53_6B_8B");
        CHECK(l[0].connected && l[0].usable);
        CHECK_EQ(l[0].rate, 16000u);
        CHECK_EQ(l[0].hint_profile, "call");
        CHECK_EQ(l[0].sink_profile, "call");
    }
    /* the sink is listed before its card (the card list was read a moment earlier): matched by the MAC in its name, usable */
    {
        FakePulse f;
        PulseDevice s = fake_bt_sink(kEvolve, 999);
        s.bt_address.clear();
        f.sinks.push_back(s);
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK_EQ(l[0].mac, kEvolve);
        CHECK(l[0].usable);
        CHECK_EQ(l[0].sink, "bluez_output.70_BF_92_53_6B_8B.1");
    }
    /* the music-mode virtual source has a colon name, the sink an underscore name: one device, one MAC, one output entry */
    {
        FakePulse f;
        PulseDevice v = fake_bt_source(kEvolve, 48000, kPulseNoIndex);
        v.name = "bluez_input.70:BF:92:53:6B:8B";
        v.bt_address.clear();
        f.sources.push_back(v);
        f.sinks.push_back(fake_bt_sink(kEvolve, 70));
        f.cards.push_back(fake_bt_card(kEvolve, 70, true, "a2dp-sink"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() == 1) CHECK_EQ(l[0].sink, "bluez_output.70_BF_92_53_6B_8B.1");
    }
    /* the card already says music while the call node is still listed (the switch is half done): the node says call */
    {
        FakePulse f;
        PulseDevice s = fake_bt_sink(kEvolve, 70);
        s.rate = 16000;
        s.channels = 1;
        s.bt_profile = "headset-head-unit";
        f.sinks.push_back(s);
        f.cards.push_back(fake_bt_card(kEvolve, 70, true, "a2dp-sink"));
        const std::vector<BtOutput> l = list_of(f);
        CHECK_EQ(l.size(), 1u);
        if (l.size() != 1) return;
        CHECK_EQ(l[0].hint_profile, "music");
        CHECK_EQ(l[0].sink_profile, "call");
    }
    /* nothing Bluetooth: no entry (USB sinks are the ALSA cards' business) */
    {
        FakePulse f;
        f.sinks.push_back(fake_usb_sink("card", 50));
        CHECK(list_of(f).empty());
    }
}
