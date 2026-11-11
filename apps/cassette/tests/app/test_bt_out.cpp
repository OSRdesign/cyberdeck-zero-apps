/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: playback and the live monitor over Bluetooth through the sound server (task T3 of the Bluetooth release),
 * the whole controller on FakePulse. The facts are the HackBerry's (study 12, docs/PULSE.md): one headset (Evolve 65) whose card
 * leaves the music profile ~0.65 s after its microphone opens and comes back ~1.85 s after it closes (P4), a speaker that has A2DP
 * only, one A2DP device at a time (P6). Real time; synthetic audio only.
 */
#include "bt_rig.hpp"

using namespace ta;
using namespace tbt;

namespace {

const std::string kSpk = "9E:0C:81:E6:5D:1A";
const std::string kSpkId = "bt:9E:0C:81:E6:5D:1A";
const std::string kSinkEv = "bluez_output.70_BF_92_53_6B_8B.1";
const std::string kSinkSpk = "bluez_output.9E_0C_81_E6_5D_1A.1";
const char *kOutLost = "The Bluetooth output disconnected.";
const char *kOutNotThere = "The Bluetooth output is not connected.";
const char *kMonGone = "The monitor output disconnected: the recording goes on.";
const char *kTake = "take_001_20261108-1105.wav";

bool has_text(const Snapshot &s, const std::string &text) {
    for (const Notice &n : s.notices)
        if (n.text == text) return true;
    return false;
}

/* a synthetic take (the ramp) at any rate: returns its samples */
std::vector<int32_t> make_take_at(const std::string &dir, const std::string &name, uint32_t rate, uint16_t ch, double seconds) {
    posix_file_ops().mkdir(dir);
    const auto s = tp::ramp_samples(static_cast<uint64_t>(seconds * rate), ch);
    tp::write_wav(dir + "/" + name, tp::afmt(rate, ch, 16), s);
    return s;
}

std::vector<int16_t> to16(const std::vector<int32_t> &s) {
    std::vector<int16_t> v;
    for (int32_t x : s) v.push_back(static_cast<int16_t>(x));
    return v;
}

void add_speaker(BtRig &b) {
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        PulseCard c = fake_bt_card(kSpk, 71, true, "a2dp-sink", false);
        c.description = "XWF-M18-M28-M38";
        b.pulse.cards.push_back(c);
    }
    b.pulse.add_sink(fake_bt_sink(kSpk, 71, "speaker")); /* the change event the server sends */
}

void make_call_only(FakePulse &f) { /* another A2DP device holds the link (P6): the headset has call profiles only */
    std::lock_guard<std::recursive_mutex> g(f.lock());
    for (PulseCard &c : f.cards)
        for (PulseProfile &p : c.profiles)
            if (pulse_profile_is_music(p.name)) p.available = false;
}

const OutputSnap *find_out(const std::vector<OutputSnap> &l, const std::string &id) {
    for (const OutputSnap &o : l)
        if (o.id == id) return &o;
    return nullptr;
}

bool wait_out(BtRig &b, const std::string &id, int ms = 4000) {
    return b.r.wait([&](const Snapshot &s) { return find_out(s.card.outputs, id) != nullptr; }, ms);
}

/* ---- the output list ---- */

void t_output_list() {
    BtRig b(one_card());
    add_speaker(b);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    CHECK(wait_out(b, kSpkId));
    Snapshot s = b.r.snap();
    CHECK_EQ(s.card.outputs.size(), s.card.output_labels.size());
    CHECK_EQ(s.card.outputs.size(), s.card.output_devices.size());
    /* the capture card's own side first, the Bluetooth devices, then "Default output" */
    CHECK_EQ(s.card.outputs.front().label, "USB Audio CODEC");
    CHECK_EQ(s.card.outputs.back().id, "default");
    const OutputSnap *ev = find_out(s.card.outputs, kId);
    const OutputSnap *sp = find_out(s.card.outputs, kSpkId);
    CHECK(ev && sp);
    if (!ev || !sp) return;
    CHECK(ev->bluetooth && ev->connected && ev->usable);
    CHECK_EQ(ev->label, "Evolve 65");
    CHECK_EQ(ev->kind, "headset");
    CHECK_EQ(ev->rate, 48000u);
    CHECK_EQ(ev->hint_profile, "music");
    CHECK(sp->bluetooth && sp->usable);
    CHECK_EQ(sp->label, "XWF-M18-M28-M38");
    CHECK_EQ(sp->kind, "speaker");
    CHECK(!s.card.outputs.front().bluetooth && s.card.outputs.front().kind.empty());
    CHECK_EQ(s.card.output_devices[s.card.output_selected < 0 ? 0 : static_cast<size_t>(s.card.output_selected)], "hw:CARD=Device,DEV=0");
    CHECK_EQ(s.playback.state, "idle");
    CHECK_EQ(s.playback.quality, ""); /* a sound card: no Bluetooth quality to tell */
    /* choose the speaker: stored as bt:<MAC>, shown as the output in use; a bad address is refused */
    CHECK(!c.select_output("bt:nonsense"));
    CHECK(c.select_output("bt:9e:0c:81:e6:5d:1a"));
    s = b.r.snap();
    CHECK_EQ(s.card.output_label, "XWF-M18-M28-M38");
    CHECK_EQ(s.card.output_devices[static_cast<size_t>(s.card.output_selected)], kSpkId);
    CHECK(s.playback.bluetooth);
    CHECK_EQ(s.playback.quality, "music");
    CHECK(slurp(b.r.home + "/.config/cassette/settings.json").find("\"output_device\":\"bt:9E:0C:81:E6:5D:1A\"") != std::string::npos ||
          slurp(b.r.home + "/.config/cassette/settings.json").find("bt:9E:0C:81:E6:5D:1A") != std::string::npos);
    /* it is gone: the choice stays as a placeholder, nothing else changes, PLAY says why */
    b.pulse.remove_sink(kSinkSpk);
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.cards.erase(std::remove_if(b.pulse.cards.begin(), b.pulse.cards.end(),
                                           [](const PulseCard &x) { return x.name == "bluez_card.9E_0C_81_E6_5D_1A"; }),
                            b.pulse.cards.end());
    }
    CHECK(b.r.wait([](const Snapshot &x) {
        const OutputSnap *o = find_out(x.card.outputs, kSpkId);
        return o && !o->connected && !o->usable;
    }));
    s = b.r.snap();
    CHECK_EQ(s.card.output_label, "XWF-M18-M28-M38"); /* its last label */
    CHECK(s.card.present);                            /* the microphone never noticed */
    make_take_at(b.r.rec(), kTake, 16000, 1, 1.0);
    c.refresh_takes();
    c.load(kTake);
    c.ack_notices();
    c.key_event(KeyEvent::PlayPause);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK(has_text(b.r.snap(), kOutNotThere));
    CHECK_EQ(count_streams(b.pulse, false), 0); /* nothing was opened anywhere */
    /* the next start: the choice is still there (placeholder), the server's other devices are listed */
    b.r.close();
    b.r.open();
    CHECK(b.r.wait([](const Snapshot &x) {
        const OutputSnap *o = find_out(x.card.outputs, kSpkId);
        return o && !o->connected;
    }));
    CHECK_EQ(b.r.snap().card.outputs.back().id, "default");
    /* it comes back: usable again, no restart */
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        PulseCard cd = fake_bt_card(kSpk, 71, true, "a2dp-sink", false);
        b.pulse.cards.push_back(cd);
    }
    b.pulse.add_sink(fake_bt_sink(kSpk, 71, "speaker"));
    CHECK(b.r.wait([](const Snapshot &x) {
        const OutputSnap *o = find_out(x.card.outputs, kSpkId);
        return o && o->connected && o->usable;
    }));
}

void t_output_server_missing() {
    BtRig b(one_card());
    b.r.cfg.server_running = [] { return false; };
    b.r.open();
    CHECK(b.r.wait_meter());
    const Snapshot s = b.r.snap();
    CHECK_EQ(s.card.bt_status, "Bluetooth audio is not set up");
    for (const OutputSnap &o : s.card.outputs) CHECK(!o.bluetooth); /* no Bluetooth entry while the server is unreachable */
    CHECK_EQ(s.card.outputs.back().id, "default");
    /* a choice made earlier still shows, as a placeholder */
    CHECK(b.r.ctl().select_output(kSpkId));
    const Snapshot t = b.r.snap();
    CHECK(find_out(t.card.outputs, kSpkId) != nullptr);
    CHECK(!find_out(t.card.outputs, kSpkId)->connected);
}

/* ---- playback ---- */

/* a take at 8, 16 and 48 kHz through a speaker: the stream is the take's rate, the samples are exact, the counter runs */
void t_play_rates_exact() {
    for (uint32_t rate : {8000u, 16000u, 48000u}) {
        BtRig b(one_card());
        add_speaker(b);
        const auto s = make_take_at(b.r.rec(), kTake, rate, 1, 0.6);
        PlaySrv srv(b.pulse);
        b.r.open();
        Controller &c = b.r.ctl();
        CHECK(b.r.wait_meter());
        CHECK(wait_out(b, kSpkId));
        CHECK(c.select_output(kSpkId));
        c.key_event(KeyEvent::PlayPause);
        CHECK(b.r.wait_state(kDeckPlaying));
        CHECK(b.r.wait([](const Snapshot &x) { return x.playback.state == "playing"; }, 2000));
        int pi = -1;
        for (int i = 0; i < 100 && pi < 0; ++i) {
            pi = stream_index(b.pulse, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(pi >= 0);
        if (pi < 0) return;
        {
            std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
            CHECK_EQ(b.pulse.streams[static_cast<size_t>(pi)].requested, kSinkSpk);
            CHECK_EQ(b.pulse.streams[static_cast<size_t>(pi)].rate, rate); /* the take's rate: the server resamples */
            CHECK_EQ(b.pulse.streams[static_cast<size_t>(pi)].channels, 1u);
            CHECK((b.pulse.streams[static_cast<size_t>(pi)].flags & 0x200u) != 0); /* DONT_MOVE */
        }
        CHECK_EQ(b.r.snap().playback.quality, "music");
        CHECK(b.r.wait([](const Snapshot &x) { return x.time.position_s > 0.15; }, 2000)); /* the counter runs */
        CHECK(b.r.wait_state(kDeckIdle, 4000));
        CHECK_EQ(b.r.snap().playback.state, "idle");
        const auto got = srv.got(static_cast<size_t>(pi));
        CHECK_EQ(got.size(), s.size());
        CHECK(got == to16(s)); /* sample exact */
    }
}

/* pause, resume, the 5x scan through the controller, on the stream of one take */
void t_play_pause_scan() {
    BtRig b(one_card());
    add_speaker(b);
    make_take_at(b.r.rec(), kTake, 16000, 1, 20.0);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    CHECK(wait_out(b, kSpkId));
    CHECK(c.select_output(kSpkId));
    c.key_event(KeyEvent::PlayPause);
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.position_s > 0.4; }, 3000));
    c.key_event(KeyEvent::PlayPause); /* pause: the stream stays, the counter stops */
    CHECK(b.r.wait_state(kDeckPlayingPaused));
    const double p0 = b.r.snap().time.position_s;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(std::abs(b.r.snap().time.position_s - p0) < 0.02);
    CHECK_EQ(b.r.snap().playback.underruns, 0u); /* the server ran dry while paused: not an underrun */
    CHECK_EQ(count_streams(b.pulse, false), 1);
    c.key_event(KeyEvent::PlayPause); /* resume */
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK(b.r.wait([p0](const Snapshot &x) { return x.time.position_s > p0 + 0.3; }, 3000));
    CHECK_EQ(count_streams(b.pulse, false), 1);
    /* hold FWD: the 5x scan, the counter moves about five times faster */
    const double before = b.r.snap().time.position_s;
    c.key_event(KeyEvent::FwdDown);
    CHECK(b.r.wait([](const Snapshot &x) { return x.scan.active && x.scan.direction == 1; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const double during = b.r.snap().time.position_s;
    c.key_event(KeyEvent::FwdUp);
    CHECK(during - before > 1.5); /* 0.6 s of tape cue is about 3 s of take */
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK_EQ(count_streams(b.pulse, false), 1);
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
}

/* the headset disconnects while a take plays, or while it is paused: it stops cleanly with the notice, nothing moves elsewhere */
void t_disconnect_during_playback() {
    for (int paused = 0; paused < 2; ++paused) {
        BtRig b(one_card());
        add_speaker(b);
        make_take_at(b.r.rec(), kTake, 16000, 1, 20.0);
        PlaySrv srv(b.pulse);
        b.r.open();
        Controller &c = b.r.ctl();
        CHECK(b.r.wait_meter());
        CHECK(wait_out(b, kSpkId));
        CHECK(c.select_output(kSpkId));
        c.key_event(KeyEvent::PlayPause);
        CHECK(b.r.wait_state(kDeckPlaying));
        CHECK(b.r.wait([](const Snapshot &x) { return x.time.position_s > 0.3; }, 3000));
        if (paused) {
            c.key_event(KeyEvent::PlayPause);
            CHECK(b.r.wait_state(kDeckPlayingPaused));
        }
        c.ack_notices();
        b.pulse.remove_sink(kSinkSpk);
        /* the deck stops where it was (paused at the frame heard), with the notice; the stream is closed */
        CHECK(b.r.wait([](const Snapshot &x) { return x.deck.state == kDeckPlayingPaused && has_text(x, kOutLost); }, 4000));
        const Snapshot s = b.r.snap();
        CHECK_EQ(count_streams(b.pulse, false), 1); /* never moved to another sink, never reopened */
        CHECK(s.card.present);                      /* the microphone is not affected */
        CHECK(s.time.position_s > 0.2);             /* the position is kept */
        {   /* PLAY again with the output still gone: refused with a notice, nothing opens anywhere */
            c.ack_notices();
            c.key_event(KeyEvent::PlayPause);
            CHECK(b.r.wait_state(kDeckIdle, 3000));
            CHECK(has_text(b.r.snap(), kOutNotThere));
            CHECK_EQ(count_streams(b.pulse, false), 1);
        }
    }
}

/* ---- the same headset as input and output ---- */

/* PLAY right after recording: the mic closes, the card goes back to music after ~1.85 s, then the stream opens */
void t_wait_for_music() {
    BtRig b(one_card(), "a2dp-sink", true);
    ProfileSim sim(b.pulse, 650, 1850);
    const auto take = make_take_at(b.r.rec(), kTake, 16000, 1, 0.8);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    CHECK(c.select_output(kId));
    CHECK(b.pick());
    CHECK(b.wait_present());
    /* the microphone is open: the card goes to the call profile and the headset's sink with it */
    CHECK(wait_until([&] { return sim.to_call.load() > 0; }, 4000));
    CHECK(b.r.wait([](const Snapshot &s) { return !s.capture.connecting; }, 4000));
    CHECK_EQ(count_streams(b.pulse, false), 0);
    c.key_event(KeyEvent::PlayPause);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(b.r.wait_state(kDeckPlaying));
    Snapshot s = b.r.snap();
    CHECK_EQ(s.playback.state, "switching_to_music");
    CHECK_EQ(s.playback.quality, "call");
    CHECK_EQ(s.playback.switch_timeout_s, 4.0);
    /* nothing plays yet, the microphone is closed so the headset can go back */
    CHECK(wait_until([&] { return !b.stream_open(0); }, 1000));
    CHECK_EQ(count_streams(b.pulse, false), 0);
    CHECK(b.r.wait([](const Snapshot &x) { return x.playback.switch_elapsed_s > 0.4; }, 2000)); /* the elapsed time runs */
    CHECK_EQ(b.r.snap().playback.state, "switching_to_music");
    CHECK(b.r.wait([](const Snapshot &x) { return x.playback.state == "playing"; }, 6000));
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(waited > 1.5 && waited < 3.6); /* the 1.85 s of the card plus the poll */
    CHECK_EQ(sim.to_music.load(), 1);
    s = b.r.snap();
    CHECK_EQ(s.playback.quality, "music");
    CHECK(!s.playback.switch_timed_out);
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        const int pi = stream_index(b.pulse, false);
        CHECK(pi >= 0);
        if (pi >= 0) {
            CHECK_EQ(b.pulse.streams[static_cast<size_t>(pi)].requested, kSinkEv);
            CHECK_EQ(b.pulse.streams[static_cast<size_t>(pi)].rate, 16000u);
        }
    }
    CHECK(b.r.wait_state(kDeckIdle, 4000));
    const int pi = stream_index(b.pulse, false);
    CHECK(pi >= 0);
    if (pi >= 0) CHECK(srv.got(static_cast<size_t>(pi)) == to16(take));
    /* back on the recorder the microphone opens again (connecting) */
    CHECK(b.r.wait([](const Snapshot &x) { return x.card.present; }, 4000));
}

/* the card never gets back to music: after 4 s PLAY goes on at call quality, never stuck */
void t_wait_timeout() {
    BtRig b(one_card(), "a2dp-sink", true);
    ProfileSim sim(b.pulse, 650, 60000);
    make_take_at(b.r.rec(), kTake, 16000, 1, 0.5);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    CHECK(c.select_output(kId));
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(wait_until([&] { return sim.to_call.load() > 0; }, 4000));
    c.key_event(KeyEvent::PlayPause);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK_EQ(b.r.snap().playback.state, "switching_to_music");
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    CHECK_EQ(b.r.snap().playback.state, "switching_to_music"); /* still waiting at 3 s */
    CHECK(b.r.wait([](const Snapshot &x) { return x.playback.state == "playing" || x.deck.state == kDeckIdle; }, 3000));
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(waited > 3.8 && waited < 5.5);
    const Snapshot s = b.r.snap();
    CHECK(s.playback.switch_timed_out || s.deck.state == kDeckIdle);
    CHECK_EQ(sim.to_music.load(), 0);
    CHECK(count_streams(b.pulse, false) >= 1); /* it played on the sink that exists */
    CHECK(b.r.wait_state(kDeckIdle, 4000));
}

/* a headset with no music profile (another A2DP device holds the link): PLAY at once, 'call quality' */
void t_call_profile_only() {
    BtRig b(one_card(), "headset-head-unit", true);
    make_call_only(b.pulse);
    const auto take = make_take_at(b.r.rec(), kTake, 16000, 1, 0.8);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    CHECK(c.select_output(kId));
    CHECK(b.pick());
    CHECK(b.wait_present());
    {
        const Snapshot sn = b.r.snap();
        const OutputSnap *o = find_out(sn.card.outputs, kId);
        CHECK(o && o->hint_profile == "call");
    }
    c.key_event(KeyEvent::PlayPause);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(b.r.wait_state(kDeckPlaying));
    bool saw_switching = false;
    int pi = -1;
    for (int i = 0; i < 200 && pi < 0; ++i) {
        if (b.r.snap().playback.state == "switching_to_music") saw_switching = true;
        pi = stream_index(b.pulse, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!saw_switching);
    CHECK(pi >= 0);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000)); /* at once */
    CHECK(b.r.wait([](const Snapshot &x) { return x.playback.state == "playing"; }, 1000));
    CHECK_EQ(b.r.snap().playback.quality, "call");
    CHECK(b.r.wait_state(kDeckIdle, 4000));
    if (pi >= 0) CHECK(srv.got(static_cast<size_t>(pi)) == to16(take));
}

/* pressing STOP while waiting cancels the wait: nothing is ever opened */
void t_wait_cancel() {
    BtRig b(one_card(), "a2dp-sink", true);
    ProfileSim sim(b.pulse, 650, 1850);
    make_take_at(b.r.rec(), kTake, 16000, 1, 0.5);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    CHECK(c.select_output(kId));
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(wait_until([&] { return sim.to_call.load() > 0; }, 4000));
    c.key_event(KeyEvent::PlayPause);
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK_EQ(b.r.snap().playback.state, "switching_to_music");
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK_EQ(b.r.snap().playback.state, "idle");
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    CHECK_EQ(count_streams(b.pulse, false), 0);
    CHECK_EQ(b.r.snap().deck.state, kDeckIdle);
}

/* ---- the monitor ---- */

/* The same headset (call mode, mSBC mono sink): off by default for a Bluetooth input, on when the user says, the data path,
 * the delay figure from the servers' latency reports, the notice rule. */
void t_monitor_same_headset() {
    BtRig b(one_card(), "headset-head-unit", true);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait([](const Snapshot &s) { return !s.capture.connecting; }, 4000));
    Snapshot s = b.r.snap();
    CHECK(s.monitor.available); /* the headset is its own monitor target */
    CHECK(s.card.has_playback);
    CHECK_EQ(s.monitor.route, "same_headset");
    CHECK_EQ(s.monitor.target_label, "Evolve 65");
    CHECK(!s.settings.monitor && !s.monitor.active); /* a Bluetooth input starts with the monitor Off */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(count_streams(b.pulse, false), 0);
    CHECK(c.set_monitor(true));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 3000));
    CHECK(b.r.snap().card.monitor_active);
    const int pi = stream_index(b.pulse, false);
    CHECK(pi >= 0);
    if (pi < 0) return;
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        const FakeStream &st = b.pulse.streams[static_cast<size_t>(pi)];
        CHECK_EQ(st.requested, kSinkEv);
        CHECK_EQ(st.rate, 16000u); /* the capture's rate: the server resamples to the sink */
        CHECK_EQ(st.channels, 1u);
        CHECK((st.flags & 0x200u) != 0);
        /* ms-sized buffers: about 40 ms target, not the 100 ms of a take */
        CHECK(st.attr.tlength <= 16000u * 2 * 45 / 1000);
        CHECK(st.attr.tlength >= 16000u * 2 * 35 / 1000);
    }
    /* what is heard is the microphone at the monitor level (50 %: a quarter of the 0.5 tone) */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    {
        const auto got = srv.got(static_cast<size_t>(pi));
        CHECK(got.size() > 16000u / 4);
        int peak = 0;
        for (size_t i = got.size() / 2; i < got.size(); ++i) peak = std::max(peak, std::abs(static_cast<int>(got[i])));
        CHECK(peak > 3000 && peak < 5200);
    }
    /* the delay: capture latency + ring + playback latency from the servers' figures */
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.streams[0].latency_us = 30000; /* the capture stream */
        b.pulse.streams[static_cast<size_t>(pi)].latency_us = 50000;
    }
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.delay_ms > 70 && x.monitor.delay_ms < 140; }, 3500));
    CHECK_EQ(b.r.snap().monitor.notice, ""); /* under 150 ms on the same headset: no warning */
    CHECK_EQ(b.r.snap().monitor.route, "same_headset");
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.streams[static_cast<size_t>(pi)].latency_us = 200000; /* a slow sink */
    }
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.delay_ms > 150 && x.monitor.notice == "delay"; }, 3500));
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.streams[static_cast<size_t>(pi)].latency_us = 50000;
    }
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.delay_ms < 150 && x.monitor.notice.empty(); }, 3500));
    CHECK(c.set_monitor(false));
    CHECK(b.r.wait([](const Snapshot &x) { return !x.monitor.active && x.monitor.delay_ms == 0; }, 2000));
    CHECK(wait_until([&] { return !b.stream_open(static_cast<size_t>(pi)); }, 1000));
}

/* the monitor is not started while the microphone is connecting, and starts when the profile switch is over */
void t_monitor_not_while_connecting() {
    BtRig b(one_card(), "a2dp-sink", true);
    b.feed->mode = Feeder::Zeros; /* the profile switch: digital zero, the card still on music */
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(c.set_monitor(true));
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.snap().capture.connecting);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(b.r.snap().capture.connecting);
    CHECK(!b.r.snap().monitor.active);
    CHECK_EQ(count_streams(b.pulse, false), 0);
    b.feed->mode = Feeder::Tone; /* the headset is up: real audio arrives */
    CHECK(b.r.wait([](const Snapshot &x) { return !x.capture.connecting; }, 3000));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 3000));
    CHECK_EQ(count_streams(b.pulse, false), 1);
}

/* the monitor is off while a take plays (as with a card) and comes back after, once the microphone is up again */
void t_monitor_off_during_playback() {
    BtRig b(one_card(), "a2dp-sink", true);
    ProfileSim sim(b.pulse, 100, 300);
    make_take_at(b.r.rec(), kTake, 16000, 1, 0.8);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    CHECK(c.select_output(kId));
    CHECK(c.set_monitor(true));
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 5000));
    c.key_event(KeyEvent::PlayPause);
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK(b.r.wait([](const Snapshot &x) { return !x.monitor.active; }, 1500));
    const int mon_stream = stream_index(b.pulse, false);
    CHECK(mon_stream >= 0);
    CHECK(wait_until([&] { return !b.stream_open(static_cast<size_t>(mon_stream)); }, 1500)); /* its stream is closed */
    CHECK(b.r.wait_state(kDeckIdle, 8000));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 8000)); /* back after the microphone and the switch */
}

/* a local microphone to a Bluetooth A2DP speaker: only on an explicit choice, with the delay notice */
void t_monitor_local_mic_to_speaker() {
    BtRig b(one_card(), "a2dp-sink", false);
    add_speaker(b);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    CHECK(wait_out(b, kSpkId));
    Snapshot s = b.r.snap();
    CHECK(c.set_monitor(true)); /* automatic: the USB card's own playback side (the ALSA path) */
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 3000));
    s = b.r.snap();
    CHECK_EQ(s.monitor.route, "card");
    CHECK_EQ(s.monitor.notice, "");
    CHECK_EQ(count_streams(b.pulse, false), 0); /* the sound server is not involved */
    CHECK_EQ(s.monitor.output_selected, 0);
    CHECK_EQ(s.monitor.outputs.front().label, "Automatic");
    CHECK(find_out(s.monitor.outputs, kSpkId) != nullptr);
    CHECK(find_out(s.monitor.outputs, "default") != nullptr);
    /* the explicit choice: the speaker */
    CHECK(!c.set_monitor_output("bt:oops"));
    CHECK(c.set_monitor_output("bt:9e:0c:81:e6:5d:1a"));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active && x.monitor.route == "other_output"; }, 4000));
    s = b.r.snap();
    CHECK_EQ(s.monitor.output, kSpkId);
    CHECK_EQ(s.monitor.notice, "delay"); /* an A2DP sink: 'you will hear an echo' */
    CHECK_EQ(s.monitor.target_label, "XWF-M18-M28-M38");
    CHECK(s.monitor.output_selected > 0 && s.monitor.outputs[static_cast<size_t>(s.monitor.output_selected)].id == kSpkId);
    const int pi = stream_index(b.pulse, false);
    CHECK(pi >= 0);
    if (pi >= 0) {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        const FakeStream &st = b.pulse.streams[static_cast<size_t>(pi)];
        CHECK_EQ(st.requested, kSinkSpk);
        CHECK_EQ(st.rate, 48000u); /* the USB card's rate; the server resamples to the speaker's sink */
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (pi >= 0) CHECK(srv.got(static_cast<size_t>(pi)).size() > 48000u / 4);
    /* the choice is stored; back to automatic restores the card route */
    CHECK(slurp(b.r.home + "/.config/cassette/settings.json").find("monitor_output") != std::string::npos);
    CHECK(c.set_monitor_output(""));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active && x.monitor.route == "card"; }, 4000));
    CHECK_EQ(b.r.snap().monitor.notice, "");
    CHECK(wait_until([&] { return pi < 0 || !b.stream_open(static_cast<size_t>(pi)); }, 1500));
}

/* the monitor's sink disappears while a take records: the monitor stops with a notice, the take is never affected */
void t_monitor_sink_lost_recording_continues() {
    BtRig b(one_card(), "headset-head-unit", true);
    PlaySrv srv(b.pulse);
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(c.set_monitor(true));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 5000));
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.4; }, 4000));
    c.ack_notices();
    b.pulse.remove_sink(kSinkEv); /* only the headset's output is lost, the microphone stays */
    CHECK(b.r.wait([](const Snapshot &x) { return !x.monitor.active; }, 3000));
    CHECK(b.r.wait([](const Snapshot &x) {
        for (const Notice &n : x.notices)
            if (n.text == kMonGone) return true;
        return false;
    }, 3000));
    const double at = b.r.snap().time.elapsed_s;
    CHECK(b.r.wait([at](const Snapshot &x) { return x.deck.state == kDeckRecording && x.time.elapsed_s > at + 0.6; }, 4000));
    CHECK(b.r.snap().card.present);
    CHECK_EQ(b.r.snap().problems.frames_dropped, 0u);
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    const std::vector<std::string> w = b.r.wavs();
    CHECK_EQ(w.size(), 1u);
    if (!w.empty()) {
        const WavData d = read_wav(b.r.rec() + "/" + w[0]);
        CHECK(d.ok);
        CHECK(d.frames() > static_cast<uint64_t>(16000 * 1.0)); /* the take kept recording after the sink went */
    }
    /* the sink comes back: the monitor restarts by itself (the user still has it On) */
    b.pulse.add_sink(fake_bt_sink(kMac, 70));
    CHECK(b.r.wait([](const Snapshot &x) { return x.monitor.active; }, 5000));
}

/* ---- a hung server never freezes the controller ---- */

void t_hung_server_responsive() {
    BtRig b(one_card());
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    CHECK(b.wait_inputs(2));
    /* the server stops answering: every list, connect and card call now takes 1.5 s */
    b.pulse.call_delay_ms = 1500;
    auto t0 = std::chrono::steady_clock::now();
    double worst_ms = 0;
    int calls = 0;
    int ev = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2600)) {
        if (ev++ % 8 == 0) b.pulse.add_source(fake_usb_source("noise" + std::to_string(ev))); /* change events: re-listings */
        const auto a = std::chrono::steady_clock::now();
        c.refresh_cards();                  /* what the Settings page does on opening */
        c.set_soft_gain_db(ev % 2 ? 3.0 : 0.0);
        const Snapshot s = c.snapshot();
        (void)s;
        c.set_page(ev % 2 ? UiPage::Settings : UiPage::Recorder);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        worst_ms = std::max(worst_ms, ms);
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(calls > 40);
    CHECK(worst_ms < 120.0); /* a few ms in practice; never the 1.5 s of the server */
    /* the USB card records through the hang */
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording, 1000));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }, 3000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle, 2000));
    CHECK_EQ(b.r.wavs().size(), 1u);
    b.pulse.call_delay_ms = 0;
    /* a controller that starts against a hung server: start() returns, the card works, the Bluetooth part says it is not answering
     * only when the server's own timeout fires (here: it answers late) */
    {
        BtRig h(one_card());
        h.pulse.call_delay_ms = 1500;
        const auto s0 = std::chrono::steady_clock::now();
        h.r.open();
        const double open_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
        CHECK(open_ms < 1000.0); /* the first scan waits at most 0.3 s */
        CHECK(h.r.wait_meter());
        CHECK(h.r.snap().card.present);
        h.r.ctl().key_event(KeyEvent::Rec);
        CHECK(h.r.wait_state(kDeckRecording, 1000));
        h.r.ctl().key_event(KeyEvent::Stop);
        CHECK(h.r.wait_state(kDeckIdle, 2000));
        h.pulse.call_delay_ms = 0;
        CHECK(h.wait_inputs(2, 8000)); /* it is listed once the server answers */
    }
}

}  // namespace

void test_bt_out_app() {
    t_output_list();
    t_output_server_missing();
    t_play_rates_exact();
    t_play_pause_scan();
    t_disconnect_during_playback();
    t_wait_for_music();
    t_wait_timeout();
    t_call_profile_only();
    t_wait_cancel();
    t_monitor_same_headset();
    t_monitor_not_while_connecting();
    t_monitor_off_during_playback();
    t_monitor_local_mic_to_speaker();
    t_monitor_sink_lost_recording_continues();
    t_hung_server_responsive();
}
