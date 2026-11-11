/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the synthetic demonstration model and its driver (see demo.hpp).
 */
#include "demo.hpp"

#include "ui_format.hpp"
#include "wave_geom.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cassette {
namespace ui {

namespace {

void log_action(const std::string &what)
{
    std::fprintf(stderr, "[cassette-demo] %s\n", what.c_str());
    std::fflush(stderr);
}

/* the boards' synthetic wave: one value per block index */
float wave_at(long i, long seed)
{
    const double k = static_cast<double>(i + seed);
    const double a = 0.30 + 0.50 * std::fabs(std::sin(k * 0.37)) * std::fabs(std::sin(k * 0.083 + 1)) +
                     0.18 * (static_cast<double>(((i + seed) * 7919) % 13) / 13.0);
    return static_cast<float>(std::min(1.0, a));
}

/* CASSETTE_DEMO_TRIM_QUIET=<peak 0..1>: the trim page's demo take is quiet, its loudest peak at that level (0.12 = -18 dBFS):
 * the factor the synthetic wave is multiplied by (1 when not set); CASSETTE_DEMO_WAVE_FIT=1: the saved waveform scale is FIT */
float quiet_factor()
{
    const char *q = std::getenv("CASSETTE_DEMO_TRIM_QUIET");
    if (!q || !*q) return 1.0f;
    float top = 0.0f;
    for (int i = 0; i < 110; ++i) top = std::max(top, wave_at(i, 0));
    const float want = std::clamp(static_cast<float>(std::atof(q)), 0.0f, 1.0f);
    return top > 0.0f ? want / top : 1.0f;
}

void scale_all(std::vector<float> &v)
{
    const float k = quiet_factor();
    if (k != 1.0f)
        for (float &x : v) x *= k;
}

SettingRow choice(const char *id, const char *label, std::vector<std::string> choices, int pick)
{
    SettingRow r;
    r.id = id;
    r.label = label;
    r.kind = SettingKind::Choice;
    r.choices = std::move(choices);
    r.choice = pick;
    r.value = r.choices[static_cast<std::size_t>(pick)];
    return r;
}

SettingRow toggle(const char *id, const char *label, bool on)
{
    SettingRow r;
    r.id = id;
    r.label = label;
    r.kind = SettingKind::Toggle;
    r.on = on;
    return r;
}

TakeRow take(const char *sticker, const char *detail, std::int64_t ms)
{
    TakeRow t;
    t.sticker = sticker;
    t.id = std::string("take_") + sticker;
    t.name = std::string("Take ") + sticker;
    t.detail = detail;
    t.length_ms = ms;
    t.prefix = "take";
    return t;
}

/* What follows from the transport alone: the hubs' speed and the PLAY/PAUSE key's latch (ui_format.hpp). */
void transport_looks(RecorderVm &r)
{
    r.hub_dps[0] = r.hub_dps[1] = hub_speed_dps(r.transport);
    r.keys[static_cast<int>(KeyId::PlayPause)].latched = play_key_look(r.transport).latched;
}

/* A take playing: the played level on both bars (a mono take drives L and R equally), no PEAK lamp. */
void meter_played(RecorderVm &r, int bar, int hold)
{
    r.meter.channels = 2;
    for (ChannelMeter &c : r.meter.ch) {
        c.bar = bar;
        c.hold_bar = hold;
        c.peak_lamp = false;
    }
}

void keys_recording(RecorderVm &r)
{
    r.keys[static_cast<int>(KeyId::Rew)].enabled = false;
    r.keys[static_cast<int>(KeyId::Fwd)].enabled = false;
    r.keys[static_cast<int>(KeyId::Rec)].latched = true;
}

/* The MON lamp as the controller would show it: drawn with the Monitor rows (a card with a playback side), lit while the
 * switch is on and nothing plays (playback, scanning and a list preview suspend the monitor). */
void monitor_looks(ViewModel &vm)
{
    RecorderVm &r = vm.rec;
    const SettingRow *sw = nullptr;
    for (const SettingRow &row : vm.settings.rows)
        if (row.id == "monitor") sw = &row;
    const Transport t = r.transport;
    const bool playing = t == Transport::Playing || t == Transport::PlayingPaused || t == Transport::ScanReverse ||
                         t == Transport::ScanForward;
    r.monitor_shown = sw != nullptr;
    r.monitor_active = sw && sw->on && !playing && vm.list.previewing < 0;
}

/* MERGE TAKES in the demo: each synthetic take's codec and rate (the folder's takes are WAV 48 kHz but Take 005, a FLAC take at
 * 44.1 kHz, and Take 003, a WAV take at 44.1 kHz) and its size in MB (the detail lines' figures) */
std::string demo_format(const TakeRow &t)
{
    if (t.sticker == "005") return "FLAC 44.1 kHz";
    if (t.sticker == "003") return "WAV 44.1 kHz";
    return "WAV 48 kHz";
}
int demo_mb(const TakeRow &t)
{
    static const struct {
        const char *sticker;
        int mb;
    } sizes[] = {{"008", 90}, {"007", 4}, {"006", 69}, {"005", 17}, {"004", 4}, {"003", 357}};
    for (const auto &x : sizes)
        if (t.sticker == x.sticker) return x.mb;
    return 1;
}
/* Why row i cannot join a merge whose first take is row `first` (-1: none chosen yet): the merge rule's texts. */
std::string demo_reason(const ListVm &l, int first, int i)
{
    if (first < 0 || first == i) return "";
    const std::string a = demo_format(l.takes[static_cast<std::size_t>(first)]), b = demo_format(l.takes[static_cast<std::size_t>(i)]);
    if (a == b) return "";
    if (a.substr(0, a.find(' ')) != b.substr(0, b.find(' '))) return "Different format: " + b;
    return "Different rate";
}
const char *const kMergeName = "take_008_20261111-1502.wav";
/* The list in merge mode for the chosen rows (in the order chosen): the badges' numbers, the dimmed rows, the strip. */
void demo_apply_merge(ListVm &l, const std::vector<int> &sel)
{
    const int first = sel.empty() ? -1 : sel.front();
    std::int64_t ms = 0;
    int mb = 0;
    for (std::size_t i = 0; i < l.takes.size(); ++i) {
        TakeRow &t = l.takes[i];
        const auto it = std::find(sel.begin(), sel.end(), static_cast<int>(i));
        t.merge_order = it == sel.end() ? 0 : static_cast<int>(it - sel.begin()) + 1;
        t.merge_reason = t.merge_order ? std::string() : demo_reason(l, first, static_cast<int>(i));
        t.merge_ok = t.merge_reason.empty();
        if (t.merge_order) {
            ms += t.length_ms;
            mb += demo_mb(t);
        }
    }
    const std::string dot = " \xC2\xB7 ";
    l.merge_strip = "Merge" + dot + std::to_string(sel.size()) + (sel.size() == 1 ? " take" : " takes");
    if (!sel.empty()) l.merge_strip += dot + fmt_mmss(ms) + dot + "about " + std::to_string(mb) + " MB";
    l.merge_ready = sel.size() >= 2;
}
void demo_merge_box(ViewModel &vm, const std::vector<int> &sel)
{
    std::int64_t ms = 0;
    int mb = 0;
    for (int i : sel) {
        ms += vm.list.takes[static_cast<std::size_t>(i)].length_ms;
        mb += demo_mb(vm.list.takes[static_cast<std::size_t>(i)]);
    }
    vm.merge_box.shown = true;
    vm.merge_box.title = "Merge " + std::to_string(sel.size()) + " takes?";
    vm.merge_box.name = kMergeName;
    vm.merge_box.detail = fmt_mmss(ms) + " \xC2\xB7 about " + std::to_string(mb) + " MB";
}
void demo_progress(ViewModel &vm, std::size_t n)
{
    vm.progress.shown = true;
    vm.progress.title = "Merging... 37%";
    vm.progress.body = std::to_string(n) + " takes into " + kMergeName;
}

/* CASSETTE_DEMO_BT (demo.hpp): the Bluetooth states, built with the same text helpers as the real mapping (ui_format.hpp). */
BtDeviceFacts bt_dev(const char *label, const char *kind, std::uint32_t rate, const char *profile, bool connected, bool usable,
                     const char *reason)
{
    BtDeviceFacts f;
    f.label = label;
    f.kind = kind;
    f.rate = rate;
    f.hint_profile = profile;
    f.connected = connected;
    f.usable = usable;
    f.reason = reason;
    return f;
}

FormatEntry plain_entry(const std::string &label)
{
    FormatEntry e;
    e.name = label;
    return e;
}

SettingRow *find_row(SettingsVm &s, const char *id)
{
    for (SettingRow &r : s.rows)
        if (r.id == id) return &r;
    return nullptr;
}

void erase_row(SettingsVm &s, const char *id)
{
    s.rows.erase(std::remove_if(s.rows.begin(), s.rows.end(), [id](const SettingRow &r) { return r.id == id; }), s.rows.end());
}

/* rows inserted after the row `after` */
void insert_after(SettingsVm &s, const char *after, const std::vector<SettingRow> &rows)
{
    auto it = std::find_if(s.rows.begin(), s.rows.end(), [after](const SettingRow &r) { return r.id == after; });
    if (it != s.rows.end()) ++it;
    s.rows.insert(it, rows.begin(), rows.end());
}

/* the headset as the input: no hardware level (the row greyed), no Auto gain, the meter label BT */
void bt_input_rows(ViewModel &vm, std::vector<std::string> labels, std::vector<FormatEntry> entries, int pick, bool footer)
{
    SettingsVm &s = vm.settings;
    if (SettingRow *in = find_row(s, "input")) {
        in->choices = std::move(labels);
        in->choice = pick;
        in->value = in->choices[static_cast<std::size_t>(pick)];
        in->choice_entries = std::move(entries);
        if (footer) in->formats_footer = bt_input_footer(true, false);
    }
    erase_row(s, "agc");
    erase_row(s, "mute");
    SettingRow lvl;
    lvl.id = "level";
    lvl.label = "Input level";
    lvl.kind = SettingKind::Info;
    lvl.enabled = false;
    lvl.value = "Not applicable";
    if (SettingRow *old = find_row(s, "level")) *old = lvl;
    else insert_after(s, "input", {lvl});
    vm.rec.bluetooth = true;
    vm.rec.card.shown = false;
}

const char *const kEvolve = "Evolve 65";
const char *const kSpeaker = "XWF-M18 Speaker";

/* Monitor, Monitor level (and its delay line), Monitor to (the picker's choices), after Limiter */
void bt_monitor_rows(ViewModel &vm, bool with_switch, const std::string &detail, bool warn, int pick, bool available,
                     const std::string &reason)
{
    SettingsVm &s = vm.settings;
    erase_row(s, "monitor");
    erase_row(s, "monitor_level");
    std::vector<SettingRow> rows;
    if (with_switch) {
        SettingRow sw = toggle("monitor", "Monitor", true);
        sw.detail = detail;
        sw.detail_warn = warn;
        rows.push_back(sw);
        SettingRow ml;
        ml.id = "monitor_level";
        ml.label = "Monitor level";
        ml.kind = SettingKind::Slider;
        ml.min = 0;
        ml.max = 20;
        ml.pos = 10;
        ml.value_mul = 5;
        ml.value_suffix = " %";
        ml.value_text = "50 %";
        rows.push_back(ml);
    }
    SettingRow to = choice("monitor_to", "Monitor to", {"Automatic", "USB Audio Device", kEvolve, kSpeaker}, pick);
    to.choice_entries = {plain_entry("Automatic"), plain_entry("USB Audio Device"),
                         bt_output_entry(bt_dev(kEvolve, "headset", 16000, "call", true, true, "")),
                         bt_output_entry(bt_dev(kSpeaker, "speaker", 48000, "music", true, true, ""))};
    if (pick == 0) to.value = "Automatic";
    if (!available) {
        to.value = reason;
        to.dimmed = true;
    }
    rows.push_back(to);
    insert_after(s, "limiter", rows);
}

void apply_bt_demo(ViewModel &vm, const std::string &st)
{
    RecorderVm &r = vm.rec;
    const std::vector<std::string> usb_and_bt = {"USB Audio Device", kEvolve, "Beats Fit Pro",
                                                 "WH-1000XM4 Wireless Noise Cancelling Headphones"};
    const std::vector<FormatEntry> bt_entries = {
        plain_entry("USB Audio Device"), bt_input_entry(bt_dev(kEvolve, "headset", 16000, "call", true, true, "")),
        bt_input_entry(bt_dev("Beats Fit Pro", "headset", 16000, "music", true, false, "The headset microphone is off")),
        bt_input_entry(bt_dev("WH-1000XM4 Wireless Noise Cancelling Headphones", "other", 0, "", false, false, "Not connected"))};
    if (st == "input") {
        bt_input_rows(vm, usb_and_bt, bt_entries, 3, true);
    } else if (st == "notsetup") {
        if (SettingRow *in = find_row(vm.settings, "input")) in->formats_footer = bt_input_footer(false, true);
    } else if (st == "output") {
        SettingsVm &s = vm.settings;
        erase_row(s, "output");
        SettingRow out = choice("output", "Output", {"USB Audio Device", kSpeaker, kEvolve, "Beats Fit Pro", "Default output"}, 1);
        out.choice_entries = {plain_entry("USB Audio Device"),
                              bt_output_entry(bt_dev(kSpeaker, "speaker", 48000, "music", true, true, "")),
                              bt_output_entry(bt_dev(kEvolve, "headset", 16000, "call", true, true, "")),
                              bt_output_entry(bt_dev("Beats Fit Pro", "other", 0, "", false, false, "Not connected")),
                              plain_entry("Default output")};
        s.rows.push_back(out);
    } else if (st == "monitor") {
        bt_input_rows(vm, usb_and_bt, bt_entries, 1, true);
        bt_monitor_rows(vm, true, monitor_delay_text(90, false), false, 0, true, "");
    } else if (st == "echo") {
        bt_monitor_rows(vm, true, monitor_delay_text(250, true), true, 3, true, "");
    } else if (st == "monitor-none") {
        bt_monitor_rows(vm, false, "", false, 0, false, "The sound card has no playback side");
    } else if (st == "rec" || st == "connecting" || st == "nosound" || st == "gone") {
        bt_input_rows(vm, usb_and_bt, bt_entries, 1, true);
        r.info.codec = r.tape.format = "WAV 16k";
        r.info.format = "16k/16 WAV";
        if (st == "connecting") {
            r.standing_notice = kBtConnectingNotice;
            r.meter.ch[0] = ChannelMeter(); /* nothing real yet: the segments dark */
        }
        if (st == "nosound") {
            r.standing_notice = kBtNoSoundNotice;
            r.meter.ch[0] = ChannelMeter();
        }
        if (st == "gone") vm.notice = "The Bluetooth headset disconnected: the take was closed and kept.";
    } else if (st == "switching" || st == "callq") {
        r.bluetooth = false; /* the input is the USB card; the output is the headset */
        if (st == "switching") {
            r.info.play_status = switching_text(1.3, 4.0);
            meter_played(r, 0, 0); /* nothing plays yet */
            r.info.play_pos_ms = 0;
            r.counter_ms = 0;
        } else {
            vm.notice = kCallQualityNotice;
        }
    }
}

} // namespace

std::vector<float> demo_window_peaks(std::int64_t length_ms, std::int64_t marker_ms, std::int64_t span_ms, int n)
{
    std::vector<float> out(static_cast<std::size_t>(std::max(0, n)), 0.0f);
    if (n <= 0 || span_ms <= 0) return out;
    const double block = static_cast<double>(span_ms) / 110.0;
    const std::int64_t start = marker_ms - span_ms / 2;
    for (int i = 0; i < n; ++i) {
        const double t = start + (i + 0.5) * static_cast<double>(span_ms) / n;
        if (t < 0 || t > length_ms) continue;
        out[static_cast<std::size_t>(i)] = wave_at(static_cast<long>(std::floor(t / block)), 40);
    }
    return out;
}

std::vector<float> demo_overview_peaks(std::int64_t length_ms, int n)
{
    (void)length_ms;
    std::vector<float> out(static_cast<std::size_t>(std::max(0, n)));
    for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = wave_at(i * 110 / std::max(1, n), 0);
    return out;
}

ViewModel demo_model(const std::string &scene)
{
    ViewModel vm;
    RecorderVm &r = vm.rec;
    r.tape.label = "Take 007";
    r.tape.format = "WAV 48k";
    r.info.take = "TAKE 007";
    r.info.format = "48k/16 WAV";
    r.info.codec = "WAV 48k";
    r.info.time_left = "12 h 40";
    r.info.limiter = true;
    r.info.low_cut = true;
    r.card.db = 13.0;
    r.card.can_up = true;
    r.soft.db = 6.0;
    r.meter.channels = 1;
    r.meter.ch[0].bar = 11;
    r.meter.ch[0].hold_bar = 12;

    if (scene == "recording" || scene == "nocard" || scene == "stereo" || scene == "long" || scene == "recording-low" ||
        scene == "stereo-full") {
        r.transport = Transport::Recording;
        keys_recording(r);
        r.counter_ms = 42000;
        if (scene == "nocard") r.card.shown = false;
        if (scene == "recording-low") {
            r.meter.ch[0].bar = 4;
            r.meter.ch[0].hold_bar = 6;
        }
        if (scene == "stereo") {
            r.meter.channels = 2;
            r.meter.ch[1].bar = 9;
            r.meter.ch[1].hold_bar = 11;
            r.info.format = "48k/16 WAV stereo";
        }
        if (scene == "stereo-full") { /* both bars at full scale with the hold tick after the last step, PEAK lit */
            r.meter.channels = 2;
            for (ChannelMeter &c : r.meter.ch) {
                c.bar = 15;
                c.hold_bar = 15;
                c.peak_lamp = true;
            }
            r.info.format = "48k/16 WAV stereo";
        }
        if (scene == "long") {
            r.counter_ms = 3600000LL + 2 * 60000 + 3000;
            r.meter.ch[0].peak_lamp = true;
            r.meter.ch[0].bar = 15;
            r.meter.ch[0].hold_bar = 15;
        }
    } else if (scene == "countdown") {
        r.transport = Transport::Countdown;
        keys_recording(r);
        r.keys[static_cast<int>(KeyId::Rec)].latched = false;
        r.countdown_left = 3;
        r.countdown_fraction = 221.7f / 226.2f;
        r.counter_ms = 0;
    } else if (scene == "playing" || scene == "wind" || scene == "fwd" || scene == "play-paused" ||
               scene == "playing-meter") {
        /* playing Take 006 (playing-meter: the played level at -12 dBFS, 10 steps); wind: REW held (5x backwards); fwd:
         * FWD held; play-paused: the take paused (the played level falling away) */
        r.transport = scene == "wind"          ? Transport::ScanReverse
                      : scene == "fwd"         ? Transport::ScanForward
                      : scene == "play-paused" ? Transport::PlayingPaused
                                               : Transport::Playing;
        r.tape.label = "Take 006";
        r.info.take = "TAKE 006";
        r.info.play_pos_ms = 5 * 60000 + 44000;
        r.info.play_len_ms = 12 * 60000 + 8000;
        r.counter_ms = r.info.play_pos_ms;
        if (scene == "wind") r.keys[static_cast<int>(KeyId::Rew)].latched = true;
        if (scene == "fwd") r.keys[static_cast<int>(KeyId::Fwd)].latched = true;
        if (scene == "playing-meter") meter_played(r, bar_steps(-12.0f), bar_steps(-11.0f));
        else if (scene == "play-paused") meter_played(r, 3, 9);
        else meter_played(r, 11, 12);
    } else if (scene == "paused") {
        r.transport = Transport::RecordingPaused;
        keys_recording(r);
        r.keys[static_cast<int>(KeyId::PlayPause)].latched = true;
        r.counter_ms = 83000;
        r.meter.ch[0].bar = 3;
        r.meter.ch[0].hold_bar = 0;
    } else if (scene == "noinput") {
        r.input_present = false;
        r.tape.loaded = false;
        r.card.shown = false;
        r.meter.ch[0].bar = 0;
        r.meter.ch[0].hold_bar = 0;
        r.keys[static_cast<int>(KeyId::Rec)].enabled = false;
        r.info.take = "NO INPUT";
        r.info.format.clear();
        r.info.limiter = false;
        r.info.low_cut = false;
    } else if (scene == "deckempty" || scene == "notakes") { /* idle with no take loaded: the blank cassette, PLAY dimmed */
        r.transport = Transport::Idle;
        r.tape.label.clear();
        r.info.take = "TAKE 008";
        r.keys[static_cast<int>(KeyId::PlayPause)].enabled = false;
        r.meter.ch[0].bar = 2;
        r.meter.ch[0].hold_bar = 0;
    } else { /* idle (also "onetake"): Take 006 is loaded (the list's highlighted row) */
        r.transport = Transport::Idle;
        r.tape.label = "Take 006";
        r.info.take = "TAKE 006";
        r.meter.ch[0].bar = 2;
        r.meter.ch[0].hold_bar = 0;
    }

    transport_looks(r);

    /* settings: the board's rows, a card with a level (8 of 16) and auto gain */
    SettingsVm &s = vm.settings;
    /* CASSETTE_DEMO_SETTINGS=full: also the rows that depend on the card (Mic mute) and on the outputs (Output) */
    const char *ds = std::getenv("CASSETTE_DEMO_SETTINGS");
    const bool full_settings = ds && std::string(ds) == "full";
    /* CASSETTE_DEMO_CARD=<name>: the first capture card's name (a long ALSA card name wraps in the picker) */
    const char *dc = std::getenv("CASSETTE_DEMO_CARD");
    s.rows.push_back(choice("input", "Input", {dc && *dc ? dc : "USB Audio Device", "USB Headset"}, 0));
    if (scene != "nocard" && scene != "noinput") {
        SettingRow lvl;
        lvl.id = "level";
        lvl.label = "Input level";
        lvl.kind = SettingKind::Slider;
        lvl.min = 0;
        lvl.max = 16;
        lvl.pos = 8;
        lvl.value_text = "8 / 16";
        s.rows.push_back(lvl);
        s.rows.push_back(toggle("agc", "Auto gain (card)", true));
        if (full_settings) s.rows.push_back(toggle("mute", "Mic mute (card)", false));
    }
    s.rows.push_back(choice("codec", "Codec", {"WAV", "FLAC", "Opus"}, 0));
    s.rows.push_back(choice("rate", "Sample rate", {"48 kHz", "44.1 kHz"}, 0));
    s.rows.push_back(choice("quality", "Bit depth", {"16-bit", "24-bit"}, 0));
    s.rows.push_back(choice("timer", "Record timer", {"Off", "3 s", "10 s", "20 s"}, 0));
    const char *rf = std::getenv("CASSETTE_DEMO_REWFWD");
    s.rows.push_back(choice("rewfwd", "REW / FWD", {"Fast scan 5x", "Jump 5 s"}, rf && std::string(rf) == "jump" ? 1 : 0));
    {
        SettingRow ch;
        ch.id = "channels";
        ch.label = "Channels";
        ch.kind = SettingKind::Info;
        ch.enabled = false;
        ch.value = "Mono";
        s.rows.push_back(ch);
    }
    s.rows.push_back(toggle("lowcut", "Low cut 80 Hz", false));
    s.rows.push_back(toggle("limiter", "Limiter", false));
    /* CASSETTE_DEMO_MONITOR=on|off: a card with a playback side: the Monitor switch and its level (dimmed while off) */
    if (const char *mo = std::getenv("CASSETTE_DEMO_MONITOR")) {
        const bool on = std::string(mo) == "on";
        s.rows.push_back(toggle("monitor", "Monitor", on));
        SettingRow ml;
        ml.id = "monitor_level";
        ml.label = "Monitor level";
        ml.kind = SettingKind::Slider;
        ml.enabled = on;
        ml.min = 0;
        ml.max = 20;
        ml.pos = 10;
        ml.value_mul = 5;
        ml.value_suffix = " %";
        ml.value_text = "50 %";
        s.rows.push_back(ml);
    }
    s.rows.push_back(choice("sync", "Sync every", {"5 s", "10 s", "20 s", "30 s"}, 1));
    s.rows.push_back(choice("folder", "Folder", {"~/Recordings"}, 0));
    SettingRow prefix;
    prefix.id = "prefix";
    prefix.label = "File prefix";
    prefix.kind = SettingKind::Text;
    prefix.value = "take";
    s.rows.push_back(prefix);
    {
        SettingRow tn;
        tn.id = "takenum";
        tn.label = "Take number";
        tn.kind = SettingKind::Number;
        tn.value = "Next: 007";
        tn.min = 1;
        tn.max = 9999;
        tn.pos = 7;
        s.rows.push_back(tn);
    }
    s.rows.push_back(choice("split", "Auto-split", {"At 4 GiB", "Off (one RF64 file)"}, 0));
    if (full_settings) s.rows.push_back(choice("output", "Output", {"USB Audio Device", "Default output"}, 0));
    if (r.transport == Transport::Recording)
        for (SettingRow &row : s.rows)
            if (row.id == "codec" || row.id == "rate" || row.id == "quality" || row.id == "folder") row.enabled = false;

    /* the recordings: five synthetic takes, Take 006 selected */
    ListVm &l = vm.list;
    l.takes = {take("007", "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB", 42000),
               take("006", "today 11:05 \xC2\xB7 12 min \xC2\xB7 69 MB", 728000),
               take("005", "yesterday 18:20 \xC2\xB7 3 min \xC2\xB7 17 MB", 201000),
               take("004", "nodate \xC2\xB7 41 s \xC2\xB7 4 MB", 41000),
               take("003", "2 Nov \xC2\xB7 1 h 02 \xC2\xB7 357 MB", 3720000)};
    l.selected = 1;
    /* CASSETTE_DEMO_LIST_TAGS=1: the format tag the real list adds at the end of the detail line (a folder can hold all three) */
    if (const char *lt = std::getenv("CASSETTE_DEMO_LIST_TAGS"); lt && *lt && std::string(lt) != "0") {
        static const char *const tags[] = {"FLAC", "WAV", "OPUS", "WAV", "FLAC"};
        for (size_t i = 0; i < l.takes.size() && i < 5; ++i) l.takes[i].detail += std::string(" \xC2\xB7 ") + tags[i];
    }
    /* CASSETTE_DEMO_LIST_LONG: take names that wrap (a name is up to 24 characters of prefix, spaces allowed, plus the number).
     * 1: three long names (24-character prefixes with spaces, a 27-character word with no space), Take 006 -> the second row
     * selected; 2: one long row (the third, selected) among normal ones; 3: every name long, the first row selected (the scene steps down to the last) */
    if (const char *ll = std::getenv("CASSETTE_DEMO_LIST_LONG"); ll && *ll && std::string(ll) != "0") {
        const std::string mode = ll;
        const char *long_names[] = {"Field recording of birds 006", "Interview with the mayor 005", "ABCDEFGHIJKLMNOPQRSTUVWX004"};
        if (mode == "2") {
            l.takes[2].name = long_names[1];
            l.selected = 2;
        } else {
            l.takes[1].name = long_names[0];
            l.takes[2].name = long_names[1];
            l.takes[3].name = long_names[2];
            if (mode == "3") {
                l.takes[0].name = "Morning walk with the dog 007";
                l.takes[4].name = "Lecture on naval history 003";
                l.selected = 0;
            }
        }
    }
    /* the shorter forms of a narrow list: without the size, then the date shortest (as the real mapping builds them) */
    for (TakeRow &t : l.takes) {
        const std::string dot = " · ";
        std::vector<std::string> parts;
        for (size_t at = 0, e; at <= t.detail.size(); at = e + dot.size()) {
            e = t.detail.find(dot, at);
            if (e == std::string::npos) e = t.detail.size();
            parts.push_back(t.detail.substr(at, e - at));
            if (e == t.detail.size()) break;
        }
        if (parts.size() < 3) continue;
        parts.erase(parts.begin() + 2);
        auto joined = [&] { std::string o; for (size_t i = 0; i < parts.size(); ++i) o += (i ? dot : "") + parts[i]; return o; };
        t.detail_short.push_back(joined());
        if (parts[0].rfind("today ", 0) == 0) parts[0] = parts[0].substr(6);
        else if (parts[0].rfind("yesterday ", 0) == 0) parts[0] = "yest " + parts[0].substr(10);
        t.detail_short.push_back(joined());
    }
    if (scene == "notakes") {
        l.takes.clear();
        l.selected = 0;
    } else if (scene == "onetake") { /* only Take 006: deleting it empties the deck */
        l.takes = {l.takes[1]};
        l.selected = 0;
    }
    l.can_load = r.transport != Transport::Recording && r.transport != Transport::RecordingPaused &&
                 r.transport != Transport::Countdown;
    l.take_count = scene == "notakes" ? 0 : scene == "onetake" ? 1 : 12;
    l.folder = "~/Recordings";
    l.free_space = "41 GB";
    l.free_hours = "117 h";
    l.format_short = "48k/16";
    l.can_merge = l.takes.size() >= 2;
    /* CASSETTE_DEMO_MERGE=0|1|3|box|confirm|progress|done: merge mode with nothing chosen, Take 006 chosen, three takes chosen
     * (006, 007, 004), then the merge box, the confirm before deleting the originals, the progress box; done: the list after
     * a merge (the new Take 008 on top, highlighted, the notice) */
    if (const char *mg = std::getenv("CASSETTE_DEMO_MERGE"); mg && *mg && l.takes.size() >= 5) {
        const std::string m = mg;
        if (m == "done") {
            TakeRow t8 = take("008", "today 15:02 \xC2\xB7 16 min \xC2\xB7 90 MB", 971000);
            t8.detail_short = {"today 15:02 \xC2\xB7 16 min", "15:02 \xC2\xB7 16 min"};
            l.takes.insert(l.takes.begin(), t8);
            l.selected = 0;
            l.take_count = 13;
            vm.notice = "Merged 3 takes into Take 008";
        } else {
            l.merge_mode = true;
            std::vector<int> sel;
            if (m == "1") sel = {1};
            else if (m != "0") sel = {1, 0, 3};
            demo_apply_merge(l, sel);
            if (m == "box" || m == "confirm") demo_merge_box(vm, sel);
            if (m == "progress") demo_progress(vm, sel.size());
        }
    }

    /* the trim page: Take 006, a 60 s window at 5:44.2, two splits (the second selected) */
    TrimVm &t = vm.trim;
    t.take_id = "take_006";
    t.sticker = "006";
    t.take = "TAKE 006";
    t.length_ms = 728000;
    t.marker_ms = 344200;
    t.span_ms = 60000;
    t.in_ms = 178700;
    t.out_ms = 628700;
    const char *tm = std::getenv("CASSETTE_DEMO_TRIM");
    const std::string trim_mode = tm ? tm : "";
    if (trim_mode != "none") {
        t.splits = {321154, 365063};
        t.selected_split = trim_mode == "unselected" ? -1 : 1;
    }
    /* CASSETTE_DEMO_TRIM_IN / _OUT=<ms>: the handles elsewhere; CASSETTE_DEMO_TRIM_SPLITS=<ms,ms,...> the splits,
     * CASSETTE_DEMO_TRIM_SELECTED=<index, -1 none> the selected one (scenes for the tags above the lines) */
    if (const char *v = std::getenv("CASSETTE_DEMO_TRIM_IN"); v && *v) t.in_ms = std::atoll(v);
    if (const char *v = std::getenv("CASSETTE_DEMO_TRIM_OUT"); v && *v) t.out_ms = std::atoll(v);
    if (const char *v = std::getenv("CASSETTE_DEMO_TRIM_SPLITS"); v && *v) {
        t.splits.clear();
        for (const char *c = v; *c;) {
            char *end = nullptr;
            const long long ms = std::strtoll(c, &end, 10);
            if (end == c) break;
            t.splits.push_back(ms);
            c = *end == ',' ? end + 1 : end;
        }
        t.selected_split = -1;
    }
    if (const char *v = std::getenv("CASSETTE_DEMO_TRIM_SELECTED"); v && *v) t.selected_split = std::atoi(v);
    /* CASSETTE_DEMO_TRIM_MARKER=<ms>: the marker elsewhere (scenes for the ruler at the take's start and end) */
    if (const char *v = std::getenv("CASSETTE_DEMO_TRIM_MARKER"); v && *v)
        t.marker_ms = std::clamp<std::int64_t>(std::atoll(v), 0, t.length_ms);
    t.window_peaks = demo_window_peaks(t.length_ms, t.marker_ms, t.span_ms, 220);
    t.overview_peaks = demo_overview_peaks(t.length_ms, 110);
    scale_all(t.window_peaks);
    scale_all(t.overview_peaks);
    {
        const char *wf = std::getenv("CASSETTE_DEMO_WAVE_FIT");
        t.fit = wf && *wf && std::string(wf) != "0";
        float top = 0.0f;
        for (float v : t.overview_peaks) top = std::max(top, v);
        t.fit_gain = fit_gain_for(top);
    }

    vm.save.files = {{"take_006_20261108-1105-1.wav", 142300}, {"take_006_20261108-1105-2.wav", 44000},
                     {"take_006_20261108-1105-3.wav", 263700}};
    vm.confirm.title = "Delete the original take?";
    vm.confirm.body = "It is deleted only after every new file is written and synced.";
    vm.confirm.yes = "DELETE";
    if (const char *mg = std::getenv("CASSETTE_DEMO_MERGE"); mg && std::string(mg) == "confirm") {
        vm.confirm.title = "Delete the originals?";
        vm.confirm.body = "They are deleted only after the new file is written and synced.";
        vm.confirm.shown = true;
    }
    /* CASSETTE_DEMO_NOTICE=<text>: a notice that stays until a tap dismisses it (the narrow recorder) or the scene ends */
    if (const char *n = std::getenv("CASSETTE_DEMO_NOTICE")) vm.notice = n;
    /* CASSETTE_DEMO_LIM / CASSETTE_DEMO_LOWCUT = on|off: the LIM and LOW CUT lamps (lit: the function is on in Settings) */
    if (const char *v = std::getenv("CASSETTE_DEMO_LIM"); v && *v) vm.rec.info.limiter = std::string(v) == "on";
    if (const char *v = std::getenv("CASSETTE_DEMO_LOWCUT"); v && *v) vm.rec.info.low_cut = std::string(v) == "on";
    /* CASSETTE_DEMO_SERVER=1: the capture path is the sound server (idle: the card label leads the strip; recording: "(server)"
     * after the format); CASSETTE_DEMO_LOST_MS=<ms>: audio lost so far in the take (the red "lost 0.3 s", while recording) */
    if (const char *sv = std::getenv("CASSETTE_DEMO_SERVER")) {
        vm.rec.info.server = std::string(sv) == "1";
        if (vm.rec.info.server && vm.rec.transport == Transport::Idle) vm.rec.info.card = "USB AUDIO CODEC";
    }
    if (const char *lm = std::getenv("CASSETTE_DEMO_LOST_MS")) vm.rec.info.lost_ms = std::atoll(lm);
    /* CASSETTE_DEMO_CODEC=wav|flac|opus[:<kbps>]: the codec of the recording choice (the info box's first line, Opus with its bit
     * rate, default 32) and of the take in the deck; CASSETTE_DEMO_NAME=<text>: the name on the cassette label only (a long
     * one, up to 24 characters with spaces, wraps to two lines; the landscape strip keeps its "TAKE 007") */
    if (const char *c = std::getenv("CASSETTE_DEMO_CODEC")) {
        const std::string s = c;
        const std::string tag = s.compare(0, 4, "opus") == 0 ? "OPUS" : s.compare(0, 4, "flac") == 0 ? "FLAC" : "WAV";
        vm.rec.info.codec = vm.rec.tape.format = tag + " 48k";
        vm.rec.info.format = std::string("48k/16 ") + tag;
        const std::size_t colon = s.find(':');
        vm.rec.info.kbps = tag == "OPUS" ? (colon == std::string::npos ? 32 : std::atoi(s.c_str() + colon + 1)) : 0;
    }
    if (const char *n = std::getenv("CASSETTE_DEMO_NAME"); n && *n && !vm.rec.tape.label.empty()) {
        vm.rec.tape.label = n;
    }
    /* CASSETTE_DEMO_LEFT=<text>: the time left ("955 h": the box adds " left") */
    if (const char *l = std::getenv("CASSETTE_DEMO_LEFT"); l && *l) vm.rec.info.time_left = l;
    if (const char *b = std::getenv("CASSETTE_DEMO_BOX")) {
        if (std::string(b) == "save") vm.save.shown = true;
        if (std::string(b) == "confirm") vm.confirm.shown = true;
    }
    if (const char *bt = std::getenv("CASSETTE_DEMO_BT"); bt && *bt) apply_bt_demo(vm, bt);
    monitor_looks(vm);
    return vm;
}

void DemoDriver::set_transport(Transport t)
{
    RecorderVm &r = vm_.rec;
    r.transport = t;
    const bool rec = t == Transport::Recording || t == Transport::RecordingPaused || t == Transport::Countdown;
    vm_.list.can_load = !rec;
    r.keys[static_cast<int>(KeyId::Rew)].enabled = !rec;
    r.keys[static_cast<int>(KeyId::Fwd)].enabled = !rec;
    r.keys[static_cast<int>(KeyId::Rec)].latched = t == Transport::Recording || t == Transport::RecordingPaused;
    transport_looks(r);
    r.keys[static_cast<int>(KeyId::Rew)].latched = t == Transport::ScanReverse;
    r.keys[static_cast<int>(KeyId::Fwd)].latched = t == Transport::ScanForward;
    monitor_looks(vm_);
}

/* The cassette and the strip name list row `i`; the counter at its start. */
void DemoDriver::show_take(int i)
{
    if (i < 0 || i >= static_cast<int>(vm_.list.takes.size())) return;
    const TakeRow &t = vm_.list.takes[static_cast<std::size_t>(i)];
    RecorderVm &r = vm_.rec;
    r.tape.label = t.name;
    r.info.take = "TAKE " + t.sticker;
    r.info.play_pos_ms = 0;
    r.info.play_len_ms = t.length_ms;
    r.counter_ms = 0;
}

/* Ends a list preview: nothing plays, the loaded take is in the deck again. */
void DemoDriver::stop_preview()
{
    if (previewing_ < 0) return;
    previewing_ = -1;
    vm_.list.previewing = -1;
    set_transport(Transport::Idle);
    show_take(loaded_);

}

void DemoDriver::deck(KeyId k, KeyEdge e)
{
    RecorderVm &r = vm_.rec;
    const Transport t = r.transport;
    const bool playing = t == Transport::Playing || t == Transport::PlayingPaused;
    const bool jump = std::any_of(vm_.settings.rows.begin(), vm_.settings.rows.end(),
                                  [](const SettingRow &row) { return row.id == "rewfwd" && row.choice == 1; });
    if ((k == KeyId::Rew || k == KeyId::Fwd) && playing && !jump && e == KeyEdge::Down) {
        before_scan_ = t;
        set_transport(k == KeyId::Rew ? Transport::ScanReverse : Transport::ScanForward);
    } else if ((k == KeyId::Rew || k == KeyId::Fwd) && !jump && e == KeyEdge::Up &&
               (t == Transport::ScanReverse || t == Transport::ScanForward)) {
        set_transport(before_scan_);
    } else if ((k == KeyId::Rew || k == KeyId::Fwd) && jump && playing && e == KeyEdge::Click) {
        const std::int64_t d = k == KeyId::Rew ? -5000 : 5000;
        r.info.play_pos_ms = std::clamp<std::int64_t>(r.info.play_pos_ms + d, 0, r.info.play_len_ms);
        r.counter_ms = r.info.play_pos_ms;
    } else if (e != KeyEdge::Click) {
        return;
    } else if (k == KeyId::Rec && (t == Transport::Idle || playing)) {
        set_transport(Transport::Recording);
        r.tape.label = "Take 008";
        r.info.take = "TAKE 008";
        r.counter_ms = 0;
    } else if (k == KeyId::PlayPause) {
        if (t == Transport::Idle && loaded_ < 0) return; /* nothing in the deck */
        if (t == Transport::Idle) {
            set_transport(Transport::Playing);
            show_take(loaded_);
            if (loaded_ == 1) { /* Take 006 plays from 05:44 in the scenes */
                r.info.play_pos_ms = 5 * 60000 + 44000;
                r.counter_ms = r.info.play_pos_ms;
            }
        } else if (t == Transport::Playing) set_transport(Transport::PlayingPaused);
        else if (t == Transport::PlayingPaused) set_transport(Transport::Playing);
        else if (t == Transport::Recording) set_transport(Transport::RecordingPaused);
        else if (t == Transport::RecordingPaused) set_transport(Transport::Recording);
        else return;
    } else if (k == KeyId::Stop && t != Transport::Idle) {
        set_transport(Transport::Idle);
    } else {
        return;
    }
    emit();
}

void DemoDriver::tick_live(std::uint32_t elapsed_ms)
{
    RecorderVm &r = vm_.rec;
    const double t = elapsed_ms / 1000.0;
    /* a speech-like level between -30 and -8 dBFS, the hold following its peaks */
    const double db = -19.0 + 9.0 * std::sin(t * 2.3) * std::fabs(std::sin(t * 0.7 + 0.4)) + 2.0 * std::sin(t * 11.0);
    for (int c = 0; c < r.meter.channels; ++c) {
        ChannelMeter &ch = r.meter.ch[c];
        ch.bar = bar_steps(static_cast<float>(db - 2.0 * c));
        ch.hold_bar = std::max(ch.bar, ch.hold_bar - ((elapsed_ms / 33) % 15 == 0 ? 1 : 0));
    }
    if (r.transport == Transport::Recording || r.transport == Transport::Playing) r.counter_ms += 33;
    emit();
}

/* Merge mode: row i chosen next, or dropped (the later ones move up); a row that cannot join says why. */
void DemoDriver::merge_toggle(int i)
{
    ListVm &l = vm_.list;
    if (!l.merge_mode || vm_.merge_box.shown || i < 0 || i >= static_cast<int>(l.takes.size())) return;
    const auto it = std::find(merge_sel_.begin(), merge_sel_.end(), i);
    if (it != merge_sel_.end()) {
        merge_sel_.erase(it);
    } else {
        const std::string why = demo_reason(l, merge_sel_.empty() ? -1 : merge_sel_.front(), i);
        if (!why.empty()) vm_.notice = "Not available: " + why;
        else merge_sel_.push_back(i);
    }
    demo_apply_merge(l, merge_sel_);
    emit();
}

void DemoDriver::emit()
{
    if (changed) changed(vm_);
}

void DemoDriver::refresh_trim()
{
    TrimVm &t = vm_.trim;
    t.marker_ms = std::clamp<std::int64_t>(t.marker_ms, 0, t.length_ms);
    t.window_peaks = demo_window_peaks(t.length_ms, t.marker_ms, t.span_ms, 220);
    scale_all(t.window_peaks);
}

UiActions DemoDriver::actions()
{
    UiActions a;
    static const char *const keys[5] = {"REW", "PLAY/PAUSE", "FWD", "STOP", "REC"};
    static const char *const edges[3] = {"down", "up", "click"};
    a.deck_key = [this](KeyId k, KeyEdge e) {
        log_action(std::string("deck_key ") + keys[static_cast<int>(k)] + " " + edges[static_cast<int>(e)]);
        deck(k, e);
    };
    a.countdown_cancel = [this] {
        log_action("countdown_cancel");
        vm_.rec.transport = Transport::Idle;
        vm_.rec.keys[static_cast<int>(KeyId::Rew)].enabled = true;
        vm_.rec.keys[static_cast<int>(KeyId::Fwd)].enabled = true;
        emit();
    };
    a.card_gain = [this](int d) {
        log_action("card_gain " + std::to_string(d));
        vm_.rec.card.db = std::clamp(vm_.rec.card.db + d, 0.0, 23.8);
        emit();
    };
    a.soft_gain = [this](int d) {
        log_action("soft_gain " + std::to_string(d));
        vm_.rec.soft.db = std::clamp(vm_.rec.soft.db + d, 0.0, 24.0);
        emit();
    };
    a.notice_dismiss = [this] {
        log_action("notice_dismiss");
        vm_.notice.clear();
        emit();
    };
    a.open = [this](Screen s) {
        static const char *const n[4] = {"recorder", "settings", "list", "trim"};
        log_action(std::string("open ") + n[static_cast<int>(s)]);
        if (s != Screen::List && previewing_ >= 0) { /* closing the list ends its preview */
            stop_preview();
            emit();
        }
    };
    a.setting_choice = [this](const std::string &id, int c) {
        log_action("setting_choice " + id + " " + std::to_string(c));
        for (SettingRow &r : vm_.settings.rows)
            if (r.id == id && c >= 0 && c < static_cast<int>(r.choices.size())) {
                r.choice = c;
                r.value = r.choices[static_cast<std::size_t>(c)];
            }
        emit();
    };
    a.setting_toggle = [this](const std::string &id, bool on) {
        log_action("setting_toggle " + id + (on ? " on" : " off"));
        for (SettingRow &r : vm_.settings.rows)
            if (r.id == id) r.on = on;
        if (id == "lowcut") vm_.rec.info.low_cut = on;
        if (id == "limiter") vm_.rec.info.limiter = on;
        if (id == "monitor")
            for (SettingRow &r : vm_.settings.rows)
                if (r.id == "monitor_level") r.enabled = on;
        monitor_looks(vm_);
        emit();
    };
    a.setting_slider = [this](const std::string &id, int pos) {
        log_action("setting_slider " + id + " " + std::to_string(pos));
        for (SettingRow &r : vm_.settings.rows)
            if (r.id == id) {
                r.pos = pos;
                r.value_text = r.value_mul > 0 ? std::to_string(pos * r.value_mul) + r.value_suffix
                                               : std::to_string(pos) + " / " + std::to_string(r.max);
            }
        emit();
    };
    a.setting_text = [this](const std::string &id, const std::string &text) {
        log_action("setting_text " + id + " " + text);
        for (SettingRow &r : vm_.settings.rows)
            if (r.id == id) r.value = text;
        emit();
    };
    a.setting_number = [this](const std::string &id, int n) {
        log_action("setting_number " + id + " " + std::to_string(n));
        char b[16];
        std::snprintf(b, sizeof b, "%03d", n);
        for (SettingRow &r : vm_.settings.rows)
            if (r.id == id) {
                r.pos = n;
                r.value = std::string("Next: ") + b;
            }
        emit();
    };
    a.list_select = [this](int i) {
        log_action("list_select " + std::to_string(i));
        vm_.list.selected = i;
        emit();
    };
    a.list_activate = [this](int i) {
        log_action("list_activate " + std::to_string(i));
        if (i < 0 || i >= static_cast<int>(vm_.list.takes.size()) || !vm_.list.can_load) return;
        if (previewing_ == i) { /* the previewing row again: stop and rewind, no restart */
            stop_preview();
        } else { /* another row: the preview switches to it */
            previewing_ = i;
            vm_.list.previewing = i;
            set_transport(Transport::Playing);
            show_take(i);
        }
        emit();
    };
    a.list_load = [this](int i) {
        log_action("list_load " + std::to_string(i));
        if (i < 0 || i >= static_cast<int>(vm_.list.takes.size()) || !vm_.list.can_load) return;
        previewing_ = -1;
        vm_.list.previewing = -1;
        loaded_ = i;
        set_transport(Transport::Idle);
        show_take(i);
        vm_.rec.tape.loaded = true;
        vm_.rec.keys[static_cast<int>(KeyId::PlayPause)].enabled = true;
        ++vm_.rec.tape.load_seq;
        emit();
    };
    a.list_rename = [this](int i, const std::string &p) {
        log_action("list_rename " + std::to_string(i) + " " + p);
        if (i >= 0 && i < static_cast<int>(vm_.list.takes.size())) vm_.list.takes[static_cast<std::size_t>(i)].prefix = p;
        emit();
    };
    a.list_trim = [](int i) { log_action("list_trim " + std::to_string(i)); };
    a.list_delete = [this](int i) {
        log_action("list_delete " + std::to_string(i));
        if (i < 0 || i >= static_cast<int>(vm_.list.takes.size())) return;
        pending_delete_ = i;
        vm_.confirm.title = "Delete " + vm_.list.takes[static_cast<std::size_t>(i)].name + "?";
        vm_.confirm.body = "The file is removed from the recordings folder.";
        vm_.confirm.yes = "DELETE";
        vm_.confirm.shown = true;
        emit();
    };
    a.list_merge = [this](int i) {
        log_action("list_merge " + std::to_string(i));
        if (vm_.list.takes.size() < 2) {
            vm_.notice = "Merging needs two takes";
            emit();
            return;
        }
        vm_.list.merge_mode = true;
        merge_sel_.clear();
        merge_toggle(i);
    };
    a.merge_toggle = [this](int i) {
        log_action("merge_toggle " + std::to_string(i));
        merge_toggle(i);
    };
    a.merge_cancel = [this] {
        log_action("merge_cancel");
        vm_.list.merge_mode = false;
        merge_sel_.clear();
        demo_apply_merge(vm_.list, merge_sel_);
        emit();
    };
    a.merge_run = [this] {
        log_action("merge_run");
        if (merge_sel_.size() < 2) vm_.notice = "Merging needs two takes";
        else demo_merge_box(vm_, merge_sel_);
        emit();
    };
    a.merge_choose = [this](MergeChoice c) {
        static const char *const n[3] = {"keep", "delete", "cancel"};
        log_action(std::string("merge_choose ") + n[static_cast<int>(c)]);
        if (c == MergeChoice::DeleteOriginals) {
            pending_delete_ = -2;
            vm_.confirm.title = "Delete the originals?";
            vm_.confirm.body = "They are deleted only after the new file is written and synced.";
            vm_.confirm.yes = "DELETE";
            vm_.confirm.shown = true;
        } else {
            vm_.merge_box.shown = false;
            if (c == MergeChoice::KeepOriginals) demo_progress(vm_, merge_sel_.size());
        }
        emit();
    };
    a.merge_stop = [this] {
        log_action("merge_stop");
        vm_.progress.shown = false;
        vm_.notice = "Merge cancelled";
        emit();
    };
    a.trim_zoom = [this](int d) {
        log_action("trim_zoom " + std::to_string(d));
        TrimVm &t = vm_.trim;
        t.span_ms = d > 0 ? std::max<std::int64_t>(2000, t.span_ms / 2) : std::min<std::int64_t>(t.length_ms, t.span_ms * 2);
        refresh_trim();
        emit();
    };
    a.trim_nudge = [this](std::int64_t delta) {
        log_action("trim_nudge " + std::to_string(delta));
        vm_.trim.marker_ms += delta;
        refresh_trim();
        emit();
    };
    a.trim_scrub = [this](std::int64_t m) {
        vm_.trim.marker_ms = m;
        refresh_trim();
        emit();
    };
    a.trim_set_in = [this] {
        log_action("trim_set_in");
        vm_.trim.in_ms = vm_.trim.marker_ms;
        emit();
    };
    a.trim_set_out = [this] {
        log_action("trim_set_out");
        vm_.trim.out_ms = vm_.trim.marker_ms;
        emit();
    };
    a.trim_split = [this] {
        log_action("trim_split");
        TrimVm &t = vm_.trim;
        t.splits.push_back(t.marker_ms);
        std::sort(t.splits.begin(), t.splits.end());
        emit();
    };
    a.trim_remove_split = [this](int i) {
        log_action("trim_remove_split " + std::to_string(i));
        TrimVm &t = vm_.trim;
        if (i >= 0 && i < static_cast<int>(t.splits.size())) t.splits.erase(t.splits.begin() + i);
        t.selected_split = -1;
        emit();
    };
    a.trim_select_split = [this](int i) {
        log_action("trim_select_split " + std::to_string(i));
        TrimVm &t = vm_.trim;
        t.selected_split = i;
        if (i >= 0 && i < static_cast<int>(t.splits.size())) t.marker_ms = t.splits[static_cast<std::size_t>(i)];
        refresh_trim();
        emit();
    };
    a.trim_wave_fit = [this](bool fit) {
        log_action(std::string("trim_wave_fit ") + (fit ? "fit" : "true"));
        vm_.trim.fit = fit;
        emit();
    };
    a.trim_play = [this] {
        log_action("trim_play");
        vm_.trim.playing = !vm_.trim.playing;
        emit();
    };
    a.trim_save = [this] {
        log_action("trim_save");
        vm_.save.shown = true;
        emit();
    };
    a.save_choose = [this](SaveMode m) {
        static const char *const n[3] = {"new files", "replace", "cancel"};
        log_action(std::string("save_choose ") + n[static_cast<int>(m)]);
        if (m == SaveMode::Replace) {
            pending_delete_ = -1;
            vm_.confirm.title = "Delete the original take?";
            vm_.confirm.body = "It is deleted only after every new file is written and synced.";
            vm_.confirm.yes = "DELETE";
            vm_.confirm.shown = true;
        } else {
            vm_.save.shown = false;
        }
        emit();
    };
    a.confirm_answer = [this](bool yes) {
        log_action(std::string("confirm_answer ") + (yes ? "yes" : "no"));
        vm_.confirm.shown = false;
        if (pending_delete_ == -2) { /* the merge's "Delete the originals?" */
            pending_delete_ = -1;
            if (yes) {
                vm_.merge_box.shown = false;
                demo_progress(vm_, merge_sel_.size());
            }
            emit();
            return;
        }
        if (yes && pending_delete_ >= 0 && pending_delete_ < static_cast<int>(vm_.list.takes.size())) {
            vm_.list.takes.erase(vm_.list.takes.begin() + pending_delete_);
            if (pending_delete_ == loaded_) { /* the loaded take is gone: the blank cassette, nothing to play */
                loaded_ = -1;
                vm_.rec.tape.label.clear();
                vm_.rec.info.take = "TAKE 008";
                vm_.rec.keys[static_cast<int>(KeyId::PlayPause)].enabled = false;
            } else if (pending_delete_ < loaded_) {
                --loaded_;
            }
            vm_.list.take_count = std::max(0, vm_.list.take_count - 1);
            vm_.list.selected = std::min(vm_.list.selected, static_cast<int>(vm_.list.takes.size()) - 1);
        } else if (yes) {
            vm_.save.shown = false;
        }
        pending_delete_ = -1;
        emit();
    };
    return a;
}

} // namespace ui
} // namespace cassette
