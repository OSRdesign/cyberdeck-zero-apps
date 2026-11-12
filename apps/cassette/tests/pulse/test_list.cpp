/*
 * SPDX-License-Identifier: MIT
 *
 * Listing and the Bluetooth summary: the fake lists, the change callback, the name and profile helpers, pulse_probe()
 * on a HackBerry-like set (study 12: Evolve 65 headset, Beats Fit Pro limited to the call profile, XWF A2DP speaker,
 * a USB headset, monitors).
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_probe.hpp"

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";
const std::string kBeats = "F8:66:5A:18:D3:69";
const std::string kXwf = "9E:0C:81:E6:5D:1A";
const std::string kPhones = "11:22:33:44:55:66";

void test_helpers() {
    CHECK_EQ(pulse_mac_from_name("bluez_input.70_BF_92_53_6B_8B.0"), kEvolve);
    CHECK_EQ(pulse_mac_from_name("bluez_output.70_BF_92_53_6B_8B.1.monitor"), kEvolve);
    CHECK_EQ(pulse_mac_from_name("bluez_card.70_BF_92_53_6B_8B"), kEvolve);
    CHECK_EQ(pulse_mac_from_name("bluez_source.70_BF_92_53_6B_8B.headset_head_unit"), kEvolve); /* PulseAudio */
    CHECK_EQ(pulse_mac_from_name("bluez_sink.F8_66_5A_18_D3_69.a2dp_sink"), kBeats);
    CHECK_EQ(pulse_mac_from_name("alsa_input.usb-GN_Netcom_A_S_Jabra_EVOLVE_LINK-00.mono-fallback"), std::string());
    CHECK_EQ(pulse_mac_from_name("auto_null.monitor"), std::string());
    CHECK_EQ(pulse_mac_from_name("bluez_input.70_BF_92_53_6B.0"), std::string());          /* too short */
    CHECK_EQ(pulse_mac_from_name("bluez_input.70_BF_92_53_6B_8BX.0"), std::string());      /* runs on */
    CHECK_EQ(pulse_parse_mac("70:BF:92:53:6B:8B"), kEvolve);
    CHECK_EQ(pulse_parse_mac("70:bf:92:53:6b:8b"), std::string("70:bf:92:53:6b:8b"));
    CHECK_EQ(pulse_parse_mac("70-BF-92-53-6B-8B"), std::string());
    CHECK_EQ(pulse_parse_mac("70:BF:92:53:6B:8G"), std::string());
    CHECK_EQ(pulse_parse_mac("hw:CARD=Device"), std::string());
    CHECK_EQ(pulse_parse_mac(""), std::string());

    CHECK(pulse_profile_is_music("a2dp-sink"));
    CHECK(pulse_profile_is_music("a2dp-sink-sbc_xq"));
    CHECK(pulse_profile_is_music("a2dp_sink")); /* PulseAudio */
    CHECK(!pulse_profile_is_music("a2dp-source"));
    CHECK(!pulse_profile_is_music("headset-head-unit"));
    CHECK(pulse_profile_is_call("headset-head-unit"));
    CHECK(pulse_profile_is_call("headset-head-unit-cvsd"));
    CHECK(pulse_profile_is_call("headset-head-unit-msbc"));
    CHECK(pulse_profile_is_call("headset_head_unit"));
    CHECK(pulse_profile_is_call("handsfree_head_unit"));
    CHECK(!pulse_profile_is_call("off"));
    CHECK(!pulse_profile_is_call("a2dp-sink"));
    CHECK(!pulse_profile_is_call(""));
}

void hackberry(FakePulse &f) {
    PulseDevice null_mon;
    null_mon.name = "auto_null.monitor";
    null_mon.description = "Monitor of Dummy Output";
    null_mon.rate = 48000;
    null_mon.channels = 2;
    null_mon.is_monitor = true;
    null_mon.monitor_of = "auto_null";
    f.sources.push_back(null_mon);
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.sources.push_back(fake_bt_monitor(kEvolve, 70));
    f.sources.push_back(fake_usb_source("Jabra_EVOLVE_LINK", 16000, 1, 50));
    f.sources.push_back(fake_bt_source(kBeats, 16000, 90));
    f.sources.push_back(fake_bt_monitor(kXwf, 80));
    f.sinks.push_back(fake_bt_sink(kEvolve, 70, "headset"));
    f.sinks.push_back(fake_bt_sink(kXwf, 80, "speaker"));
    f.sinks.push_back(fake_bt_sink(kBeats, 90, "headphone"));
    f.sinks.push_back(fake_usb_sink("Jabra_EVOLVE_LINK", 50));
    PulseDevice null_sink;
    null_sink.name = "auto_null";
    null_sink.description = "Dummy Output";
    null_sink.rate = 48000;
    null_sink.channels = 2;
    f.sinks.push_back(null_sink);
    f.cards.push_back(fake_bt_card(kEvolve, 70, true, "headset-head-unit"));
    PulseCard xwf = fake_bt_card(kXwf, 80, true, "a2dp-sink", false);
    xwf.form_factor = "speaker";
    xwf.description = "XWF-M18-M28-M38";
    f.cards.push_back(xwf);
    f.cards.push_back(fake_bt_card(kBeats, 90, false, "headset-head-unit")); /* P6: no A2DP while the speaker holds it */
    PulseCard usb;
    usb.index = 50;
    usb.name = "alsa_card.usb-GN_Netcom_A_S_Jabra_EVOLVE_LINK-00";
    usb.bus = "usb";
    usb.device_api = "alsa";
    usb.active_profile = "output:analog-stereo+input:mono-fallback";
    f.cards.push_back(usb);
    f.server.default_source = fake_bt_source(kBeats).name; /* P8: the last headset connected */
    f.server.default_sink = "auto_null";
}

const PulseProbeDevice *find(const PulseProbe &p, const std::string &name) {
    for (const PulseProbeDevice &d : p.devices)
        if (d.name == name) return &d;
    return nullptr;
}

void test_listing() {
    FakePulse f;
    hackberry(f);
    CHECK_EQ(f.connect("Cassette", 1000), 0);
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cards;
    PulseServerInfo si;
    CHECK_EQ(f.list_sources(src), 0);
    CHECK_EQ(f.list_sinks(snk), 0);
    CHECK_EQ(f.list_cards(cards), 0);
    CHECK_EQ(f.server_info(si), 0);
    CHECK_EQ(src.size(), 6u);
    CHECK_EQ(snk.size(), 5u);
    CHECK_EQ(cards.size(), 4u);
    CHECK(si.on_pipewire);
    CHECK_EQ(src[1].name, std::string("bluez_input.70_BF_92_53_6B_8B.0"));
    CHECK_EQ(src[1].rate, 16000u);
    CHECK_EQ(src[1].channels, 1u);
    CHECK_EQ(src[1].bt_codec, std::string("msbc"));
    PulseCard c;
    CHECK_EQ(f.card_by_name("bluez_card.70_BF_92_53_6B_8B", c), 0);
    CHECK_EQ(c.profiles.size(), 5u);
    CHECK_EQ(f.card_by_name("bluez_card.00_00_00_00_00_00", c), kPulseNoDevice);

    PulseProbe p = pulse_probe(src, snk, cards, &si);
    CHECK_EQ(p.devices.size(), 11u);
    CHECK_EQ(p.default_source, std::string("bluez_input.F8_66_5A_18_D3_69.0"));

    const PulseProbeDevice *ev = find(p, "bluez_input.70_BF_92_53_6B_8B.0");
    CHECK(ev != nullptr);
    if (ev) {
        CHECK(ev->source);
        CHECK(ev->bluetooth);
        CHECK_EQ(ev->mac, kEvolve);
        CHECK(ev->kind == PulseKind::Headset);
        CHECK_EQ(ev->rate, 16000u);
        CHECK(!ev->monitor);
        CHECK_EQ(ev->card, std::string("bluez_card.70_BF_92_53_6B_8B"));
        CHECK_EQ(ev->profile, std::string("headset-head-unit"));
        CHECK_EQ(ev->codec, std::string("msbc"));
        CHECK(!ev->is_default);
        CHECK_EQ(ev->description, std::string("Evolve 65"));
    }
    const PulseProbeDevice *evout = find(p, "bluez_output.70_BF_92_53_6B_8B.1");
    CHECK(evout && !evout->source && evout->bluetooth && evout->kind == PulseKind::Headset); /* same device: headset */
    const PulseProbeDevice *evmon = find(p, "bluez_output.70_BF_92_53_6B_8B.1.monitor");
    CHECK(evmon && evmon->monitor && evmon->source && evmon->bluetooth);
    const PulseProbeDevice *xwf = find(p, "bluez_output.9E_0C_81_E6_5D_1A.1");
    CHECK(xwf && xwf->kind == PulseKind::Speaker); /* sink only, its monitor is not a microphone */
    const PulseProbeDevice *xmon = find(p, "bluez_output.9E_0C_81_E6_5D_1A.1.monitor");
    CHECK(xmon && xmon->monitor && xmon->kind == PulseKind::Speaker);
    const PulseProbeDevice *beats = find(p, "bluez_input.F8_66_5A_18_D3_69.0");
    CHECK(beats && beats->kind == PulseKind::Headset && beats->is_default);
    const PulseProbeDevice *usb = find(p, "alsa_input.usb-Jabra_EVOLVE_LINK.mono-fallback");
    CHECK(usb && !usb->bluetooth && usb->kind == PulseKind::Other && usb->mac.empty());
    CHECK_EQ(usb ? usb->card : std::string(), std::string("alsa_card.usb-GN_Netcom_A_S_Jabra_EVOLVE_LINK-00"));
    const PulseProbeDevice *nul = find(p, "auto_null");
    CHECK(nul && nul->is_default && !nul->bluetooth && nul->card.empty());
    const PulseProbeDevice *nulmon = find(p, "auto_null.monitor");
    CHECK(nulmon && nulmon->monitor && !nulmon->bluetooth);

    CHECK_EQ(p.cards.size(), 3u); /* the Bluetooth cards only */
    const PulseProbeCard *ce = p.card_of(kEvolve);
    CHECK(ce != nullptr);
    if (ce) {
        CHECK(ce->music_offered && ce->music_available && ce->call_offered);
        CHECK(ce->active_is_call && !ce->active_is_music);
        CHECK_EQ(ce->active_profile, std::string("headset-head-unit"));
        CHECK_EQ(ce->music_profiles.size(), 2u);
        CHECK_EQ(ce->call_profiles.size(), 2u);
    }
    const PulseProbeCard *cb = p.card_of("f8:66:5a:18:d3:69"); /* case does not matter */
    CHECK(cb != nullptr);
    if (cb) {
        CHECK(cb->music_offered);
        CHECK(!cb->music_available); /* L4: limited to call quality */
        CHECK(cb->music_profiles.empty());
        CHECK(cb->active_is_call);
    }
    const PulseProbeCard *cx = p.card_of(kXwf);
    CHECK(cx && cx->music_available && cx->active_is_music && !cx->call_offered);
    CHECK(p.card_of("00:00:00:00:00:00") == nullptr);
    CHECK(p.kind_of(kEvolve) == PulseKind::Headset);
    CHECK(p.kind_of(kXwf) == PulseKind::Speaker);
    CHECK(p.kind_of("00:00:00:00:00:00") == PulseKind::Other);
    CHECK_EQ(std::string(pulse_kind_name(PulseKind::Headphones)), std::string("headphones"));
}

void test_kinds() {
    /* a headset in music mode shows a sink only: the call profile on its card makes it a headset */
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cards;
    snk.push_back(fake_bt_sink(kEvolve, 70, "headset"));
    cards.push_back(fake_bt_card(kEvolve, 70, true, "a2dp-sink"));
    PulseProbe p = pulse_probe(src, snk, cards);
    CHECK_EQ(p.devices.size(), 1u);
    CHECK(p.devices[0].kind == PulseKind::Headset);
    CHECK(p.cards[0].active_is_music);

    /* headphones without a microphone: sink only, no call profile, form factor headphone */
    snk.clear();
    cards.clear();
    snk.push_back(fake_bt_sink(kPhones, 60, "headphone"));
    cards.push_back(fake_bt_card(kPhones, 60, true, "a2dp-sink", false));
    p = pulse_probe(src, snk, cards);
    CHECK(p.devices[0].kind == PulseKind::Headphones);
    CHECK(!p.cards[0].call_offered);

    /* no form factor at all: a speaker */
    snk[0].form_factor.clear();
    cards[0].form_factor.clear();
    p = pulse_probe(src, snk, cards);
    CHECK(p.devices[0].kind == PulseKind::Speaker);

    /* the MAC from the name when the properties have none, the card from the index */
    PulseDevice d = fake_bt_source(kEvolve, 8000, 70);
    d.bt_address.clear();
    d.bus.clear();
    d.device_api.clear();
    src.assign(1, d);
    snk.clear();
    cards.assign(1, fake_bt_card(kEvolve, 70, true, "headset-head-unit-cvsd"));
    p = pulse_probe(src, snk, cards);
    CHECK(p.devices[0].bluetooth);
    CHECK_EQ(p.devices[0].mac, kEvolve);
    CHECK_EQ(p.devices[0].rate, 8000u);
    CHECK(p.devices[0].kind == PulseKind::Headset);
    CHECK(p.cards[0].active_is_call);

    /* Bluetooth recognised from the owner card alone (a renamed node) */
    d.name = "my_mic";
    src.assign(1, d);
    p = pulse_probe(src, snk, cards);
    CHECK(p.devices[0].bluetooth);
    CHECK_EQ(p.devices[0].mac, kEvolve);

    /* ... from device.bus alone, without any address: a source is a headset */
    PulseDevice e;
    e.name = "some_source";
    e.bus = "bluetooth";
    e.rate = 16000;
    e.channels = 1;
    src.assign(1, e);
    cards.clear();
    p = pulse_probe(src, snk, cards);
    CHECK(p.devices[0].bluetooth);
    CHECK(p.devices[0].mac.empty());
    CHECK(p.devices[0].kind == PulseKind::Headset);

    /* nothing at all */
    p = pulse_probe({}, {}, {});
    CHECK(p.devices.empty() && p.cards.empty());
}

void test_changes() {
    FakePulse f;
    hackberry(f);
    CHECK_EQ(f.connect("Cassette", 1000), 0);
    std::vector<PulseChange> seen;
    f.set_change_callback([&](const PulseChange &c) { seen.push_back(c); });
    PulseDevice extra = fake_bt_source(kPhones, 16000, 60);
    f.add_source(extra);
    f.set_profile("bluez_card.70_BF_92_53_6B_8B", "a2dp-sink");
    f.remove_source(extra.name);
    f.add_sink(fake_bt_sink(kPhones, 60));
    CHECK_EQ(seen.size(), 4u);
    if (seen.size() == 4) {
        CHECK(seen[0].what == PulseChange::What::Source && seen[0].kind == PulseChange::Kind::New);
        CHECK(seen[0].index != kPulseNoIndex);
        CHECK(seen[1].what == PulseChange::What::Card && seen[1].kind == PulseChange::Kind::Change);
        CHECK_EQ(seen[1].index, 70u);
        CHECK(seen[2].what == PulseChange::What::Source && seen[2].kind == PulseChange::Kind::Remove);
        CHECK_EQ(seen[2].index, seen[0].index);
        CHECK(seen[3].what == PulseChange::What::Sink && seen[3].kind == PulseChange::Kind::New);
    }
    PulseCard c;
    CHECK_EQ(f.card_by_name("bluez_card.70_BF_92_53_6B_8B", c), 0);
    CHECK_EQ(c.active_profile, std::string("a2dp-sink"));
    f.set_change_callback(nullptr);
    f.add_source(fake_bt_source("AA:BB:CC:DD:EE:FF"));
    CHECK_EQ(seen.size(), 4u);
    f.set_change_callback([&](const PulseChange &ch) { seen.push_back(ch); });
    f.crash();
    CHECK_EQ(seen.size(), 5u);
    CHECK(!seen.empty() && seen.back().kind == PulseChange::Kind::Disconnected);
    CHECK(!f.connected());
    f.disconnect();
    f.add_source(fake_bt_source("AA:BB:CC:DD:EE:00"));
    CHECK_EQ(seen.size(), 5u); /* no callback after disconnect() */
}

}  // namespace

void test_list() {
    test_helpers();
    test_listing();
    test_kinds();
    test_changes();
}
