/*
 * SPDX-License-Identifier: MIT
 *
 * Playback streams on the fake server: open, write into the bounded ring, the server consuming it, underrun counting,
 * drop, drain, delay, latency, and the same kill / move / suspend / identity behaviour as capture.
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_abi.hpp"

#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";
const std::string kXwf = "9E:0C:81:E6:5D:1A";

void setup(FakePulse &f) {
    f.sinks.push_back(fake_bt_sink(kEvolve, 70, "headset"));
    f.sinks.push_back(fake_bt_sink(kXwf, 80, "speaker"));
    f.sinks.push_back(fake_usb_sink("Jabra_EVOLVE_LINK", 50));
    f.server.default_sink = fake_usb_sink("Jabra_EVOLVE_LINK").name;
    f.connect("Cassette", 1000);
}

PulsePlaybackRequest req_for(const std::string &sink, uint32_t rate = 48000, unsigned channels = 2, uint32_t ms = 100) {
    PulsePlaybackRequest r;
    r.sink = sink;
    r.rate = rate;
    r.channels = channels;
    r.buffer_ms = ms;
    return r;
}

std::vector<PulseStreamEvent> events(PulsePlayback &p) {
    std::vector<PulseStreamEvent> v;
    PulseStreamEvent ev;
    while (p.poll_event(ev)) v.push_back(ev);
    return v;
}

std::vector<int16_t> ramp(size_t frames, unsigned channels) {
    std::vector<int16_t> v(frames * channels);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int16_t>(i % 30000);
    return v;
}

void test_open_write_underrun() {
    FakePulse f;
    setup(f);
    const std::string sink = fake_bt_sink(kEvolve).name;
    std::unique_ptr<PulsePlayback> p;
    std::string why;
    CHECK_EQ(f.open_playback(req_for(sink), p, &why), 0);
    CHECK(p != nullptr);
    if (!p) return;
    std::vector<PulseStreamEvent> ev = events(*p);
    CHECK_EQ(ev.size(), 2u);
    CHECK(ev.size() == 2 && ev[0].kind == PulseEvent::Connecting && ev[1].kind == PulseEvent::Running);
    CHECK(p->state() == PulseStreamState::Running);
    CHECK((f.streams[0].flags & pa::kStreamDontMove) != 0);
    CHECK_EQ(f.streams[0].attr.tlength, pulse_ms_to_bytes(100, 48000, 2));
    CHECK_EQ(p->format().device, sink);
    CHECK_EQ(p->stats().ring_frames, 4800u); /* ring_ms 0 -> buffer_ms */
    CHECK(p->sink_identity_ok());

    std::vector<int16_t> data = ramp(6000, 2);
    CHECK_EQ(p->write(data.data(), 6000, 0), 4800L); /* as much as fits */
    CHECK_EQ(p->write(data.data() + 4800 * 2, 1200, 0), 0L); /* full, no wait */
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(p->write(data.data() + 4800 * 2, 1200, 20), 0L); /* full, waited */
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(15));
    std::vector<int16_t> got;
    CHECK_EQ(f.consume(0, 4800, &got), 4800u);
    CHECK(got.size() == 4800u * 2 && std::equal(got.begin(), got.end(), data.begin()));
    CHECK_EQ(p->underruns(), 0u);
    CHECK_EQ(f.consume(0, 480), 0u); /* the server wants more than there is: an underrun */
    CHECK_EQ(p->underruns(), 1u);
    CHECK_EQ(p->write(data.data(), 100, 0), 100L);
    CHECK_EQ(f.consume(0, 480), 100u); /* short again */
    CHECK_EQ(p->underruns(), 2u);
    f.underflow(0);
    CHECK_EQ(p->stats().underruns, 3u);
    PulsePlaybackStats st = p->stats();
    CHECK_EQ(st.frames_written, 4900u);
    CHECK_EQ(st.frames_to_server, 4900u);

    /* a writer that waits for room, freed by the server on another thread */
    CHECK_EQ(p->write(data.data(), 4800, 0), 4800L);
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.consume(0, 960);
    });
    CHECK_EQ(p->write(data.data(), 2000, 2000), 960L);
    t.join();

    /* drop: discarded at once */
    p->drop();
    CHECK_EQ(p->stats().frames_dropped, 4800u);
    CHECK_EQ(f.consume(0, 10), 0u); /* nothing left: the server underruns */
    CHECK_EQ(p->underruns(), 4u);
    /* delay = the ring + the server's latency (20 ms = 960 frames) */
    CHECK_EQ(p->write(data.data(), 480, 0), 480L);
    CHECK_EQ(p->delay_frames(), 480L + 960L);
    PulseLatency l;
    CHECK_EQ(p->latency(l), 0);
    CHECK_NEAR(l.total_ms, 20.0, 1e-9);
    /* drain: everything played */
    CHECK_EQ(p->drain(500), 0);
    CHECK_EQ(p->delay_frames(), 960L);
    p->close();
    CHECK(!f.streams[0].open);
    CHECK_EQ(p->write(data.data(), 10, 0), static_cast<long>(kPulseState));
    CHECK_EQ(p->drain(10), kPulseState);
    f.underflow(0); /* after the close: nothing reaches the stream */
    CHECK_EQ(p->underruns(), 4u);
}

void test_mono_and_rates() {
    FakePulse f;
    setup(f);
    std::unique_ptr<PulsePlayback> p;
    PulsePlaybackRequest r = req_for(fake_usb_sink("Jabra_EVOLVE_LINK").name, 16000, 1, 20);
    r.ring_ms = 40;
    CHECK_EQ(f.open_playback(r, p, nullptr), 0);
    CHECK(p && p->format().rate == 16000 && p->format().channels == 1);
    CHECK_EQ(f.streams[0].attr.tlength, 640u); /* 20 ms of 16 kHz mono */
    CHECK_EQ(p->stats().ring_frames, 640u);    /* 40 ms */
    std::unique_ptr<PulsePlayback> q;
    std::string why;
    CHECK_EQ(f.open_playback(req_for("bluez_output.00_00_00_00_00_00.1"), q, &why), kPulseNoDevice);
    CHECK(why.find("00_00_00_00_00_00") != std::string::npos);
    CHECK_EQ(f.open_playback(req_for(fake_bt_sink(kXwf).name, 48000, 3), q, &why), kPulseRefused);
    CHECK_EQ(f.open_playback(req_for("default"), q, &why), kPulseNoDevice);
    f.connect_error_stream = kPulseRefused;
    CHECK_EQ(f.open_playback(req_for(fake_bt_sink(kXwf).name), q, &why), kPulseRefused);
    CHECK(!q);
}

void test_end_events() {
    FakePulse f;
    setup(f);
    const std::string evo = fake_bt_sink(kEvolve).name;
    const std::string xwf = fake_bt_sink(kXwf).name;
    std::unique_ptr<PulsePlayback> a, b;
    CHECK_EQ(f.open_playback(req_for(evo), a, nullptr), 0);
    CHECK_EQ(f.open_playback(req_for(xwf), b, nullptr), 0);
    events(*a);
    events(*b);
    f.suspend(evo, true);
    CHECK(a->state() == PulseStreamState::Suspended);
    f.suspend(evo, false);
    std::vector<PulseStreamEvent> ev = events(*a);
    CHECK(ev.size() == 2 && ev[0].kind == PulseEvent::Suspended && ev[1].kind == PulseEvent::Running);
    f.remove_sink(evo); /* DONT_MOVE honoured: killed */
    ev = events(*a);
    CHECK(ev.size() == 1 && ev[0].kind == PulseEvent::Killed && ev[0].code == kPulseKilled);
    std::vector<int16_t> d(200, 1);
    CHECK_EQ(a->write(d.data(), 100, 0), static_cast<long>(kPulseKilled));
    CHECK_EQ(a->drain(10), kPulseKilled);
    CHECK(a->state() == PulseStreamState::Ended);
    f.honour_dont_move = false; /* a server that moves anyway: Moved, ended, nothing more is written */
    f.remove_sink(xwf);
    ev = events(*b);
    CHECK(ev.size() == 1 && ev[0].kind == PulseEvent::Moved && ev[0].device == f.server.default_sink);
    CHECK_EQ(b->write(d.data(), 100, 0), static_cast<long>(kPulseMoved));
    std::string actual;
    CHECK(!b->sink_identity_ok(&actual));
    CHECK_EQ(actual, f.server.default_sink);

    /* deferred acceptance, and a server that attaches it elsewhere */
    std::unique_ptr<PulsePlayback> c;
    f.defer_ready = true;
    f.attach_elsewhere = xwf;
    CHECK_EQ(f.open_playback(req_for(f.server.default_sink), c, nullptr), 0);
    CHECK(c->state() == PulseStreamState::Connecting);
    CHECK_EQ(c->write(d.data(), 100, 0), 100L); /* queued before the server is ready */
    f.ready(2);
    ev = events(*c);
    CHECK(ev.size() == 2 && ev[1].kind == PulseEvent::Moved && ev[1].device == xwf);
    CHECK(c->ended());
    f.defer_ready = false;
    std::unique_ptr<PulsePlayback> e;
    CHECK_EQ(f.open_playback(req_for(f.server.default_sink), e, nullptr), 0);
    f.fail(3, kPulseFailed);
    ev = events(*e);
    CHECK(ev.size() == 3 && ev[2].kind == PulseEvent::Error && ev[2].code == kPulseFailed);
}

}  // namespace

void test_playback() {
    test_open_write_underrun();
    test_mono_and_rates();
    test_end_events();
}
