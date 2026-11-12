/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: a Bluetooth headset as the INPUT through the sound server (task T2 of the Bluetooth release), on
 * FakePulse. The facts they are built on were measured on the HackBerry (study 12, docs/PULSE.md): in music mode the only
 * capture node is a virtual source named "bluez_input.<MAC with colons>" that reports 48 kHz float and makes the card switch
 * to the call profile after about 0.65 s; the stream says Running at once but the first second is digital zero; a headset
 * that goes away kills the stream; another headset can deliver zeros forever. All audio is synthetic.
 */
#include "bt_rig.hpp"

using namespace ta;
using namespace tbt;

namespace {

const char *kNoSound = "The headset is connected but sends no sound.";
const char *kLostTake = "The Bluetooth headset disconnected: the take was closed and kept.";
const char *kNotConnected = "The Bluetooth headset is not connected.";
const char *kNotSetUp = "Bluetooth audio is not set up";


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

/* the longest run of zero samples in a take, and its first sample */
size_t max_zero_run(const std::vector<int32_t> &s) {
    size_t best = 0, run = 0;
    for (int32_t v : s) {
        run = v == 0 ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

/* ---- the input list and the open ---- */

void t_list_and_music_mode_open() {
    BtRig b(one_card());
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(2));
    Snapshot s = b.r.snap();
    CHECK_EQ(s.card.bt_status, "");
    CHECK_EQ(s.card.inputs.size(), s.card.labels.size());
    CHECK_EQ(s.card.inputs.size(), 2u);
    if (s.card.inputs.size() != 2u) return;
    CHECK(!s.card.inputs[0].bluetooth); /* the USB card first */
    CHECK(s.card.inputs[0].hw_level);
    CHECK(s.card.inputs[0].connected && s.card.inputs[0].usable);
    const InputSnap &h = s.card.inputs[1];
    CHECK(h.bluetooth);
    CHECK_EQ(h.id, kId);
    CHECK_EQ(h.label, "Evolve 65");
    CHECK_EQ(h.kind, "headset");
    CHECK_EQ(h.rate, 16000u); /* the call rate that will be asked for, not the 48 kHz the virtual node reports */
    CHECK(h.connected && h.usable);
    CHECK_EQ(h.hint_profile, "music");
    CHECK(h.reason.empty());
    CHECK(!h.hw_level);
    CHECK_EQ(s.card.labels[1], "Evolve 65");
    CHECK(!s.card.bluetooth); /* the USB card is the input */
    CHECK_EQ(s.card.label, "USB Audio CODEC");
    CHECK(s.card.present && !s.capture.connecting);

    CHECK(c.select_card(1));
    CHECK(b.wait_present());
    s = b.r.snap();
    CHECK(s.card.bluetooth);
    CHECK_EQ(s.card.bt_rate, 16000u);
    CHECK_EQ(s.card.label, "Evolve 65");
    CHECK_EQ(s.card.selected, 1);
    CHECK(!s.card.via_server); /* not "the card is in use": the headset has its own mark */
    CHECK(!s.card.has_level && !s.card.has_agc && !s.card.has_mute);
    CHECK(!c.set_level_db(6.0)); /* no hardware level: software gain only */
    CHECK(c.set_soft_gain_db(6.0));
    CHECK(s.card.has_playback && !s.card.monitor_active); /* T3: the same headset is a monitor target; the monitor itself is off */
    CHECK(!s.capture.bt_gone);
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        CHECK_EQ(b.pulse.streams.size(), 1u);
        if (b.pulse.streams.empty()) return;
        CHECK_EQ(b.pulse.streams[0].requested, kVirt);   /* the virtual source in music mode */
        CHECK_EQ(b.pulse.streams[0].rate, 16000u);       /* explicit: the server resamples, not 48000 */
        CHECK_EQ(b.pulse.streams[0].channels, 1u);
        CHECK((b.pulse.streams[0].flags & 0x200u) != 0); /* PA_STREAM_DONT_MOVE */
    }
    CHECK(b.r.wait_meter());
    CHECK_EQ(b.r.snap().settings.card, kId); /* the choice is stored by MAC */
    CHECK(b.r.snap().meter.channels == 1);
    /* the format model on this input: 16 kHz mono, one rate */
    s = b.r.snap();
    CHECK(s.format_inputs.state == CaptureState::Known);
    CHECK_EQ(s.format_inputs.capture.rate, 16000u);
    CHECK_EQ(s.format_inputs.capture.channels, 1u);
    CHECK(s.format_inputs.speech_only);
    CHECK_EQ(s.format_inputs.rates.size(), 1u);
    CHECK_EQ(s.take.format_text, "16k/16 WAV");
}

void t_call_mode_and_narrowband() {
    {   /* call mode: the real source, 16 kHz */
        BtRig b({}, "headset-head-unit");
        b.r.open();
        CHECK(b.pick());
        CHECK(b.wait_present());
        CHECK_EQ(b.r.snap().card.inputs[0].hint_profile, "call");
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        CHECK(!b.pulse.streams.empty());
        if (!b.pulse.streams.empty()) {
            CHECK_EQ(b.pulse.streams[0].requested, kReal);
            CHECK_EQ(b.pulse.streams[0].rate, 16000u);
        }
    }
    {   /* only the CVSD profile: 8 kHz (the card has no mSBC) */
        BtRig b({}, "a2dp-sink", false);
        b.pulse.sources.push_back(virtual_source());
        PulseCard card = fake_bt_card(kMac, 70, true, "a2dp-sink");
        card.profiles.erase(std::remove_if(card.profiles.begin(), card.profiles.end(),
                                           [](const PulseProfile &p) { return p.name == "headset-head-unit"; }),
                            card.profiles.end());
        b.pulse.cards.push_back(card);
        b.r.open();
        CHECK(b.wait_inputs(1));
        CHECK_EQ(b.r.snap().card.inputs[0].rate, 8000u);
        CHECK(b.pick());
        CHECK(b.wait_present());
        CHECK_EQ(b.r.snap().card.bt_rate, 8000u);
        CHECK_EQ(b.r.snap().take.format_text, "8k/16 WAV");
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        if (!b.pulse.streams.empty()) CHECK_EQ(b.pulse.streams[0].rate, 8000u);
    }
}

/* ---- connecting, the leading zeros, no sound ---- */

void t_profile_switch_zeros_dropped() {
    BtRig b;
    b.feed->mode = Feeder::Zeros; /* the profile switch: Running at once, nothing real yet */
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait([](const Snapshot &s) { return s.capture.connecting; }, 2000));
    CHECK(!b.r.snap().capture.no_sound);
    c.key_event(KeyEvent::Rec); /* REC while connecting: the take starts at the first real audio */
    CHECK(b.r.wait_state(kDeckRecording));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Snapshot s = b.r.snap();
    CHECK(s.capture.connecting);
    CHECK_EQ(s.time.elapsed_s, 0.0); /* the leading zeros are not recording time */
    CHECK_EQ(s.deck.state, static_cast<int>(kDeckRecording));
    CHECK(b.feed->zero_frames.load() > 6000);
    b.feed->mode = Feeder::Tone;
    CHECK(b.r.wait([](const Snapshot &x) { return !x.capture.connecting && x.time.elapsed_s > 0; }, 3000));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.6; }, 4000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    const uint64_t tone = b.feed->tone_frames.load();
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    CHECK_EQ(rows[0].rate, 16000u);
    WavData w = read_wav(rows[0].path);
    CHECK(w.ok && w.consistent);
    CHECK_EQ(w.fmt.rate, 16000u);
    CHECK_EQ(w.fmt.channels, 1u);
    if (w.s.empty()) return;
    CHECK(w.s[0] != 0);                    /* the take starts at the first real sample */
    CHECK(max_zero_run(w.s) < 40);         /* no second of zeros inside it */
    CHECK(w.frames() <= tone);             /* nothing but tone: the zeros were dropped */
    CHECK(w.frames() >= 8000);
    CHECK(rows[0].seconds < 3.0);
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(b.r.rec(), file_stem(rows[0].name)), je), 0);
    CHECK_EQ(je.state, "closed");
}

void t_call_profile_ends_connecting_and_no_sound() {
    BtRig b({}, "headset-head-unit"); /* the card is already on the call profile; the headset sends zeros forever (P3) */
    b.feed->mode = Feeder::Zeros;
    b.r.open();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.snap().capture.connecting);
    /* a call profile and 300 ms of running: connecting ends although no audio came */
    CHECK(b.r.wait([](const Snapshot &s) { return !s.capture.connecting; }, 2000));
    CHECK(!b.r.snap().capture.no_sound); /* the detector counts 3 s */
    CHECK(b.r.wait([](const Snapshot &s) { return s.capture.no_sound; }, 6000));
    Snapshot s = b.r.snap();
    CHECK(has_text(s, kNoSound));
    CHECK(s.card.present); /* still recording-ready: nothing is stopped */
    /* a take started now keeps going and is not auto-stopped */
    b.r.ctl().key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(b.r.snap().deck.state, static_cast<int>(kDeckRecording));
    /* sound comes: the state clears and so does the notice */
    b.feed->mode = Feeder::Tone;
    CHECK(b.r.wait([](const Snapshot &x) { return !x.capture.no_sound; }, 3000));
    CHECK(!has_text(b.r.snap(), kNoSound));
    b.r.ctl().key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK_EQ(b.r.ctl().takes().size(), 1u);
}

/* ---- the headset goes away ---- */

void t_killed_mid_take_and_reconnect() {
    BtRig b;
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait_meter());
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 1.0; }, 4000));
    const uint64_t before = b.feed->tone_frames.load();
    b.pulse.remove_source(kVirt); /* "The sound device went away": the stream is killed, nothing silently moves */
    CHECK(b.r.wait_state(kDeckIdle, 3000));
    Snapshot s = b.r.snap();
    CHECK(s.problems.unplugged);
    CHECK_EQ(s.problems.message, kLostTake);
    CHECK(has_text(s, kLostTake));
    CHECK(has_notice(s, Notice::CardLost));
    CHECK(!s.card.present);
    CHECK(s.capture.bt_gone);
    CHECK(!s.take.recording);
    CHECK(b.stream_open(0) == false);
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK(!rows[0].damaged);
        CHECK(rows[0].seconds > 0.9);
        WavData w = read_wav(rows[0].path);
        CHECK(w.ok && w.consistent);
        CHECK(w.frames() >= 15000);          /* the frames before the cut are in the file */
        CHECK(w.frames() <= before + 1600);
        JournalEntry je;
        CHECK_EQ(journal_read(posix_file_ops(), journal_file(b.r.rec(), file_stem(rows[0].name)), je), 0);
        CHECK_EQ(je.state, "closed");
    }
    /* it never reopened on another device by itself: still one stream, none on the other source */
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_EQ(b.streams(), 1u);
    c.ack_notices();

    /* the headset connects again: its source reappears and the input is reopened for the meter, no restart */
    b.pulse.add_source(virtual_source());
    CHECK(b.r.wait([](const Snapshot &x) { return x.card.present; }, 5000));
    s = b.r.snap();
    CHECK(has_text(s, "The Bluetooth headset is back"));
    CHECK(!s.capture.bt_gone);
    CHECK_EQ(b.streams(), 2u);
    CHECK(b.r.wait_meter());
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }, 4000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK_EQ(c.takes().size(), 2u);
}

void t_moved_ends_the_take() {
    BtRig b;
    b.pulse.honour_dont_move = false; /* a server that ignores DONT_MOVE (P5): it moves the stream to the default source */
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }, 4000));
    b.pulse.remove_source(kVirt);
    CHECK(b.r.wait_state(kDeckIdle, 3000));
    Snapshot s = b.r.snap();
    CHECK(s.problems.unplugged);
    CHECK(has_text(s, kLostTake));
    CHECK(!s.card.present);
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        WavData w = read_wav(rows[0].path);
        CHECK(w.ok && w.consistent);
        CHECK(w.frames() >= 6000);
    }
    CHECK_EQ(b.streams(), 1u); /* and no new stream on the other microphone */
}

void t_identity_checks() {
    BtRig b;
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait_meter());
    {   /* attached elsewhere without a move notice: REC is refused, nothing is created */
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.streams[0].attached = "alsa_input.other";
    }
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckIdle, 2000));
    Snapshot s = b.r.snap();
    CHECK(has_text(s, "The headset microphone could not be confirmed: the take did not start."));
    CHECK(b.r.wavs().empty());
    CHECK(!s.take.recording);
    c.ack_notices();
    {   /* back on the headset: the take starts; then it is moved silently: the check during the take ends and keeps it */
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.streams[0].attached = kVirt;
    }
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }, 4000));
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.streams[0].attached = "alsa_input.other";
    }
    CHECK(b.r.wait_state(kDeckIdle, 6000)); /* checked every 3 s during a take */
    s = b.r.snap();
    CHECK(s.problems.unplugged);
    CHECK(has_text(s, kLostTake));
    CHECK_EQ(c.takes().size(), 1u);
}

void t_refuse_when_absent_and_hotplug() {
    BtRig b({}, "a2dp-sink", false); /* the headset is chosen in the settings but not connected */
    write_settings(b.r.home, "{\"version\":1,\"card\":\"" + kId + "\"}");
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.wait_inputs(1));
    Snapshot s = b.r.snap();
    CHECK_EQ(s.card.inputs.size(), 1u);
    if (s.card.inputs.size() != 1u) return;
    CHECK(s.card.inputs[0].bluetooth);
    CHECK_EQ(s.card.inputs[0].id, kId);
    CHECK(!s.card.inputs[0].connected && !s.card.inputs[0].usable);
    CHECK(!s.card.inputs[0].reason.empty());
    CHECK(s.capture.bt_gone);
    CHECK(!s.card.present);
    CHECK(s.card.bluetooth);
    CHECK_EQ(s.card.selected, 0);
    CHECK(!c.select_card(0)); /* an entry that is not there cannot be picked */
    c.ack_notices();
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckIdle, 2000));
    CHECK(has_text(b.r.snap(), kNotConnected));
    CHECK(b.r.wavs().empty());
    c.ack_notices();
    /* it connects after the app started: listed, and the chosen input opens by itself (no restart) */
    b.add_headset("a2dp-sink");
    b.pulse.add_source(fake_usb_source("noise")); /* the change event the server would send */
    CHECK(b.wait_present());
    s = b.r.snap();
    CHECK(s.card.inputs[0].connected && s.card.inputs[0].usable);
    CHECK(!s.capture.bt_gone);
    CHECK_EQ(s.card.label, "Evolve 65");
    CHECK(!has_text(s, "The Bluetooth headset is back")); /* it was never lost this run */
}

void t_hotplug_listing() {
    BtRig b(one_card(), "a2dp-sink", false);
    b.r.open();
    CHECK(b.r.wait_meter());
    CHECK_EQ(b.r.snap().card.inputs.size(), 1u);
    CHECK_EQ(b.r.snap().card.bt_status, "");
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.sources.push_back(virtual_source()); /* appears in the server's lists ... */
        b.pulse.cards.push_back(fake_bt_card(kMac, 70, true, "a2dp-sink"));
    }
    b.pulse.add_sink(fake_bt_sink(kMac, 70)); /* ... and the change event arrives */
    CHECK(b.wait_inputs(2));
    CHECK_EQ(b.r.snap().card.inputs[1].id, kId);
    CHECK(b.r.snap().card.present); /* the USB card was never touched */
    CHECK_EQ(b.r.snap().card.label, "USB Audio CODEC");
    b.pulse.remove_source(kVirt);
    {
        std::lock_guard<std::recursive_mutex> g(b.pulse.lock());
        b.pulse.cards.clear();
    }
    b.pulse.remove_sink(fake_bt_sink(kMac, 70).name);
    CHECK(b.r.wait([](const Snapshot &s) { return s.card.inputs.size() == 1; }, 4000));
}

/* ---- the server is not there ---- */

void t_server_missing() {
    for (int v = 0; v < 4; ++v) {
        BtRig b(one_card());
        if (v == 0) b.pulse.lib_missing = true;
        if (v == 1) b.pulse.server_down = true;
        if (v == 2) b.r.cfg.server_running = [] { return false; };
        if (v == 3) b.pulse.server_hangs = true;
        b.r.open();
        CHECK(b.r.wait_meter());
        const Snapshot s = b.r.snap();
        CHECK_EQ(s.card.inputs.size(), 1u); /* the USB card, unaffected */
        CHECK_EQ(s.card.bt_status, v == 3 ? "Bluetooth audio is not responding" : kNotSetUp);
        CHECK(s.card.present && !s.card.bluetooth);
        CHECK_EQ(s.card.label, "USB Audio CODEC");
        b.r.ctl().key_event(KeyEvent::Rec);
        CHECK(b.r.wait_state(kDeckRecording));
        CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }, 4000));
        b.r.ctl().key_event(KeyEvent::Stop);
        CHECK(b.r.wait_state(kDeckIdle));
        CHECK_EQ(b.r.ctl().takes().size(), 1u);
    }
    {   /* no pulse at all (the demo, the appliance boards) */
        Rig r(one_card());
        r.open();
        CHECK(r.wait_meter());
        CHECK_EQ(r.snap().card.bt_status, kNotSetUp);
        CHECK_EQ(r.snap().card.inputs.size(), 1u);
    }
    {   /* a server that appears later: connected on the retry timer */
        BtRig b(one_card());
        b.pulse.server_down = true;
        b.r.open();
        CHECK(b.r.wait_meter());
        CHECK_EQ(b.r.snap().card.bt_status, kNotSetUp);
        b.pulse.server_down = false;
        CHECK(b.wait_inputs(2, 6000));
        CHECK_EQ(b.r.snap().card.bt_status, "");
    }
}

/* ---- the microphone is open on the recorder and closed elsewhere ---- */

void t_mic_policy_pages() {
    BtRig b;
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait_meter());
    c.set_page(UiPage::Settings); /* the settings page keeps it open */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(b.r.snap().card.present);

    c.set_page(UiPage::List);
    CHECK(b.r.wait([](const Snapshot &s) { return !s.card.present && s.capture.mic_closed; }, 2000));
    CHECK(!b.stream_open(0)); /* the headset returns to music mode */
    CHECK(!b.r.snap().capture.bt_gone);
    CHECK(b.r.snap().card.inputs[0].connected);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(b.streams(), 1u); /* and stays closed */
    CHECK(!b.r.snap().card.present);
    /* back on the recorder: it opens again, in the connecting state */
    b.feed->mode = Feeder::Zeros;
    c.set_page(UiPage::Recorder);
    CHECK(b.r.wait([](const Snapshot &s) { return s.card.present && s.capture.connecting; }, 2000));
    CHECK_EQ(b.streams(), 2u);
    CHECK(!b.r.snap().capture.mic_closed);
    b.feed->mode = Feeder::Tone;
    CHECK(b.r.wait([](const Snapshot &s) { return !s.capture.connecting; }, 2000));

    /* the Edit page: closed while a take is open for trimming, reopened when it is left */
    make_take(b.r.rec(), "take_001_20261108-1105.wav", 3.0);
    c.refresh_takes();
    CHECK(c.open_trim("take_001_20261108-1105.wav"));
    c.set_page(UiPage::Edit);
    CHECK(b.r.wait([](const Snapshot &s) { return !s.card.present && s.capture.mic_closed; }, 2000));
    c.close_trim();
    c.set_page(UiPage::Recorder);
    CHECK(b.r.wait([](const Snapshot &s) { return s.card.present; }, 3000));
    CHECK_EQ(b.streams(), 3u);

    /* playback: closed while a take plays, reopened when it stops */
    b.r.sink.sleep_us = 8000;
    make_take(b.r.rec(), "take_002_20261108-1106.wav", 20.0);
    c.refresh_takes();
    CHECK(c.load("take_002_20261108-1106.wav")); /* the empty deck gets its take */
    CHECK(c.play_take("take_002_20261108-1106.wav"));
    CHECK(b.r.wait_state(kDeckPlaying));
    CHECK(b.r.wait([](const Snapshot &s) { return !s.card.present && s.capture.mic_closed; }, 2000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK(b.r.wait([](const Snapshot &s) { return s.card.present; }, 3000));
    CHECK_EQ(b.streams(), 4u);

    /* never closed during a take, whatever the page says */
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    c.set_page(UiPage::List);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(b.r.snap().card.present);
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK(b.r.wait([](const Snapshot &s) { return !s.card.present && s.capture.mic_closed; }, 2000));
    c.set_page(UiPage::Recorder);
    CHECK(b.r.wait([](const Snapshot &s) { return s.card.present; }, 3000));
}

void t_mic_policy_usb_unchanged() {
    BtRig b(one_card());
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    c.set_page(UiPage::List);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(b.r.snap().card.present); /* a USB card stays open on the list */
    CHECK(!b.r.snap().capture.mic_closed);
    c.set_page(UiPage::Recorder);
    CHECK_EQ(b.streams(), 0u);
}

/* ---- settings ---- */

void t_settings_round_trip() {
    BtRig b(one_card());
    b.r.open();
    CHECK(b.pick());
    CHECK(b.wait_present());
    b.r.close();
    b.r.open();
    CHECK(b.r.wait([](const Snapshot &s) { return s.card.present && s.card.bluetooth; }, 4000));
    CHECK_EQ(b.r.snap().settings.card, kId);
    CHECK_EQ(b.r.snap().card.label, "Evolve 65");
    CHECK_EQ(b.r.snap().card.selected, 1);
    b.r.close();
    /* the per-card map takes the same key; unknown keys survive */
    FileOps &ops = posix_file_ops();
    const std::string path = b.r.home + "/cfg.json";
    write_settings(b.r.home, "{}");
    {
        FILE *f = std::fopen(path.c_str(), "wb");
        std::fputs("{\"version\":1,\"card\":\"bt:70:BF:92:53:6B:8B\",\"cards\":{\"bt:70:BF:92:53:6B:8B\":{\"mute\":true,\"extra\":5}}}", f);
        std::fclose(f);
    }
    SettingsStore st(ops, path);
    CHECK_EQ(st.load(), 0);
    CHECK_EQ(st.values().card, kId);
    CardState cs;
    CHECK(st.card_state(kId, cs));
    CHECK(cs.mute);
    cs.mute = false;
    cs.auto_gain = true;
    st.set_card_state(kId, cs);
    CHECK_EQ(st.save(), 0);
    SettingsStore again(ops, path);
    CHECK_EQ(again.load(), 0);
    CHECK(again.card_state(kId, cs) && cs.auto_gain && !cs.mute);
    CHECK(slurp(path).find("\"extra\"") != std::string::npos);
}

void t_old_settings_unchanged() {
    BtRig b(std::vector<FakeCard>{fake_uc02(0), fake_pcm2902(1)});
    /* a file from before the Bluetooth release: a card descriptor, per-card state */
    write_settings(b.r.home,
                   "{\"version\":1,\"format\":\"wav\",\"card\":\"USB Audio CODEC||\",\"soft_gain_db\":3,"
                   "\"cards\":{\"USB Audio CODEC||\":{\"level_db\":6,\"auto_gain\":false,\"mute\":false}}}");
    b.r.open();
    CHECK(b.r.wait_meter());
    const Snapshot s = b.r.snap();
    CHECK_EQ(s.card.label, "USB Audio CODEC");
    CHECK(!s.card.bluetooth);
    CHECK_EQ(s.settings.card, "USB Audio CODEC||");
    CHECK(s.card.has_level); /* its own mixer */
    CHECK_EQ(s.card.inputs.size(), 3u);
    CHECK(s.card.inputs[2].bluetooth);
    CHECK_EQ(s.settings.soft_gain_db, 3.0);
}

/* ---- formats at 16 kHz ---- */

void t_formats_16k() {
    BtRig b(one_card());
    b.r.cfg.codecs = [] {
        CodecAvailability a;
        a.flac = a.opus = true;
        return a;
    };
    b.r.cfg.make_encoder = [](const FormatChoice &c, const AudioFormat &f) -> std::unique_ptr<Encoder> {
        if (c.key.format == RecFormat::Flac) return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096));
        return std::unique_ptr<Encoder>(new fx::PlainOggOpus(f, static_cast<unsigned>(c.opus_bitrate_bps / 8 / 50), 50));
    };
    b.r.open();
    Controller &c = b.r.ctl();
    CHECK(b.r.wait_meter());
    {   /* a card at 48 kHz: the music presets are fine (unchanged) */
        const std::vector<FormatChoice> l = list_formats(b.r.snap().format_inputs);
        const FormatChoice *m = find_choice(l, FormatKey{RecFormat::Opus, "music64"});
        CHECK(m && m->compatible);
    }
    CHECK(b.pick());
    CHECK(b.wait_present());
    CHECK(b.r.wait_meter());
    const Snapshot s = b.r.snap();
    CHECK(s.format_inputs.speech_only);
    const std::vector<FormatChoice> l = list_formats(s.format_inputs);
    CHECK_EQ(l.size(), 6u);
    CHECK_EQ(l[2].id, "opus-speech24"); /* the speech presets come first among the Opus ones */
    CHECK_EQ(l[3].id, "opus-speech32");
    CHECK(l[0].compatible && l[1].compatible && l[2].compatible && l[3].compatible); /* WAV, FLAC, speech */
    CHECK(!l[4].compatible && !l[5].compatible);                                     /* music presets: dimmed */
    CHECK(l[4].why_not.find("speech") != std::string::npos);
    CHECK(l[5].why_not.find("32 kbps") != std::string::npos);
    CHECK_EQ(card_rates(s.format_inputs).size(), 1u); /* the Sample rate row has one value */
    CHECK_EQ(card_rates(s.format_inputs)[0], 16000u);
    /* a stored music preset falls back to WAV; a speech preset records Opus at 16 kHz */
    CHECK(c.set_format_choice(RecFormat::Opus, "music128"));
    CHECK(b.r.wait([](const Snapshot &x) { return x.take.format_text == "16k/16 WAV"; }, 2000));
    CHECK(c.set_format_choice(RecFormat::Opus, "speech32"));
    CHECK(b.r.wait([](const Snapshot &x) { return x.take.format_text == "16k/16 OPUS"; }, 2000));
    CHECK_EQ(b.r.snap().take.opus_kbps, 32);
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }, 4000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK_EQ(rows[0].format_tag, "OPUS");
        CHECK_EQ(rows[0].rate, 16000u);
    }
    /* FLAC and WAV record as they are at 16 kHz */
    CHECK(c.set_format_choice(RecFormat::Flac, ""));
    CHECK(b.r.wait([](const Snapshot &x) { return x.take.format_text == "16k/16 FLAC"; }, 2000));
    c.key_event(KeyEvent::Rec);
    CHECK(b.r.wait_state(kDeckRecording));
    CHECK(b.r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.3; }, 4000));
    c.key_event(KeyEvent::Stop);
    CHECK(b.r.wait_state(kDeckIdle));
    CHECK_EQ(c.takes().size(), 2u);
}

}  // namespace

void test_bt_app() {
    t_list_and_music_mode_open();
    t_call_mode_and_narrowband();
    t_profile_switch_zeros_dropped();
    t_call_profile_ends_connecting_and_no_sound();
    t_killed_mid_take_and_reconnect();
    t_moved_ends_the_take();
    t_identity_checks();
    t_refuse_when_absent_and_hotplug();
    t_hotplug_listing();
    t_server_missing();
    t_mic_policy_pages();
    t_mic_policy_usb_unchanged();
    t_settings_round_trip();
    t_old_settings_unchanged();
    t_formats_16k();
}
