/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding tests: the Bluetooth parts of the view model (task T4): the Input, Output and Monitor to pickers' rows and
 * their dimming per state, the Input picker's footer, Input level greyed, the Monitor delay line, the recorder's BT label,
 * standing notices and switching line, the call-quality notice once per play, the controller's notices passed through, the
 * Monitor to action. Synthetic names and figures only.
 */
#include "check.hpp"
#include "fake_port.hpp"

#include "bind/binder.hpp"
#include "bind/view_map.hpp"
#include "ui/ui_format.hpp"

#include <algorithm>

using namespace cassette;
using namespace cassette::bind;
using tb::base_snapshot;
using tb::FakePort;
using tb::row;

namespace {

constexpr int64_t kNow = 1794139200;
const std::string kDot = " \xC2\xB7 ";

ui::ViewModel map(const Snapshot &s)
{
    static const std::vector<TakeRow> rows;
    static const LocalState local;
    MapInput in;
    in.snap = &s;
    in.rows = &rows;
    in.local = &local;
    in.now_wall_s = kNow;
    in.utc = true;
    return map_view(in);
}

const ui::SettingRow *find_row(const ui::ViewModel &vm, const std::string &id)
{
    for (const ui::SettingRow &r : vm.settings.rows)
        if (r.id == id) return &r;
    return nullptr;
}

int row_index(const ui::ViewModel &vm, const std::string &id)
{
    for (size_t i = 0; i < vm.settings.rows.size(); ++i)
        if (vm.settings.rows[i].id == id) return static_cast<int>(i);
    return -1;
}

InputSnap usb(const std::string &label)
{
    InputSnap in;
    in.id = "hw:" + label;
    in.label = label;
    return in;
}

InputSnap bt_in(const std::string &label, const std::string &kind, uint32_t rate, bool connected = true, bool usable = true,
                const std::string &reason = "")
{
    InputSnap in;
    in.id = "bt:70:BF:92:53:6B:8B";
    in.label = label;
    in.bluetooth = true;
    in.kind = kind;
    in.rate = rate;
    in.connected = connected;
    in.usable = usable;
    in.reason = reason;
    in.hw_level = false;
    in.hint_profile = "call";
    return in;
}

OutputSnap alsa_out(const std::string &label, const std::string &id)
{
    OutputSnap o;
    o.id = id;
    o.label = label;
    return o;
}

OutputSnap bt_out(const std::string &label, const std::string &kind, const std::string &profile, bool connected = true,
                  bool usable = true, const std::string &reason = "")
{
    OutputSnap o;
    o.id = "bt:9E:0C:81:E6:5D:1A";
    o.label = label;
    o.bluetooth = true;
    o.kind = kind;
    o.hint_profile = profile;
    o.connected = connected;
    o.usable = usable;
    o.reason = reason;
    return o;
}

/* a USB card, a connected headset, a headset with its microphone off, the chosen headset that is not there */
Snapshot bt_inputs_snapshot(int selected)
{
    Snapshot s = base_snapshot();
    CardSnap &c = s.card;
    c.inputs = {usb("Demo Microphone"), bt_in("Evolve 65", "headset", 16000),
                bt_in("Beats Fit Pro", "headset", 16000, true, false, "The headset microphone is off"),
                bt_in("WH-1000XM4 Wireless Noise Cancelling Headphones", "other", 0, false, false, "Not connected")};
    c.labels.clear();
    for (const InputSnap &in : c.inputs) c.labels.push_back(in.label);
    c.selected = selected;
    c.label = c.labels[static_cast<size_t>(selected)];
    c.bluetooth = c.inputs[static_cast<size_t>(selected)].bluetooth;
    if (c.bluetooth) c.has_level = c.has_agc = false;
    return s;
}

void input_picker()
{
    /* no Bluetooth at all (a snapshot from before T4): the Input row exactly as it was */
    {
        const ui::ViewModel vm = map(base_snapshot());
        const ui::SettingRow *in = find_row(vm, "input");
        CHECK(in && in->choice_entries.empty() && in->formats_footer.empty() && in->enabled);
        CHECK(find_row(vm, "level") && find_row(vm, "level")->kind == ui::SettingKind::Slider);
        CHECK(!find_row(vm, "monitor_to"));
    }
    /* the Bluetooth entries: two-line rows; the USB card plain */
    {
        const ui::ViewModel vm = map(bt_inputs_snapshot(1));
        const ui::SettingRow *in = find_row(vm, "input");
        CHECK(in != nullptr);
        if (!in) return;
        CHECK_EQ(in->choice_entries.size(), 4u);
        CHECK_EQ(in->choices.size(), 4u);
        CHECK_EQ(in->choice, 1);
        CHECK_EQ(in->value, "Evolve 65");
        CHECK_EQ(in->formats_footer, "Bluetooth headsets record speech quality (16 kHz)");
        if (in->choice_entries.size() == 4) {
            const ui::FormatEntry &u = in->choice_entries[0], &e = in->choice_entries[1], &off = in->choice_entries[2],
                                  &gone = in->choice_entries[3];
            CHECK_EQ(u.name, "Demo Microphone");
            CHECK(u.detail.empty() && u.size.empty() && u.compatible); /* plain: the page draws the old one-line row */
            CHECK_EQ(e.name, "Evolve 65");
            CHECK_EQ(e.size, "16 kHz");
            CHECK_EQ(e.detail, "Bluetooth headset" + kDot + "speech quality");
            CHECK(e.compatible);
            CHECK(!off.compatible);
            CHECK_EQ(off.detail, "The headset microphone is off");
            CHECK_EQ(off.reason, "The headset microphone is off");
            CHECK(!gone.compatible);
            CHECK_EQ(gone.detail, "Not connected");
            CHECK_EQ(gone.size, "");
        }
        /* the hardware level does not apply: the row greyed (Info, as Channels), Auto gain gone */
        const ui::SettingRow *lvl = find_row(vm, "level");
        CHECK(lvl && lvl->kind == ui::SettingKind::Info && !lvl->enabled && lvl->value == "Not applicable");
        CHECK(!find_row(vm, "agc"));
        CHECK_EQ(row_index(vm, "level"), 1);
    }
    /* the chosen headset is absent: the selected marker stays on its (dimmed) row */
    {
        Snapshot s = bt_inputs_snapshot(3);
        s.card.present = false;
        const ui::ViewModel vm = map(s);
        const ui::SettingRow *in = find_row(vm, "input");
        CHECK(in && in->choice == 3 && !in->choice_entries[3].compatible && in->value == "WH-1000XM4 Wireless Noise Cancelling Headphones");
        CHECK(find_row(vm, "level") && find_row(vm, "level")->value == "Not applicable");
    }
    /* the sound server missing or not running: no Bluetooth rows, the footer says so; the USB cards stay */
    {
        Snapshot s = base_snapshot();
        s.card.bt_status = "Bluetooth audio is not set up";
        const ui::ViewModel vm = map(s);
        const ui::SettingRow *in = find_row(vm, "input");
        CHECK(in && in->choice_entries.empty() && in->choices.size() == 2);
        CHECK_EQ(in->formats_footer, "Bluetooth audio is not set up on this board");
        CHECK(in->enabled);
        /* no card at all: the picker still opens for the footer */
        s.card.labels.clear();
        s.card.inputs.clear();
        s.card.label.clear();
        s.card.present = false;
        const ui::ViewModel v2 = map(s);
        CHECK(find_row(v2, "input") && find_row(v2, "input")->enabled && find_row(v2, "input")->value == "None");
        /* ... but not while recording */
        s.deck.state = kDeckRecording;
        CHECK(!find_row(map(s), "input")->enabled);
    }
    /* the server does not answer: neither footer */
    {
        Snapshot s = base_snapshot();
        s.card.bt_status = "Bluetooth audio is not responding";
        const ui::SettingRow *in = find_row(map(s), "input");
        CHECK(in && in->formats_footer.empty());
        s.card.labels.clear();
        CHECK(!find_row(map(s), "input")->enabled); /* nothing to show: as before */
    }
}

void output_picker()
{
    Snapshot s = base_snapshot();
    s.card.output_labels = {"Demo Microphone", "XWF-M18 Speaker", "Evolve 65", "Beats Fit Pro", "Default output"};
    s.card.output_devices = {"hw:CARD=Demo,DEV=0", "bt:9E:0C:81:E6:5D:1A", "bt:70:BF:92:53:6B:8B", "bt:F8:66:5A:18:D3:69",
                             "default"};
    s.card.outputs = {alsa_out("Demo Microphone", "hw:CARD=Demo,DEV=0"), bt_out("XWF-M18 Speaker", "speaker", "music"),
                      bt_out("Evolve 65", "headset", "call"), bt_out("Beats Fit Pro", "other", "", false, false, "Not connected"),
                      alsa_out("Default output", "default")};
    s.card.output_selected = 1;
    const ui::ViewModel vm = map(s);
    const ui::SettingRow *o = find_row(vm, "output");
    CHECK(o != nullptr);
    if (!o) return;
    CHECK_EQ(o->value, "XWF-M18 Speaker");
    CHECK_EQ(o->choice_entries.size(), 5u);
    if (o->choice_entries.size() == 5) {
        CHECK(o->choice_entries[0].detail.empty());
        CHECK_EQ(o->choice_entries[1].detail, "Music quality");
        CHECK_EQ(o->choice_entries[1].size, "");
        CHECK(o->choice_entries[1].compatible);
        CHECK_EQ(o->choice_entries[2].detail, "Call quality (16 kHz)");
        CHECK(!o->choice_entries[3].compatible);
        CHECK_EQ(o->choice_entries[3].detail, "Not connected");
        CHECK(o->choice_entries[4].detail.empty() && o->choice_entries[4].name == "Default output");
    }
    CHECK(o->formats_footer.empty());
    /* only ALSA outputs: plain rows as before */
    s.card.output_labels = {"Demo Microphone", "Default output"};
    s.card.output_devices = {"hw:CARD=Demo,DEV=0", "default"};
    s.card.outputs = {alsa_out("Demo Microphone", "hw:CARD=Demo,DEV=0"), alsa_out("Default output", "default")};
    s.card.output_selected = 0;
    CHECK(find_row(map(s), "output")->choice_entries.empty());
}

void monitor_rows()
{
    /* the headset as input, monitored on itself: Monitor's delay line (muted), Monitor to Automatic */
    Snapshot s = bt_inputs_snapshot(1);
    s.card.has_playback = true;
    s.settings.monitor = true;
    MonitorSnap &m = s.monitor;
    m.active = true;
    m.route = "same_headset";
    m.delay_ms = 87;
    m.available = true;
    m.target_label = "Evolve 65";
    m.outputs = {alsa_out("Automatic", ""), alsa_out("Demo Microphone", "hw:CARD=Demo,DEV=0"),
                 bt_out("Evolve 65", "headset", "call"), bt_out("XWF-M18 Speaker", "speaker", "music")};
    m.output_selected = 0;
    ui::ViewModel vm = map(s);
    const ui::SettingRow *sw = find_row(vm, "monitor");
    CHECK(sw && sw->detail == "Delay about 90 ms" && !sw->detail_warn);
    const ui::SettingRow *to = find_row(vm, "monitor_to");
    CHECK(to != nullptr);
    if (!to) return;
    CHECK_EQ(to->label, "Monitor to");
    CHECK_EQ(to->value, "Automatic");
    CHECK(!to->dimmed && to->enabled);
    CHECK_EQ(to->choice, 0);
    CHECK_EQ(to->choices.size(), 4u);
    CHECK_EQ(to->choice_entries.size(), 4u);
    if (to->choice_entries.size() == 4) {
        CHECK(to->choice_entries[0].detail.empty() && to->choice_entries[0].name == "Automatic");
        CHECK_EQ(to->choice_entries[2].detail, "Call quality (16 kHz)");
        CHECK_EQ(to->choice_entries[3].detail, "Music quality");
    }
    CHECK(row_index(vm, "monitor_to") == row_index(vm, "monitor_level") + 1);

    /* a delay over 150 ms or an A2DP target: the echo wording, amber */
    m.delay_ms = 250;
    m.notice = "delay";
    m.output = "bt:9E:0C:81:E6:5D:1A";
    m.output_selected = 3;
    m.route = "other_output";
    vm = map(s);
    sw = find_row(vm, "monitor");
    CHECK(sw && sw->detail == "Delay about 0.25 s: you will hear an echo" && sw->detail_warn);
    CHECK_EQ(find_row(vm, "monitor_to")->value, "XWF-M18 Speaker");
    /* not running yet, the A2DP target known: the estimate */
    m.active = false;
    m.delay_ms = 0;
    vm = map(s);
    sw = find_row(vm, "monitor");
    CHECK(sw && sw->detail == "Delay about 0.2-0.3 s: you will hear an echo" && sw->detail_warn);
    /* not running, no notice: nothing */
    m.notice.clear();
    vm = map(s);
    sw = find_row(vm, "monitor");
    CHECK(sw && sw->detail.empty() && !sw->detail_warn);
    /* the boundaries */
    m.active = true;
    m.delay_ms = 950;
    CHECK_EQ(find_row(map(s), "monitor")->detail, "Delay about 950 ms");
    m.delay_ms = 1200;
    CHECK_EQ(find_row(map(s), "monitor")->detail, "Delay about 1.2 s");

    /* no target: Monitor to greyed with the reason, still opens (a target can be chosen); no Monitor switch */
    Snapshot n = base_snapshot();
    n.card.has_playback = false;
    n.monitor.available = false;
    n.monitor.reason = "The sound card has no playback side";
    n.monitor.outputs = {alsa_out("Automatic", ""), bt_out("XWF-M18 Speaker", "speaker", "music")};
    vm = map(n);
    to = find_row(vm, "monitor_to");
    CHECK(to && to->dimmed && to->enabled && to->value == "The sound card has no playback side");
    CHECK(!find_row(vm, "monitor"));
    CHECK(row_index(vm, "monitor_to") == row_index(vm, "limiter") + 1);
    /* no Bluetooth anywhere and nothing chosen: no Monitor to row (the Settings as before) */
    n.monitor.outputs = {alsa_out("Automatic", ""), alsa_out("Default output", "default")};
    CHECK(!find_row(map(n), "monitor_to"));
    /* ... but a target chosen earlier keeps it (the way back to Automatic) */
    n.monitor.output = "hw:CARD=Other,DEV=0";
    CHECK(find_row(map(n), "monitor_to") != nullptr);
}

void recorder()
{
    Snapshot s = bt_inputs_snapshot(1);
    ui::ViewModel vm = map(s);
    CHECK(vm.rec.bluetooth);
    CHECK_EQ(vm.rec.standing_notice, "");
    CHECK(!map(base_snapshot()).rec.bluetooth);
    /* connecting: the standing notice, the meter dark */
    s.capture.connecting = true;
    vm = map(s);
    CHECK_EQ(vm.rec.standing_notice, "Connecting the headset...");
    CHECK_EQ(vm.rec.meter.ch[0].bar, 0);
    CHECK_EQ(vm.rec.meter.ch[0].hold_bar, 0);
    CHECK_EQ(vm.notice, ""); /* the timed notice is not touched */
    /* no sound */
    s.capture.connecting = false;
    s.capture.no_sound = true;
    vm = map(s);
    CHECK_EQ(vm.rec.standing_notice, "The headset is connected but sends no sound.");
    CHECK_EQ(vm.rec.meter.ch[0].bar, 11);
    s.capture.no_sound = false;
    CHECK_EQ(map(s).rec.standing_notice, "");
    CHECK_EQ(standing_notice_text(CaptureSnap()), "");

    /* PLAY waits for the music mode: the switching line in the box / strip */
    Snapshot p = base_snapshot();
    p.deck.state = kDeckPlaying;
    p.take.name = "take_006_20261108-1105.wav";
    p.time.length_s = 728;
    p.playback.state = "switching_to_music";
    p.playback.bluetooth = true;
    p.playback.switch_elapsed_s = 1.4;
    vm = map(p);
    CHECK_EQ(vm.rec.info.play_status, "Switching to music mode... 1 s");
    CHECK_EQ(ui::info_detail(vm.rec), "Switching to music mode... 1 s");
    p.playback.switch_elapsed_s = 9;
    CHECK_EQ(map(p).rec.info.play_status, "Switching to music mode... 4 s");
    p.playback.state = "playing";
    CHECK_EQ(map(p).rec.info.play_status, "");
    /* same() sees the new recorder fields */
    ui::RecorderVm a = map(base_snapshot()).rec, b = a;
    b.bluetooth = true;
    CHECK(!same(a, b));
    b = a;
    b.standing_notice = "x";
    CHECK(!same(a, b));
    b = a;
    b.info.play_status = "x";
    CHECK(!same(a, b));
}

void call_quality_watch()
{
    Snapshot s = base_snapshot();
    CallQualityWatch w;
    /* idle: nothing */
    CHECK(!w.step(s, 0));
    /* a call-only headset (no wait): at once, once */
    s.deck.state = kDeckPlaying;
    s.playback.state = "playing";
    s.playback.bluetooth = true;
    s.playback.quality = "call";
    CHECK(w.step(s, 100));
    CHECK(!w.step(s, 200));
    s.deck.state = kDeckPlayingPaused; /* pause and resume: the same play */
    CHECK(!w.step(s, 300));
    s.deck.state = kDeckPlaying;
    CHECK(!w.step(s, 400));
    /* stop, play again: once more */
    s.deck.state = kDeckIdle;
    s.playback.state = "idle";
    CHECK(!w.step(s, 500));
    s.deck.state = kDeckPlaying;
    s.playback.state = "playing";
    CHECK(w.step(s, 600));
    /* a wait that reached music: a stale "call" reading right after it is not a notice */
    s.deck.state = kDeckIdle;
    w.step(s, 700);
    s.deck.state = kDeckPlaying;
    s.playback.state = "switching_to_music";
    CHECK(!w.step(s, 800));
    s.playback.state = "playing";
    s.playback.quality = "call";
    CHECK(!w.step(s, 900));
    s.playback.quality = "music";
    CHECK(!w.step(s, 1000));
    CHECK(!w.step(s, 5000));
    /* the wait timed out: the notice (same text) */
    s.deck.state = kDeckIdle;
    w.step(s, 6000);
    s.deck.state = kDeckPlaying;
    s.playback.state = "switching_to_music";
    s.playback.switch_timed_out = false;
    CHECK(!w.step(s, 6100));
    s.playback.state = "playing";
    s.playback.switch_timed_out = true;
    CHECK(w.step(s, 10100));
    /* a stale timed-out flag from an earlier play without a wait now: no notice when the quality is music */
    s.deck.state = kDeckIdle;
    w.step(s, 11000);
    s.deck.state = kDeckPlaying;
    s.playback.quality = "music";
    CHECK(!w.step(s, 11100));
    /* a call reading that lasts past the grace after a wait: the notice */
    s.deck.state = kDeckIdle;
    w.step(s, 12000);
    s.deck.state = kDeckPlaying;
    s.playback.switch_timed_out = false;
    s.playback.state = "switching_to_music";
    w.step(s, 12100);
    s.playback.state = "playing";
    s.playback.quality = "call";
    CHECK(!w.step(s, 12200));
    CHECK(w.step(s, 12200 + kCallQualityGraceMs));
    /* not a Bluetooth output: never */
    CallQualityWatch w2;
    s.playback.bluetooth = false;
    s.playback.quality = "";
    s.playback.state = "playing";
    CHECK(!w2.step(s, 0));
    CHECK(!w2.step(s, 5000));
}

struct Rig {
    FakePort port;
    Worker worker{false};
    Binder binder;
    ui::UiActions act;
    int64_t t = 1000;
    Rig() : binder(port, worker, opts())
    {
        port.snap = base_snapshot();
        port.rows = {row("take_006_20261108-1105.wav", 728, 69ull << 20, "2026-11-08 11:05", true)};
        act = binder.actions();
        tick();
        tick();
        port.take_calls();
    }
    static Binder::Options opts()
    {
        Binder::Options o;
        o.utc = true;
        return o;
    }
    void tick(int64_t dt = 33)
    {
        t += dt;
        binder.tick(t, kNow);
    }
};

bool has(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }

void binder_bt()
{
    /* Monitor to: the entry's id to set_monitor_output */
    {
        Rig r;
        r.port.snap.monitor.outputs = {alsa_out("Automatic", ""), bt_out("XWF-M18 Speaker", "speaker", "music")};
        r.tick();
        r.act.setting_choice("monitor_to", 1);
        CHECK(has(r.port.take_calls(), "set_monitor_output bt:9E:0C:81:E6:5D:1A"));
        r.act.setting_choice("monitor_to", 0);
        CHECK(has(r.port.take_calls(), "set_monitor_output "));
        r.act.setting_choice("monitor_to", 5); /* out of range: nothing */
        CHECK(r.port.take_calls().empty());
    }
    /* the call-quality notice: once per play, through the notice queue */
    {
        Rig r;
        r.port.snap.deck.state = kDeckPlaying;
        r.port.snap.playback.state = "playing";
        r.port.snap.playback.bluetooth = true;
        r.port.snap.playback.quality = "call";
        r.tick();
        CHECK_EQ(r.binder.vm().notice, "Playing in call quality: this headset has no music mode.");
        r.tick(4100);
        CHECK_EQ(r.binder.vm().notice, "");
        r.tick(4100);
        CHECK_EQ(r.binder.vm().notice, ""); /* not again in the same play */
    }
    /* the controller's Bluetooth notices pass through as they are (the headset gone: the take closed and kept) */
    {
        Rig r;
        Notice n;
        n.kind = Notice::CardLost;
        n.text = "The Bluetooth headset disconnected: the take was closed and kept.";
        r.port.snap.notices = {n};
        r.port.snap.capture.bt_gone = true;
        r.tick();
        r.tick();
        CHECK_EQ(r.binder.vm().notice, n.text);
    }
    /* the standing notices go to the recorder, not into the timed notice */
    {
        Rig r;
        r.port.snap.capture.connecting = true;
        r.tick();
        CHECK_EQ(r.binder.vm().rec.standing_notice, "Connecting the headset...");
        CHECK_EQ(r.binder.vm().notice, "");
    }
}

/* T9 (B27, B29): the chosen headset not connected says so (window, short box text); a microphone closed on purpose keeps the view */
void missing_and_held()
{
    /* not connected: the empty window with the Bluetooth text, the strip the controller's text, the box the short form */
    Snapshot s = bt_inputs_snapshot(1);
    s.card.present = false;
    s.card.message = "The Bluetooth headset is not connected.";
    s.capture.bt_gone = true;
    s.meter = MeterSnap();
    ui::ViewModel vm = map(s);
    CHECK(!vm.rec.input_present);
    CHECK(vm.rec.bt_missing);
    CHECK(vm.rec.bluetooth);
    CHECK(!vm.rec.tape.loaded);
    CHECK_EQ(vm.rec.info.notice, "The Bluetooth headset is not connected.");
    CHECK_EQ(vm.rec.info.box_notice, "Not connected");
    CHECK_EQ(ui::info_detail(vm.rec), "Not connected");                         /* the box: one short line */
    CHECK_EQ(ui::info_line(vm.rec), "The Bluetooth headset is not connected."); /* the strip: the whole text */
    CHECK(!vm.rec.keys[static_cast<int>(ui::KeyId::Rec)].enabled);
    /* a USB card that is not there: the USB window text, no short form */
    Snapshot u = base_snapshot();
    u.card.present = false;
    u.card.message = "The chosen sound card is not connected";
    vm = map(u);
    CHECK(!vm.rec.input_present);
    CHECK(!vm.rec.bt_missing);
    CHECK_EQ(vm.rec.info.box_notice, "");
    CHECK_EQ(ui::info_detail(vm.rec), "The chosen sound card is not connected");
    /* the headset there, its microphone closed on purpose (the list, the Edit page, playback): the normal view, BT */
    Snapshot h = bt_inputs_snapshot(1);
    h.card.present = false;
    h.card.message.clear();
    h.capture.mic_closed = true;
    h.meter = MeterSnap();
    for (int ds : {kDeckIdle, kDeckPlaying}) {
        h.deck.state = ds;
        h.take.name = "take_006_20261108-1105.wav";
        vm = map(h);
        CHECK(vm.rec.input_present);
        CHECK(vm.rec.tape.loaded);
        CHECK(!vm.rec.bt_missing);
        CHECK(vm.rec.bluetooth);
        CHECK_EQ(vm.rec.info.notice, "");
        CHECK(vm.rec.info.take != "NO INPUT");
        if (ds == kDeckIdle) CHECK_EQ(vm.rec.meter.channels, 1); /* the one bar, labelled BT */
    }
    CHECK(mic_held(h));
    /* ... the Settings page keeps the Monitor rows (the controller keeps has_playback while the microphone is held) */
    h.deck.state = kDeckIdle;
    h.card.has_playback = true;
    vm = map(h);
    CHECK(find_row(vm, "monitor") != nullptr);
    /* closed on purpose but the headset went away meanwhile: that is the not-connected state */
    h.capture.bt_gone = true;
    h.card.message = "The Bluetooth headset is not connected.";
    CHECK(!mic_held(h));
    vm = map(h);
    CHECK(!vm.rec.input_present);
    CHECK(vm.rec.bt_missing);
    /* same() sees the new fields */
    ui::RecorderVm a = map(base_snapshot()).rec, b = a;
    b.bt_missing = true;
    CHECK(!same(a, b));
    b = a;
    b.info.box_notice = "x";
    CHECK(!same(a, b));
}

/* T8: the launcher's facts in the rows: paired devices that are not connected are dimmed two-line rows; the footer by bt_status */
void launcher_facts_rows()
{
    const std::string reason = "Not connected: connect it in Settings > Bluetooth";
    Snapshot s = bt_inputs_snapshot(1);
    InputSnap paired = bt_in("Bluetooth device D3:69", "headset", 0, false, false, reason);
    paired.id = "bt:F8:66:5A:18:D3:69";
    paired.hint_profile = "unknown";
    s.card.inputs.push_back(paired);
    s.card.labels.push_back(paired.label);
    ui::ViewModel vm = map(s);
    const ui::SettingRow *in = find_row(vm, "input");
    CHECK(in != nullptr);
    if (in && in->choice_entries.size() == 5) {
        const ui::FormatEntry &p = in->choice_entries[4];
        CHECK_EQ(p.name, "Bluetooth device D3:69");
        CHECK(!p.compatible);
        CHECK_EQ(p.detail, reason);
        CHECK_EQ(p.reason, reason);
        CHECK_EQ(p.size, "");
    } else {
        CHECK(false);
    }
    /* the output picker: a paired speaker, dimmed, after the live entries */
    Snapshot o = base_snapshot();
    o.card.output_labels = {"Demo Speaker", "XWF Speaker", "Default output"};
    o.card.output_devices = {"hw:CARD=Demo,DEV=0", "bt:9E:0C:81:E6:5D:1A", "default"};
    o.card.outputs = {alsa_out("Demo Speaker", "hw:CARD=Demo,DEV=0"), bt_out("XWF Speaker", "speaker", "unknown", false, false, reason),
                      alsa_out("Default output", "default")};
    o.card.output_selected = 0;
    vm = map(o);
    const ui::SettingRow *out = find_row(vm, "output");
    CHECK(out != nullptr);
    if (out && out->choice_entries.size() == 3) {
        CHECK(out->choice_entries[0].compatible);
        CHECK(!out->choice_entries[1].compatible);
        CHECK_EQ(out->choice_entries[1].detail, reason);
        CHECK_EQ(out->choice_entries[1].size, "");
    } else {
        CHECK(false);
    }
    /* the footer follows bt_status when no Bluetooth input is listed */
    Snapshot n = base_snapshot();
    n.card.bt_status = "Bluetooth audio is not set up";
    CHECK_EQ(find_row(map(n), "input")->formats_footer, "Bluetooth audio is not set up on this board");
    n.card.bt_status = "The sound server is not running";
    CHECK_EQ(find_row(map(n), "input")->formats_footer, "The sound server is not running");
    n.card.bt_status = "";
    CHECK_EQ(find_row(map(n), "input")->formats_footer, "");
    /* a listed Bluetooth entry wins over the status */
    s.card.bt_status = "The sound server is not running";
    CHECK_EQ(find_row(map(s), "input")->formats_footer, "Bluetooth headsets record speech quality (16 kHz)");
}

} // namespace

void test_bt_map()
{
    input_picker();
    output_picker();
    monitor_rows();
    recorder();
    call_quality_watch();
    binder_bt();
    missing_and_held();
    launcher_facts_rows();
}
