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

/* D11 as the owner changed it (2026-11-12) without a sound server: the playing card's own mixer */
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

    /* the sound server's curve (the coordinator, 2026-11-13): dB = 60 log10(p / 100) and back */
    CHECK_EQ(volume_cdb_for(100), 0L);
    CHECK_EQ(volume_cdb_for(50), -1806L);
    CHECK_EQ(volume_cdb_for(40), -2388L);
    CHECK_EQ(volume_cdb_for(25), -3612L);
    CHECK(volume_cdb_for(0) < -100000);
    CHECK_EQ(volume_pct_for(0), 100);
    CHECK_EQ(volume_pct_for(-1806), 50);
    CHECK_EQ(volume_pct_for(-2388), 40);
    CHECK_EQ(volume_pct_for(-6000), 10);
    for (int p = 1; p <= 100; ++p) CHECK_EQ(volume_pct_for(volume_cdb_for(p)), p);

    /* the deck zero7's Apple adapter (first noted as zero8's: the coordinator's correction, 2026-11-13): 'Headset
     * Playback Volume' 0..120 = -60..0 dB (0.5 dB a step). The probe's 60/120 (-30 dB) is 32 % on the server's curve;
     * 50 % is -18 dB (84), 40 % -24 dB (72), 25 % -36 dB (48), 100 % 0 dB (120) */
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 120, true, -6000, 0), switch_ctl("Headset Playback Switch")}, m));
    CHECK(m.has_db);
    CHECK_EQ(m.top, 120L);
    CHECK_EQ(mixer_pct_for(m, 60), 32);
    CHECK_EQ(mixer_raw_for(m, 50), 84L);
    CHECK_EQ(mixer_pct_for(m, 84), 50);
    CHECK_EQ(mixer_raw_for(m, 40), 72L);
    CHECK_EQ(mixer_pct_for(m, 72), 40);
    CHECK_EQ(mixer_raw_for(m, 25), 48L);
    CHECK_EQ(mixer_pct_for(m, 48), 25);
    CHECK_EQ(mixer_raw_for(m, 100), 120L);
    CHECK_EQ(mixer_raw_for(m, 0), 0L);
    CHECK_EQ(mixer_raw_for(m, 5), 0L);   /* -78 dB: below the control, clamped to its minimum */
    CHECK_EQ(mixer_pct_for(m, 0), 10);   /* -60 dB is 10 % on the curve: the chip shows what is set */
    CHECK_EQ(mixer_cdb_for(m, 80), -2000L);
    CHECK_EQ(mixer_pct_for(m, 80), 46);
    CHECK_EQ(mixer_raw_for(m, 46), 80L);
    /* steps of 5 %: down from 15 to 10 (the minimum), then a step that cannot move the control mutes (0) */
    CHECK_EQ(mixer_step_target(m, 15, -kVolumeStep), 10);
    CHECK_EQ(mixer_step_target(m, 10, -kVolumeStep), 0);
    CHECK_EQ(mixer_step_target(m, 0, kVolumeStep), 5);
    CHECK_EQ(mixer_step_target(m, 50, kVolumeStep), 55);
    CHECK_EQ(mixer_step_target(m, 100, kVolumeStep), 100);
    /* a range that starts higher: -34..0 dB in 68 steps; 25 % (-36 dB) clamps at the minimum, which reads back 27 % */
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 68, true, -3400, 0)}, m));
    CHECK_EQ(mixer_raw_for(m, 50), 32L);
    CHECK_EQ(mixer_pct_for(m, 32), 50);
    CHECK_EQ(mixer_raw_for(m, 25), 0L);
    CHECK_EQ(mixer_pct_for(m, 0), 27);
    CHECK_EQ(mixer_step_target(m, 30, -kVolumeStep), 25); /* moves the control (5 -> 0): kept */
    CHECK_EQ(mixer_step_target(m, 27, -kVolumeStep), 0);  /* the minimum already: mute */
    /* -87..+12 dB in 99 steps: never above 0 dB (step 87); 50 % = step 69 (-18 dB), 5 % = step 9 (-78 dB) */
    CHECK(choose_playback_mixer({vol_ctl("Master Playback Volume", 0, 99, true, -8700, 1200)}, m));
    CHECK_EQ(m.top, 87L);
    CHECK_EQ(m.db_top, 0L);
    CHECK_EQ(mixer_raw_for(m, 100), 87L);
    CHECK_EQ(mixer_pct_for(m, 87), 100);
    CHECK_EQ(mixer_raw_for(m, 50), 69L);
    CHECK_EQ(mixer_pct_for(m, 69), 50);
    CHECK_EQ(mixer_raw_for(m, 5), 9L);
    CHECK_EQ(mixer_pct_for(m, 9), 5);
    CHECK_EQ(mixer_pct_for(m, 99), 100); /* someone else set it to +12 dB: 100 % (the coordinator, 2026-11-13: above 0 dB reads 100 %) */
    /* the HackBerry's Sharkoon: 'PCM' 0..45, 1 dB a step, -45..0 dB; 22 = -23 dB = 41 % */
    CHECK(choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, m));
    CHECK_EQ(mixer_pct_for(m, 22), 41);
    CHECK_EQ(mixer_raw_for(m, 41), 22L);
    /* a control that goes above 0 dB: never past 0 dB (-60..+6 dB over 0..110: 0 dB at step 100) */
    CHECK(choose_playback_mixer({vol_ctl("Speaker Playback Volume", 0, 110, true, -6000, 600)}, m));
    CHECK_EQ(m.top, 100L);
    CHECK_EQ(mixer_raw_for(m, 100), 100L);
    CHECK_EQ(mixer_pct_for(m, 100), 100);
    CHECK_EQ(mixer_pct_for(m, 110), 100);
    /* a headset whose driver gives no dB (the kernel drops bogus values): no dB to put on the curve, linear in steps */
    CHECK(choose_playback_mixer({vol_ctl("Headset Playback Volume", 0, 37)}, m));
    CHECK(!m.has_db);
    CHECK_EQ(mixer_raw_for(m, 50), 19L);
    CHECK_EQ(mixer_pct_for(m, 19), 51);
    CHECK_EQ(mixer_raw_for(m, 150), 37L);
    CHECK_EQ(mixer_step_target(m, 0, kVolumeStep), 5);
    /* the software volume of a card without a mixer: the same curve */
    CHECK_NEAR(20.0 * std::log10(volume_gain(50)), -18.06, 0.01);
    CHECK_NEAR(20.0 * std::log10(volume_gain(40)), -23.88, 0.01);
    CHECK_NEAR(volume_gain(100), 1.0, 1e-9);
    CHECK_NEAR(volume_gain(0), 0.0, 1e-9);

    /* the card behind an ALSA device */
    CHECK_EQ(mixer_card_for("plughw:CARD=UC02,DEV=0"), std::string("hw:CARD=UC02"));
    CHECK_EQ(mixer_card_for("hw:CARD=Device"), std::string("hw:CARD=Device"));
    CHECK_EQ(mixer_card_for("default"), std::string("default"));
    CHECK_EQ(mixer_card_for("plughw:1,0"), std::string("default"));

    /* read and write through the card (the fake mixer) */
    FakeAlsa a;
    a.ctls = {vol_ctl("Headset Playback Volume", 0, 120, true, -6000, 0), switch_ctl("Headset Playback Switch")};
    a.values["Headset Playback Volume"] = 80;
    a.values["Headset Playback Switch"] = 1;
    std::string chosen;
    MixerChoice c;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.has_mixer);
    CHECK_EQ(lv.pct, 46); /* -20 dB on the server's curve */
    CHECK(!lv.muted);
    CHECK_EQ(chosen, std::string("hw:CARD=A"));
    CHECK_EQ(write_mixer(a, "hw:CARD=A", c, 51), 0); /* a step up from 46 */
    CHECK_EQ(a.values["Headset Playback Volume"], 85L);
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK_EQ(lv.pct, 51);
    CHECK_EQ(write_mixer(a, "hw:CARD=A", c, 0), 0); /* 0 %: the switch mutes */
    CHECK_EQ(a.values["Headset Playback Switch"], 0L);
    CHECK_EQ(read_mixer(a, "hw:CARD=A", chosen, c, lv), 0);
    CHECK(lv.muted);
    CHECK_EQ(write_mixer(a, "hw:CARD=A", c, 5), 0); /* back up: un-muted */
    CHECK_EQ(a.values["Headset Playback Switch"], 1L);
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
    CHECK_EQ(write_mixer(bare, "hw:CARD=B", c, 50), -ENOENT);
    CHECK_EQ(bare.sets, 0);

    /* the steps of the strip and the keys */
    CHECK_EQ(volume_after_step(98, kVolumeStep), 100);
    CHECK_EQ(volume_after_step(120, kVolumeStep), 120); /* never louder than 100 by a step */
    CHECK_EQ(volume_after_step(120, -kVolumeStep), 115);
    CHECK_EQ(volume_after_step(3, -kVolumeStep), 0);
    CHECK_EQ(volume_after_step(-1, kVolumeStep), 5);
}

/* D11 as changed with a sound server: the playing sink's own volume, set on the worker, shown from the server's event */
void test_output_sink_level() {
    FakePulse f;
    PulseDevice dac = fake_usb_sink("Sharkoon", 50);
    dac.volume_pct = 40;
    PulseDevice xwf = fake_bt_sink(kXwf, 80, "speaker");
    xwf.volume_pct = 50;
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
    int pct = -1;
    bool muted = false;
    CHECK(sink_volume_of(world, dac.name, pct, muted));
    CHECK_EQ(pct, 40); /* the chip shows the DAC's real 40 %, not a stream level */

    /* a step up sets the DAC (never the default sink), the server's event brings the level back */
    CHECK(w.request_sink_level(dac.name, 2, volume_after_step(pct, kVolumeStep)));
    for (int i = 0; i < 200 && w.sink_levels_done() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(watch.take_change());
    ScanRequest again;
    again.connect = true;
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    world.sinks = r.sinks;
    CHECK(sink_volume_of(world, dac.name, pct, muted));
    CHECK_EQ(pct, 45);
    CHECK(sink_volume_of(world, xwf.name, pct, muted));
    CHECK_EQ(pct, 50); /* the default sink untouched */
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK(!f.levels_set.empty() && f.levels_set.back().first == dac.name);
    }
    /* quick presses: the newest level wins, no queue */
    f.call_delay_ms.store(20);
    for (int lv = 50; lv <= 70; lv += 5) CHECK(w.request_sink_level(dac.name, 2, lv));
    for (int i = 0; i < 300; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::recursive_mutex> g(f.lock());
        if (!f.levels_set.empty() && f.levels_set.back().second == 70) break;
    }
    f.call_delay_ms.store(0);
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK_EQ(f.levels_set.back().second, 70);
        CHECK(f.levels_set.size() < 7); /* 45 plus at most a few of the five: not one call each */
    }
    /* a sink that is gone: refused, nothing set */
    CHECK_EQ(f.set_sink_level("bluez_output.00_11_22_33_44_55.1", 2, 30), kPulseNoDevice);
    /* a reconnect: the device's own level comes back as the server reports it; the chip shows that */
    f.remove_sink(xwf.name);
    PulseDevice back = xwf;
    back.index = kPulseNoIndex;
    back.volume_pct = 35;
    f.add_sink(back);
    CHECK(watch.take_change());
    CHECK(w.request_scan(again));
    CHECK(wait_scan(w, r));
    world.sinks = r.sinks;
    CHECK(sink_volume_of(world, xwf.name, pct, muted));
    CHECK_EQ(pct, 35);
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

/* The owner's report from the deck (2026-11-13): "zero7 is showing muted but volume bar shows 18". The Sharkoon's
 * 'PCM Playback Volume' 0..45 = -45..0 dB with its 'PCM Playback Switch': the floor (-45 dB) reads 18 % on the curve;
 * "-" at the floor mutes, "-" while muted does nothing, "+" brings the sound back (one step above the floor), a mute by
 * something else shows, and "+" then un-mutes at the level kept. */
void test_mute_steps() {
    FakeAlsa a;
    a.ctls = {vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0), switch_ctl("PCM Playback Switch")};
    a.values["PCM Playback Volume"] = mixer_raw_for([] {
        MixerChoice m;
        choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, m);
        return m;
    }(), 23);
    a.values["PCM Playback Switch"] = 1;
    std::string chosen;
    MixerChoice c;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, "hw:CARD=S", chosen, c, lv), 0);
    CHECK_EQ(lv.pct, 23);
    auto press = [&](int d) {
        const VolumePress p = mixer_press(c, lv, d);
        if (p.kind == VolumePress::Kind::Set) write_mixer(a, "hw:CARD=S", c, p.pct);
        if (p.kind == VolumePress::Kind::Mute) write_mixer_switch(a, "hw:CARD=S", c, false);
        if (p.kind == VolumePress::Kind::Unmute) write_mixer_switch(a, "hw:CARD=S", c, true);
        read_mixer(a, "hw:CARD=S", chosen, c, lv);
        return p.kind;
    };
    /* down to the floor: 23 -> 18 (-45 dB, the card's lowest) */
    CHECK(press(-kVolumeStep) == VolumePress::Kind::Set);
    CHECK_EQ(lv.pct, 18);
    CHECK_EQ(lv.raw, 0L);
    CHECK(!lv.muted);
    /* "-" at the floor mutes (the switch); the level is kept */
    CHECK(press(-kVolumeStep) == VolumePress::Kind::Mute);
    CHECK(lv.muted);
    CHECK_EQ(a.values["PCM Playback Switch"], 0L);
    CHECK_EQ(lv.pct, 18);
    /* "-" while muted does nothing */
    const int sets = a.sets;
    CHECK(press(-kVolumeStep) == VolumePress::Kind::None);
    CHECK_EQ(a.sets, sets);
    CHECK(lv.muted);
    /* "+" while muted at the floor: one step above it, un-muted, so something is heard */
    CHECK(press(kVolumeStep) == VolumePress::Kind::Set);
    CHECK(!lv.muted);
    CHECK_EQ(lv.pct, 23);
    /* muted by something else (alsamixer) at 23: shown muted; "+" un-mutes at 23, not a step above */
    a.values["PCM Playback Switch"] = 0;
    read_mixer(a, "hw:CARD=S", chosen, c, lv);
    CHECK(lv.muted);
    CHECK(press(kVolumeStep) == VolumePress::Kind::Unmute);
    CHECK(!lv.muted);
    CHECK_EQ(lv.pct, 23);
    /* a card without a switch: the floor is as low as it goes */
    MixerChoice nosw;
    choose_playback_mixer({vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0)}, nosw);
    MixerLevel floor;
    floor.has_mixer = true;
    floor.raw = 0;
    floor.pct = 18;
    CHECK(mixer_press(nosw, floor, -kVolumeStep).kind == VolumePress::Kind::None);
    /* the sound server's sinks: floor 0 %, "+" un-mutes at the level, "-" muted does nothing */
    CHECK(sink_press(40, true, -kVolumeStep).kind == VolumePress::Kind::None);
    VolumePress p = sink_press(40, true, kVolumeStep);
    CHECK(p.kind == VolumePress::Kind::Set && p.pct == 40);
    p = sink_press(0, true, kVolumeStep);
    CHECK(p.kind == VolumePress::Kind::Set && p.pct == 5);
    p = sink_press(3, false, -kVolumeStep);
    CHECK(p.kind == VolumePress::Kind::Set && p.pct == 0);
    CHECK(sink_press(0, false, -kVolumeStep).kind == VolumePress::Kind::None);
    CHECK(sink_press(-1, false, kVolumeStep).kind == VolumePress::Kind::None);
    /* the server's placeholder at start never shows a level */
    OutputWorld w;
    w.server = true;
    PulseDevice null_sink;
    null_sink.name = "auto_null";
    null_sink.volume_pct = 100;
    w.sinks = {null_sink};
    w.default_sink = "auto_null";
    int pct = -1;
    bool muted = false;
    CHECK(!sink_volume_of(w, "", pct, muted));
    CHECK(placeholder_sink("auto_null"));
    CHECK(!placeholder_sink("alsa_output.usb-Sharkoon.analog-stereo"));
}

/* The Pi 4B (2026-11-13, the owner: "i'm at 5% and it's like 80 to 100% perceived"): bcm2835 Headphones, 'PCM
 * Playback Volume' -10239..400 in hundredths of a dB (dBscale-min=-102.39dB step=0.01dB mute=1), joined mono, and
 * 'PCM Playback Switch'. snd_ctl_get_dB_range reports its min as SND_CTL_TLV_DB_GAIN_MUTE (-9999999): taken as is, 5 %
 * landed on raw 391 (+3.91 dB) and read back as 5 %; each step moved the control by one raw value. */
void test_pi4b_pcm() {
    const std::string card = "hw:CARD=Headphones";
    auto pcm = [] {
        CtlInfo c = vol_ctl("PCM Playback Volume", -10239, 400, true, kAlsaDbGainMute, 400); /* as get_dB_range says */
        c.count = 1;
        return c;
    };
    /* the bug, reproduced: the sentinel taken as the dB of min */
    {
        MixerChoice bad;
        bad.volume = "PCM Playback Volume";
        bad.min = -10239;
        bad.max = 400;
        bad.has_db = true;
        bad.db_min = kAlsaDbGainMute;
        bad.db_max = 400;
        bad.db_top = 0;
        bad.top = 399;
        CHECK_EQ(mixer_raw_for(bad, 5), 391L);
        CHECK_EQ(mixer_pct_for(bad, 391), 5);
    }
    /* mixer_list's fix: the dB of min + 1 (snd_ctl_convert_to_dB) gives the real dB of min back */
    CtlInfo c = pcm();
    alsa_fix_mute_db(c, true, -10238);
    CHECK(c.has_db);
    CHECK(c.min_is_mute);
    CHECK_EQ(c.db_min_cdb, -10239L);
    CHECK_EQ(c.db_max_cdb, 400L);
    /* without that dB: no dB data (linear in steps), still never mute for a level above 0 */
    CtlInfo nodb = pcm();
    alsa_fix_mute_db(nodb, false, 0);
    CHECK(!nodb.has_db);
    CHECK(nodb.min_is_mute);
    /* a control without the mute flag is left alone */
    CtlInfo plain = vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0);
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
    CHECK_EQ(m.top, 0L); /* never above 0 dB: raw 0, not 400 */
    CHECK_EQ(lv.pct, 100); /* above 0 dB reads 100 % */
    CHECK(!lv.muted);
    /* the curve: 5 % = -78.06 dB, 50 % = -18.06 dB, 100 % = 0 dB; never above raw 0 */
    CHECK_EQ(mixer_raw_for(m, 5), -7806L);
    CHECK_EQ(mixer_raw_for(m, 50), -1806L);
    CHECK_EQ(mixer_raw_for(m, 100), 0L);
    CHECK_EQ(mixer_raw_for(m, 1), -10239L + 1); /* 1 % (-120 dB) is under the range: the lowest sound, not mute */
    CHECK_EQ(mixer_raw_for(m, 0), -10239L);     /* 0 %: the value that is mute */
    for (int p = 0; p <= 100; ++p) CHECK(mixer_raw_for(m, p) <= 0);
    for (int p = 2; p <= 100; ++p) CHECK_EQ(mixer_pct_for(m, mixer_raw_for(m, p)), p);
    /* the steps from 100 down write what the curve says, and the fresh read gives it back */
    auto press = [&](int d) {
        const VolumePress p = mixer_press(m, lv, d);
        if (p.kind == VolumePress::Kind::Set) write_mixer(a, card, m, p.pct);
        if (p.kind == VolumePress::Kind::Mute) write_mixer_switch(a, card, m, false);
        if (p.kind == VolumePress::Kind::Unmute) write_mixer_switch(a, card, m, true);
        read_mixer(a, card, chosen, m, lv);
        return p;
    };
    CHECK(press(-kVolumeStep).pct == 95);
    CHECK_EQ(a.values["PCM Playback Volume"], -134L); /* -1.34 dB */
    CHECK_EQ(lv.pct, 95);
    for (int i = 0; i < 17; ++i) press(-kVolumeStep);
    CHECK_EQ(lv.pct, 10);
    CHECK_EQ(a.values["PCM Playback Volume"], -6000L);
    press(-kVolumeStep);
    CHECK_EQ(lv.pct, 5);
    CHECK_EQ(a.values["PCM Playback Volume"], -7806L);
    /* "-" at 5 %: the switch mutes, the level kept; "+" brings 5 % back */
    CHECK(press(-kVolumeStep).kind == VolumePress::Kind::Mute);
    CHECK(lv.muted);
    CHECK_EQ(a.values["PCM Playback Volume"], -7806L);
    CHECK(press(kVolumeStep).kind == VolumePress::Kind::Unmute);
    CHECK(!lv.muted);
    CHECK_EQ(lv.pct, 5);
    /* the control at its mute value (alsamixer to 0): shown muted, 0 %; "+" sets 5 % */
    a.values["PCM Playback Volume"] = -10239;
    read_mixer(a, card, chosen, m, lv);
    CHECK(lv.muted);
    CHECK_EQ(lv.pct, 0);
    const VolumePress up = press(kVolumeStep);
    CHECK(up.kind == VolumePress::Kind::Set && up.pct == 5);
    CHECK_EQ(a.values["PCM Playback Volume"], -7806L);
    CHECK(!lv.muted);
    /* the same card without its switch: "-" at 5 % goes to the mute value (0 %), "-" there does nothing */
    MixerChoice nosw;
    CHECK(choose_playback_mixer({c}, nosw));
    MixerLevel five;
    five.has_mixer = true;
    five.raw = -7806;
    five.pct = 5;
    const VolumePress down = mixer_press(nosw, five, -kVolumeStep);
    CHECK(down.kind == VolumePress::Kind::Set && down.pct == 0);
    MixerLevel zero;
    zero.has_mixer = true;
    zero.raw = -10239;
    zero.pct = 0;
    zero.muted = true;
    CHECK(mixer_press(nosw, zero, -kVolumeStep).kind == VolumePress::Kind::None);
    /* the log line */
    CHECK_EQ(mixer_set_log_text(card, m, 5, -7806, 0, -7806),
             std::string("set mixer hw:CARD=Headphones 'PCM Playback Volume' 5 % raw=-7806 (target -78.06 dB) read back raw=-7806"));
    CHECK_EQ(mixer_set_log_text(card, m, 5, -7806, -19, 0),
             std::string("set mixer hw:CARD=Headphones 'PCM Playback Volume' 5 % raw=-7806 (target -78.06 dB) read back failed (-19)"));
}

}  // namespace

void test_volume() {
    test_volume_pct();
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
