/*
 * SPDX-License-Identifier: MIT
 *
 * Capture streams on the fake server: the open (native rates, refusals, DONT_MOVE and the properties), reading, the
 * Connecting state and the start timeout, kill / move / suspend with their exact events, the identity guard, errors,
 * the bounded ring, close. Then the silence detector (test_silence).
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_abi.hpp"

#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";
const std::string kBeats = "F8:66:5A:18:D3:69";
const std::string kCvsd = "AA:BB:CC:00:11:22";

std::string evolve() { return fake_bt_source(kEvolve).name; }
std::string beats() { return fake_bt_source(kBeats).name; }
std::string usb() { return fake_usb_source("Jabra_EVOLVE_LINK", 48000, 2).name; }

void setup(FakePulse &f) {
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.sources.push_back(fake_bt_source(kBeats, 16000, 90));
    f.sources.push_back(fake_bt_source(kCvsd, 8000, 95));
    f.sources.push_back(fake_usb_source("Jabra_EVOLVE_LINK", 48000, 2, 50));
    f.sources.push_back(fake_bt_monitor(kEvolve, 70));
    f.server.default_source = usb();
    f.connect("Cassette", 1000);
}

PulseCaptureRequest req_for(const std::string &source) {
    PulseCaptureRequest r;
    r.source = source;
    return r;
}

std::vector<PulseStreamEvent> events(PulseCapture &c) {
    std::vector<PulseStreamEvent> v;
    PulseStreamEvent ev;
    while (c.poll_event(ev)) v.push_back(ev);
    return v;
}

std::vector<int16_t> ramp(size_t frames, unsigned channels, int16_t start = 1) {
    std::vector<int16_t> v(frames * channels);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int16_t>(start + static_cast<int16_t>(i % 1000));
    return v;
}

void test_native_rates() {
    FakePulse f;
    setup(f);
    struct Case {
        std::string name;
        uint32_t rate;
        unsigned channels;
    } cases[] = {{fake_bt_source(kCvsd).name, 8000, 1}, {evolve(), 16000, 1}, {usb(), 48000, 2}};
    size_t i = 0;
    for (const Case &c : cases) {
        std::unique_ptr<PulseCapture> cap;
        std::string why;
        CHECK_EQ(f.open_capture(req_for(c.name), cap, &why), 0);
        CHECK(cap != nullptr);
        if (!cap) continue;
        const PulseStreamFormat fmt = cap->format();
        CHECK_EQ(fmt.rate, c.rate); /* rate 0: the source's own, no resampling */
        CHECK_EQ(fmt.channels, c.channels);
        CHECK_EQ(fmt.device_rate, c.rate);
        CHECK_EQ(fmt.device, c.name);
        CHECK_EQ(f.streams[i].rate, c.rate);
        CHECK_EQ(f.streams[i].channels, c.channels);
        CHECK_EQ(f.streams[i].attr.fragsize, pulse_ms_to_bytes(20, c.rate, c.channels));
        CHECK_EQ(f.streams[i].attr.maxlength, pulse_ms_to_bytes(2000, c.rate, c.channels));
        CHECK((f.streams[i].flags & pa::kStreamDontMove) != 0);
        bool dont_reconnect = false, dont_fallback = false;
        for (const auto &p : f.streams[i].props) {
            dont_reconnect |= p.first == "node.dont-reconnect" && p.second == "true";
            dont_fallback |= p.first == "node.dont-fallback" && p.second == "true";
        }
        CHECK(dont_reconnect && dont_fallback);
        CHECK(cap->state() == PulseStreamState::Connecting);
        CHECK_EQ(cap->stats().ring_frames, static_cast<size_t>(2.0 * c.rate)); /* ring_ms 2000 */
        cap->close();
        ++i;
    }
    /* an explicit rate and channel count win (the server converts) */
    PulseCaptureRequest r = req_for(evolve());
    r.rate = 48000;
    r.channels = 2;
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    CHECK(cap && cap->format().rate == 48000 && cap->format().channels == 2 && cap->format().device_rate == 16000);
    cap.reset();
    /* more than two channels are limited to two */
    PulseDevice multi = fake_usb_source("Multi", 48000, 4, 51);
    multi.name = "alsa_input.multi";
    f.add_source(multi);
    CHECK_EQ(f.open_capture(req_for("alsa_input.multi"), cap, nullptr), 0);
    CHECK(cap && cap->format().channels == 2);
}

void test_refusals() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap;
    std::string why;
    CHECK_EQ(f.open_capture(req_for("bluez_input.00_00_00_00_00_00.0"), cap, &why), kPulseNoDevice);
    CHECK(!cap);
    CHECK(why.find("bluez_input.00_00_00_00_00_00.0") != std::string::npos);
    CHECK(f.streams.empty()); /* absent: no stream was even created (never another device) */
    CHECK_EQ(f.open_capture(req_for(""), cap, &why), kPulseNoDevice);
    CHECK_EQ(f.open_capture(req_for("default"), cap, &why), kPulseNoDevice);
    CHECK_EQ(f.open_capture(req_for(fake_bt_monitor(kEvolve).name), cap, &why), kPulseRefused);
    CHECK(why.find("monitor") != std::string::npos);
    PulseDevice norate = fake_bt_source("01:02:03:04:05:06", 0, 1);
    f.add_source(norate);
    CHECK_EQ(f.open_capture(req_for(norate.name), cap, &why), kPulseRefused);
    f.connect_error_stream = kPulseRefused;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, &why), kPulseRefused);
    CHECK(!cap);
    f.disconnect();
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, &why), kPulseState);

    /* the shared format rules directly */
    PulseStreamFormat fmt;
    PulseDevice src = fake_bt_source(kEvolve);
    CHECK(!pulse_capture_format(req_for("@DEFAULT_SOURCE@"), src, fmt, &why));
    CHECK(pulse_capture_format(req_for(src.name), src, fmt, &why));
    PulseCaptureRequest r = req_for(src.name);
    r.rate = 500;
    CHECK(!pulse_capture_format(r, src, fmt, &why));
    PulsePlaybackRequest p;
    p.sink = "default";
    CHECK(!pulse_playback_format(p, src, fmt, &why));
    p.sink = "bluez_output.x";
    p.channels = 3;
    CHECK(!pulse_playback_format(p, src, fmt, &why));
    p.channels = 0;
    CHECK(!pulse_playback_format(p, src, fmt, &why));
}

void test_read_and_running() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, nullptr), 0);
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    CHECK(!ev.empty() && ev[0].kind == PulseEvent::Connecting && ev[0].device == evolve());
    std::vector<int16_t> buf(4096);
    CHECK_EQ(cap->read(buf.data(), 1024, 0), 0L);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(cap->read(buf.data(), 1024, 30), 0L); /* waited, nothing came */
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(25));
    f.advance(0.65); /* the profile switch (P4) */
    std::vector<int16_t> data = ramp(320, 1);
    CHECK(f.deliver(0, data.data(), 320));
    CHECK(cap->state() == PulseStreamState::Running);
    CHECK_EQ(cap->read(buf.data(), 100, 0), 100L);
    CHECK(std::equal(buf.begin(), buf.begin() + 100, data.begin()));
    CHECK_EQ(cap->read(buf.data(), 1024, 10), 220L);
    CHECK(std::equal(buf.begin(), buf.begin() + 220, data.begin() + 100));
    ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) {
        CHECK(ev[0].kind == PulseEvent::Running);
        CHECK_NEAR(ev[0].t, 0.65, 1e-9); /* the time of the first frame */
    }
    PulseCaptureStats st = cap->stats();
    CHECK_EQ(st.frames_in, 320u);
    CHECK_EQ(st.frames_read, 320u);
    CHECK_NEAR(st.first_frame_s, 0.65, 1e-9);
    CHECK_EQ(st.frames_overflow, 0u);
    /* a read woken by a delivery from another thread */
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.deliver(0, data.data(), 50);
    });
    CHECK_EQ(cap->read(buf.data(), 1024, 2000), 50L);
    t.join();
    PulseLatency l;
    CHECK_EQ(cap->latency(l), 0);
    CHECK_NEAR(l.total_ms, 20.0, 1e-9);
    CHECK(l.timing_info);
    f.hole(0, 40);
    CHECK_EQ(cap->stats().frames_hole, 40u);
}

void test_connecting_timeout() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, nullptr), 0);
    CHECK(cap->state() == PulseStreamState::Connecting);
    CHECK_NEAR(cap->connecting_seconds(), 0.0, 1e-9);
    f.advance(0.65);
    CHECK_NEAR(cap->connecting_seconds(), 0.65, 1e-9);
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 1u); /* Connecting only: 0.65 s is a normal switch */
    f.advance(2.0);
    CHECK_EQ(events(*cap).size(), 0u); /* 2.65 s: still tolerated */
    f.advance(0.4);
    ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) {
        CHECK(ev[0].kind == PulseEvent::StartTimeout);
        CHECK_EQ(ev[0].code, kPulseTimeout);
        CHECK_NEAR(ev[0].t, 3.05, 1e-9);
    }
    CHECK(cap->state() == PulseStreamState::Connecting); /* the stream stays open */
    CHECK(!cap->ended());
    f.advance(1.0);
    CHECK_EQ(events(*cap).size(), 0u); /* reported once */
    f.deliver_value(0, 100, 160);
    ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    CHECK(!ev.empty() && ev[0].kind == PulseEvent::Running && std::fabs(ev[0].t - 4.05) < 1e-9);
    CHECK(cap->state() == PulseStreamState::Running);
    CHECK_NEAR(cap->connecting_seconds(), 0.0, 1e-9);

    /* frames in time: no StartTimeout ever */
    std::unique_ptr<PulseCapture> c2;
    f.clock = 0;
    CHECK_EQ(f.open_capture(req_for(beats()), c2, nullptr), 0);
    f.advance(0.65);
    f.deliver_value(1, 7, 160);
    f.advance(10);
    ev = events(*c2);
    CHECK_EQ(ev.size(), 2u);
    CHECK(ev.size() == 2 && ev[0].kind == PulseEvent::Connecting && ev[1].kind == PulseEvent::Running);

    /* the timeout can be switched off */
    PulseCaptureRequest r = req_for(fake_bt_source(kCvsd).name);
    r.first_frame_timeout_ms = 0;
    std::unique_ptr<PulseCapture> c3;
    f.clock = 0;
    CHECK_EQ(f.open_capture(r, c3, nullptr), 0);
    f.advance(30);
    ev = events(*c3);
    CHECK_EQ(ev.size(), 1u);
    CHECK_NEAR(c3->connecting_seconds(), 30.0, 1e-9);
}

void test_killed() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, nullptr), 0);
    std::vector<int16_t> data = ramp(100, 1);
    f.deliver(0, data.data(), 100);
    f.advance(13.0);
    f.remove_source(evolve()); /* the headset switched off (P5 b8): DONT_MOVE -> the server kills the stream */
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 3u);
    if (ev.size() == 3) {
        CHECK(ev[0].kind == PulseEvent::Connecting);
        CHECK(ev[1].kind == PulseEvent::Running);
        CHECK(ev[2].kind == PulseEvent::Killed);
        CHECK_EQ(ev[2].code, kPulseKilled);
        CHECK_EQ(ev[2].device, evolve());
        CHECK_NEAR(ev[2].t, 13.0, 1e-9);
        CHECK_EQ(ev[2].frame, 100u);
    }
    CHECK(cap->ended());
    CHECK(cap->state() == PulseStreamState::Ended);
    CHECK_EQ(cap->end_code(), kPulseKilled);
    std::vector<int16_t> buf(1024);
    CHECK_EQ(cap->read(buf.data(), 1024, 100), 100L); /* the frames before the end are kept */
    CHECK(std::equal(buf.begin(), buf.begin() + 100, data.begin()));
    CHECK_EQ(cap->read(buf.data(), 1024, 100), static_cast<long>(kPulseKilled));
    CHECK_EQ(cap->read(buf.data(), 1024, 0), static_cast<long>(kPulseKilled));
    CHECK(f.streams[0].attached.empty());
    CHECK(!cap->source_identity_ok());
    PulseLatency l;
    CHECK_EQ(cap->latency(l), kPulseKilled);
    /* the device comes back: the ended stream stays ended (the owner opens a new one) */
    f.add_source(fake_bt_source(kEvolve, 16000, 70));
    CHECK(cap->ended());
    CHECK_EQ(events(*cap).size(), 0u);
}

void test_dont_move() {
    /* a server that ignores DONT_MOVE (as P5 measured without it) moves the stream to the default source: the stream ends
     * with Moved, and no frame of the other device reaches the reader */
    FakePulse f;
    setup(f);
    f.honour_dont_move = false;
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, nullptr), 0);
    f.deliver_value(0, 1000, 160);
    f.remove_source(evolve());
    CHECK_EQ(f.streams[0].attached, usb());
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 3u);
    if (ev.size() == 3) {
        CHECK(ev[2].kind == PulseEvent::Moved);
        CHECK_EQ(ev[2].code, kPulseMoved);
        CHECK_EQ(ev[2].device, usb());
    }
    f.deliver_value(0, -555, 480); /* the other microphone's audio */
    std::vector<int16_t> buf(4096);
    CHECK_EQ(cap->read(buf.data(), 4096, 10), 160L);
    bool ours = true;
    for (int i = 0; i < 160; ++i) ours &= buf[static_cast<size_t>(i)] == 1000;
    CHECK(ours);
    CHECK_EQ(cap->read(buf.data(), 4096, 10), static_cast<long>(kPulseMoved));
    CHECK_EQ(cap->stats().frames_after_end, 480u);
    CHECK_EQ(cap->stats().frames_in, 160u);
    std::string actual;
    CHECK(!cap->source_identity_ok(&actual));
    CHECK_EQ(actual, usb());

    /* the same through an explicit move */
    std::unique_ptr<PulseCapture> c2;
    CHECK_EQ(f.open_capture(req_for(beats()), c2, nullptr), 0);
    f.move(1, "alsa_input.other");
    ev = events(*c2);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Moved && ev.back().device == "alsa_input.other");
    CHECK_EQ(c2->end_code(), kPulseMoved);

    /* a server that honours DONT_MOVE kills instead (the same removal) */
    FakePulse g;
    setup(g);
    std::unique_ptr<PulseCapture> c3;
    CHECK_EQ(g.open_capture(req_for(evolve()), c3, nullptr), 0);
    g.remove_source(evolve());
    ev = events(*c3);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Killed);
    CHECK(g.streams[0].attached.empty());

    /* a device that is not there at the start (P5 bt6), with the lookup skipped (the probe's diagnostics path): the
     * server refuses (DONT_MOVE honoured) or attaches it elsewhere (ignored): either way no stream is returned */
    PulseCaptureRequest r = req_for("bluez_input.00_00_00_00_00_00.0");
    r.lookup_first = false;
    std::unique_ptr<PulseCapture> c4;
    std::string why;
    CHECK_EQ(g.open_capture(r, c4, &why), kPulseNoDevice);
    CHECK(!c4);
    CHECK_EQ(g.streams.size(), 2u); /* it was really connected, and refused by the server */
    CHECK(!g.streams[1].open);
    g.honour_dont_move = false;
    g.server.default_source = usb();
    CHECK_EQ(g.open_capture(r, c4, &why), kPulseMoved);
    CHECK(!c4);
    CHECK(why.find(usb()) != std::string::npos);
    CHECK(!g.streams[2].open);
}

void test_suspend() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, nullptr), 0);
    f.deliver_value(0, 9, 160);
    events(*cap);
    f.advance(1);
    f.suspend(evolve(), true);
    CHECK(cap->state() == PulseStreamState::Suspended);
    f.suspend(evolve(), true); /* twice: one event */
    f.advance(1);
    f.suspend(evolve(), false);
    CHECK(cap->state() == PulseStreamState::Running);
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 2u);
    if (ev.size() == 2) {
        CHECK(ev[0].kind == PulseEvent::Suspended);
        CHECK_NEAR(ev[0].t, 1.0, 1e-9);
        CHECK(ev[1].kind == PulseEvent::Running);
        CHECK_NEAR(ev[1].t, 2.0, 1e-9);
    }
    CHECK(!cap->ended());
    /* fail_on_suspend: the server kills the stream instead */
    PulseCaptureRequest r = req_for(beats());
    r.fail_on_suspend = true;
    std::unique_ptr<PulseCapture> c2;
    CHECK_EQ(f.open_capture(r, c2, nullptr), 0);
    CHECK((f.streams[1].flags & pa::kStreamFailOnSuspend) != 0);
    f.suspend(beats(), true);
    ev = events(*c2);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Killed && ev.back().code == kPulseKilled);
    CHECK(c2->ended());
}

void test_identity() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, nullptr), 0);
    std::string actual;
    CHECK(cap->source_identity_ok(&actual));
    CHECK_EQ(actual, evolve());
    CHECK_EQ(f.streams[0].identity_queries, 1u); /* asked the server, not a cached name */
    /* the server re-links the stream silently (no moved notification): the guard sees it */
    f.streams[0].attached = usb();
    CHECK(!cap->source_identity_ok(&actual));
    CHECK_EQ(actual, usb());
    cap.reset();

    /* the server attaches a new stream to another device: the open refuses (closed again) */
    f.attach_elsewhere = usb();
    std::string why;
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, &why), kPulseMoved);
    CHECK(!cap);
    CHECK(why.find(usb()) != std::string::npos);
    CHECK(!f.streams[1].open);

    /* ... the same when the server accepts it only after the open returned */
    f.defer_ready = true;
    f.attach_elsewhere = usb();
    CHECK_EQ(f.open_capture(req_for(evolve()), cap, &why), 0);
    CHECK(cap && cap->state() == PulseStreamState::Connecting);
    f.ready(2);
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 2u);
    CHECK(ev.size() == 2 && ev[1].kind == PulseEvent::Moved && ev[1].device == usb());
    CHECK(cap->ended());
    cap.reset();

    /* deferred and right: accepted, the device filled in then */
    f.defer_ready = true;
    CHECK_EQ(f.open_capture(req_for(beats()), cap, &why), 0);
    CHECK(cap->format().device.empty());
    f.ready(3);
    CHECK_EQ(cap->format().device, beats());
    CHECK(!cap->ended());
    CHECK(cap->source_identity_ok());
}

void test_errors() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> a, b, c, d;
    CHECK_EQ(f.open_capture(req_for(evolve()), a, nullptr), 0);
    CHECK_EQ(f.open_capture(req_for(beats()), b, nullptr), 0);
    CHECK_EQ(f.open_capture(req_for(usb()), c, nullptr), 0);
    f.fail(0, kPulseFailed);
    f.fail(1, kPulseNoDevice);
    std::vector<PulseStreamEvent> ev = events(*a);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Error && ev.back().code == kPulseFailed);
    ev = events(*b);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Killed && ev.back().code == kPulseNoDevice);
    f.fail(0, kPulseKilled); /* the first end wins */
    CHECK_EQ(a->end_code(), kPulseFailed);
    CHECK_EQ(events(*a).size(), 0u);
    f.crash(); /* the server went away */
    ev = events(*c);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Killed && ev.back().code == kPulseDisconnected);
    std::vector<int16_t> buf(16);
    CHECK_EQ(c->read(buf.data(), 16, 0), static_cast<long>(kPulseDisconnected));
    CHECK(!f.connected());
    f.connect("Cassette", 100);
    CHECK_EQ(f.open_capture(req_for(evolve()), d, nullptr), 0);
    f.disconnect(); /* our own disconnect: the open streams end too */
    ev = events(*d);
    CHECK(!ev.empty() && ev.back().kind == PulseEvent::Killed && ev.back().code == kPulseDisconnected);
}

void test_overflow_and_close() {
    FakePulse f;
    setup(f);
    PulseCaptureRequest r = req_for(evolve());
    r.ring_ms = 100; /* 1600 frames at 16 kHz */
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    CHECK_EQ(cap->stats().ring_frames, 1600u);
    std::vector<int16_t> data = ramp(1000, 1);
    f.deliver(0, data.data(), 1000);
    f.deliver(0, data.data(), 1000); /* the reader is late: 400 do not fit */
    PulseCaptureStats st = cap->stats();
    CHECK_EQ(st.frames_in, 2000u);
    CHECK_EQ(st.frames_overflow, 400u);
    std::vector<int16_t> buf(4096);
    CHECK_EQ(cap->read(buf.data(), 4096, 0), 1600L);
    CHECK(std::equal(buf.begin(), buf.begin() + 1000, data.begin())); /* the oldest are kept, the newest dropped */
    f.deliver(0, data.data(), 1000);
    CHECK_EQ(cap->read(buf.data(), 4096, 0), 1000L);
    CHECK_EQ(cap->stats().frames_overflow, 400u);

    cap->close();
    CHECK(!f.streams[0].open);
    CHECK(f.streams[0].cap == nullptr);
    CHECK_EQ(cap->read(buf.data(), 16, 0), static_cast<long>(kPulseState));
    CHECK(cap->state() == PulseStreamState::Ended);
    CHECK(!f.deliver(0, data.data(), 100)); /* the server's late frames go nowhere */
    CHECK_EQ(f.streams[0].delivered_after_close, 100u);
    CHECK_EQ(cap->stats().frames_in, 3000u);
    CHECK(!cap->source_identity_ok());
    PulseLatency l;
    CHECK_EQ(cap->latency(l), kPulseState);
    cap->close(); /* idempotent */
    PulseStreamEvent ev;
    while (cap->poll_event(ev)) {
    }
    cap.reset();
    CHECK_EQ(f.streams[0].delivered_after_close, 100u);
}

/* ---- silence */

std::unique_ptr<PulseCapture> open_running(FakePulse &f, const std::string &name, double silence_s = 3.0) {
    PulseCaptureRequest r = req_for(name);
    r.silence_seconds = silence_s;
    r.ring_ms = 30000;
    std::unique_ptr<PulseCapture> cap;
    f.open_capture(r, cap, nullptr);
    if (cap) {
        PulseStreamEvent ev;
        cap->poll_event(ev); /* Connecting */
    }
    return cap;
}

void test_silence_threshold() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap = open_running(f, beats()); /* P3: the Beats runs and sends zeros */
    CHECK(cap != nullptr);
    if (!cap) return;
    f.deliver_value(0, 0, 47999); /* 3 s at 16 kHz less one frame */
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 1u); /* Running only */
    CHECK(!cap->stats().silent);
    CHECK_EQ(cap->stats().zero_run, 47999u);
    f.deliver_value(0, 0, 1);
    ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) {
        CHECK(ev[0].kind == PulseEvent::SilenceDetected);
        CHECK_EQ(ev[0].frame, 48000u);
        CHECK_NEAR(ev[0].t, 3.0, 1e-9);
    }
    CHECK(cap->stats().silent);
    CHECK(cap->state() == PulseStreamState::Running); /* running and silent: exactly P3 */
    f.deliver_value(0, 0, 160000);
    CHECK_EQ(events(*cap).size(), 0u); /* once */
    /* a sound ends it, at the first non-zero sample */
    std::vector<int16_t> chunk(160, 0);
    chunk[40] = 3;
    f.deliver(0, chunk.data(), 160);
    ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) {
        CHECK(ev[0].kind == PulseEvent::SilenceCleared);
        CHECK_EQ(ev[0].frame, 48000u + 160000u + 40u);
    }
    CHECK(!cap->stats().silent);
    CHECK_EQ(cap->stats().zero_run, 119u); /* the zeros after the sample */
}

void test_silence_low_level() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap = open_running(f, evolve());
    /* a very quiet room: +-1 LSB of noise for 10 s is NOT digital silence */
    std::vector<int16_t> quiet(1600);
    for (size_t i = 0; i < quiet.size(); ++i) quiet[i] = (i % 2) ? 1 : -1;
    for (int k = 0; k < 100; ++k) f.deliver(0, quiet.data(), 1600);
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 1u);
    CHECK(!cap->stats().silent);
    CHECK_EQ(cap->stats().zero_run, 0u);
    /* zeros broken by one sample every 2.9 s never reach 3 s */
    std::vector<int16_t> sparse(46400, 0);
    sparse.back() = 1;
    for (int k = 0; k < 5; ++k) f.deliver(0, sparse.data(), sparse.size());
    CHECK_EQ(events(*cap).size(), 0u);
    CHECK(!cap->stats().silent);
}

void test_silence_sequences() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulseCapture> cap = open_running(f, fake_bt_source(kCvsd).name, 1.0); /* 1 s at 8 kHz = 8000 */
    std::vector<int16_t> one(1, 5);
    /* detected, cleared, detected again between two polls: three events in order */
    f.deliver_value(0, 0, 8000);
    f.deliver(0, one.data(), 1);
    f.deliver_value(0, 0, 9000);
    std::vector<PulseStreamEvent> ev = events(*cap);
    CHECK_EQ(ev.size(), 4u);
    if (ev.size() == 4) {
        CHECK(ev[0].kind == PulseEvent::Running);
        CHECK(ev[1].kind == PulseEvent::SilenceDetected);
        CHECK(ev[2].kind == PulseEvent::SilenceCleared);
        CHECK(ev[3].kind == PulseEvent::SilenceDetected);
        CHECK_EQ(ev[3].frame, 8001u + 8000u); /* the last one has its exact position */
    }
    /* one chunk: a sound at the start, then more than the threshold of zeros */
    std::unique_ptr<PulseCapture> c2 = open_running(f, beats(), 1.0); /* 16000 frames */
    std::vector<int16_t> big(20000, 0);
    big[0] = 100;
    f.deliver(1, big.data(), big.size());
    ev = events(*c2);
    CHECK_EQ(ev.size(), 2u);
    CHECK(ev.size() == 2 && ev[1].kind == PulseEvent::SilenceDetected && ev[1].frame == 1u + 16000u);

    /* stereo: one silent channel is not silence */
    std::unique_ptr<PulseCapture> c3 = open_running(f, usb(), 0.5); /* 24000 frames at 48 kHz */
    std::vector<int16_t> lr(2 * 48000, 0);
    for (size_t i = 1; i < lr.size(); i += 2) lr[i] = -2; /* right channel only */
    f.deliver(2, lr.data(), 48000);
    CHECK_EQ(events(*c3).size(), 1u);
    CHECK(!c3->stats().silent);
    std::vector<int16_t> both(2 * 24000, 0);
    f.deliver(2, both.data(), 24000);
    ev = events(*c3);
    CHECK(ev.size() == 1 && ev[0].kind == PulseEvent::SilenceDetected);

    /* off */
    std::unique_ptr<PulseCapture> c4 = open_running(f, evolve(), 0.0);
    f.deliver_value(3, 0, 16000 * 20);
    CHECK_EQ(events(*c4).size(), 1u);
    CHECK(!c4->stats().silent);

    /* no frames is Connecting, not silence */
    std::unique_ptr<PulseCapture> c5 = open_running(f, evolve(), 1.0);
    f.advance(3.5);
    ev = events(*c5);
    CHECK(ev.size() == 1 && ev[0].kind == PulseEvent::StartTimeout);
    CHECK(!c5->stats().silent);

    /* frames after the end do not count */
    std::unique_ptr<PulseCapture> c6 = open_running(f, beats(), 1.0);
    f.deliver_value(5, 0, 8000);
    f.fail(5, kPulseKilled);
    f.deliver_value(5, 0, 16000);
    ev = events(*c6);
    CHECK(ev.size() == 2 && ev[0].kind == PulseEvent::Running && ev[1].kind == PulseEvent::Killed);
    CHECK(!c6->stats().silent);
}

}  // namespace

void test_capture() {
    test_native_rates();
    test_refusals();
    test_read_and_running();
    test_connecting_timeout();
    test_killed();
    test_dont_move();
    test_suspend();
    test_identity();
    test_errors();
    test_overflow_and_close();
}

void test_silence() {
    test_silence_threshold();
    test_silence_low_level();
    test_silence_sequences();
}
