/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding tests: the view model mapping (bind/view_map.hpp), field by field. Synthetic values only.
 */
#include "check.hpp"
#include "fake_port.hpp"

#include "bind/view_map.hpp"

#include <cmath>

using namespace cassette;
using namespace cassette::bind;
using tb::base_snapshot;
using tb::row;

namespace {

constexpr int64_t kNow = 1794139200; /* 2026-11-08 12:00:00 UTC */

ui::ViewModel map(const Snapshot &s, const std::vector<TakeRow> &rows = {}, const LocalState &local = LocalState(),
                  const std::vector<PeakBar> *window = nullptr, const std::vector<PeakBar> *overview = nullptr)
{
    MapInput in;
    in.snap = &s;
    in.rows = &rows;
    in.local = &local;
    in.window = window;
    in.overview = overview;
    in.now_wall_s = kNow;
    in.utc = true;
    return map_view(in);
}

const ui::KeyState &key(const ui::ViewModel &vm, ui::KeyId k) { return vm.rec.keys[static_cast<int>(k)]; }

const ui::SettingRow *find_row(const ui::ViewModel &vm, const std::string &id)
{
    for (const ui::SettingRow &r : vm.settings.rows)
        if (r.id == id) return &r;
    return nullptr;
}

void test_helpers()
{
    /* transport and families */
    CHECK(transport_of(kDeckIdle) == ui::Transport::Idle);
    CHECK(transport_of(kDeckCountdown) == ui::Transport::Countdown);
    CHECK(transport_of(kDeckRecording) == ui::Transport::Recording);
    CHECK(transport_of(kDeckRecordingPaused) == ui::Transport::RecordingPaused);
    CHECK(transport_of(kDeckPlaying) == ui::Transport::Playing);
    CHECK(transport_of(kDeckPlayingPaused) == ui::Transport::PlayingPaused);
    CHECK(transport_of(kDeckScanningForward) == ui::Transport::ScanForward);
    CHECK(transport_of(kDeckScanningReverse) == ui::Transport::ScanReverse);
    CHECK(transport_of(99) == ui::Transport::Idle);
    CHECK(recording_family(kDeckCountdown) && recording_family(kDeckRecording) && recording_family(kDeckRecordingPaused));
    CHECK(!recording_family(kDeckIdle) && !recording_family(kDeckPlaying));
    CHECK(playing_family(kDeckPlaying) && playing_family(kDeckPlayingPaused) && playing_family(kDeckScanningForward) &&
          playing_family(kDeckScanningReverse));
    CHECK(!playing_family(kDeckIdle) && !playing_family(kDeckRecording));

    /* names */
    CHECK_EQ(display_name("take_006_20261108-1105.wav"), "Take 006");
    CHECK_EQ(display_name("take_006_20261108-1105-2.wav"), "Take 006-2");
    CHECK_EQ(display_name("interview_012_nodate.wav"), "Interview 012");
    CHECK_EQ(display_name("take_007_nodate_p2.wav"), "Take 007 p2");
    CHECK_EQ(display_name("holiday.wav"), "holiday");
    CHECK_EQ(display_name("", "008"), "Take 008");
    CHECK_EQ(sticker_of("take_006_20261108-1105-1.wav"), "006-1");
    CHECK_EQ(sticker_of("holiday.wav"), "holiday");
    CHECK_EQ(prefix_of("my-set_003_nodate.wav"), "my-set");
    CHECK_EQ(prefix_of("holiday.wav"), "");
    CHECK_EQ(upper("Take 006-1"), "TAKE 006-1");

    /* times, sizes */
    CHECK_EQ(time_left_text(12.0 + 40.0 / 60.0), "12 h 40");
    CHECK_EQ(time_left_text(0.09), "0 h 05");
    CHECK_EQ(time_left_text(117.6), "117 h");
    CHECK_EQ(time_left_text(0), "");
    CHECK_EQ(time_left_text(-1), "");
    CHECK_EQ(hours_text(117.6), "117 h");
    CHECK_EQ(hours_text(0.5), "30 min");
    CHECK_EQ(hours_text(0), "");
    CHECK_EQ(size_text(4ull << 20), "4 MB");
    CHECK_EQ(size_text(69ull << 20), "69 MB");
    CHECK_EQ(size_text(512ull << 10), "512 KB");
    CHECK_EQ(size_text(100), "1 KB");
    CHECK_EQ(size_text(static_cast<uint64_t>(4.25 * (1ull << 30))), "4.2 GB");
    CHECK_EQ(size_text(41ull << 30), "41 GB");
    CHECK_EQ(length_text(42), "42 s");
    CHECK_EQ(length_text(0), "0 s");
    CHECK_EQ(length_text(201), "3 min");
    CHECK_EQ(length_text(728), "12 min");
    CHECK_EQ(length_text(3720), "1 h 02");
    CHECK_EQ(date_part("2026-11-08 14:32", kNow, true), "today 14:32");
    CHECK_EQ(date_part("2026-11-07 18:20", kNow, true), "yesterday 18:20");
    CHECK_EQ(date_part("2026-11-02 09:00", kNow, true), "2 Nov");
    CHECK_EQ(date_part("2025-12-31 23:59", kNow, true), "31 Dec 2025");
    CHECK_EQ(date_part("", kNow, true), "nodate");
    CHECK_EQ(date_part("garbage", kNow, true), "nodate");

    TakeRow r = row("take_007_20261108-1432.wav", 42, 4ull << 20, "2026-11-08 14:32");
    CHECK_EQ(take_detail(r, kNow, true), "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB");
    r.repaired = true;
    CHECK_EQ(take_detail(r, kNow, true), "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB \xC2\xB7 repaired");
    r = row("take_004_nodate.wav", 41, 4ull << 20, "");
    CHECK_EQ(take_detail(r, kNow, true), "nodate \xC2\xB7 41 s \xC2\xB7 4 MB");
    r.damaged = true;
    CHECK_EQ(take_detail(r, kNow, true), "damaged \xC2\xB7 4 MB");

    /* formats */
    CHECK_EQ(tape_format("48k/16 WAV"), "WAV 48k");
    CHECK_EQ(tape_format("44.1k/16 stereo WAV"), "WAV 44.1k");
    CHECK_EQ(short_format("48k/16 WAV"), "48k/16");
    CHECK_EQ(format_words("48k/16 WAV"), "WAV 48 kHz 16-bit");
    CHECK_EQ(format_words("44.1k/24 stereo WAV"), "WAV 44.1 kHz 24-bit stereo");

    /* peaks */
    PeakBar b;
    b.min = -32767;
    b.max = 100;
    CHECK(std::fabs(peak_level(b) - 1.0f) < 1e-6f);
    b.min = -100;
    b.max = 16384;
    CHECK(std::fabs(peak_level(b) - 0.5f) < 0.001f);
    b.min = -32768;
    b.max = 0;
    CHECK(peak_level(b) == 1.0f);
    CHECK(peak_levels({b, PeakBar()}).size() == 2);
    CHECK(peak_levels({b, PeakBar()})[1] == 0.0f);

    /* ms and frames */
    CHECK_EQ(frames_to_ms(48000, 48000), 1000);
    CHECK_EQ(frames_to_ms(44100 * 3, 44100), 3000);
    CHECK_EQ(frames_to_ms(24000, 0), 500); /* rate 0: 48 kHz */
    CHECK_EQ(ms_to_frames(100, 48000), 4800);
    CHECK_EQ(ms_to_frames(-1000, 48000), -48000);

    /* the zoom limit: a 2 s window at most deep */
    TrimSnap t;
    t.rate = 48000;
    t.frames = 48000 * 64; /* 64 s */
    t.max_zoom = 9;
    CHECK_EQ(zoom_limit(t), 5); /* 64 >> 5 = 2 s */
    t.frames = 48000;           /* 1 s: shorter than 2 s, the whole take only */
    t.max_zoom = 3;
    CHECK_EQ(zoom_limit(t), 0);
    t.frames = 48000 * 3; /* 3 s: 1.5 s would be under 2 s */
    t.max_zoom = 4;
    CHECK_EQ(zoom_limit(t), 0);
}

void test_recorder()
{
    Snapshot s = base_snapshot();
    LocalState local;
    local.next_take = 8;

    /* idle: the next take on the cassette */
    ui::ViewModel vm = map(s, {}, local);
    CHECK(vm.rec.input_present);
    CHECK(vm.rec.transport == ui::Transport::Idle);
    CHECK(vm.rec.tape.loaded);
    CHECK_EQ(vm.rec.tape.label, "Take 008");
    CHECK_EQ(vm.rec.tape.side, "SIDE A");
    CHECK_EQ(vm.rec.tape.format, "WAV 48k");
    CHECK_EQ(vm.rec.info.take, "TAKE 008");
    CHECK_EQ(vm.rec.info.format, "48k/16 WAV");
    CHECK_EQ(vm.rec.info.time_left, "12 h 40");
    CHECK_EQ(vm.rec.counter_ms, 0);
    CHECK(!vm.rec.info.limiter && !vm.rec.info.low_cut);
    CHECK_EQ(vm.rec.info.notice, "");
    CHECK(key(vm, ui::KeyId::Rec).enabled && !key(vm, ui::KeyId::Rec).latched);
    CHECK(key(vm, ui::KeyId::PlayPause).enabled && !key(vm, ui::KeyId::PlayPause).latched);
    CHECK(!key(vm, ui::KeyId::Stop).enabled);
    CHECK_EQ(vm.rec.meter.channels, 1);
    CHECK_EQ(vm.rec.meter.ch[0].bar, 11);
    CHECK_EQ(vm.rec.meter.ch[0].hold_bar, 12);
    CHECK(!vm.rec.meter.ch[0].peak_lamp);
    CHECK(vm.rec.card.shown);
    CHECK(std::fabs(vm.rec.card.db - 11.9) < 1e-9);
    CHECK(vm.rec.card.can_down && vm.rec.card.can_up);
    CHECK(vm.rec.soft.shown);
    CHECK(std::fabs(vm.rec.soft.db - 6.0) < 1e-9);
    CHECK(vm.rec.soft.can_down && vm.rec.soft.can_up);

    /* the next take is not known yet: 001; a custom prefix */
    s.settings.prefix = "interview";
    vm = map(s);
    CHECK_EQ(vm.rec.tape.label, "Interview 001");
    s.settings.prefix = "take";

    /* the steppers at their ends */
    s.card.level_db = 0;
    s.card.soft_gain_db = 24;
    vm = map(s, {}, local);
    CHECK(!vm.rec.card.can_down && vm.rec.card.can_up);
    CHECK(vm.rec.soft.can_down && !vm.rec.soft.can_up);
    s.card.level_db = 23.81;
    s.card.soft_gain_db = 0;
    vm = map(s, {}, local);
    CHECK(vm.rec.card.can_down && !vm.rec.card.can_up);
    CHECK(!vm.rec.soft.can_down && vm.rec.soft.can_up);
    s.card.level_db = 11.9;
    s.card.soft_gain_db = 6;

    /* a card without a level control: no CARD stepper */
    s.card.has_level = false;
    vm = map(s, {}, local);
    CHECK(!vm.rec.card.shown);
    CHECK(!vm.rec.card.can_down && !vm.rec.card.can_up);
    s.card.has_level = true;

    /* recording: the new take, the length, REC latched (not dimmed although the deck says a press does nothing) */
    s.deck.state = kDeckRecording;
    s.deck.rec.enabled = false;
    s.deck.rec.active = true;
    s.deck.rew.enabled = s.deck.fwd.enabled = false;
    s.deck.stop.enabled = true;
    s.deck.play_pause.active = true; /* the deck's "shows pause": not the latch */
    s.take.name = "take_009_20261108-1200.wav";
    s.take.recording = true;
    s.time.elapsed_s = 42.4;
    s.settings.limiter = true;
    s.settings.low_cut = true;
    s.meter.ch[0].peak_lamp = true;
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::Recording);
    CHECK_EQ(vm.rec.tape.label, "Take 009");
    CHECK_EQ(vm.rec.info.take, "TAKE 009");
    CHECK_EQ(vm.rec.counter_ms, 42400);
    CHECK(key(vm, ui::KeyId::Rec).latched && key(vm, ui::KeyId::Rec).enabled);
    CHECK(!key(vm, ui::KeyId::Rew).enabled && !key(vm, ui::KeyId::Fwd).enabled);
    CHECK(key(vm, ui::KeyId::Stop).enabled);
    CHECK(!key(vm, ui::KeyId::PlayPause).latched);
    CHECK(vm.rec.info.limiter && vm.rec.info.low_cut);
    CHECK(vm.rec.meter.ch[0].peak_lamp);

    /* paused recording: PAUSE latched */
    s.deck.state = kDeckRecordingPaused;
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::RecordingPaused);
    CHECK(key(vm, ui::KeyId::PlayPause).latched);
    CHECK(key(vm, ui::KeyId::Rec).latched);

    /* countdown: the number and the ring; REC not latched (board CassetteCount), counter 0 */
    s.deck.state = kDeckCountdown;
    s.time.countdown_seconds = 3;
    s.time.countdown_fraction = 0.8f;
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::Countdown);
    CHECK_EQ(vm.rec.countdown_left, 3);
    CHECK(std::fabs(vm.rec.countdown_fraction - 0.8f) < 1e-6f);
    CHECK(!key(vm, ui::KeyId::Rec).latched);
    CHECK_EQ(vm.rec.counter_ms, 0);
    CHECK_EQ(vm.rec.tape.label, "Take 008"); /* the take is not created yet */

    /* playing: the position, PLAY latched, the take playing */
    s = base_snapshot();
    s.deck.state = kDeckPlaying;
    s.deck.stop.enabled = true;
    s.take.name = "take_006_20261108-1105.wav";
    s.time.position_s = 344.2;
    s.time.length_s = 728.0;
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::Playing);
    CHECK_EQ(vm.rec.counter_ms, 344200);
    CHECK_EQ(vm.rec.info.play_pos_ms, 344200);
    CHECK_EQ(vm.rec.info.play_len_ms, 728000);
    CHECK_EQ(vm.rec.tape.label, "Take 006");
    CHECK(key(vm, ui::KeyId::PlayPause).latched);
    s.deck.state = kDeckPlayingPaused;
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::PlayingPaused);
    CHECK(key(vm, ui::KeyId::PlayPause).latched);

    /* scanning: the badge's speed, REW latched */
    s.deck.state = kDeckScanningReverse;
    s.deck.rew.active = true;
    s.scan.active = true;
    s.scan.direction = -1;
    s.scan.speed_text = "5x";
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::ScanReverse);
    CHECK_EQ(vm.rec.scan_speed, 5);
    CHECK(key(vm, ui::KeyId::Rew).latched && !key(vm, ui::KeyId::Fwd).latched);
    CHECK(key(vm, ui::KeyId::PlayPause).latched);
    s.deck.state = kDeckScanningForward;
    s.deck.rew.active = false;
    s.deck.fwd.active = true;
    s.scan.speed_text = "8x";
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::ScanForward);
    CHECK_EQ(vm.rec.scan_speed, 8);
    CHECK(key(vm, ui::KeyId::Fwd).latched);

    /* stereo meter */
    s = base_snapshot();
    s.meter.channels = 2;
    s.meter.ch[1].bar = 9;
    s.meter.ch[1].hold_bar = 10;
    s.meter.ch[1].peak_lamp = true;
    vm = map(s, {}, local);
    CHECK_EQ(vm.rec.meter.channels, 2);
    CHECK_EQ(vm.rec.meter.ch[1].bar, 9);
    CHECK_EQ(vm.rec.meter.ch[1].hold_bar, 10);
    CHECK(vm.rec.meter.ch[1].peak_lamp);
    s.meter.channels = 0; /* before the first period */
    vm = map(s, {}, local);
    CHECK_EQ(vm.rec.meter.channels, 1);

    /* a notice replaces the strip's text */
    LocalState with_notice = local;
    with_notice.notice = "The sound card was unplugged";
    vm = map(base_snapshot(), {}, with_notice);
    CHECK_EQ(vm.notice, "The sound card was unplugged"); /* the page that shows shows it: every page */
    CHECK_EQ(vm.rec.info.notice, "");

    /* no sound input: the empty window, NO INPUT, REC dimmed, no CARD, no level */
    s = base_snapshot();
    s.card.present = false;
    s.card.labels.clear();
    s.card.label.clear();
    s.card.message = "No sound card with a microphone input";
    vm = map(s, {}, local);
    CHECK(!vm.rec.input_present);
    CHECK(!vm.rec.tape.loaded);
    CHECK_EQ(vm.rec.info.take, "NO INPUT");
    CHECK_EQ(vm.rec.info.format, "");
    CHECK_EQ(vm.rec.info.notice, ""); /* the page says it */
    CHECK(!key(vm, ui::KeyId::Rec).enabled);
    CHECK(!vm.rec.card.shown);
    CHECK_EQ(vm.rec.meter.ch[0].bar, 0);
    CHECK_EQ(vm.rec.meter.ch[0].hold_bar, 0);
    /* a card that is listed but cannot be used: its message in the strip */
    s.card.labels = {"Demo Microphone"};
    s.card.message = "The sound card is busy";
    vm = map(s, {}, local);
    CHECK_EQ(vm.rec.info.notice, "The sound card is busy");
    with_notice.notice = "Recovered take_003";
    vm = map(s, {}, with_notice);
    CHECK_EQ(vm.notice, "Recovered take_003"); /* a notice is its own field; the standing reason stays beside it */
    CHECK_EQ(vm.rec.info.notice, "The sound card is busy");
}

void test_settings()
{
    Snapshot s = base_snapshot();
    s.card.auto_gain = true;
    ui::ViewModel vm = map(s);
    std::vector<std::string> ids;
    for (const ui::SettingRow &r : vm.settings.rows) ids.push_back(r.id);
    const std::vector<std::string> want = {"input", "level", "agc", "format", "timer", "rewfwd", "channels", "lowcut",
                                           "limiter", "sync", "folder", "prefix", "split"};
    CHECK(ids == want);
    const ui::SettingRow *r = find_row(vm, "input");
    CHECK(r && r->kind == ui::SettingKind::Choice && r->label == "Input" && r->enabled);
    CHECK(r && r->choices.size() == 2 && r->choice == 0 && r->value == "Demo Microphone");
    r = find_row(vm, "level");
    CHECK(r && r->kind == ui::SettingKind::Slider && r->label == "Input level");
    CHECK(r && r->min == 0 && r->max == 24 && r->pos == 12);
    CHECK(r && r->value_text == "+12 dB");
    r = find_row(vm, "agc");
    CHECK(r && r->kind == ui::SettingKind::Toggle && r->on && r->label == "Auto gain (card)");
    r = find_row(vm, "format");
    CHECK(r && r->value == "WAV 48 kHz 16-bit" && r->choices.size() == 1 && r->enabled);
    r = find_row(vm, "timer");
    CHECK(r && r->choices.size() == 4 && r->choice == 0 && r->value == "Off");
    r = find_row(vm, "rewfwd");
    CHECK(r && r->choice == 0 && r->value == "Fast scan 5x");
    r = find_row(vm, "channels");
    CHECK(r && !r->enabled && r->value == "Mono");
    r = find_row(vm, "lowcut");
    CHECK(r && r->kind == ui::SettingKind::Toggle && !r->on);
    r = find_row(vm, "limiter");
    CHECK(r && !r->on);
    r = find_row(vm, "folder");
    CHECK(r && r->choices.size() == 1 && r->value == "~/Recordings" && r->enabled);
    r = find_row(vm, "prefix");
    CHECK(r && r->kind == ui::SettingKind::Text && r->value == "take");
    r = find_row(vm, "split");
    CHECK(r && r->choice == 0 && r->value == "At 4 GiB");

    r = find_row(vm, "channels");
    CHECK(r && r->kind == ui::SettingKind::Info && !r->enabled && r->choices.empty());
    r = find_row(vm, "sync");
    CHECK(r && r->kind == ui::SettingKind::Choice && r->label == "Sync every" && r->choices.size() == 4);
    CHECK(r && r->choice == 1 && r->value == "10 s"); /* the default */
    CHECK(!find_row(vm, "mute") && !find_row(vm, "output")); /* no mute control, one output */

    /* T6d: the final order with the conditional rows, and their values */
    {
        Snapshot f = base_snapshot();
        f.card.has_mute = true;
        f.card.mute = true;
        f.card.output_labels = {"USB Audio Device", "Default output"};
        f.card.output_devices = {"hw:CARD=Demo,DEV=0", "default"};
        f.card.output_selected = 1;
        f.settings.sync_interval_s = 20;
        ui::ViewModel fv = map(f);
        std::vector<std::string> fids;
        for (const ui::SettingRow &row : fv.settings.rows) fids.push_back(row.id);
        const std::vector<std::string> fwant = {"input", "level", "agc", "mute", "format", "timer", "rewfwd", "channels",
                                                "lowcut", "limiter", "sync", "folder", "prefix", "split", "output"};
        CHECK(fids == fwant);
        const ui::SettingRow *m = find_row(fv, "mute");
        CHECK(m && m->kind == ui::SettingKind::Toggle && m->on && m->label == "Mic mute (card)");
        const ui::SettingRow *o = find_row(fv, "output");
        CHECK(o && o->kind == ui::SettingKind::Choice && o->label == "Output" && o->choice == 1 && o->value == "Default output");
        CHECK(find_row(fv, "sync")->value == "20 s");
        f.settings.sync_interval_s = 17; /* any value of 5..30: the nearest entry */
        CHECK(find_row(map(f), "sync")->value == "20 s");
        f.settings.sync_interval_s = 6;
        CHECK(find_row(map(f), "sync")->value == "5 s");
        /* the card is gone: no mute row (it needs a present card), one output left: no Output row */
        f.card.present = false;
        f.card.output_labels = {"Default output"};
        fv = map(f);
        CHECK(!find_row(fv, "mute") && !find_row(fv, "output"));
    }

    /* other values */
    s.settings.record_timer_s = 10;
    s.settings.rew_fwd = ScanMode::Jump;
    s.settings.low_cut = s.settings.limiter = true;
    s.settings.folder = "/media/usb/Rec";
    s.settings.prefix = "set";
    s.settings.auto_split = false;
    s.meter.channels = 2;
    s.card.selected = 1;
    s.card.label = "Demo Line Input";
    vm = map(s);
    CHECK(find_row(vm, "timer")->choice == 2 && find_row(vm, "timer")->value == "10 s");
    CHECK(find_row(vm, "rewfwd")->value == "Jump 5 s");
    CHECK(find_row(vm, "lowcut")->on && find_row(vm, "limiter")->on);
    CHECK(find_row(vm, "folder")->choices.size() == 2 && find_row(vm, "folder")->choices[1] == "~/Recordings");
    CHECK(find_row(vm, "folder")->value == "/media/usb/Rec");
    CHECK(find_row(vm, "prefix")->value == "set");
    CHECK(find_row(vm, "split")->value == "Off (one RF64 file)");
    CHECK(find_row(vm, "channels")->value == "Stereo");
    CHECK(find_row(vm, "input")->choice == 1 && find_row(vm, "input")->value == "Demo Line Input");

    /* while recording: Format, Folder and Input dimmed */
    s.deck.state = kDeckRecording;
    vm = map(s);
    CHECK(!find_row(vm, "format")->enabled && !find_row(vm, "folder")->enabled && !find_row(vm, "input")->enabled);
    CHECK(find_row(vm, "timer")->enabled && find_row(vm, "prefix")->enabled);

    /* a card without controls: no Input level, no Auto gain */
    s = base_snapshot();
    s.card.has_level = s.card.has_agc = false;
    vm = map(s);
    CHECK(!find_row(vm, "level") && !find_row(vm, "agc"));
    CHECK(vm.settings.rows.size() == 11);
    /* no card at all */
    s.card.present = false;
    s.card.labels.clear();
    s.card.label.clear();
    s.card.selected = -1;
    vm = map(s);
    CHECK(find_row(vm, "input") && !find_row(vm, "input")->enabled && find_row(vm, "input")->value == "None");
}

void test_list()
{
    Snapshot s = base_snapshot();
    const std::vector<TakeRow> rows = {row("take_007_20261108-1432.wav", 42, 4ull << 20, "2026-11-08 14:32"),
                                       row("take_006_20261108-1105.wav", 728, 69ull << 20, "2026-11-08 11:05", true),
                                       row("take_006_20261108-1105-1.wav", 201, 17ull << 20, "2026-11-07 18:20"),
                                       row("take_004_nodate.wav", 41, 4ull << 20, "")};
    ui::ViewModel vm = map(s, rows);
    const ui::ListVm &l = vm.list;
    CHECK_EQ(l.takes.size(), 4u);
    CHECK_EQ(l.takes[0].id, "take_007_20261108-1432.wav");
    CHECK_EQ(l.takes[0].sticker, "007");
    CHECK_EQ(l.takes[0].name, "Take 007");
    CHECK_EQ(l.takes[0].detail, "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB");
    CHECK_EQ(l.takes[0].length_ms, 42000);
    CHECK_EQ(l.takes[0].prefix, "take");
    CHECK_EQ(l.takes[2].sticker, "006-1");
    CHECK_EQ(l.takes[2].name, "Take 006-1");
    CHECK_EQ(l.takes[2].detail, "yesterday 18:20 \xC2\xB7 3 min \xC2\xB7 17 MB");
    CHECK_EQ(l.takes[3].detail, "nodate \xC2\xB7 41 s \xC2\xB7 4 MB");
    CHECK_EQ(l.selected, 1); /* the current take */
    CHECK_EQ(l.take_count, 4);
    CHECK_EQ(l.folder, "~/Recordings");
    CHECK_EQ(l.free_space, "41 GB");
    CHECK_EQ(l.free_hours, "12 h");
    CHECK_EQ(l.format_short, "48k/16");

    LocalState local;
    local.list_sel = "take_004_nodate.wav";
    vm = map(s, rows, local);
    CHECK_EQ(vm.list.selected, 3); /* the user's highlight */
    local.list_sel = "gone.wav";
    vm = map(s, rows, local);
    CHECK_EQ(vm.list.selected, 1); /* back to the current take */
    std::vector<TakeRow> none_current = rows;
    for (TakeRow &r : none_current) r.current = false;
    vm = map(s, none_current);
    CHECK_EQ(vm.list.selected, 0);
    vm = map(s, {});
    CHECK_EQ(vm.list.selected, -1);
    CHECK_EQ(vm.list.take_count, 0);
    s.take.free_bytes = 0;
    s.take.hours_left = 0;
    vm = map(s, {});
    CHECK_EQ(vm.list.free_space, "");
    CHECK_EQ(vm.list.free_hours, "");
}

void test_trim()
{
    Snapshot s = base_snapshot();
    ui::ViewModel vm = map(s);
    CHECK(vm.trim.take_id.empty()); /* closed */
    s.trim.open = true;
    s.trim.take = "take_006_20261108-1105.wav";
    s.trim.rate = 48000;
    s.trim.frames = 728ull * 48000;
    s.trim.position = 344200ull * 48;
    s.trim.visible_frames = 60ull * 48000;
    s.trim.in_frame = 178700ull * 48;
    s.trim.out_frame = 628700ull * 48;
    s.trim.splits = {321154ull * 48, 365063ull * 48};
    s.trim.selected_split = 1;
    s.trim.previewing = true;
    std::vector<PeakBar> window(220), overview(110);
    window[10].max = 32767;
    overview[5].min = -16384;
    LocalState local;
    vm = map(s, {}, local, &window, &overview);
    const ui::TrimVm &t = vm.trim;
    CHECK_EQ(t.take_id, "take_006_20261108-1105.wav");
    CHECK_EQ(t.sticker, "006");
    CHECK_EQ(t.take, "TAKE 006");
    CHECK_EQ(t.length_ms, 728000);
    CHECK_EQ(t.marker_ms, 344200);
    CHECK_EQ(t.span_ms, 60000);
    CHECK_EQ(t.in_ms, 178700);
    CHECK_EQ(t.out_ms, 628700);
    CHECK(t.splits.size() == 2 && t.splits[0] == 321154 && t.splits[1] == 365063);
    CHECK_EQ(t.selected_split, -1); /* selected by add_split, not by a tap */
    CHECK(t.playing);
    CHECK_EQ(t.window_peaks.size(), 220u);
    CHECK(t.window_peaks[10] == 1.0f && t.window_peaks[0] == 0.0f);
    CHECK_EQ(t.overview_peaks.size(), 110u);
    CHECK(std::fabs(t.overview_peaks[5] - 0.5f) < 0.001f);
    /* a tap selected it and the marker is on it */
    local.tap_selected = true;
    s.trim.position = s.trim.splits[1];
    vm = map(s, {}, local, &window, &overview);
    CHECK_EQ(vm.trim.selected_split, 1);
    /* the marker moved away */
    s.trim.position += 4800;
    vm = map(s, {}, local, &window, &overview);
    CHECK_EQ(vm.trim.selected_split, -1);
    /* another rate */
    s.trim.rate = 44100;
    s.trim.frames = 44100 * 10;
    s.trim.position = 44100 * 2;
    s.trim.visible_frames = 44100 * 10;
    vm = map(s, {}, local);
    CHECK_EQ(vm.trim.length_ms, 10000);
    CHECK_EQ(vm.trim.marker_ms, 2000);
    CHECK_EQ(vm.trim.span_ms, 10000);
    CHECK(vm.trim.window_peaks.empty()); /* not ready */
}

void test_boxes()
{
    Snapshot s = base_snapshot();
    LocalState local;
    ui::ViewModel vm = map(s, {}, local);
    CHECK(!vm.save.shown && !vm.confirm.shown);
    local.save_shown = true;
    local.save_files = {{"take_006_20261108-1105-1.wav", 142300}, {"take_006_20261108-1105-2.wav", 44000}};
    vm = map(s, {}, local);
    CHECK(vm.save.shown);
    CHECK(vm.save.files.size() == 2 && vm.save.files[1].name == "take_006_20261108-1105-2.wav" &&
          vm.save.files[1].length_ms == 44000);
    local.confirm = LocalState::Confirm::Replace;
    vm = map(s, {}, local);
    CHECK(vm.confirm.shown);
    CHECK_EQ(vm.confirm.title, "Delete the original take?");
    CHECK_EQ(vm.confirm.body, "It is deleted only after every new file is written and synced.");
    CHECK_EQ(vm.confirm.yes, "DELETE");
    local.confirm = LocalState::Confirm::Delete;
    local.confirm_take = "take_004_nodate.wav";
    vm = map(s, {}, local);
    CHECK(vm.confirm.shown);
    CHECK_EQ(vm.confirm.title, "Delete this take?");
    CHECK_EQ(vm.confirm.body, "take_004_nodate.wav");
}

void test_capture_path()
{
    /* idle, direct: nothing extra */
    Snapshot s = base_snapshot();
    ui::ViewModel vm = map(s);
    CHECK(!vm.rec.info.server);
    CHECK_EQ(vm.rec.info.card, "");
    CHECK_EQ(vm.rec.info.lost_ms, 0);

    /* idle, through the sound server: the flag and the card label (upper case, as the take) */
    s.card.via_server = true;
    vm = map(s);
    CHECK(vm.rec.info.server);
    CHECK_EQ(vm.rec.info.card, "DEMO MICROPHONE");
    CHECK_EQ(vm.rec.info.lost_ms, 0);

    /* recording through the sound server: the flag stays, the card label is not on the strip */
    s.deck.state = kDeckRecording;
    s.take.name = "take_008_20261108-1200.wav";
    s.take.recording = true;
    vm = map(s);
    CHECK(vm.rec.info.server);
    CHECK_EQ(vm.rec.info.card, "");
    CHECK_EQ(vm.rec.info.lost_ms, 0);

    /* lost audio: seconds to milliseconds; a tiny loss still shows (at least 1 ms) */
    s.problems.frames_lost = 14400;
    s.problems.lost_seconds = 0.3;
    vm = map(s);
    CHECK_EQ(vm.rec.info.lost_ms, 300);
    s.problems.frames_lost = 3;
    s.problems.lost_seconds = 0.00006;
    vm = map(s);
    CHECK_EQ(vm.rec.info.lost_ms, 1);

    /* direct recording with a loss */
    s.card.via_server = false;
    s.problems.frames_lost = 14400;
    s.problems.lost_seconds = 0.3;
    vm = map(s);
    CHECK(!vm.rec.info.server);
    CHECK_EQ(vm.rec.info.lost_ms, 300);

    /* after the take the count is not shown (the idle strip), though the problem record still holds it */
    s.deck.state = kDeckIdle;
    s.take.recording = false;
    vm = map(s);
    CHECK_EQ(vm.rec.info.lost_ms, 0);
    CHECK(vm.rec.info.take.rfind("TAKE", 0) == 0);

    /* paused take: still counted */
    s.deck.state = kDeckRecordingPaused;
    vm = map(s);
    CHECK_EQ(vm.rec.info.lost_ms, 300);

    /* the fields take part in change detection */
    const ui::ViewModel a = map(base_snapshot());
    ui::ViewModel c = a;
    c.rec.info.server = true;
    CHECK(!same(a, c));
    c = a;
    c.rec.info.card = "X";
    CHECK(!same(a, c));
    c = a;
    c.rec.info.lost_ms = 5;
    CHECK(!same(a, c));
}

void test_same()
{
    const Snapshot s = base_snapshot();
    const ui::ViewModel a = map(s), b = map(s);
    CHECK(same(a, b));
    ui::ViewModel c = a;
    c.rec.counter_ms += 1;
    CHECK(!same(a, c));
    c = a;
    c.rec.keys[4].latched = !c.rec.keys[4].latched;
    CHECK(!same(a, c));
    c = a;
    c.rec.meter.ch[0].hold_bar += 1;
    CHECK(!same(a, c));
    c = a;
    c.rec.info.notice = "x";
    CHECK(!same(a, c));
    c = a;
    c.notice = "x";
    CHECK(!same(a, c));
    c = a;
    c.rec.soft.db += 1;
    CHECK(!same(a, c));
    c = a;
    c.list.selected = 3;
    CHECK(!same(a, c));
    c = a;
    c.trim.marker_ms = 5;
    CHECK(!same(a, c));
    c = a;
    c.save.shown = true;
    CHECK(!same(a, c));
    c = a;
    c.confirm.shown = true;
    CHECK(!same(a, c));
    c = a;
    c.settings.rows[0].choice = 1;
    CHECK(!same(a, c));
    c = a;
    c.rec.tape.label = "Take 999";
    CHECK(!same(a, c));
}

} // namespace

void test_map()
{
    test_helpers();
    test_recorder();
    test_settings();
    test_list();
    test_trim();
    test_boxes();
    test_capture_path();
    test_same();
}
