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
#include "engine/engine.hpp"
#include "volume/output_volume.hpp"

#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
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
    CHECK_EQ(app_volume_of(s, "bt:C"), 100); /* one never set: 100 (unity: the software level of a card without a mixer) */
    set_app_volume(s, "bt:A", 140);
    CHECK_EQ(app_volume_of(s, "bt:A"), 100);
    set_app_volume(s, "bt:A", 35);
    /* a restart keeps them */
    ResumeState r;
    CHECK(resume_from_json(resume_to_json(s), r));
    CHECK_EQ(app_volume_of(r, "bt:A"), 35);
    CHECK_EQ(app_volume_of(r, "pulse:B"), 75);
    /* an older state.json with the old in-app volume: read without trouble, written back, never applied (D11 as changed) */
    CHECK(resume_from_json("{\"v\": 1, \"app_volume\": 62}", r));
    CHECK(r.app_volumes.empty());
    CHECK_EQ(r.app_volume, 62);
    CHECK_EQ(app_volume_of(r, "bt:A"), 100);
    ResumeState back;
    CHECK(resume_from_json(resume_to_json(r), back));
    CHECK_EQ(back.app_volume, 62);
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

/* a card's mixer controls, as snd_hctl lists them, with their values */
class FakeAlsa final : public AlsaApi {
public:
    std::vector<CtlInfo> ctls;
    std::map<std::string, long> values;
    int sets = 0;
    bool gone = false; /* the card is unplugged */
    bool available(std::string *) override { return true; }
    std::vector<CardInfo> cards() override { return {}; }
    int query_capture(const std::string &, CaptureOffer &) override { return -ENOSYS; }
    int open_capture(const std::string &, const HwRequest &, std::unique_ptr<AlsaPcm> &, HwGranted &) override { return -ENOSYS; }
    int mixer_list(const std::string &card, std::vector<CtlInfo> &out) override {
        last_card = card;
        if (gone) return -ENODEV;
        out = ctls;
        return 0;
    }
    int mixer_get(const std::string &, const std::string &name, long &value) override {
        if (gone) return -ENODEV;
        auto it = values.find(name);
        if (it == values.end()) return -ENOENT;
        value = it->second;
        return 0;
    }
    int mixer_set(const std::string &, const std::string &name, long value) override {
        if (gone) return -ENODEV;
        values[name] = value;
        ++sets;
        return 0;
    }
    std::string last_card;
};

CtlInfo vol_ctl(const std::string &name, long min, long max, bool db = false, long dmin = 0, long dmax = 0) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Integer;
    c.capture = name.find("Capture") != std::string::npos;
    c.count = 2;
    c.min = min;
    c.max = max;
    c.has_db = db;
    c.db_min_cdb = dmin;
    c.db_max_cdb = dmax;
    return c;
}

CtlInfo switch_ctl(const std::string &name) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Boolean;
    c.count = 2;
    c.max = 1;
    return c;
}

/* The owner's decision (2026-11-13): levels fitted to each output, linear in dB, about 2 dB a level over at most the top
 * 60 dB of its real range (Dr. Lex). The model's arithmetic and its bounds. */
void test_volume_model() {
    /* an output whose range is not known (Bluetooth, software sinks): -60..0 dB, 30 levels of 60 / 29 dB */
    const OutputVolumeModel d = default_volume_model();
    CHECK_NEAR(d.lo_db, -60.0, 1e-9);
    CHECK_NEAR(d.hi_db, 0.0, 1e-9);
    CHECK_EQ(d.n, 30);
    CHECK_NEAR(d.step_db(), 60.0 / 29, 1e-9);
    CHECK_NEAR(d.db_of(1), -60.0, 1e-9); /* level 1: the window's bottom */
    CHECK_NEAR(d.db_of(30), 0.0, 1e-9);  /* level N: its top */
    CHECK_EQ(d.pct_of(0), 0);
    CHECK_EQ(d.pct_of(1), 3);
    CHECK_EQ(d.pct_of(15), 50);
    CHECK_EQ(d.pct_of(30), 100);
    for (int i = 1; i <= d.n; ++i) {
        CHECK_EQ(d.nearest(d.db_of(i)), i);
        CHECK_EQ(d.from_pct(d.pct_of(i)), i);
        if (i > 1) CHECK_NEAR(d.db_of(i) - d.db_of(i - 1), d.step_db(), 1e-9); /* linear in dB */
    }
    CHECK_EQ(d.from_pct(0), 0);
    CHECK_EQ(d.nearest(-500), 1); /* under the window: its first level */
    CHECK_EQ(d.nearest(12), 30);  /* over it: its top */
    CHECK_EQ(d.step(30, 1), 30);
    CHECK_EQ(d.step(1, -1), 1);
    /* a range wider than 60 dB keeps its top 60 dB; narrower ranges keep themselves */
    const OutputVolumeModel wide = volume_model(-102.38, 4.0, 0.01);
    CHECK_NEAR(wide.lo_db, -56.0, 1e-9);
    CHECK_NEAR(wide.hi_db, 4.0, 1e-9);
    CHECK_EQ(wide.n, 30);
    /* N = ceil(window / 2 dB): steps about 2 dB, never under 1 dB nor under the control's own step, at least 2 levels */
    for (double w : {60.0, 45.0, 34.0, 20.0, 10.0, 3.0, 2.0, 1.5, 1.0}) {
        const OutputVolumeModel m = volume_model(-w, 0);
        CHECK(m.n >= 2);
        CHECK_EQ(m.n, std::max(2, static_cast<int>(std::ceil(w / 2 - 1e-9))));
        CHECK(m.step_db() >= 1.0 - 1e-9);
        CHECK(m.step_db() <= 3.0 + 1e-9);
    }
    CHECK_EQ(volume_model(-45, 0).n, 23);
    CHECK_EQ(volume_model(-10, 0).n, 5);
    const OutputVolumeModel coarse = volume_model(-30, 0, 3.0); /* a control of 3 dB steps: its own steps */
    CHECK_EQ(coarse.n, 11);
    CHECK_NEAR(coarse.step_db(), 3.0, 1e-9);
    const OutputVolumeModel tiny = volume_model(-0.5, 0); /* half a dB: two levels, as close as the range allows */
    CHECK_EQ(tiny.n, 2);
    const OutputVolumeModel one = volume_model(0, 0); /* one value */
    CHECK_EQ(one.n, 1);
    CHECK_EQ(one.pct_of(1), 100);
    /* the sound server's dB (pa_sw_volume_from_dB / to_dB: the cube of the linear factor) */
    CHECK_EQ(sink_volume_for_db(0), 0x10000u);
    CHECK_EQ(sink_volume_for_db(-60), 6554u);   /* 0x10000 / 10 */
    CHECK_EQ(sink_volume_for_db(-1e9), 0u);
    CHECK_NEAR(sink_db_for_volume(0x10000), 0.0, 1e-9);
    CHECK_NEAR(sink_db_for_volume(0x8000), 60.0 * std::log10(0.5), 1e-9);
    CHECK(std::isinf(sink_db_for_volume(0)));
    for (int i = 1; i <= d.n; ++i) CHECK_EQ(sink_level_for_volume(d, sink_volume_for_db(d.db_of(i))), i);
    CHECK_EQ(sink_level_for_volume(d, 0), 0); /* a silent sink */
    /* the ALSA card behind a sink, from its properties */
    CHECK_EQ(pulse_alsa_card("1", "", ""), 1);
    CHECK_EQ(pulse_alsa_card("", "2", "hw:5"), 2);
    CHECK_EQ(pulse_alsa_card("", "", "hw:3"), 3);
    CHECK_EQ(pulse_alsa_card("", "", "front:1"), 1);
    CHECK_EQ(pulse_alsa_card("", "", "hw:1,0"), 1);
    CHECK_EQ(pulse_alsa_card("", "", "hw:CARD=2,DEV=0"), 2);
    CHECK_EQ(pulse_alsa_card("", "", "hw:CARD=Device"), -1); /* a name, not a number */
    CHECK_EQ(pulse_alsa_card("", "", "70:BF:92:53:6B:8B"), -1);
    CHECK_EQ(pulse_alsa_card("", "", ""), -1);
}

/* D11 as the owner changed it (2026-11-12) without a sound server: the playing card's own mixer, in its own levels */
void test_output_mixer() {
    /* the choice: Master, PCM, Speaker, Headphone, Headset, else any playback volume; never a capture control */
    MixerChoice m;
    CHECK(choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), vol_ctl("PCM Playback Volume", 0, 45),
                                 vol_ctl("Master Playback Volume", 0, 87)},
                                m));
    CHECK_EQ(m.volume, std::string("Master Playback Volume"));
    CHECK(choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), vol_ctl("Line Out Playback Volume", 0, 20),
                                 switch_ctl("Line Out Playback Switch")},
                                m));
    CHECK_EQ(m.volume, std::string("Line Out Playback Volume"));
    CHECK_EQ(m.sw, std::string("Line Out Playback Switch"));
    CHECK(!choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), switch_ctl("Mic Capture Switch")}, m));
    CHECK(!choose_playback_mixer({}, m));
    /* a control whose real top is above 0 dB keeps it (100 % = the output's top): -87..+12 dB over 0..99 */
    CHECK(choose_playback_mixer({vol_ctl("Master Playback Volume", 0, 99, true, -8700, 1200)}, m));
    CHECK_NEAR(m.model.hi_db, 12.0, 1e-9);
    CHECK_NEAR(m.model.lo_db, -48.0, 1e-9);
    CHECK_EQ(mixer_raw_for_level(m, m.model.n), 99L);
    /* a headset whose driver gives no dB (the kernel drops bogus values): its raw steps, at most 30 levels */
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 37)}, m));
    CHECK(!m.has_db);
    CHECK(!m.model.db);
    CHECK_EQ(m.model.n, 30);
    CHECK_EQ(mixer_raw_for_level(m, 30), 37L);
    CHECK(mixer_raw_for_level(m, 1) > 0); /* level 1 is above the minimum */
    for (int i = 1; i <= 30; ++i) CHECK_EQ(mixer_level_for_raw(m, mixer_raw_for_level(m, i)), i);
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 7)}, m));
    CHECK_EQ(m.model.n, 7); /* fewer raw steps than 30: one level each */

    /* the card behind an ALSA device */
    CHECK_EQ(mixer_card_for("plughw:CARD=UC02,DEV=0"), std::string("hw:CARD=UC02"));
    CHECK_EQ(mixer_card_for("hw:CARD=Device"), std::string("hw:CARD=Device"));
    CHECK_EQ(mixer_card_for("default"), std::string("default"));
    CHECK_EQ(mixer_card_for("plughw:1,0"), std::string("default"));

    /* read and write through the card (the fake mixer): the Apple adapter, -60..0 dB in 0.5 dB steps */
    FakeAlsa a;
    a.ctls = {vol_ctl("Headset Playback Volume", 0, 120, true, -6000, 0), switch_ctl("Headset Playback Switch")};
    a.values["Headset Playback Volume"] = 80; /* -20 dB, set by something else */
    a.values["Headset Playback Switch"] = 1;
    std::string chosen;
    MixerChoice c;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.has_mixer);
    CHECK_EQ(lv.level, 20); /* the nearest level: -60 + 19 * 60 / 29 = -20.69 dB */
    CHECK_EQ(lv.pct, 67);
    CHECK(!lv.muted);
    CHECK_EQ(chosen, std::string("hw:CARD=A"));
    CHECK_EQ(write_mixer_level(a, "hw:CARD=A", c, 21), 0); /* one level up: -18.62 dB */
    CHECK_EQ(a.values["Headset Playback Volume"], 83L);
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0); /* a fresh read */
    CHECK_EQ(lv.level, 21);
    CHECK_EQ(lv.pct, 70);
    CHECK_EQ(write_mixer_level(a, "hw:CARD=A", c, 0), 0); /* level 0: the switch mutes */
    CHECK_EQ(a.values["Headset Playback Switch"], 0L);
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.muted);
    CHECK_EQ(write_mixer_level(a, "hw:CARD=A", c, 1), 0); /* back up: un-muted, the window's bottom */
    CHECK_EQ(a.values["Headset Playback Switch"], 1L);
    CHECK_EQ(a.values["Headset Playback Volume"], 0L);
    /* changed elsewhere (alsamixer): the nearest level shows */
    a.values["Headset Playback Volume"] = 61; /* -29.5 dB */
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK_EQ(lv.level, 16); /* -28.97 dB is the nearest */
    /* the card unplugged: an error, no mixer; plugged back: found again */
    a.gone = true;
    CHECK(read_mixer(a, "hw:CARD=A", chosen, c, lv) < 0);
    CHECK(!lv.has_mixer);
    a.gone = false;
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.has_mixer);
    /* a card without a playback mixer: none, Vinyl's software level then */
    FakeAlsa bare;
    bare.ctls = {vol_ctl("Mic Capture Volume", 0, 30)};
    chosen.clear();
    CHECK_EQ(read_mixer(bare, "hw:CARD=B", chosen, c, lv), 0);
    CHECK(!lv.has_mixer);
    CHECK_EQ(write_mixer_level(bare, "hw:CARD=B", c, 5), -ENOENT);
    CHECK_EQ(bare.sets, 0);
    /* the software level of such a card: -60..0 dB, the amplitude of the level's dB */
    const OutputVolumeModel sw = default_volume_model();
    CHECK_NEAR(volume_gain_db(sw.db_of(30)), 1.0, 1e-6);
    CHECK_NEAR(volume_gain_db(sw.db_of(1)), 0.001, 1e-6);
    CHECK_NEAR(20.0 * std::log10(volume_gain_db(sw.db_of(15))), sw.db_of(15), 1e-4);
}

/* One table per output class (the board tests of the owner's decision, 2026-11-13). `name`: what the step table in the
 * log says; the tables print for the QA notes. */
void print_table(const char *name, const OutputVolumeModel &m) {
    const int mid = (m.n + 1) / 2;
    int at50 = 0;
    for (int i = 1; i <= m.n; ++i)
        if (m.pct_of(i) == 50) at50 = i;
    std::fprintf(stderr, "  levels %s: %s; the middle level %d = %d %% = %.2f dB%s\n", name, volume_model_text(m).c_str(), mid,
                 m.pct_of(mid), m.db_of(mid), at50 ? (", 50 % = level " + std::to_string(at50)).c_str() : ", no level shows 50 %");
}

void test_board_classes() {
    /* the Sharkoon DAC (HackBerry through PipeWire, zero8 without a server): 'PCM Playback Volume' 0..45 = -45..0 dB,
     * 1 dB a step, with its switch. 23 levels of 2.05 dB over the whole range; level 1 is the card's own -45 dB (not
     * mute: it sounds), the middle level 12 is -22.5 dB */
    MixerChoice dac;
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0), switch_ctl("PCM Playback Switch")}, dac));
    CHECK_EQ(dac.model.n, 23);
    CHECK_NEAR(dac.model.lo_db, -45.0, 1e-9);
    CHECK_NEAR(dac.model.hi_db, 0.0, 1e-9);
    CHECK_NEAR(dac.model.step_db(), 45.0 / 22, 1e-9);
    CHECK_NEAR(dac.model.db_of(12), -22.5, 1e-9);
    CHECK_EQ(mixer_raw_for_level(dac, 1), 0L);
    CHECK_EQ(mixer_raw_for_level(dac, 23), 45L);
    for (int i = 2; i <= dac.model.n; ++i) CHECK(mixer_raw_for_level(dac, i) > mixer_raw_for_level(dac, i - 1)); /* each level moves it */
    print_table("Sharkoon DAC -45..0 dB", dac.model);

    /* the same DAC on two boards: ALSA-direct (zero8) and as a sink of the server (the HackBerry, its card found from the
     * sink's properties): the same levels at the same dB, and a sink never gets a target under the card's -45 dB floor */
    const OutputVolumeModel through_server = sink_volume_model(&dac);
    CHECK_EQ(through_server.n, dac.model.n);
    CHECK_NEAR(through_server.lo_db, dac.model.lo_db, 1e-9);
    CHECK_NEAR(through_server.hi_db, dac.model.hi_db, 1e-9);
    for (int i = 1; i <= dac.model.n; ++i) {
        CHECK_NEAR(through_server.db_of(i), dac.model.db_of(i), 1e-9);
        CHECK_EQ(through_server.pct_of(i), dac.model.pct_of(i));
        CHECK(sink_volume_for_db(through_server.db_of(i)) >= sink_volume_for_db(-45.0) - 1);
    }
    MixerChoice dac_again; /* the same card model on another board: the same table */
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, dac_again));
    CHECK_EQ(dac_again.model.n, dac.model.n);
    for (int p = 0; p <= 100; ++p) CHECK_NEAR(dac_again.model.db_of(dac_again.model.from_pct(p)), dac.model.db_of(dac.model.from_pct(p)), 1e-9);

    /* the Raspberry Pi headphone jack (Pi 4B, Pi 3A+): -102.39..+4.00 dB in 0.01 dB, mute at min, joined mono: the top
     * 60 dB, -56..+4 dB (its +4 dB is its 100 %), 30 levels; level 1 (-56 dB) is far above the mute value */
    CtlInfo pi = vol_ctl("PCM Playback Volume", -10239, 400, true, kAlsaDbGainMute, 400);
    pi.count = 1;
    alsa_fix_mute_db(pi, true, -10238);
    MixerChoice jack;
    CHECK(choose_playback_mixer({pi, switch_ctl("PCM Playback Switch")}, jack));
    CHECK_EQ(jack.model.n, 30);
    CHECK_NEAR(jack.model.lo_db, -56.0, 1e-9);
    CHECK_NEAR(jack.model.hi_db, 4.0, 1e-9);
    CHECK_EQ(mixer_raw_for_level(jack, 1), -5600L);
    CHECK_EQ(mixer_raw_for_level(jack, 30), 400L);
    CHECK(mixer_raw_for_level(jack, 1) > jack.min + 1); /* the first level sounds */
    print_table("Pi headphone jack -102.39..+4.00 dB", jack.model);
    /* through a server the same card's levels sit under the sink's 0 dB (the server puts the hardware's top there) */
    const OutputVolumeModel jack_sink = sink_volume_model(&jack);
    CHECK_EQ(jack_sink.n, 30);
    CHECK_NEAR(jack_sink.lo_db, -60.0, 1e-9);
    CHECK_NEAR(jack_sink.step_db(), jack.model.step_db(), 1e-9);

    /* the Apple adapter (the deck zero7): 'Headset Playback Volume' 0..120 = -60..0 dB, 0.5 dB a step */
    MixerChoice apple;
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 120, true, -6000, 0), switch_ctl("Headset Playback Switch")}, apple));
    CHECK_EQ(apple.model.n, 30);
    CHECK_NEAR(apple.model.step_db(), 60.0 / 29, 1e-9);
    CHECK_EQ(mixer_raw_for_level(apple, 15), 58L); /* 50 %: -31.03 dB */
    print_table("Apple adapter -60..0 dB", apple.model);

    /* a sink with no known range (a Bluetooth headset's A2DP volume, a software sink): -60..0 dB */
    const OutputVolumeModel bt = sink_volume_model(nullptr);
    CHECK_EQ(bt.n, 30);
    CHECK_NEAR(bt.lo_db, -60.0, 1e-9);
    print_table("unknown range (Bluetooth, software sinks) -60..0 dB", bt);
    MixerChoice nodb;
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45)}, nodb));
    CHECK_EQ(sink_volume_model(&nodb).n, 30); /* a card without dB data: the unknown window */
}

/* D11 as changed with a sound server: the playing sink's own volume, set on the worker as the dB of a level, shown from
 * the server's event */
void test_output_sink_level() {
    const OutputVolumeModel m = default_volume_model();
    FakePulse f;
    PulseDevice dac = fake_usb_sink("Sharkoon", 50);
    dac.volume_raw = sink_volume_for_db(m.db_of(12)); /* 40 % */
    PulseDevice xwf = fake_bt_sink(kXwf, 80, "speaker");
    xwf.volume_raw = sink_volume_for_db(m.db_of(15));
    f.sinks = {dac, xwf};
    f.server.default_sink = xwf.name; /* the launcher's keys reach the XWF, Vinyl plays on the DAC */
    PulseWorker w(f);
    SinkWatch watch;
    ScanRequest rq;
    rq.connect = true;
    rq.callback = watch.callback();
    CHECK(w.request_scan(rq));
    ScanResult r;
    CHECK(wait_scan(w, r));
    watch.scanned(r);
    OutputWorld world;
    world.server = true;
    world.sinks = r.sinks;
    world.default_sink = r.info.default_sink;
    const PulseDevice *d = sink_device_of(world, dac.name);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_EQ(sink_level_for_volume(m, static_cast<uint32_t>(d->volume_raw)), 12);

    /* a level up sets the DAC (never the default sink), its mute cleared; the server's event brings the level back */
    CHECK(w.request_sink_volume(dac.name, 2, sink_volume_for_db(m.db_of(13)), 0, ""));
    for (int i = 0; i < 200 && w.sink_levels_done() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(watch.take_change());
    ScanRequest again;
    again.connect = true;
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    world.sinks = r.sinks;
    d = sink_device_of(world, dac.name);
    CHECK(d && sink_level_for_volume(m, static_cast<uint32_t>(d->volume_raw)) == 13);
    d = sink_device_of(world, xwf.name);
    CHECK(d && sink_level_for_volume(m, static_cast<uint32_t>(d->volume_raw)) == 15); /* the default sink untouched */
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK(!f.levels_set.empty() && f.levels_set.back().first == dac.name);
    }
    /* the read-back is a fresh ask of the sink */
    uint32_t back = 0;
    bool back_muted = true;
    CHECK_EQ(f.read_sink_volume(dac.name, back, back_muted), 0);
    CHECK_EQ(back, sink_volume_for_db(m.db_of(13)));
    CHECK(!back_muted);
    /* a mute (level kept), then quick presses: the newest level wins, no queue */
    CHECK(w.request_sink_volume(dac.name, 2, back, 1, ""));
    for (int i = 0; i < 200 && w.sink_levels_done() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK_EQ(f.read_sink_volume(dac.name, back, back_muted), 0);
    CHECK(back_muted);
    f.call_delay_ms.store(20);
    for (int lv = 14; lv <= 18; ++lv) CHECK(w.request_sink_volume(dac.name, 2, sink_volume_for_db(m.db_of(lv)), 0, ""));
    for (int i = 0; i < 300; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::recursive_mutex> g(f.lock());
        if (!f.levels_set.empty() && f.levels_set.back().second == sink_volume_for_db(m.db_of(18))) break;
    }
    f.call_delay_ms.store(0);
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK_EQ(f.levels_set.back().second, sink_volume_for_db(m.db_of(18)));
        CHECK(f.levels_set.size() < 8); /* two plus at most a few of the five: not one call each */
    }
    /* a sink that is gone: refused, nothing set */
    CHECK_EQ(f.set_sink_volume_raw("bluez_output.00_11_22_33_44_55.1", 2, 0x8000, 0), kPulseNoDevice);
    /* a reconnect: the device's own level comes back as the server reports it; the group shows its nearest level */
    f.remove_sink(xwf.name);
    PulseDevice again_xwf = xwf;
    again_xwf.index = kPulseNoIndex;
    again_xwf.volume_raw = 0x4000; /* 25 % of 0x10000: -36.1 dB */
    f.add_sink(again_xwf);
    CHECK(watch.take_change());
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    world.sinks = r.sinks;
    d = sink_device_of(world, xwf.name);
    CHECK(d && sink_level_for_volume(m, static_cast<uint32_t>(d->volume_raw)) == m.nearest(60.0 * std::log10(0.25)));
    w.stop();
}

/* B18 (2026-11-13): an unknown level is never shown or logged as a number: the sink removed, its info not read (the
 * list failed), a sink the server reports no volume for; a muted sink says Muted */
void test_unknown_level() {
    OutputWorld w;
    w.server = true;
    PulseDevice ev = fake_bt_sink(kEvolve, 70, "headset");
    ev.volume_pct = 45;
    w.sinks = {ev};
    w.default_sink = ev.name;
    int pct = -1;
    bool muted = false;
    CHECK(sink_volume_of(w, ev.name, pct, muted));
    CHECK_EQ(volume_chip_text(pct, muted), std::string("Vol 45"));
    /* the headset switched off: the sink is gone from the list */
    w.sinks.clear();
    pct = -7;
    CHECK(!sink_volume_of(w, ev.name, pct, muted));
    CHECK_EQ(volume_chip_text(-1, false), std::string("Vol --"));
    CHECK_EQ(volume_log_text(-1), std::string("-- (not known)"));
    /* the list could not be read (a server error): no server in the world, nothing known */
    OutputWorld lost;
    CHECK(!sink_volume_of(lost, ev.name, pct, muted));
    /* the server lists the sink without a level */
    ev.volume_pct = -1;
    w.sinks = {ev};
    CHECK(!sink_volume_of(w, ev.name, pct, muted));
    /* muted: Muted while the level is known, "--" when it is not */
    ev.volume_pct = 30;
    ev.muted = true;
    w.sinks = {ev};
    CHECK(sink_volume_of(w, ev.name, pct, muted));
    CHECK_EQ(volume_chip_text(pct, muted), std::string("Muted"));
    CHECK_EQ(volume_chip_text(-1, true), std::string("Vol --"));
    CHECK_EQ(volume_log_text(30), std::string("30 %"));
    for (int v : {-1, -2, -100}) {
        CHECK(volume_chip_text(v, false).find('-' + std::to_string(-v)) == std::string::npos);
        CHECK(volume_log_text(v).find(std::to_string(v)) == std::string::npos);
    }
}

/* the server's profile words are not a device's name (the board screenshot of 2026-11-13: "USB Audio...") */
void test_sink_names() {
    PulseDevice d;
    d.description = "USB Audio Device Analog Stereo";
    CHECK_EQ(sink_label(d), std::string("USB Audio Device"));
    d.description = "Sharkoon Gaming DAC Pro S Digital Stereo (IEC958)";
    CHECK_EQ(sink_label(d), std::string("Sharkoon Gaming DAC Pro S"));
    d.description = "Built-in Audio Digital Stereo (HDMI)";
    CHECK_EQ(sink_label(d), std::string("HDMI"));
    d.description = "USB sound card UC02";
    CHECK_EQ(sink_label(d), std::string("USB sound card UC02"));
    d.description = "Analog Stereo"; /* nothing but the words: kept */
    CHECK_EQ(sink_label(d), std::string("Analog Stereo"));
}

/* The owner's report from the deck (2026-11-13): "zero7 is showing muted but volume bar shows 18". With the levels of
 * the Sharkoon (its 'PCM Playback Volume' 0..45 = -45..0 dB and its switch): "-" down to level 1 (the card's -45 dB,
 * still heard), "-" there mutes, "-" while muted does nothing, "+" brings level 1 back; a mute by something else shows,
 * and "+" then un-mutes at the level kept. */
void test_mute_steps() {
    FakeAlsa a;
    a.ctls = {vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0), switch_ctl("PCM Playback Switch")};
    a.values["PCM Playback Volume"] = 23; /* -22 dB */
    a.values["PCM Playback Switch"] = 1;
    std::string chosen;
    MixerChoice c;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, "hw:CARD=S", chosen, c, lv), 0);
    CHECK_EQ(lv.level, 12);
    CHECK_EQ(lv.pct, 52);
    auto press = [&](int d) {
        const VolumePress p = mixer_press(c, lv, d);
        if (p.kind == VolumePress::Kind::Set) write_mixer_level(a, "hw:CARD=S", c, p.level);
        if (p.kind == VolumePress::Kind::Mute) write_mixer_switch(a, "hw:CARD=S", c, false);
        if (p.kind == VolumePress::Kind::Unmute) write_mixer_switch(a, "hw:CARD=S", c, true);
        read_mixer(a, "hw:CARD=S", chosen, c, lv);
        return p.kind;
    };
    /* one level a press, down to level 1 (raw 0, -45 dB) */
    for (int i = 11; i >= 1; --i) {
        CHECK(press(-1) == VolumePress::Kind::Set);
        CHECK_EQ(lv.level, i);
    }
    CHECK_EQ(lv.raw, 0L);
    CHECK(!lv.muted);
    CHECK_EQ(lv.pct, 4); /* round(100 / 23) */
    /* "-" at level 1 mutes (the switch); the level is kept */
    CHECK(press(-1) == VolumePress::Kind::Mute);
    CHECK(lv.muted);
    CHECK_EQ(a.values["PCM Playback Switch"], 0L);
    CHECK_EQ(lv.level, 1);
    /* "-" while muted does nothing */
    const int sets = a.sets;
    CHECK(press(-1) == VolumePress::Kind::None);
    CHECK_EQ(a.sets, sets);
    CHECK(lv.muted);
    /* "+" while muted: the level it had (level 1 sounds: the card's own floor) */
    CHECK(press(1) == VolumePress::Kind::Unmute);
    CHECK(!lv.muted);
    CHECK_EQ(lv.level, 1);
    CHECK(press(1) == VolumePress::Kind::Set);
    CHECK_EQ(lv.level, 2);
    /* muted by something else (alsamixer) at level 12: shown muted; "+" un-mutes at 12, not a level above */
    a.values["PCM Playback Volume"] = 23;
    a.values["PCM Playback Switch"] = 0;
    read_mixer(a, "hw:CARD=S", chosen, c, lv);
    CHECK(lv.muted);
    CHECK_EQ(lv.level, 12);
    CHECK(press(1) == VolumePress::Kind::Unmute);
    CHECK(!lv.muted);
    CHECK_EQ(lv.level, 12);
    /* the top: "+" does nothing more */
    a.values["PCM Playback Volume"] = 45;
    read_mixer(a, "hw:CARD=S", chosen, c, lv);
    CHECK_EQ(lv.level, 23);
    CHECK_EQ(lv.pct, 100);
    CHECK(press(1) == VolumePress::Kind::None);
    /* a card without a switch: level 1 is as low as it goes */
    MixerChoice nosw;
    choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, nosw);
    MixerLevel floor;
    floor.has_mixer = true;
    floor.raw = 0;
    floor.level = 1;
    CHECK(mixer_press(nosw, floor, -1).kind == VolumePress::Kind::None);
    /* the sound server's sinks: "-" at level 1 mutes (the sink's mute, the level kept), "+" un-mutes at the level, "-"
     * muted does nothing; a silent sink (volume 0) starts again at level 1 */
    const OutputVolumeModel m = default_volume_model();
    CHECK(sink_press(m, 12, true, -1).kind == VolumePress::Kind::None);
    VolumePress p = sink_press(m, 12, true, 1);
    CHECK(p.kind == VolumePress::Kind::Unmute && p.level == 12);
    p = sink_press(m, 0, false, 1);
    CHECK(p.kind == VolumePress::Kind::Set && p.level == 1);
    p = sink_press(m, 1, false, -1);
    CHECK(p.kind == VolumePress::Kind::Mute);
    p = sink_press(m, 5, false, -1);
    CHECK(p.kind == VolumePress::Kind::Set && p.level == 4);
    CHECK(sink_press(m, 30, false, 1).kind == VolumePress::Kind::None);
    CHECK(sink_press(m, -1, false, 1).kind == VolumePress::Kind::None);
    /* the server's placeholder at start never shows a level */
    OutputWorld w;
    w.server = true;
    PulseDevice null_sink;
    null_sink.name = "auto_null";
    null_sink.volume_pct = 100;
    null_sink.volume_raw = 0x10000;
    w.sinks = {null_sink};
    w.default_sink = "auto_null";
    int pct = -1;
    bool muted = false;
    CHECK(!sink_volume_of(w, "", pct, muted));
    CHECK(sink_device_of(w, "") == nullptr);
    CHECK(placeholder_sink("auto_null"));
    CHECK(!placeholder_sink("alsa_output.usb-Sharkoon.analog-stereo"));
}

/* The Pi 4B (2026-11-13, the owner: "i'm at 5% and it's like 80 to 100% perceived"): bcm2835 Headphones, 'PCM
 * Playback Volume' -10239..400 in hundredths of a dB (dBscale-min=-102.39dB step=0.01dB mute=1), joined mono, and
 * 'PCM Playback Switch'. snd_ctl_get_dB_range reports its min as SND_CTL_TLV_DB_GAIN_MUTE (-9999999); taken as is, every
 * level landed near raw 391 (+3.91 dB). The fixed range gives the jack its top 60 dB, -56..+4 dB, in 30 levels. */
void test_pi4b_pcm() {
    const std::string card = "hw:CARD=Headphones";
    auto pcm = [] {
        CtlInfo c = vol_ctl("PCM Playback Volume", -10239, 400, true, kAlsaDbGainMute, 400); /* as get_dB_range says */
        c.count = 1;
        return c;
    };
    /* unfixed, the sentinel is never taken for a dB value: no dB data then (raw steps), nothing near +3.91 dB for a low level */
    {
        MixerChoice bad;
        CHECK(choose_playback_mixer({pcm()}, bad));
        CHECK(!bad.has_db);
    }
    /* mixer_list's fix: the dB of min + 1 (snd_ctl_convert_to_dB) gives the real dB of min back */
    CtlInfo c = pcm();
    alsa_fix_mute_db(c, true, -10238);
    CHECK(c.has_db);
    CHECK(c.min_is_mute);
    CHECK_EQ(c.db_min_cdb, -10239L);
    CHECK_EQ(c.db_max_cdb, 400L);
    CtlInfo nodb = pcm(); /* without that dB: no dB data (raw steps), still never mute for a level above 0 */
    alsa_fix_mute_db(nodb, false, 0);
    CHECK(!nodb.has_db);
    CHECK(nodb.min_is_mute);
    CtlInfo plain = vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0); /* no mute flag: left alone */
    alsa_fix_mute_db(plain, true, -4400);
    CHECK(!plain.min_is_mute);
    CHECK_EQ(plain.db_min_cdb, -4500L);

    FakeAlsa a;
    a.ctls = {c, switch_ctl("PCM Playback Switch")};
    a.values["PCM Playback Volume"] = 393; /* where the old build left it (amixer: 393 [100%] [3.93dB]) */
    a.values["PCM Playback Switch"] = 1;
    std::string chosen;
    MixerChoice m;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, card, chosen, m, lv), 0);
    CHECK(m.has_db);
    CHECK(m.min_is_mute);
    CHECK_EQ(m.model.n, 30);
    CHECK_EQ(lv.level, 30); /* +3.93 dB: the top level */
    CHECK_EQ(lv.pct, 100);
    CHECK(!lv.muted);
    for (int i = 1; i <= 30; ++i) {
        const long raw = mixer_raw_for_level(m, i);
        CHECK(raw >= -5600 && raw <= 400); /* never under the window, never past the control's own top */
        CHECK_EQ(mixer_level_for_raw(m, raw), i);
    }
    /* a level a press from the top: what the model says, read back fresh */
    auto press = [&](int d) {
        const VolumePress p = mixer_press(m, lv, d);
        if (p.kind == VolumePress::Kind::Set) write_mixer_level(a, card, m, p.level);
        if (p.kind == VolumePress::Kind::Mute) write_mixer_switch(a, card, m, false);
        if (p.kind == VolumePress::Kind::Unmute) write_mixer_switch(a, card, m, true);
        read_mixer(a, card, chosen, m, lv);
        return p;
    };
    CHECK(press(-1).level == 29);
    CHECK_EQ(a.values["PCM Playback Volume"], 193L); /* +4 - 2.07 = 1.93 dB */
    for (int i = 0; i < 27; ++i) press(-1);
    CHECK_EQ(lv.level, 2);
    CHECK_EQ(a.values["PCM Playback Volume"], -5393L);
    press(-1);
    CHECK_EQ(lv.level, 1);
    CHECK_EQ(a.values["PCM Playback Volume"], -5600L); /* -56 dB: quiet, not the 80-100 % the old build gave */
    CHECK_EQ(lv.pct, 3);
    /* "-" at level 1: the switch mutes, the level kept; "+" brings level 1 back */
    CHECK(press(-1).kind == VolumePress::Kind::Mute);
    CHECK(lv.muted);
    CHECK_EQ(a.values["PCM Playback Volume"], -5600L);
    CHECK(press(1).kind == VolumePress::Kind::Unmute);
    CHECK(!lv.muted);
    CHECK_EQ(lv.level, 1);
    /* the control at its mute value (alsamixer to 0): shown muted, level 0; "+" sets level 1 */
    a.values["PCM Playback Volume"] = -10239;
    read_mixer(a, card, chosen, m, lv);
    CHECK(lv.muted);
    CHECK_EQ(lv.level, 0);
    CHECK_EQ(lv.pct, 0);
    const VolumePress up = press(1);
    CHECK(up.kind == VolumePress::Kind::Set && up.level == 1);
    CHECK_EQ(a.values["PCM Playback Volume"], -5600L);
    CHECK(!lv.muted);
    /* below the window, set by something else (-80 dB): the first level shows */
    a.values["PCM Playback Volume"] = -8000;
    read_mixer(a, card, chosen, m, lv);
    CHECK_EQ(lv.level, 1);
    /* the same card without its switch: "-" at level 1 goes to the mute value, "-" there does nothing */
    MixerChoice nosw;
    CHECK(choose_playback_mixer({c}, nosw));
    MixerLevel one;
    one.has_mixer = true;
    one.raw = -5600;
    one.level = 1;
    const VolumePress down = mixer_press(nosw, one, -1);
    CHECK(down.kind == VolumePress::Kind::Set && down.level == 0);
    CHECK_EQ(mixer_raw_for_level(nosw, 0), -10239L);
    MixerLevel zero;
    zero.has_mixer = true;
    zero.raw = -10239;
    zero.level = 0;
    zero.muted = true;
    CHECK(mixer_press(nosw, zero, -1).kind == VolumePress::Kind::None);
    /* the log line */
    CHECK_EQ(volume_set_log_text(card + " 'PCM Playback Volume'", m.model, 16, -2497, 0, -2497),
             std::string("set hw:CARD=Headphones 'PCM Playback Volume' step 16/30 53 % target -24.97 dB raw=-2497 read back "
                         "raw=-2497"));
    CHECK_EQ(volume_set_log_text("sink x", m.model, 0, 0, -19, 0), std::string("set sink x step 0/30 0 % target mute raw=0 read back failed (-19)"));
    CHECK_EQ(volume_model_text(m.model), std::string("-56.00..4.00 dB, 30 levels of 2.07 dB"));
}

}  // namespace

void test_volume() {
    test_volume_pct();
    test_volume_model();
    test_board_classes();
    test_pi4b_pcm();
    test_mute_steps();
    test_sink_names();
    test_unknown_level();
    test_output_mixer();
    test_output_sink_level();
    test_stream_identity();
    test_sink_volume_follows();
    test_volume_per_output();
}
