/*
 * SPDX-License-Identifier: MIT
 *
 * The text report of cassette-probe --server (pulse_report.*, task T5) on the fake server: the server line, the defaults,
 * the summary lines (the same as pulse-capture-test's list), the bt:<MAC> filter, and the exit codes when the library, the
 * server or the listing fails. HackBerry-like set (fake_hackberry), synthetic otherwise.
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_report.hpp"

using namespace cassette;

namespace {

bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

int lines(const std::string &s) {
    int n = 0;
    for (char c : s) n += c == '\n';
    return n;
}

void test_full_list() {
    FakePulse f;
    fake_hackberry(f);
    std::string out;
    CHECK_EQ(pulse_server_report(f, "", out), 0);
    CHECK(has(out, "server: PulseAudio (on PipeWire 1.4.2), version 15.0.0, libpulse 17.0.0, default rate 48000\n"));
    CHECK(has(out, "default source: bluez_input.F8_66_5A_18_D3_69.0\ndefault sink:   bluez_output.9E_0C_81_E6_5D_1A.1\n"));
    CHECK(has(out, "summary:\n"));
    /* the Evolve 65's microphone: a Bluetooth headset source at 16 kHz, its MAC */
    CHECK(has(out, "  source bluez_input.70_BF_92_53_6B_8B.0                    bt=yes mac=70:BF:92:53:6B:8B kind=headset    16000 Hz\n"));
    /* monitors are flagged */
    CHECK(has(out, "  source auto_null.monitor                                  bt=no mac=                  kind=other      48000 Hz monitor\n"));
    CHECK(has(out, "  source bluez_output.70_BF_92_53_6B_8B.1.monitor           bt=yes mac=70:BF:92:53:6B:8B kind=headset    48000 Hz monitor\n"));
    /* the USB headset is not Bluetooth */
    CHECK(has(out, "  source alsa_input.usb-Jabra_EVOLVE_LINK.mono-fallback     bt=no mac=                  kind=other      16000 Hz\n"));
    /* the defaults: the Beats source (P8, the last headset connected), the XWF speaker */
    CHECK(has(out, "  sink   bluez_output.9E_0C_81_E6_5D_1A.1                   bt=yes mac=9E:0C:81:E6:5D:1A kind=speaker    48000 Hz (default)\n"));
    CHECK(has(out, "  source bluez_input.F8_66_5A_18_D3_69.0                    bt=yes mac=F8:66:5A:18:D3:69 kind=headset    16000 Hz (default)\n"));
    /* the cards: Evolve in call mode with music available; XWF on music, no call profile; Beats without A2DP (P6) */
    CHECK(has(out, "  card bluez_card.70_BF_92_53_6B_8B: music available, call offered, active headset-head-unit (call)\n"));
    CHECK(has(out, "  card bluez_card.9E_0C_81_E6_5D_1A: music available, call not offered, active a2dp-sink (music)\n"));
    CHECK(has(out, "  card bluez_card.F8_66_5A_18_D3_69: music OFFERED BUT UNAVAILABLE, call offered, active headset-head-unit (call)\n"));
    CHECK(!f.connected()); /* disconnected afterwards */
    /* pulse-capture-test prints exactly this block (the same function) */
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cards;
    PulseServerInfo si;
    f.connect("t", 100);
    f.server_info(si);
    f.list_sources(src);
    f.list_sinks(snk);
    f.list_cards(cards);
    const std::string summary = pulse_probe_summary(pulse_probe(src, snk, cards, &si));
    CHECK(out.size() > summary.size() && out.compare(out.size() - summary.size(), summary.size(), summary) == 0);
    CHECK_EQ(lines(summary), 1 + int(src.size() + snk.size()) + 3); /* header, one per device, three Bluetooth cards */
}

void test_filter() {
    FakePulse f;
    fake_hackberry(f);
    std::string out;
    CHECK_EQ(pulse_server_report(f, "70:bf:92:53:6b:8b", out), 0); /* any case */
    CHECK(has(out, "70:BF:92:53:6B:8B"));
    CHECK(!has(out, "F8:66:5A:18:D3:69 kind"));
    CHECK(!has(out, "bluez_card.9E_0C_81_E6_5D_1A"));
    CHECK(!has(out, "auto_null.monitor"));
    CHECK(has(out, "card bluez_card.70_BF_92_53_6B_8B"));
    /* the Evolve: source, its sink's monitor, the sink, the card */
    const std::string summary = out.substr(out.find("summary:\n"));
    CHECK_EQ(lines(summary), 1 + 3 + 1);

    std::string none;
    CHECK_EQ(pulse_server_report(f, "11:22:33:44:55:66", none), 1);
    CHECK(has(none, "no device bt:11:22:33:44:55:66 on the sound server"));
    CHECK(has(none, "Settings > Bluetooth"));
}

struct FailingCards : FakePulse {
    int list_cards(std::vector<PulseCard> &) override { return kPulseTimeout; }
};

void test_failures() {
    {
        FakePulse f;
        f.lib_missing = true;
        std::string out;
        CHECK_EQ(pulse_server_report(f, "", out), 1);
        CHECK(has(out, "connect: "));
        CHECK(has(out, "libpulse0"));
        CHECK(has(out, "Bluetooth audio is not set up on this board"));
        CHECK(!has(out, "summary:"));
    }
    {
        FakePulse f;
        f.server_down = true;
        std::string out;
        CHECK_EQ(pulse_server_report(f, "", out), 1);
        CHECK(has(out, "connect: "));
        CHECK(has(out, "Bluetooth audio is not set up on this board"));
    }
    {
        FakePulse f;
        f.server_hangs = true;
        std::string out;
        CHECK_EQ(pulse_server_report(f, "", out), 1);
        CHECK(has(out, "connect: "));
    }
    {
        FailingCards f;
        fake_hackberry(f);
        std::string out;
        CHECK_EQ(pulse_server_report(f, "", out), 1);
        CHECK(has(out, "listing failed: "));
        CHECK(!f.connected());
    }
    {
        /* a server with nothing Bluetooth: listed, exit 0 */
        FakePulse f;
        f.sources.push_back(fake_usb_source("Jabra_EVOLVE_LINK", 16000, 1, 50));
        std::string out;
        CHECK_EQ(pulse_server_report(f, "", out), 0);
        CHECK(has(out, "bt=no"));
        CHECK(!has(out, "  card "));
    }
}

}  // namespace

void test_report() {
    test_full_list();
    test_filter();
    test_failures();
}
