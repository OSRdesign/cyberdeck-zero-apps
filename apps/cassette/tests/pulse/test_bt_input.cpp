/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound-server tests: the Bluetooth input model (bt_input.*: identity, the source to open, the call rate, the hint) and
 * the connecting gate of the capture PCM (pulse_pcm.*: the leading zeros of a headset's profile switch are dropped).
 */
#include "bt_input.hpp"
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_pcm.hpp"

#include <cstring>
#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";

PulseDevice virtual_source(const std::string &mac) {
    PulseDevice d = fake_bt_source(mac, 48000, kPulseNoIndex);
    d.name = "bluez_input." + mac;
    d.format = "float32le";
    d.bt_address.clear();
    d.bt_profile.clear();
    d.bus.clear();
    d.device_api.clear();
    d.form_factor.clear();
    return d;
}

std::vector<BtInput> list_of(FakePulse &f) {
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cds;
    f.connect("t", 100);
    f.list_sources(src);
    f.list_sinks(snk);
    f.list_cards(cds);
    return bt_inputs(pulse_probe(src, snk, cds));
}

void test_identity() {
    CHECK_EQ(bt_id_of("70:bf:92:53:6b:8b"), "bt:70:BF:92:53:6B:8B");
    CHECK_EQ(bt_id_of("70_BF_92_53_6B_8B"), "");
    CHECK_EQ(bt_id_of(""), "");
    CHECK_EQ(bt_mac_of_id("bt:70:bf:92:53:6b:8b"), kEvolve);
    CHECK_EQ(bt_mac_of_id("bt:nonsense"), "");
    CHECK_EQ(bt_mac_of_id("USB Audio CODEC|/dev/snd/by-id/x|08bb:2902"), "");
    CHECK(bt_source_is_virtual("bluez_input.70:BF:92:53:6B:8B"));
    CHECK(!bt_source_is_virtual("bluez_input.70_BF_92_53_6B_8B.0"));
}

void test_music_mode_virtual() {
    FakePulse f;
    f.sources.push_back(virtual_source(kEvolve));
    f.cards.push_back(fake_bt_card(kEvolve, 70, true, "a2dp-sink"));
    f.sinks.push_back(fake_bt_sink(kEvolve, 70));
    const std::vector<BtInput> l = list_of(f);
    CHECK_EQ(l.size(), 1u);
    if (l.size() != 1u) return;
    CHECK_EQ(l[0].id, "bt:" + kEvolve);
    CHECK_EQ(l[0].source, "bluez_input." + kEvolve);
    CHECK(l[0].virtual_source && l[0].usable && l[0].connected);
    CHECK_EQ(l[0].rate, 16000u); /* the card offers mSBC: not the virtual node's 48000 */
    CHECK_EQ(l[0].hint_profile, "music");
    CHECK_EQ(l[0].kind, "headset");
    CHECK_EQ(l[0].label, "Evolve 65");
    CHECK_EQ(l[0].card, "bluez_card.70_BF_92_53_6B_8B");
}

void test_call_mode_real_and_rates() {
    FakePulse f;
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.cards.push_back(fake_bt_card(kEvolve, 70, true, "headset-head-unit"));
    std::vector<BtInput> l = list_of(f);
    CHECK_EQ(l.size(), 1u);
    if (l.size() != 1u) return;
    CHECK_EQ(l[0].source, "bluez_input.70_BF_92_53_6B_8B.0");
    CHECK(!l[0].virtual_source);
    CHECK_EQ(l[0].rate, 16000u);
    CHECK_EQ(l[0].hint_profile, "call");

    FakePulse g; /* CVSD: the real source is 8 kHz */
    g.sources.push_back(fake_bt_source(kEvolve, 8000, 70));
    g.cards.push_back(fake_bt_card(kEvolve, 70, true, "headset-head-unit-cvsd"));
    l = list_of(g);
    CHECK_EQ(l.size(), 1u);
    if (!l.empty()) CHECK_EQ(l[0].rate, 8000u);

    FakePulse h; /* music mode, only CVSD offered */
    h.sources.push_back(virtual_source(kEvolve));
    PulseCard c = fake_bt_card(kEvolve, 70, true, "a2dp-sink");
    for (auto it = c.profiles.begin(); it != c.profiles.end();)
        it = it->name == "headset-head-unit" ? c.profiles.erase(it) : it + 1;
    h.cards.push_back(c);
    l = list_of(h);
    CHECK_EQ(l.size(), 1u);
    if (!l.empty()) CHECK_EQ(l[0].rate, 8000u);

    FakePulse k; /* no card information at all: the wideband default */
    k.sources.push_back(virtual_source(kEvolve));
    l = list_of(k);
    CHECK_EQ(l.size(), 1u);
    if (!l.empty()) {
        CHECK_EQ(l[0].rate, 16000u);
        CHECK_EQ(l[0].hint_profile, "unknown");
    }
}

void test_card_without_source_and_non_inputs() {
    FakePulse f;
    f.cards.push_back(fake_bt_card(kEvolve, 70, true, "off")); /* a headset whose profile is off: no source now */
    const std::string spk = "9E:0C:81:E6:5D:1A";
    f.cards.push_back(fake_bt_card(spk, 71, true, "a2dp-sink", false)); /* a speaker: no microphone profile */
    f.sinks.push_back(fake_bt_sink(spk, 71, "speaker"));
    f.sources.push_back(fake_bt_monitor(spk, 71)); /* never an input */
    f.sources.push_back(fake_usb_source("usb", 48000, 1, 50));
    const std::vector<BtInput> l = list_of(f);
    CHECK_EQ(l.size(), 1u);
    if (l.empty()) return;
    CHECK_EQ(l[0].mac, kEvolve);
    CHECK(l[0].connected && !l[0].usable);
    CHECK(!l[0].reason.empty());
    CHECK(l[0].source.empty());
}

void test_lead_gate() {
    FakePulse f;
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.connect("Cassette", 1000);
    PulseCaptureRequest r;
    r.source = fake_bt_source(kEvolve).name;
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    if (!cap) return;
    std::shared_ptr<PulseCapture> stream(std::move(cap));
    std::shared_ptr<PulseLeadGate> gate(new PulseLeadGate());
    PulseCapturePcm pcm(stream, 5, gate);
    std::vector<int16_t> buf(400);
    std::vector<int16_t> z(160, 0);
    f.deliver(0, z.data(), z.size());
    CHECK_EQ(pcm.readi(buf.data(), 400), 0L); /* all zeros: dropped, nothing handed over */
    CHECK(!gate->open.load());
    CHECK_EQ(gate->dropped_frames.load(), 160u);
    std::vector<int16_t> mix(160, 0);
    mix[100] = 5;
    mix[101] = -7;
    f.deliver(0, mix.data(), mix.size());
    const long n = pcm.readi(buf.data(), 400);
    CHECK_EQ(n, 60L); /* 100 leading zero frames dropped; the audio starts at the first non-zero sample */
    CHECK_EQ(buf[0], 5);
    CHECK_EQ(buf[1], -7);
    CHECK(gate->open.load() && gate->by_audio.load());
    CHECK_EQ(gate->dropped_frames.load(), 260u);
    f.deliver(0, z.data(), z.size()); /* once open, zeros are real silence */
    CHECK_EQ(pcm.readi(buf.data(), 400), 160L);

    /* opened by the owner (a call profile and a running stream): zeros pass at once */
    std::unique_ptr<PulseCapture> cap2;
    CHECK_EQ(f.open_capture(r, cap2, nullptr), 0);
    std::shared_ptr<PulseCapture> s2(std::move(cap2));
    std::shared_ptr<PulseLeadGate> g2(new PulseLeadGate());
    PulseCapturePcm p2(s2, 5, g2);
    g2->open.store(true);
    f.deliver(1, z.data(), z.size());
    CHECK_EQ(p2.readi(buf.data(), 400), 160L);
    CHECK(!g2->by_audio.load());

    /* no gate: unchanged behaviour */
    std::unique_ptr<PulseCapture> cap3;
    CHECK_EQ(f.open_capture(r, cap3, nullptr), 0);
    PulseCapturePcm p3(std::shared_ptr<PulseCapture>(std::move(cap3)), 5);
    f.deliver(2, z.data(), z.size());
    CHECK_EQ(p3.readi(buf.data(), 400), 160L);
}

}  // namespace

void test_bt_input() {
    test_identity();
    test_music_mode_virtual();
    test_call_mode_real_and_rates();
    test_card_without_source_and_non_inputs();
    test_lead_gate();
}
