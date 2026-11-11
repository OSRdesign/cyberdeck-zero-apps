/*
 * SPDX-License-Identifier: MIT
 *
 * pulse-capture-test: the sound-server backend of Cassette (src/core/pulse) on its own, for the boards. Not packaged; built
 * by tests/pulse (host) and cross-built for arm64 with `make -C tests/pulse cli CXX=aarch64-linux-gnu-g++` (no libpulse
 * header or library needed to build: it is loaded at run time, as in the app).
 *
 *   pulse-capture-test                       the server, every source / sink / card and the Bluetooth summary
 *   pulse-capture-test --watch SECONDS       ... then print the change events for SECONDS
 *   pulse-capture-test --source NAME [--seconds 5] [--rate 0] [--channels 0] [--fragment-ms 20] [--silence 3]
 *                      [--start-timeout-ms 3000] [--fail-on-suspend] [--no-lookup] [--wav FILE]
 *                                            record from exactly NAME: per-second RMS / peak, every event with its time,
 *                                            the identity guard at 1 s and at the end, latency, counters. --no-lookup
 *                                            connects without looking the source up first (the server's own handling of
 *                                            a missing device); --wav keeps the audio (16-bit WAV)
 *   pulse-capture-test --sink NAME --tone [--seconds 2] [--rate 48000] [--channels 2] [--buffer-ms 100] [--freq 440]
 *                      [--level-db -20]      play a sine tone to exactly NAME: underruns, latency, events
 *
 * Exit status: 0 when the stream ran to the end, 2 for a usage error, 1 otherwise (open refused, stream ended early).
 */
#include "pulse_api.hpp"
#include "pulse_probe.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace cassette;

namespace {

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void print_event(const PulseStreamEvent &ev) {
    std::printf("  event t=%.3f s %-15s", ev.t, pulse_event_name(ev.kind));
    if (ev.code) std::printf(" code=%s", pulse_error_text(ev.code).c_str());
    if (ev.kind == PulseEvent::Moved) std::printf(" now on \"%s\"", ev.device.c_str());
    if (ev.frame) std::printf(" frame=%llu", static_cast<unsigned long long>(ev.frame));
    std::printf("\n");
    std::fflush(stdout);
}

void put16(FILE *f, uint16_t v) {
    unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
void put32(FILE *f, uint32_t v) {
    unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                          static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void wav_header(FILE *f, uint32_t rate, unsigned ch, uint32_t data_bytes) {
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, static_cast<uint16_t>(ch));
    put32(f, rate);
    put32(f, rate * ch * 2);
    put16(f, static_cast<uint16_t>(ch * 2));
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, data_bytes);
}

int list(PulseApi &api) {
    PulseServerInfo si;
    std::vector<PulseDevice> src, snk;
    std::vector<PulseCard> cards;
    int r = api.server_info(si);
    if (!r) r = api.list_sources(src);
    if (!r) r = api.list_sinks(snk);
    if (!r) r = api.list_cards(cards);
    if (r) {
        std::printf("listing failed: %s\n", pulse_error_text(r).c_str());
        return 1;
    }
    std::printf("server: %s, version %s, libpulse %s, default rate %u\n", si.server_name.c_str(), si.server_version.c_str(),
                si.library_version.c_str(), si.rate);
    std::printf("default source: %s\ndefault sink:   %s\n", si.default_source.c_str(), si.default_sink.c_str());
    for (const PulseDevice &d : src)
        std::printf("source #%u %s\n    \"%s\" %s %u ch %u Hz%s, card %d, bus=%s api=%s form=%s bt=%s profile=%s codec=%s\n",
                    d.index, d.name.c_str(), d.description.c_str(), d.format.c_str(), d.channels, d.rate,
                    d.is_monitor ? (" monitor of " + d.monitor_of).c_str() : "",
                    d.card == kPulseNoIndex ? -1 : static_cast<int>(d.card), d.bus.c_str(), d.device_api.c_str(),
                    d.form_factor.c_str(), d.bt_address.c_str(), d.bt_profile.c_str(), d.bt_codec.c_str());
    for (const PulseDevice &d : snk)
        std::printf("sink   #%u %s\n    \"%s\" %s %u ch %u Hz, card %d, bus=%s api=%s form=%s bt=%s profile=%s codec=%s\n",
                    d.index, d.name.c_str(), d.description.c_str(), d.format.c_str(), d.channels, d.rate,
                    d.card == kPulseNoIndex ? -1 : static_cast<int>(d.card), d.bus.c_str(), d.device_api.c_str(),
                    d.form_factor.c_str(), d.bt_address.c_str(), d.bt_profile.c_str(), d.bt_codec.c_str());
    for (const PulseCard &k : cards) {
        std::printf("card   #%u %s \"%s\" bus=%s bt=%s active=%s\n", k.index, k.name.c_str(), k.description.c_str(),
                    k.bus.c_str(), k.bt_address.c_str(), k.active_profile.c_str());
        for (const PulseProfile &p : k.profiles)
            std::printf("    profile %-28s %s prio %u sinks %u sources %u \"%s\"\n", p.name.c_str(),
                        p.available ? "available  " : "UNAVAILABLE", p.priority, p.n_sinks, p.n_sources,
                        p.description.c_str());
    }
    const PulseProbe probe = pulse_probe(src, snk, cards, &si);
    std::printf("summary:\n");
    for (const PulseProbeDevice &d : probe.devices)
        std::printf("  %-6s %-50s bt=%s mac=%-17s kind=%-10s %u Hz%s%s\n", d.source ? "source" : "sink", d.name.c_str(),
                    d.bluetooth ? "yes" : "no", d.mac.c_str(), pulse_kind_name(d.kind), d.rate, d.monitor ? " monitor" : "",
                    d.is_default ? " (default)" : "");
    for (const PulseProbeCard &k : probe.cards)
        std::printf("  card %s: music %s, call %s, active %s (%s)\n", k.name.c_str(),
                    k.music_available ? "available" : (k.music_offered ? "OFFERED BUT UNAVAILABLE" : "not offered"),
                    k.call_offered ? "offered" : "not offered", k.active_profile.c_str(),
                    k.active_is_music ? "music" : (k.active_is_call ? "call" : "other"));
    return 0;
}

int watch(PulseApi &api, double seconds) {
    std::mutex mu;
    std::vector<PulseChange> seen;
    api.set_change_callback([&](const PulseChange &c) {
        std::lock_guard<std::mutex> g(mu); /* never held while this thread calls the PulseApi */
        seen.push_back(c);
    });
    const char *what[] = {"source", "sink", "card", "server"};
    const char *kind[] = {"new", "change", "remove", "disconnected"};
    const auto t0 = std::chrono::steady_clock::now();
    while (since(t0) < seconds) {
        std::vector<PulseChange> now;
        {
            std::lock_guard<std::mutex> g(mu);
            now.swap(seen);
        }
        for (const PulseChange &c : now)
            std::printf("  change t=%.2f s %s %s #%d\n", since(t0), what[static_cast<int>(c.what)],
                        kind[static_cast<int>(c.kind)], c.index == kPulseNoIndex ? -1 : static_cast<int>(c.index));
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    api.set_change_callback(nullptr);
    return 0;
}

int record(PulseApi &api, const PulseCaptureRequest &r, double seconds, const char *wav) {
    std::unique_ptr<PulseCapture> cap;
    std::string why;
    const auto topen = std::chrono::steady_clock::now();
    const int o = api.open_capture(r, cap, &why);
    std::printf("open %s: %s%s%s (%.3f s)\n", r.source.c_str(), o ? pulse_error_text(o).c_str() : "ok",
                why.empty() ? "" : ": ", why.c_str(), since(topen));
    if (o) return 1;
    const PulseStreamFormat f = cap->format();
    std::printf("stream: %u Hz %u ch S16 (device %u Hz %u ch), flags 0x%x, fragsize %u -> %u bytes, maxlength %u -> %u, "
                "attached to \"%s\"\n",
                f.rate, f.channels, f.device_rate, f.device_channels, f.flags, f.requested.fragsize, f.granted.fragsize,
                f.requested.maxlength, f.granted.maxlength, f.device.c_str());
    FILE *out = wav ? std::fopen(wav, "wb") : nullptr;
    if (wav && !out) std::printf("cannot write %s\n", wav);
    if (out) wav_header(out, f.rate, f.channels, 0);
    std::vector<int16_t> buf(4096 * f.channels);
    uint64_t frames = 0, sec_frames = 0;
    double sumsq = 0, total_sumsq = 0;
    int peak = 0, total_peak = 0;
    bool checked_identity = false, ended_early = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (since(t0) < seconds) {
        const long n = cap->read(buf.data(), 4096, 100);
        PulseStreamEvent ev;
        while (cap->poll_event(ev)) print_event(ev);
        if (n < 0) {
            std::printf("  read ended: %s\n", pulse_error_text(static_cast<int>(n)).c_str());
            ended_early = true;
            break;
        }
        if (n == 0 && cap->state() == PulseStreamState::Connecting && static_cast<int>(since(t0) * 10) % 5 == 0)
            std::printf("  connecting %.2f s\n", cap->connecting_seconds());
        for (long i = 0; i < n * static_cast<long>(f.channels); ++i) {
            const int s = buf[static_cast<size_t>(i)];
            sumsq += double(s) * s;
            peak = std::max(peak, std::abs(s));
        }
        if (out && n > 0) std::fwrite(buf.data(), 2 * f.channels, static_cast<size_t>(n), out);
        frames += static_cast<uint64_t>(n);
        sec_frames += static_cast<uint64_t>(n);
        if (f.rate && sec_frames >= f.rate) {
            const double rms = std::sqrt(sumsq / (double(sec_frames) * f.channels));
            std::printf("  audio second %llu: RMS %.1f peak %d\n", static_cast<unsigned long long>(frames / f.rate), rms,
                        peak);
            std::fflush(stdout);
            total_sumsq += sumsq;
            total_peak = std::max(total_peak, peak);
            sumsq = 0;
            peak = 0;
            sec_frames = 0;
        }
        if (!checked_identity && since(t0) >= 1.0) {
            checked_identity = true;
            std::string actual;
            const bool ok = cap->source_identity_ok(&actual);
            std::printf("  identity at 1 s: %s (server says \"%s\")\n", ok ? "ok" : "NOT ok", actual.c_str());
        }
    }
    total_sumsq += sumsq;
    total_peak = std::max(total_peak, peak);
    PulseStreamEvent ev;
    while (cap->poll_event(ev)) print_event(ev);
    std::string actual;
    const bool ident = cap->source_identity_ok(&actual);
    PulseLatency lat;
    const int lr = cap->latency(lat);
    const PulseCaptureStats st = cap->stats();
    std::printf("identity at the end: %s (server says \"%s\")\n", ident ? "ok" : "NOT ok", actual.c_str());
    if (lr == 0)
        std::printf("latency: %.1f ms (device %.1f ms, transport %.1f ms%s)\n", lat.total_ms, lat.device_ms,
                    lat.transport_ms, lat.timing_info ? "" : ", no timing info");
    else
        std::printf("latency: %s\n", pulse_error_text(lr).c_str());
    std::printf("result: %llu frames = %.2f s of audio in %.2f s, RMS %.1f, peak %d, first frame at %.3f s, state %s, "
                "silent %s, overflow %llu, holes %llu, after end %llu, end %s\n",
                static_cast<unsigned long long>(frames), f.rate ? double(frames) / f.rate : 0.0, since(t0),
                frames ? std::sqrt(total_sumsq / (double(frames) * f.channels)) : 0.0, total_peak, st.first_frame_s,
                pulse_state_name(st.state), st.silent ? "yes" : "no", static_cast<unsigned long long>(st.frames_overflow),
                static_cast<unsigned long long>(st.frames_hole), static_cast<unsigned long long>(st.frames_after_end),
                st.end_code ? pulse_error_text(st.end_code).c_str() : "none");
    cap->close();
    if (out) {
        std::fseek(out, 0, SEEK_SET);
        wav_header(out, f.rate, f.channels, static_cast<uint32_t>(frames * 2 * f.channels));
        std::fclose(out);
        std::printf("wrote %s\n", wav);
    }
    return ended_early ? 1 : 0;
}

int tone(PulseApi &api, const PulsePlaybackRequest &r, double seconds, double freq, double level_db) {
    std::unique_ptr<PulsePlayback> pb;
    std::string why;
    const int o = api.open_playback(r, pb, &why);
    std::printf("open %s: %s%s%s\n", r.sink.c_str(), o ? pulse_error_text(o).c_str() : "ok", why.empty() ? "" : ": ",
                why.c_str());
    if (o) return 1;
    const PulseStreamFormat f = pb->format();
    std::printf("stream: %u Hz %u ch S16 (sink %u Hz), tlength %u -> %u bytes, attached to \"%s\"\n", f.rate, f.channels,
                f.device_rate, f.requested.tlength, f.granted.tlength, f.device.c_str());
    const double amp = 32767.0 * std::pow(10.0, level_db / 20.0);
    const size_t chunk = f.rate / 100; /* 10 ms */
    std::vector<int16_t> buf(chunk * f.channels);
    uint64_t n = 0;
    const uint64_t total = static_cast<uint64_t>(seconds * f.rate);
    int rc = 0;
    while (n < total) {
        for (size_t i = 0; i < chunk; ++i) {
            const int16_t s = static_cast<int16_t>(amp * std::sin(2.0 * M_PI * freq * double(n + i) / f.rate));
            for (unsigned c = 0; c < f.channels; ++c) buf[i * f.channels + c] = s;
        }
        size_t off = 0;
        while (off < chunk) {
            const long w = pb->write(buf.data() + off * f.channels, chunk - off, 500);
            if (w < 0) {
                std::printf("  write ended: %s\n", pulse_error_text(static_cast<int>(w)).c_str());
                rc = 1;
                break;
            }
            off += static_cast<size_t>(w);
        }
        if (rc) break;
        n += chunk;
        PulseStreamEvent ev;
        while (pb->poll_event(ev)) print_event(ev);
    }
    if (!rc) {
        const int d = pb->drain(3000);
        std::printf("drain: %s\n", d ? pulse_error_text(d).c_str() : "ok");
    }
    PulseStreamEvent ev;
    while (pb->poll_event(ev)) print_event(ev);
    PulseLatency lat;
    const int lr = pb->latency(lat);
    std::string actual;
    const bool ident = pb->sink_identity_ok(&actual);
    const PulsePlaybackStats st = pb->stats();
    std::printf("result: %llu frames written, %llu to the server, underruns %llu, latency %s%.1f ms, identity %s (\"%s\"), "
                "state %s\n",
                static_cast<unsigned long long>(st.frames_written), static_cast<unsigned long long>(st.frames_to_server),
                static_cast<unsigned long long>(st.underruns), lr ? (pulse_error_text(lr) + " ").c_str() : "",
                lat.total_ms, ident ? "ok" : "NOT ok", actual.c_str(), pulse_state_name(st.state));
    pb->close();
    return rc;
}

int usage() {
    std::fprintf(stderr,
                 "usage: pulse-capture-test [--watch S]\n"
                 "       pulse-capture-test --source NAME [--seconds S] [--rate HZ] [--channels N] [--fragment-ms MS]\n"
                 "                          [--silence S] [--start-timeout-ms MS] [--fail-on-suspend] [--no-lookup]\n"
                 "                          [--wav FILE]\n"
                 "       pulse-capture-test --sink NAME --tone [--seconds S] [--rate HZ] [--channels N] [--buffer-ms MS]\n"
                 "                          [--freq HZ] [--level-db DB]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    PulseCaptureRequest cr;
    PulsePlaybackRequest pr;
    std::string source, sink;
    double seconds = -1, watch_s = 0, freq = 440, level = -20;
    bool want_tone = false;
    const char *wav = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (a == "--source" && (v = next())) source = v;
        else if (a == "--sink" && (v = next())) sink = v;
        else if (a == "--seconds" && (v = next())) seconds = std::atof(v);
        else if (a == "--rate" && (v = next())) cr.rate = pr.rate = static_cast<uint32_t>(std::atoi(v));
        else if (a == "--channels" && (v = next())) cr.channels = pr.channels = static_cast<unsigned>(std::atoi(v));
        else if (a == "--fragment-ms" && (v = next())) cr.fragment_ms = static_cast<uint32_t>(std::atoi(v));
        else if (a == "--buffer-ms" && (v = next())) pr.buffer_ms = static_cast<uint32_t>(std::atoi(v));
        else if (a == "--silence" && (v = next())) cr.silence_seconds = std::atof(v);
        else if (a == "--start-timeout-ms" && (v = next())) cr.first_frame_timeout_ms = static_cast<uint32_t>(std::atoi(v));
        else if (a == "--fail-on-suspend") cr.fail_on_suspend = pr.fail_on_suspend = true;
        else if (a == "--no-lookup") cr.lookup_first = false;
        else if (a == "--wav" && (v = next())) wav = v;
        else if (a == "--watch" && (v = next())) watch_s = std::atof(v);
        else if (a == "--tone") want_tone = true;
        else if (a == "--freq" && (v = next())) freq = std::atof(v);
        else if (a == "--level-db" && (v = next())) level = std::atof(v);
        else return usage();
    }
    if (!sink.empty() && !want_tone) return usage();
    if (pr.rate == 0) pr.rate = 48000;
    if (pr.channels == 0) pr.channels = 2;
    std::unique_ptr<PulseApi> api = make_real_pulse();
    const auto t0 = std::chrono::steady_clock::now();
    const int c = api->connect("Cassette probe", 3000);
    if (c) {
        std::printf("connect: %s (%s)\n", pulse_error_text(c).c_str(), api->last_error().c_str());
        return 1;
    }
    std::printf("connected in %.3f s\n", since(t0));
    int rc = 0;
    if (!source.empty()) {
        cr.source = source;
        rc = record(*api, cr, seconds > 0 ? seconds : 5.0, wav);
    } else if (!sink.empty()) {
        pr.sink = sink;
        rc = tone(*api, pr, seconds > 0 ? seconds : 2.0, freq, level);
    } else {
        rc = list(*api);
        if (watch_s > 0) watch(*api, watch_s);
    }
    api->disconnect();
    return rc;
}
