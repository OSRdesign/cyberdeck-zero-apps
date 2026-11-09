/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the synthetic demonstration model and its driver (see demo.hpp).
 */
#include "demo.hpp"

#include "ui_format.hpp"

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
    r.info.time_left = "12 h 40";
    r.info.limiter = true;
    r.info.low_cut = true;
    r.card.db = 13.0;
    r.card.can_up = true;
    r.soft.db = 6.0;
    r.meter.channels = 1;
    r.meter.ch[0].bar = 11;
    r.meter.ch[0].hold_bar = 12;

    if (scene == "recording" || scene == "nocard" || scene == "stereo" || scene == "long" || scene == "recording-low") {
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
    } else if (scene == "deckempty") { /* idle with nothing in the deck (the loaded take was deleted): only OPEN, PLAY dimmed */
        r.transport = Transport::Idle;
        r.tape.loaded = false;
        r.tape.label.clear();
        r.info.take = "TAKE 008";
        r.keys[static_cast<int>(KeyId::PlayPause)].enabled = false;
        r.meter.ch[0].bar = 2;
        r.meter.ch[0].hold_bar = 0;
    } else { /* idle: Take 006 is loaded (the list's highlighted row) */
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
    s.rows.push_back(choice("format", "Format", {"WAV 48 kHz 16-bit", "WAV 48 kHz 24-bit", "WAV 44.1 kHz 16-bit"}, 0));
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
            if (row.id == "format" || row.id == "folder") row.enabled = false;

    /* the recordings: five synthetic takes, Take 006 selected */
    ListVm &l = vm.list;
    l.takes = {take("007", "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB", 42000),
               take("006", "today 11:05 \xC2\xB7 12 min \xC2\xB7 69 MB", 728000),
               take("005", "yesterday 18:20 \xC2\xB7 3 min \xC2\xB7 17 MB", 201000),
               take("004", "nodate \xC2\xB7 41 s \xC2\xB7 4 MB", 41000),
               take("003", "2 Nov \xC2\xB7 1 h 02 \xC2\xB7 357 MB", 3720000)};
    l.selected = 1;
    l.can_load = r.transport != Transport::Recording && r.transport != Transport::RecordingPaused &&
                 r.transport != Transport::Countdown;
    l.take_count = 12;
    l.folder = "~/Recordings";
    l.free_space = "41 GB";
    l.free_hours = "117 h";
    l.format_short = "48k/16";

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
    t.window_peaks = demo_window_peaks(t.length_ms, t.marker_ms, t.span_ms, 220);
    t.overview_peaks = demo_overview_peaks(t.length_ms, 110);

    vm.save.files = {{"take_006_20261108-1105-1.wav", 142300}, {"take_006_20261108-1105-2.wav", 44000},
                     {"take_006_20261108-1105-3.wav", 263700}};
    vm.confirm.title = "Delete the original take?";
    vm.confirm.body = "It is deleted only after every new file is written and synced.";
    vm.confirm.yes = "DELETE";
    /* CASSETTE_DEMO_NOTICE=<text>: a notice that stays until a tap dismisses it (the narrow recorder) or the scene ends */
    if (const char *n = std::getenv("CASSETTE_DEMO_NOTICE")) vm.notice = n;
    /* CASSETTE_DEMO_SERVER=1: the capture path is the sound server (idle: the card label leads the strip; recording: "(server)"
     * after the format); CASSETTE_DEMO_LOST_MS=<ms>: audio lost so far in the take (the red "lost 0.3 s", while recording) */
    if (const char *sv = std::getenv("CASSETTE_DEMO_SERVER")) {
        vm.rec.info.server = std::string(sv) == "1";
        if (vm.rec.info.server && vm.rec.transport == Transport::Idle) vm.rec.info.card = "USB AUDIO CODEC";
    }
    if (const char *lm = std::getenv("CASSETTE_DEMO_LOST_MS")) vm.rec.info.lost_ms = std::atoll(lm);
    if (const char *b = std::getenv("CASSETTE_DEMO_BOX")) {
        if (std::string(b) == "save") vm.save.shown = true;
        if (std::string(b) == "confirm") vm.confirm.shown = true;
    }
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

void DemoDriver::emit()
{
    if (changed) changed(vm_);
}

void DemoDriver::refresh_trim()
{
    TrimVm &t = vm_.trim;
    t.marker_ms = std::clamp<std::int64_t>(t.marker_ms, 0, t.length_ms);
    t.window_peaks = demo_window_peaks(t.length_ms, t.marker_ms, t.span_ms, 220);
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
        emit();
    };
    a.setting_slider = [this](const std::string &id, int pos) {
        log_action("setting_slider " + id + " " + std::to_string(pos));
        for (SettingRow &r : vm_.settings.rows)
            if (r.id == id) {
                r.pos = pos;
                r.value_text = std::to_string(pos) + " / " + std::to_string(r.max);
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
        if (yes && pending_delete_ >= 0 && pending_delete_ < static_cast<int>(vm_.list.takes.size())) {
            vm_.list.takes.erase(vm_.list.takes.begin() + pending_delete_);
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
