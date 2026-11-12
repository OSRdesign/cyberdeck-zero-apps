/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the volume after the owner's board tests (2026-11-12).
 *   B9  the Vol chip follows a sink's volume through the server's change events (SinkWatch, the fake pulse, the real
 *       PulseWorker), the sink's own level (sink_volume_of), the launcher's status.state without a server;
 *   B7  Vinyl's own volume is kept per output (output_volume_key, app_volume_of / set_app_volume) and survives a
 *       reconnect (the same key whatever the node's suffix) and a restart (state.json); the stream's identity and its
 *       own volume towards the server (the probe of 2026-11-12: application.process.binary said "cassette", and the
 *       servers restore a stream's volume by media role, shared with every app playing music).
 */
#include "check.hpp"

#include "app/pulse_worker.hpp"
#include "app/sink_watch.hpp"
#include "engine/outputs.hpp"
#include "fake_pulse.hpp"
#include "pulse/pulse_abi.hpp"
#include "state/resume.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

using namespace vinyl;

namespace {

bool wait_scan(PulseWorker &w, ScanResult &r) {
    for (int i = 0; i < 300; ++i) {
        if (w.take_scan(r)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

const char *kEvolve = "70:BF:92:53:6B:8B";
const char *kXwf = "9E:0C:81:E6:5D:1A";

void test_sink_volume_follows() {
    FakePulse f;
    PulseDevice ev = fake_bt_sink(kEvolve, 70, "headset");
    ev.volume_pct = 40;
    PulseDevice xwf = fake_bt_sink(kXwf, 80, "speaker");
    xwf.volume_pct = 70;
    f.sinks = {ev, xwf};
    f.server.default_sink = ev.name;
    PulseWorker w(f);
    SinkWatch watch;
    ScanRequest rq;
    rq.connect = true;
    rq.callback = watch.callback();
    CHECK(static_cast<bool>(rq.callback));
    CHECK(w.request_scan(rq));
    ScanResult r;
    CHECK(wait_scan(w, r));
    watch.scanned(r);
    CHECK(watch.installed());
    CHECK(!watch.callback()); /* not passed again while installed */
    CHECK(!watch.take_change());

    OutputWorld world;
    world.server = r.connected;
    world.sinks = r.sinks;
    world.default_sink = r.info.default_sink;
    int pct = -1;
    bool muted = true;
    CHECK(sink_volume_of(world, "", pct, muted)); /* nothing open yet: the default sink */
    CHECK_EQ(pct, 40);
    CHECK(!muted);
    CHECK(sink_volume_of(world, xwf.name, pct, muted)); /* the sink Vinyl plays to */
    CHECK_EQ(pct, 70);

    /* a volume key: the server's change event raises the flag at once; the next tick's scan has the new level */
    f.set_sink_volume(ev.name, 55);
    CHECK(watch.take_change());
    CHECK(!watch.take_change()); /* once per burst */
    const auto t0 = std::chrono::steady_clock::now();
    ScanRequest again;
    again.connect = true;
    again.callback = watch.callback();
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 400); /* the list round trip; with the 100 ms tick well under half a second */
    world.sinks = r.sinks;
    CHECK(sink_volume_of(world, ev.name, pct, muted));
    CHECK_EQ(pct, 55);
    f.set_sink_volume(ev.name, 55, true); /* mute */
    CHECK(watch.take_change());
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    world.sinks = r.sinks;
    CHECK(sink_volume_of(world, ev.name, pct, muted));
    CHECK(muted);

    /* a sink that comes back (a reconnect) raises it too, with its level as the server restored it */
    f.remove_sink(ev.name);
    CHECK(watch.take_change());
    PulseDevice back = ev;
    back.index = kPulseNoIndex;
    back.volume_pct = 80;
    f.add_sink(back);
    CHECK(watch.take_change());
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    world.sinks = r.sinks;
    CHECK(sink_volume_of(world, ev.name, pct, muted));
    CHECK_EQ(pct, 80);

    /* a busy worker: the change is kept for the next tick */
    watch.keep_change();
    CHECK(watch.take_change());

    /* without a server (ALSA boards) or a level: false, the caller shows the launcher's status.state */
    OutputWorld alsa;
    CHECK(!sink_volume_of(alsa, "", pct, muted));
    world.sinks[0].volume_pct = -1;
    CHECK(!sink_volume_of(world, world.sinks[0].name, pct, muted));
    w.stop();
}

void test_volume_pct() {
    const uint32_t norm[2] = {0x10000, 0x10000};
    CHECK_EQ(pulse_volume_pct(norm, 2), 100);
    const uint32_t half[2] = {0x8000, 0x4000};
    CHECK_EQ(pulse_volume_pct(half, 2), 50); /* the loudest channel */
    const uint32_t loud[1] = {0x18000};
    CHECK_EQ(pulse_volume_pct(loud, 1), 150);
    CHECK_EQ(pulse_volume_pct(norm, 0), -1);
    CHECK_EQ(pulse_volume_pct(nullptr, 2), -1);
}

void test_volume_per_output() {
    OutputTarget autot;
    OutputTarget bt = OutputTarget::from_key(std::string("bt:") + kEvolve);
    /* a Bluetooth sink: the MAC, whatever the node's suffix after a reconnect */
    CHECK_EQ(output_volume_key("bluez_output.70_BF_92_53_6B_8B.1", true, bt), std::string("bt:70:BF:92:53:6B:8B"));
    CHECK_EQ(output_volume_key("bluez_output.70_BF_92_53_6B_8B.2", true, autot), std::string("bt:70:BF:92:53:6B:8B"));
    CHECK_EQ(output_volume_key("bluez_output.70_bf_92_53_6b_8b.1", true, autot), std::string("bt:70:BF:92:53:6B:8B"));
    CHECK_EQ(output_volume_key("alsa_output.usb-UC02.analog-stereo", true, autot), std::string("pulse:alsa_output.usb-UC02.analog-stereo"));
    OutputTarget alsa = OutputTarget::from_key("alsa:plughw:CARD=UC02,DEV=0");
    CHECK_EQ(output_volume_key("", false, alsa), std::string("alsa:plughw:CARD=UC02,DEV=0"));
    CHECK_EQ(output_volume_key("", false, autot), std::string("auto"));

    ResumeState s;
    s.app_volume = 100;
    CHECK_EQ(app_volume_of(s, "bt:A"), 100);
    set_app_volume(s, "bt:A", 40);
    set_app_volume(s, "pulse:B", 75);
    CHECK_EQ(app_volume_of(s, "bt:A"), 40); /* each output keeps its own */
    CHECK_EQ(app_volume_of(s, "pulse:B"), 75);
    CHECK_EQ(app_volume_of(s, "bt:C"), 75); /* one never set: the last level set, never louder than what was chosen last */
    set_app_volume(s, "bt:A", 140);
    CHECK_EQ(app_volume_of(s, "bt:A"), 100);
    set_app_volume(s, "bt:A", 35);
    /* a restart keeps them */
    ResumeState r;
    CHECK(resume_from_json(resume_to_json(s), r));
    CHECK_EQ(app_volume_of(r, "bt:A"), 35);
    CHECK_EQ(app_volume_of(r, "pulse:B"), 75);
    CHECK_EQ(r.app_volume, 35);
    /* an older state.json without the map: every output starts from app_volume */
    CHECK(resume_from_json("{\"v\": 1, \"app_volume\": 62}", r));
    CHECK(r.app_volumes.empty());
    CHECK_EQ(app_volume_of(r, "bt:A"), 62);
    /* bounded */
    ResumeState many;
    for (int i = 0; i < 50; ++i) set_app_volume(many, "k" + std::to_string(i), i);
    CHECK(many.app_volumes.size() <= kMaxAppVolumes);
    CHECK(resume_from_json("{\"v\": 1, \"app_volumes\": {\"bt:A\": 300, \"bt:B\": \"x\"}}", r));
    CHECK_EQ(app_volume_of(r, "bt:A"), 100);
    CHECK_EQ(r.app_volumes.count("bt:B"), static_cast<size_t>(0));
}

std::string prop(const std::vector<std::pair<std::string, std::string>> &p, const std::string &k) {
    for (const auto &kv : p)
        if (kv.first == k) return kv.second;
    return "<none>";
}

bool says_cassette(const std::vector<std::pair<std::string, std::string>> &p) {
    for (const auto &kv : p) {
        std::string v = kv.second;
        for (char &c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (v.find("cassette") != std::string::npos) return true;
    }
    return false;
}

void test_stream_identity() {
    const auto ctx = pulse_context_props("Vinyl");
    CHECK_EQ(prop(ctx, "application.name"), std::string("Vinyl"));
    CHECK_EQ(prop(ctx, "application.id"), std::string("vinyl"));
    CHECK_EQ(prop(ctx, "application.process.binary"), std::string("vinyl"));
    CHECK_EQ(prop(ctx, "application.icon_name"), std::string("vinyl"));
    CHECK(!says_cassette(ctx));
    CHECK(!says_cassette(pulse_context_props("")));
    CHECK_EQ(prop(pulse_context_props(""), "application.name"), std::string("Vinyl"));

    /* the playback stream: its own restore key and no restored volume or target, the role kept */
    PulsePlaybackRequest rq;
    const auto pb = pulse_stream_props(rq.stream_name, rq.role, "Vinyl");
    CHECK_EQ(prop(pb, "media.name"), std::string("Vinyl"));
    CHECK_EQ(prop(pb, "media.role"), std::string("music"));
    CHECK_EQ(prop(pb, "application.name"), std::string("Vinyl"));
    CHECK_EQ(prop(pb, "application.id"), std::string("vinyl"));
    CHECK_EQ(prop(pb, "module-stream-restore.id"), std::string("sink-input-by-application-id:vinyl"));
    CHECK_EQ(prop(pb, "state.restore-props"), std::string("false"));
    CHECK_EQ(prop(pb, "state.restore-target"), std::string("false"));
    CHECK_EQ(prop(pb, "node.dont-move"), std::string("true"));
    CHECK_EQ(prop(pb, "node.dont-reconnect"), std::string("true"));
    CHECK(!says_cassette(pb));
    const auto cap = pulse_stream_props("Vinyl recording", "production", "", false);
    CHECK_EQ(prop(cap, "module-stream-restore.id"), std::string("source-output-by-application-id:vinyl"));
    CHECK(!says_cassette(cap));
    PulseCaptureRequest crq;
    CHECK(crq.stream_name.find("Cassette") == std::string::npos);

    /* opened unmuted at its own volume: 100 % on each channel */
    const uint32_t flags = pulse_playback_flags(rq);
    CHECK((flags & static_cast<uint32_t>(pa::kStreamStartUnmuted)) != 0);
    CHECK((flags & static_cast<uint32_t>(pa::kStreamDontMove)) != 0);
    const PulseVolume v2 = pulse_norm_volume(2);
    CHECK_EQ(v2.channels, 2u);
    CHECK_EQ(v2.values[0], 0x10000u);
    CHECK_EQ(v2.values[1], 0x10000u);
    CHECK_EQ(v2.values[2], 0u);
    CHECK_EQ(pulse_norm_volume(0).channels, 1u);
    CHECK_EQ(pulse_volume_pct(v2.values, v2.channels), 100);

    /* what the server gets: the fake records the stream's properties and flags */
    FakePulse f;
    f.sinks = {fake_bt_sink(kEvolve, 70, "headset")};
    f.server.default_sink = f.sinks[0].name;
    CHECK_EQ(f.connect("Vinyl", 500), 0);
    PulsePlaybackRequest open_rq;
    open_rq.sink = f.sinks[0].name;
    open_rq.rate = 44100;
    std::unique_ptr<PulsePlayback> pbk;
    std::string why;
    CHECK_EQ(f.open_playback(open_rq, pbk, &why), 0);
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK_EQ(f.streams.size(), static_cast<size_t>(1));
        if (!f.streams.empty()) {
            CHECK_EQ(prop(f.streams[0].props, "module-stream-restore.id"), std::string("sink-input-by-application-id:vinyl"));
            CHECK((f.streams[0].flags & static_cast<uint32_t>(pa::kStreamStartUnmuted)) != 0);
        }
    }
    if (pbk) pbk->close();
    f.disconnect();
}

}  // namespace

void test_volume() {
    test_volume_pct();
    test_stream_identity();
    test_sink_volume_follows();
    test_volume_per_output();
}
