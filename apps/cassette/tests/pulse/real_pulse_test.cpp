/*
 * SPDX-License-Identifier: MIT
 *
 * Real sound-server smoke test (its own binary; no pulseaudio header needed, libpulse is loaded at run time):
 *
 *   real-pulse-test                     connect, print the server, the sources, the sinks, the cards and the Bluetooth
 *                                       summary; SKIPPED (exit 0) when libpulse.so.0 or the server is missing
 *   real-pulse-test SOURCE [SECONDS]    ... and record SECONDS (default 3) from SOURCE: rate, frames, RMS, events, the
 *                                       identity guard, latency
 *
 * Listen-only: never plays, never changes a profile, a default or a volume, never writes a file. tests/pulse/run.sh runs
 * it without arguments (WSLg's PulseAudio on the build PC); on a board copy it next to the probe CLI.
 */
#include "pulse_api.hpp"
#include "pulse_probe.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace cassette;

int main(int argc, char **argv) {
    std::unique_ptr<PulseApi> api = make_real_pulse();
    const int c = api->connect("Cassette smoke test", 3000);
    if (c == kPulseMissing || c == kPulseNoServer) {
        std::printf("SKIP real pulse smoke: %s (%s)\n", pulse_error_text(c).c_str(), api->last_error().c_str());
        return 0;
    }
    if (c != 0) {
        std::printf("FAIL real pulse smoke: connect: %s (%s)\n", pulse_error_text(c).c_str(), api->last_error().c_str());
        return 1;
    }
    int fails = 0;
    PulseServerInfo si;
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cards;
    if (api->server_info(si) != 0) ++fails;
    if (api->list_sources(src) != 0) ++fails;
    if (api->list_sinks(snk) != 0) ++fails;
    if (api->list_cards(cards) != 0) ++fails;
    std::printf("server: %s %s (libpulse %s), default source \"%s\", default sink \"%s\", %u Hz\n", si.server_name.c_str(),
                si.server_version.c_str(), si.library_version.c_str(), si.default_source.c_str(), si.default_sink.c_str(),
                si.rate);
    for (const PulseDevice &d : src) {
        if (d.name.empty()) ++fails;
        std::printf("source %u %s \"%s\" %u Hz %u ch %s%s card %d bus=%s bt=%s\n", d.index, d.name.c_str(),
                    d.description.c_str(), d.rate, d.channels, d.format.c_str(), d.is_monitor ? " monitor" : "",
                    d.card == kPulseNoIndex ? -1 : static_cast<int>(d.card), d.bus.c_str(), d.bt_address.c_str());
    }
    for (const PulseDevice &d : snk) {
        if (d.name.empty()) ++fails;
        std::printf("sink %u %s \"%s\" %u Hz %u ch %s card %d bus=%s bt=%s\n", d.index, d.name.c_str(), d.description.c_str(),
                    d.rate, d.channels, d.format.c_str(), d.card == kPulseNoIndex ? -1 : static_cast<int>(d.card),
                    d.bus.c_str(), d.bt_address.c_str());
    }
    for (const PulseCard &k : cards) {
        std::printf("card %u %s active \"%s\" profiles:", k.index, k.name.c_str(), k.active_profile.c_str());
        for (const PulseProfile &p : k.profiles) std::printf(" %s%s", p.name.c_str(), p.available ? "" : "(unavailable)");
        std::printf("\n");
    }
    const PulseProbe probe = pulse_probe(src, snk, cards, &si);
    for (const PulseProbeDevice &d : probe.devices)
        if (d.bluetooth)
            std::printf("bluetooth %s %s %s mac %s kind %s %u Hz%s\n", d.source ? "source" : "sink", d.name.c_str(),
                        d.description.c_str(), d.mac.c_str(), pulse_kind_name(d.kind), d.rate, d.monitor ? " monitor" : "");
    for (const PulseProbeCard &k : probe.cards)
        std::printf("bluetooth card %s music %s, active %s (%s)\n", k.name.c_str(),
                    k.music_available ? "available" : (k.music_offered ? "unavailable" : "not offered"),
                    k.active_profile.c_str(), k.active_is_music ? "music" : (k.active_is_call ? "call" : "other"));

    if (argc > 1) {
        PulseCaptureRequest r;
        r.source = argv[1];
        const double seconds = argc > 2 ? std::atof(argv[2]) : 3.0;
        std::unique_ptr<PulseCapture> cap;
        std::string why;
        const int o = api->open_capture(r, cap, &why);
        if (o != 0) {
            std::printf("record %s: open failed: %s (%s)\n", r.source.c_str(), pulse_error_text(o).c_str(), why.c_str());
            api->disconnect();
            return 1;
        }
        const PulseStreamFormat f = cap->format();
        std::vector<int16_t> buf(4096 * f.channels);
        uint64_t frames = 0;
        double sumsq = 0;
        int peak = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
            const long n = cap->read(buf.data(), 4096, 100);
            if (n < 0) break;
            for (long i = 0; i < n * static_cast<long>(f.channels); ++i) {
                const int s = buf[static_cast<size_t>(i)];
                sumsq += double(s) * s;
                peak = std::max(peak, std::abs(s));
            }
            frames += static_cast<uint64_t>(n);
            PulseStreamEvent ev;
            while (cap->poll_event(ev))
                std::printf("  event %.3f s %s%s%s\n", ev.t, pulse_event_name(ev.kind),
                            ev.code ? (" " + pulse_error_text(ev.code)).c_str() : "",
                            ev.kind == PulseEvent::Moved ? (" -> " + ev.device).c_str() : "");
        }
        PulseStreamEvent ev;
        while (cap->poll_event(ev)) std::printf("  event %.3f s %s\n", ev.t, pulse_event_name(ev.kind));
        std::string actual;
        const bool ident = cap->source_identity_ok(&actual);
        PulseLatency lat;
        const int lr = cap->latency(lat);
        const PulseCaptureStats st = cap->stats();
        const double rms = frames ? std::sqrt(sumsq / (double(frames) * f.channels)) : 0.0;
        std::printf("record %s: %u Hz %u ch, %llu frames in %.1f s (%.2f s of audio), RMS %.1f, peak %d, first frame %.3f s, "
                    "overflow %llu, holes %llu, identity %s (%s), latency %s %.1f ms, state %s\n",
                    r.source.c_str(), f.rate, f.channels, static_cast<unsigned long long>(frames), seconds,
                    f.rate ? double(frames) / f.rate : 0.0, rms, peak, st.first_frame_s,
                    static_cast<unsigned long long>(st.frames_overflow), static_cast<unsigned long long>(st.frames_hole),
                    ident ? "ok" : "NOT ok", actual.c_str(), lr == 0 ? "" : pulse_error_text(lr).c_str(), lat.total_ms,
                    pulse_state_name(st.state));
        if (!ident && !cap->ended()) ++fails;
        cap->close();
    }
    api->disconnect();
    std::printf("real pulse smoke: %zu sources, %zu sinks, %zu cards, %s\n", src.size(), snk.size(), cards.size(),
                fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
