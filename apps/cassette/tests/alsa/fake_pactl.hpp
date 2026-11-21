/*
 * SPDX-License-Identifier: MIT
 *
 * A scripted `pactl` for the Cassette tests: the output of PulseAudio's `pactl info` and `pactl list sources` on the Panasonic
 * FZ-M1 (card 0, the ALC256; a USB microphone as card 1) and a `set-source-volume` that does what PulseAudio does to the ALSA
 * "Capture Volume" of a FakeAlsa card (V -> hardware dB = dB(V) - dB(base), clamped, to a step). Policies cover a server that
 * rounds down, one that acknowledges but never writes (the control is not its to manage), and one that refuses.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "fake_alsa.hpp"
#include "server_gain.hpp"

namespace cassette {

struct FakePactl : PactlRunner {
    enum class Policy { Nearest, RoundDown, Short, Ignore, Refuse }; /* Short: needs 0.7 of a step more than the step asked for */
    FakeAlsa *alsa = nullptr;
    size_t card = 0;                      /* index into alsa->cards_ of the ALC256 */
    bool server = true;                   /* false: no socket, every call fails (a console board) */
    Policy policy = Policy::Nearest;
    uint32_t base = 6554;                 /* the source's Base Volume: 10 % = -60 dB */
    uint32_t volume = 3277;               /* what `list sources` shows */
    std::string default_source = "alsa_input.pci-0000_00_1f.3.analog-stereo";
    /* runs f on the card with the fake's own lock (tests whose capture thread reads the same controls) */
    std::function<void(const std::function<void(FakeAlsa &)> &)> with;
    std::vector<std::vector<std::string>> calls;
    int sets = 0;

    static std::string base_line(uint32_t b) {
        char t[96];
        std::snprintf(t, sizeof t, "\tBase Volume: %6u / %3u%% / %.2f dB\n", b, static_cast<unsigned>(std::lround(b * 100.0 / 65536)),
                      60.0 * std::log10(b / 65536.0));
        return t;
    }

    std::string sources_text() const {
        char vol[200];
        const unsigned pct = static_cast<unsigned>(std::lround(volume * 100.0 / 65536));
        const double vdb = 60.0 * std::log10(volume / 65536.0);
        std::snprintf(vol, sizeof vol, "\tVolume: front-left: %u / %3u%% / %.2f dB,   front-right: %u / %3u%% / %.2f dB\n", volume, pct,
                      vdb, volume, pct, vdb);
        std::string s;
        s += "Source #0\n\tState: SUSPENDED\n\tName: alsa_output.pci-0000_00_1f.3.analog-stereo.monitor\n"
             "\tDescription: Monitor of Built-in Audio Analog Stereo\n\tMute: no\n"
             "\tVolume: front-left: 65536 / 100% / 0.00 dB,   front-right: 65536 / 100% / 0.00 dB\n"
             "\t        balance 0.00\n\tBase Volume: 65536 / 100% / 0.00 dB\n"
             "\tMonitor of Sink: alsa_output.pci-0000_00_1f.3.analog-stereo\n\tProperties:\n\t\tdevice.class = \"monitor\"\n";
        s += "Source #1\n\tState: SUSPENDED\n\tName: alsa_input.pci-0000_00_1f.3.analog-stereo\n"
             "\tDescription: Built-in Audio Analog Stereo\n\tDriver: module-alsa-card.c\n\tMute: no\n";
        s += vol;
        s += "\t        balance 0.00\n";
        s += base_line(base);
        s += "\tMonitor of Sink: n/a\n\tProperties:\n\t\talsa.resolution_bits = \"16\"\n\t\tdevice.api = \"alsa\"\n"
             "\t\talsa.card = \"0\"\n\t\talsa.card_name = \"HDA Intel PCH\"\n\t\tdevice.bus = \"pci\"\n\t\tdevice.class = \"sound\"\n"
             "\t\tdevice.string = \"front:0\"\n";
        s += "Source #2\n\tState: SUSPENDED\n\tName: alsa_input.usb-C-Media_Electronics_Inc._USB_PnP_Sound_Device-00.mono-fallback\n"
             "\tMute: no\n\tVolume: mono: 40000 / 61% / -13.60 dB\n\tBase Volume: 65536 / 100% / 0.00 dB\n"
             "\tMonitor of Sink: n/a\n\tProperties:\n\t\talsa.card = \"1\"\n\t\tdevice.bus = \"usb\"\n";
        s += "Source #3\n\tName: bluez_input.70_BF_92_53_6B_8B.0\n\tMute: no\n\tMonitor of Sink: n/a\n\tProperties:\n\t\tdevice.api = \"bluez5\"\n";
        return s;
    }

    bool run(const std::vector<std::string> &args, std::string &out, int) override {
        out.clear();
        calls.push_back(args);
        if (!server || args.empty()) return false;
        if (args[0] == "info") {
            out = "Server String: /run/user/1000/pulse/native\nLibrary Protocol Version: 35\nServer Name: pulseaudio\n"
                  "Server Version: 17.0\nDefault Sample Specification: s16le 2ch 44100Hz\nDefault Sink: alsa_output.pci-0000_00_1f.3.analog-stereo\n"
                  "Default Source: " + default_source + "\nCookie: 1234:5678\n";
            return true;
        }
        if (args.size() == 2 && args[0] == "list" && args[1] == "sources") {
            out = sources_text();
            return true;
        }
        if (args.size() == 3 && args[0] == "set-source-volume") {
            ++sets;
            if (policy == Policy::Refuse) return false;
            const uint32_t v = static_cast<uint32_t>(std::stoul(args[2]));
            if (policy == Policy::Ignore) return true;
            volume = v;
            const double db = 60.0 * std::log10(v / 65536.0) - 60.0 * std::log10(base / 65536.0); /* hardware gain */
            const double x = (db + 17.25) / 0.75;
            const long raw = policy == Policy::RoundDown ? static_cast<long>(std::floor(x))
                             : policy == Policy::Short   ? static_cast<long>(std::floor(x - 0.7))
                                                         : std::lround(x);
            auto put = [this, raw](FakeAlsa &a) { a.cards_[card].values["Capture Volume"] = std::max(0L, std::min(63L, raw)); };
            if (with) with(put);
            else put(*alsa);
            return true;
        }
        return false;
    }
};

} // namespace cassette
