/*
 * SPDX-License-Identifier: MIT
 *
 * cassette-probe: lists the capture cards, what each offers, the mixer controls with their state, then opens each card (or the
 * one chosen with --card) for 2 seconds, counts frames and prints the result, including the sound-server fallback on EBUSY.
 * Listen-only: no file is written and no mixer control is changed. Safe to run on a board.
 *
 *   cassette-probe [--card NAME_OR_BY-ID_PATH] [--seconds N] [--no-open] [--fake]
 *   cassette-probe --server [--card bt:<MAC>] [--fake]
 *
 * --server lists the sound server's sources, sinks and Bluetooth cards instead (PipeWire through pipewire-pulse, or
 * PulseAudio; libpulse loaded at run time; it never starts a server and opens no stream): the summary lines of the T1 board
 * CLI pulse-capture-test (src/core/pulse/pulse_report.*). --card bt:<MAC> (implies --server) keeps that device only. Exit
 * status 1 when the library or the server is missing, the listing failed or the device is not there.
 * --fake runs the same code against the test fakes (only in the test build).
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../capture.hpp"
#include "../card.hpp"
#include "../mixer.hpp"
#include "../open_policy.hpp"
#include "pulse_report.hpp"

#ifdef CASSETTE_PROBE_WITH_FAKE
#include "fake_alsa.hpp"
#include "fake_pulse.hpp"
#endif

using namespace cassette;

namespace {

struct CountSink : FrameSink {
    size_t write(const void *, size_t n) override {
        frames += n;
        return n;
    }
    uint64_t frames = 0;
};

const char *ctl_type(const CtlInfo &c) {
    return c.type == CtlInfo::Type::Integer ? "int" : c.type == CtlInfo::Type::Boolean ? "bool" : "other";
}

void show_controls(AlsaApi &api, const CardInfo &card) {
    Mixer mx(api, mixer_device(card));
    int r = mx.discover();
    if (r < 0) {
        std::printf("    mixer: cannot read (%s)\n", alsa_error_text(r).c_str());
        return;
    }
    for (const CtlInfo &c : mx.controls()) {
        long v = 0;
        bool got = c.readable && api.mixer_get(mixer_device(card), c.name, v) >= 0;
        std::printf("    ctl %-28s %-5s", c.name.c_str(), ctl_type(c));
        if (c.type == CtlInfo::Type::Integer) {
            std::printf(" range %ld..%ld", c.min, c.max);
            if (c.has_db) std::printf(" (%.2f..%.2f dB)", c.db_min_cdb / 100.0, c.db_max_cdb / 100.0);
            else std::printf(" (no dB from the driver)");
        }
        if (got) std::printf(" = %ld", v);
        std::printf("%s\n", c.writable ? "" : " [read-only]");
    }
    const MixerCaps &k = mx.caps();
    std::printf("    capabilities: has_level=%d has_agc=%d has_mute=%d", k.has_level, k.has_agc, k.has_mute);
    if (k.has_level) {
        const LevelRange &lr = mx.level_range();
        MixerState st = mx.read();
        std::printf("  level %.2f dB%s (range %.2f..%.2f dB%s)", st.level_db, lr.db_inferred ? " INFERRED" : "", lr.db_min,
                    lr.db_max, lr.db_inferred ? ", assumed 0..23.81" : "");
    }
    std::printf("\n");
}

bool try_open(AlsaApi &api, const CardInfo &card, double seconds) {
    OpenResult o = open_capture(api, card);
    std::printf("    open: device %s -> %s", o.device.c_str(), o.ok() ? "OK" : "FAILED");
    if (!o.ok()) {
        std::printf(" (%s)\n", o.message.c_str());
        return false;
    }
    std::printf("%s: %u Hz, %u ch, %u-bit, period %u, buffer %u frames (%.0f ms)\n", o.via_server ? " VIA SERVER" : " direct",
                o.format.rate, o.format.channels, o.format.bits, o.hw.period_frames, o.hw.buffer_frames,
                1000.0 * o.hw.buffer_frames / (o.format.rate ? o.format.rate : 1));
    if (o.via_server) std::printf("    note: %s\n", o.message.c_str());
    CountSink sink;
    Capture cap(std::move(o), sink);
    if (!cap.start()) {
        std::printf("    capture: could not start\n");
        return false;
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    cap.stop();
    CaptureStats s = cap.stats();
    std::printf("    captured %llu frames in %.1f s (expected about %.0f), overruns %u, lost (est.) %llu, error: %s%s%s\n",
                static_cast<unsigned long long>(s.frames_captured), seconds, seconds * cap.format().rate, s.overruns,
                static_cast<unsigned long long>(s.frames_lost_xrun), capture_error_name(s.last_error),
                s.message.empty() ? "" : " - ", s.message.c_str());
    return s.frames_captured > 0;
}

} // namespace

int main(int argc, char **argv) {
    std::string want;
    double seconds = 2.0;
    bool do_open = true, fake = false, server = false;
#ifdef CASSETTE_PROBE_WITH_FAKE
    const char *fake_opt = " [--fake]";
#else
    const char *fake_opt = "";
#endif
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--card") && i + 1 < argc) want = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-open")) do_open = false;
        else if (!std::strcmp(argv[i], "--fake")) fake = true;
        else if (!std::strcmp(argv[i], "--server")) server = true;
        else {
            std::fprintf(stderr,
                         "usage: cassette-probe [--card NAME|/dev/snd/by-id/PATH] [--seconds N] [--no-open]%s\n"
                         "       cassette-probe --server [--card bt:MAC]%s   (the sound server's devices: Bluetooth)\n",
                         fake_opt, fake_opt);
            return 2;
        }
    }
    /* the sound server's list (Bluetooth): --server, or --card bt:<MAC> */
    std::string bt_mac;
    if (want.compare(0, 3, "bt:") == 0) {
        bt_mac = pulse_parse_mac(want.substr(3));
        if (bt_mac.empty()) {
            std::fprintf(stderr, "--card bt:MAC needs a Bluetooth address such as bt:70:BF:92:53:6B:8B\n");
            return 2;
        }
        server = true;
    } else if (server && !want.empty()) {
        std::fprintf(stderr, "--server takes --card bt:MAC only (a sound card name goes without --server)\n");
        return 2;
    }
    if (server) {
        std::unique_ptr<PulseApi> real_server;
        PulseApi *papi = nullptr;
#ifdef CASSETTE_PROBE_WITH_FAKE
        FakePulse fp;
        fake_hackberry(fp);
        if (fake) papi = &fp;
#else
        if (fake) {
            std::fprintf(stderr, "--fake is not in this build\n");
            return 2;
        }
#endif
        if (!papi) {
            real_server = make_real_pulse();
            papi = real_server.get();
        }
        std::string out;
        const int src = pulse_server_report(*papi, bt_mac, out);
        std::fputs(out.c_str(), stdout);
        return src;
    }
    std::unique_ptr<AlsaApi> real;
    AlsaApi *api = nullptr;
#ifdef CASSETTE_PROBE_WITH_FAKE
    FakeAlsa fk = fake_demo();
    if (fake) api = &fk;
#else
    if (fake) {
        std::fprintf(stderr, "--fake is not in this build\n");
        return 2;
    }
#endif
    if (!api) {
        real = make_real_alsa();
        api = real.get();
    }
    std::string why;
    bool lib_ok = api->available(&why);
    std::printf("ALSA library: %s\n", lib_ok ? "loaded" : why.c_str());

    std::vector<CaptureCard> cards = list_capture_cards(*api, lib_ok);
    if (cards.empty()) {
        std::printf("no capture cards\n");
        return 1;
    }
    int rc = 0;
    for (const CaptureCard &c : cards) {
        if (!want.empty()) {
            std::vector<CardInfo> one{c.info};
            if (!select_card(one, descriptor_from_text(want)).found()) continue;
        }
        std::printf("card \"%s\"\n  index %d (this boot only)  id %s  name %s\n  long: %s\n  usb %s  port %s  by-id %s\n",
                    c.label.c_str(), c.info.index, c.info.id.c_str(), c.info.name.c_str(), c.info.long_name.c_str(),
                    c.info.usb_id.empty() ? "-" : c.info.usb_id.c_str(), c.info.usb_port.empty() ? "-" : c.info.usb_port.c_str(),
                    c.info.usb_by_id.empty() ? "-" : c.info.usb_by_id.c_str());
        if (c.offer_known) std::printf("  offers: %s\n", offer_summary(c.offer).c_str());
        else std::printf("  offers: unknown (%s)\n", alsa_error_text(c.offer_error).c_str());
        if (lib_ok) show_controls(*api, c.info);
        if (do_open && lib_ok && !try_open(*api, c.info, seconds)) rc = 1;
    }
    return rc;
}
