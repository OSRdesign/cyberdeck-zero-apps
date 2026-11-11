/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests (task T9): the owner's board tests B27-B29 on the HackBerry, replayed on FakePulse with PipeWire's real habits
 * (BoardSim, bt_rig.hpp): a profile switch destroys the headset's sink node and creates a new one a moment later (new index, same
 * name), the headset switched off and on comes back with NEW indexes, the card flaps between music and call as the microphone closes
 * and opens. PLAY to the same headset must play after all of it, without choosing the output again, and the output entry must follow
 * the server's nodes by MAC after every event. Real time; synthetic audio only.
 */
#include "bt_rig.hpp"

using namespace ta;
using namespace tbt;

namespace {

const char *kOutNotThere = "The Bluetooth output is not connected.";
const char *kTake = "take_001_20261111-2130.wav";

bool has_text(const Snapshot &s, const std::string &text) {
    for (const Notice &n : s.notices)
        if (n.text == text) return true;
    return false;
}

void write_settings(const std::string &home, const std::string &json) {
    posix_file_ops().mkdir(home + "/.config");
    posix_file_ops().mkdir(home + "/.config/cassette");
    FILE *f = std::fopen((home + "/.config/cassette/settings.json").c_str(), "wb");
    if (!f) return;
    std::fputs(json.c_str(), f);
    std::fclose(f);
}

std::vector<int32_t> make_take(const std::string &dir, const std::string &name, double seconds) {
    posix_file_ops().mkdir(dir);
    const auto s = tp::ramp_samples(static_cast<uint64_t>(seconds * 16000), 1);
    tp::write_wav(dir + "/" + name, tp::afmt(16000, 1, 16), s);
    return s;
}

std::vector<int16_t> to16(const std::vector<int32_t> &s) {
    std::vector<int16_t> v;
    for (int32_t x : s) v.push_back(static_cast<int16_t>(x));
    return v;
}

const OutputSnap *find_out(const Snapshot &s, const std::string &id) {
    for (const OutputSnap &o : s.card.outputs)
        if (o.id == id) return &o;
    return nullptr;
}

bool out_ok(const Snapshot &s) {
    const OutputSnap *o = find_out(s, kId);
    return o && o->connected && o->usable;
}

/* The headset as input and output, the take of B25 in the folder, the microphone open and the card on the call profile. */
struct Board {
    BtRig b;
    BoardSim sim;
    PlaySrv srv;
    std::vector<int32_t> take;
    Board(int down, int up, int gap) : b(one_card(), "a2dp-sink", true), sim(b.pulse, down, up, gap), srv(b.pulse) {
        take = make_take(b.r.rec(), kTake, 0.8);
    }
    Controller &c() { return b.r.ctl(); }
    bool start() {
        b.r.open();
        if (!b.wait_inputs(2)) return false;
        if (!c().select_output(kId)) return false;
        if (!b.pick()) return false;
        if (!b.wait_present()) return false;
        if (!wait_until([&] { return sim.to_call.load() > 0; }, 4000)) return false;
        return b.r.wait([](const Snapshot &s) { return s.card.present && !s.capture.connecting && out_ok(s); }, 4000);
    }
};

/* PLAY from the recorder and follow it: true when the take played to its end through the headset (no "not connected", one playback
 * stream on the headset's sink, the samples exact). */
bool play_through(Board &x, const char *what) {
    Controller &c = x.c();
    c.ack_notices();
    const int before = count_streams(x.b.pulse, false);
    c.key_event(KeyEvent::PlayPause);
    bool refused = false, played = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(12000);
    while (std::chrono::steady_clock::now() < end) {
        const Snapshot s = x.b.r.snap();
        refused = refused || has_text(s, kOutNotThere);
        if (s.playback.state == "playing") played = true;
        if (played && s.deck.state == kDeckIdle) break;
        if (!played && s.deck.state == kDeckIdle) break; /* stopped without playing */
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (refused) std::fprintf(stderr, "  [%s] PLAY said: %s\n", what, kOutNotThere);
    CHECK(!refused);
    CHECK(played);
    const int pi = stream_index(x.b.pulse, false, before);
    CHECK(pi >= 0);
    bool exact = false;
    if (pi >= 0) {
        {
            std::lock_guard<std::recursive_mutex> g(x.b.pulse.lock());
            CHECK_EQ(x.b.pulse.streams[static_cast<size_t>(pi)].requested, kSinkName);
        }
        exact = wait_until([&] { return x.srv.got(static_cast<size_t>(pi)) == to16(x.take); }, 3000);
        CHECK(exact);
    }
    if (!(played && !refused && exact)) std::fprintf(stderr, "  [%s] did not play through\n", what);
    return played && !refused && exact;
}

/* ---- B27-B29: off, on with new ids, PLAY without choosing the output again ---- */

/* the headset is chosen as input and output while connected, then switched off and on (B27, B28), then PLAY (B29) */
void t_reconnect_new_ids_then_play() {
    Board x(650, 1850, 120);
    CHECK(x.start());
    const uint32_t card_before = x.sim.card_index();
    /* B27: switched off */
    x.c().ack_notices();
    x.sim.off();
    CHECK(x.b.r.wait([](const Snapshot &s) { return !s.card.present && s.capture.bt_gone; }, 4000));
    CHECK(x.b.r.wait([](const Snapshot &s) {
        const OutputSnap *o = find_out(s, kId);
        return o && !o->connected && !o->usable;
    }, 4000));
    {
        const Snapshot s = x.b.r.snap(); /* the choice stays (a placeholder) */
        CHECK(s.card.output_selected >= 0 && static_cast<size_t>(s.card.output_selected) < s.card.output_devices.size() &&
              s.card.output_devices[static_cast<size_t>(s.card.output_selected)] == kId);
    }
    /* B28: on again, in call mode as on the board; every node and the card have new indexes */
    x.sim.on("headset-head-unit");
    CHECK(x.sim.card_index() != card_before);
    CHECK(x.b.r.wait([](const Snapshot &s) { return s.card.present && !s.capture.bt_gone; }, 6000)); /* the input is back by itself */
    /* Settings > Output: the headset is connected again, no reselection */
    CHECK(x.b.r.wait(out_ok, 4000));
    {
        const Snapshot s = x.b.r.snap();
        const OutputSnap *o = find_out(s, kId);
        CHECK(o && o->rate == 16000u && o->hint_profile == "call");
        CHECK(s.card.output_selected >= 0 && s.card.output_devices[static_cast<size_t>(s.card.output_selected)] == kId);
    }
    CHECK(x.b.r.wait([](const Snapshot &s) { return !s.capture.connecting; }, 4000));
    /* B29: PLAY at once from the recorder */
    CHECK(play_through(x, "after reconnect"));
    CHECK(x.b.r.wait([](const Snapshot &s) { return s.card.present; }, 6000)); /* the microphone comes back after it */
    CHECK(play_through(x, "second play"));
}

/* the choice was saved earlier; the headset is off when Cassette starts and comes on later (no reselection anywhere) */
void t_saved_choice_headset_on_later() {
    BtRig b(one_card(), "a2dp-sink", false);
    write_settings(b.r.home, "{\"version\":1,\"card\":\"" + kId + "\",\"output_device\":\"" + kId + "\"}");
    BoardSim sim(b.pulse, 650, 1850, 120, false);
    PlaySrv srv(b.pulse);
    const auto take = make_take(b.r.rec(), kTake, 0.8);
    b.r.open();
    CHECK(b.r.wait([](const Snapshot &s) {
        const OutputSnap *o = find_out(s, kId);
        return o && !o->connected && s.capture.bt_gone;
    }, 4000));
    /* while it is off the server is asked now and then (the replug timer, the output re-listing), not at the tick rate: 2 s at a
     * 10 ms tick were ~200 listings before T9 */
    const unsigned l0 = b.pulse.listings.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    const unsigned asked = b.pulse.listings.load() - l0;
    if (asked >= 40) std::fprintf(stderr, "  listings in 2 s with the headset off: %u\n", asked);
    CHECK(asked < 40);
    sim.on("a2dp-sink");
    CHECK(b.wait_present(6000));
    CHECK(b.r.wait(out_ok, 4000));
    CHECK(wait_until([&] { return sim.to_call.load() > 0; }, 4000));
    CHECK(b.r.wait([](const Snapshot &s) { return !s.capture.connecting && out_ok(s); }, 4000));
    /* the same steps as play_through, on this rig */
    Controller &c = b.r.ctl();
    c.ack_notices();
    c.key_event(KeyEvent::PlayPause);
    bool refused = false, played = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(12000);
    while (std::chrono::steady_clock::now() < end) {
        const Snapshot s = b.r.snap();
        refused = refused || has_text(s, kOutNotThere);
        if (s.playback.state == "playing") played = true;
        if (s.deck.state == kDeckIdle) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(!refused);
    CHECK(played);
    const int pi = stream_index(b.pulse, false);
    CHECK(pi >= 0);
    if (pi >= 0) CHECK(wait_until([&] { return srv.got(static_cast<size_t>(pi)) == to16(take); }, 3000));
}

/* ---- the profile flaps: list -> recorder -> list, then PLAY ---- */

void t_flaps_then_play() {
    Board x(650, 1850, 120);
    CHECK(x.start());
    for (int i = 0; i < 3; ++i) {
        const int music = x.sim.to_music.load(), call = x.sim.to_call.load();
        x.c().set_page(UiPage::List); /* the microphone closes: back to music after ~1.85 s */
        CHECK(wait_until([&] { return x.sim.to_music.load() > music; }, 4000));
        if (i == 1) std::this_thread::sleep_for(std::chrono::milliseconds(30)); /* back while the sink is barely there */
        x.c().set_page(UiPage::Recorder); /* it opens again: call mode 0.65 s later */
        CHECK(wait_until([&] { return x.sim.to_call.load() > call; }, 4000));
    }
    CHECK(x.b.r.wait([](const Snapshot &s) { return s.card.present && !s.capture.connecting; }, 4000));
    CHECK(x.b.r.wait(out_ok, 4000));
    CHECK(play_through(x, "after flaps"));
}

/* ---- PLAY right after the microphone closed ---- */

void t_play_right_after_mic_close() {
    Board x(650, 1850, 120);
    CHECK(x.start());
    /* from the list (the microphone closed, the card on its way back to music): back to the recorder and PLAY at once */
    const int music = x.sim.to_music.load();
    x.c().set_page(UiPage::List);
    std::this_thread::sleep_for(std::chrono::milliseconds(1800)); /* the switch is about to happen */
    x.c().set_page(UiPage::Recorder);
    CHECK(play_through(x, "list then PLAY"));
    CHECK(x.sim.to_music.load() > music);
    /* PLAY in the middle of a switch: the sink is gone for a moment when PLAY is pressed */
    CHECK(x.b.r.wait([](const Snapshot &s) { return s.card.present && !s.capture.connecting; }, 6000));
    CHECK(x.b.r.wait(out_ok, 4000));
    x.sim.gap_ms = 400;
    x.sim.autoswitch = false;
    std::thread sw([&] { x.sim.switch_to("headset-head-unit"); }); /* the call sink is re-created (a codec change) */
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    x.sim.autoswitch = true;
    CHECK(play_through(x, "PLAY during a switch"));
    sw.join();
}

/* ---- the headset stays in call mode: PLAY waits, then plays at call quality ---- */

void t_play_in_call_mode() {
    Board x(650, -1, 120); /* WirePlumber never goes back to music (the owner's final state: call profile, no microphone) */
    CHECK(x.start());
    x.c().set_page(UiPage::List);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK_EQ(x.sim.profile(), "headset-head-unit");
    x.c().set_page(UiPage::Recorder);
    CHECK(x.b.r.wait([](const Snapshot &s) { return s.card.present && !s.capture.connecting; }, 4000));
    CHECK(play_through(x, "call mode"));
    CHECK(x.b.r.snap().playback.switch_timed_out || x.b.r.snap().deck.state == kDeckIdle);
}

/* ---- the output entry after each server event ---- */

void t_output_entry_each_event() {
    BtRig b(one_card(), "a2dp-sink", true);
    BoardSim sim(b.pulse, 650, 1850, 0);
    sim.autoswitch = false;
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    CHECK(c.select_output(kId));
    auto entry = [&](bool connected, bool usable, uint32_t rate, const char *step) {
        const bool ok = b.r.wait([&](const Snapshot &s) {
            const OutputSnap *o = find_out(s, kId);
            return o && o->connected == connected && o->usable == usable && (!usable || o->rate == rate);
        }, 3000);
        if (!ok) {
            const Snapshot s = b.r.snap();
            const OutputSnap *o = find_out(s, kId);
            std::fprintf(stderr, "  [%s] entry: %s connected=%d usable=%d rate=%u\n", step, o ? "yes" : "none", o ? o->connected : -1,
                         o ? o->usable : -1, o ? o->rate : 0u);
        }
        CHECK(ok);
    };
    entry(true, true, 48000, "music");
    b.pulse.remove_sink(kSinkName); /* a switch begins: the node goes, the card stays */
    entry(true, false, 0, "sink gone, card there");
    b.pulse.set_profile(kCard, "headset-head-unit");
    b.pulse.add_sink(sim.sink_for("headset-head-unit", sim.card_index())); /* the new node, a new index */
    entry(true, true, 16000, "call sink");
    sim.off();
    entry(false, false, 0, "off");
    {
        const Snapshot s = b.r.snap();
        const OutputSnap *o = find_out(s, kId);
        CHECK(o && o->reason == "Not connected"); /* the placeholder of the choice */
    }
    const uint32_t card = b.pulse.add_card(fake_bt_card(kMac, kPulseNoIndex, true, "a2dp-sink"));
    entry(true, false, 0, "card back, no sink yet");
    b.pulse.add_sink(sim.sink_for("a2dp-sink", card));
    entry(true, true, 48000, "sink back");
    /* the PLAY target follows: a take plays to the new node */
    PlaySrv srv(b.pulse);
    const auto take = make_take(b.r.rec(), kTake, 0.5);
    c.refresh_takes();
    c.load(kTake);
    c.key_event(KeyEvent::PlayPause);
    CHECK(b.r.wait([](const Snapshot &s) { return s.playback.state == "playing"; }, 4000));
    CHECK(b.r.wait_state(kDeckIdle, 5000));
    const int pi = stream_index(b.pulse, false);
    CHECK(pi >= 0);
    if (pi >= 0) CHECK(wait_until([&] { return srv.got(static_cast<size_t>(pi)) == to16(take); }, 3000));
}

/* ---- the stream attached to no source for a moment (B29 log, t=55: source output on index 4294967295) ---- */

const char *kIdentity = "The headset microphone could not be confirmed: the take did not start.";
const char *kLostTake = "The Bluetooth headset disconnected: the take was closed and kept.";

int open_capture_index(FakePulse &f) {
    std::lock_guard<std::recursive_mutex> g(f.lock());
    for (size_t i = f.streams.size(); i-- > 0;)
        if (f.streams[i].capture && f.streams[i].open && f.streams[i].cap) return static_cast<int>(i);
    return -1;
}

void set_attached(FakePulse &f, int i, const std::string &to) {
    std::lock_guard<std::recursive_mutex> g(f.lock());
    if (i >= 0 && static_cast<size_t>(i) < f.streams.size()) f.streams[static_cast<size_t>(i)].attached = to;
}

unsigned identity_queries(FakePulse &f, int i) {
    std::lock_guard<std::recursive_mutex> g(f.lock());
    return i >= 0 && static_cast<size_t>(i) < f.streams.size() ? f.streams[static_cast<size_t>(i)].identity_queries : 0;
}

void t_identity_no_source_for_a_moment() {
    BtRig b;
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait_meter());
    const int ci = open_capture_index(b.pulse);
    CHECK(ci >= 0);
    /* REC while the server names no source for 0.4 s: the check is asked again, the take starts */
    set_attached(b.pulse, ci, "");
    std::thread back([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        set_attached(b.pulse, ci, kVirt);
    });
    c.ack_notices();
    c.key_event(KeyEvent::Rec);
    back.join();
    CHECK(b.r.wait_state(kDeckRecording, 3000));
    CHECK(!has_text(b.r.snap(), kIdentity));
    CHECK(b.r.wait([](const Snapshot &s) { return s.time.elapsed_s >= 0.5; }, 4000));
    /* during the take: no source for a moment around a check: the take goes on */
    const unsigned q0 = identity_queries(b.pulse, ci);
    set_attached(b.pulse, ci, "");
    CHECK(wait_until([&] { return identity_queries(b.pulse, ci) > q0; }, 5000)); /* the 3 s check saw no source */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    set_attached(b.pulse, ci, kVirt);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_EQ(b.r.snap().deck.state, kDeckRecording);
    CHECK(!b.r.snap().problems.unplugged);
    CHECK(identity_queries(b.pulse, ci) >= q0 + 2); /* asked again soon, not 3 s later */
    /* no source for longer than 1 s during a take: it ends and is kept, as a loss */
    const unsigned q1 = identity_queries(b.pulse, ci);
    set_attached(b.pulse, ci, "");
    CHECK(wait_until([&] { return identity_queries(b.pulse, ci) > q1; }, 5000));
    CHECK(b.r.wait_state(kDeckIdle, 3000));
    CHECK(has_text(b.r.snap(), kLostTake));
    CHECK_EQ(c.takes().size(), 1u);
}

void t_identity_no_source_too_long_at_rec() {
    BtRig b;
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait_meter());
    const int ci = open_capture_index(b.pulse);
    CHECK(ci >= 0);
    /* no source for 1.6 s: REC is refused after about 1 s (not at once), nothing is recorded */
    set_attached(b.pulse, ci, "");
    c.ack_notices();
    const auto t0 = std::chrono::steady_clock::now();
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckIdle, 3000));
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took > 0.9 && took < 2.0);
    CHECK(has_text(b.r.snap(), kIdentity));
    CHECK(b.r.wavs().empty());
    CHECK(identity_queries(b.pulse, ci) >= 5); /* asked again every 0.1 s */
    /* not for good: when the server names the headset's source again, REC records */
    set_attached(b.pulse, ci, kVirt);
    c.ack_notices();
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording, 3000));
    CHECK(b.r.wait([](const Snapshot &s) { return s.time.elapsed_s >= 0.3; }, 4000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK_EQ(c.takes().size(), 1u);
    /* another source is refused at once (C16, unchanged) */
    set_attached(b.pulse, ci, "alsa_input.other");
    c.ack_notices();
    const auto t1 = std::chrono::steady_clock::now();
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckIdle, 3000));
    CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(500));
    CHECK(has_text(b.r.snap(), kIdentity));
}

}  // namespace

void test_bt_reconnect_app() {
    t_output_entry_each_event();
    t_reconnect_new_ids_then_play();
    t_saved_choice_headset_on_later();
    t_flaps_then_play();
    t_play_right_after_mic_close();
    t_play_in_call_mode();
    t_identity_no_source_for_a_moment();
    t_identity_no_source_too_long_at_rec();
}
