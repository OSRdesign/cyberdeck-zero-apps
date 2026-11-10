/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding tests: the view model mapping (bind/view_map.hpp), field by field. Synthetic values only.
 */
#include "check.hpp"
#include "fake_port.hpp"

#include "bind/view_map.hpp"
#include "ui/ui_format.hpp"

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
    /* the format tag of the row (a folder can mix WAV, FLAC and Opus takes) */
    r = row("take_009_20261108-1432.flac", 42, 4ull << 20, "2026-11-08 14:32");
    r.format_tag = "FLAC";
    CHECK_EQ(take_detail(r, kNow, true), "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB \xC2\xB7 FLAC");
    r.repaired = true;
    CHECK_EQ(take_detail(r, kNow, true), "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB \xC2\xB7 FLAC \xC2\xB7 repaired");
    r.format_tag = "WAV";
    r.repaired = false;
    CHECK_EQ(take_detail(r, kNow, true), "today 14:32 \xC2\xB7 42 s \xC2\xB7 4 MB \xC2\xB7 WAV");
    r.damaged = true;
    CHECK_EQ(take_detail(r, kNow, true), "damaged \xC2\xB7 4 MB");
    /* the shorter forms of a narrow list: size dropped first, then the date shortest; the format tag always stays */
    {
        const char *D = "·";
        auto j = [&](std::initializer_list<const char *> p) { std::string o; for (const char *x : p) { if (!o.empty()) o += std::string(" ") + D + " "; o += x; } return o; };
        TakeRow o = row("take_005.opus", 180, 17ull << 20, "2026-11-07 18:20");
        o.format_tag = "OPUS";
        std::vector<std::string> f = take_detail_forms(o, kNow, true);
        CHECK_EQ(f.size(), size_t(3));
        CHECK_EQ(f[0], j({"yesterday 18:20", "3 min", "17 MB", "OPUS"}));
        CHECK_EQ(f[1], j({"yesterday 18:20", "3 min", "OPUS"}));
        CHECK_EQ(f[2], j({"yest 18:20", "3 min", "OPUS"}));
        CHECK_EQ(take_detail(o, kNow, true), f[0]); /* the wide line is the full one */
        o = row("take_007.flac", 42, 4ull << 20, "2026-11-08 14:32");
        o.format_tag = "FLAC";
        f = take_detail_forms(o, kNow, true);
        CHECK_EQ(f[1], j({"today 14:32", "42 s", "FLAC"}));
        CHECK_EQ(f[2], j({"14:32", "42 s", "FLAC"}));
        o = row("take_003.wav", 3720, 357ull << 20, "2025-12-31 23:59");
        o.format_tag = "WAV";
        f = take_detail_forms(o, kNow, true);
        CHECK_EQ(f[1], j({"31 Dec 2025", "1 h 02", "WAV"}));
        CHECK_EQ(f[2], j({"31 Dec 25", "1 h 02", "WAV"}));
        o.repaired = true;
        CHECK_EQ(take_detail_forms(o, kNow, true)[2], j({"31 Dec 25", "1 h 02", "WAV", "repaired"}));
        o = row("take_002.wav", 41, 4ull << 20, "");
        f = take_detail_forms(o, kNow, true); /* no tag, no date: the forms that do not differ are not repeated */
        CHECK_EQ(f.size(), size_t(2));
        CHECK_EQ(f[1], j({"nodate", "41 s"}));
        o.damaged = true;
        CHECK_EQ(take_detail_forms(o, kNow, true).size(), size_t(1));
        CHECK_EQ(date_part("2026-11-02 09:00", kNow, true, true), "2 Nov");
    }
    /* the tape label and the names of an Opus or FLAC take */
    CHECK_EQ(tape_format("48k/16 OPUS"), "OPUS 48k");
    CHECK_EQ(display_name("take_009_20261108-1432.opus"), "Take 009");
    CHECK_EQ(sticker_of("take_009_20261108-1432_p2-1.flac"), "009 p2-1");
    CHECK_EQ(prefix_of("field rec_012_nodate.opus"), "field rec");

    /* formats */
    CHECK_EQ(tape_format("48k/16 WAV"), "WAV 48k");
    CHECK_EQ(tape_format("44.1k/16 stereo WAV"), "WAV 44.1k");
    CHECK_EQ(short_format("48k/16 WAV"), "48k/16");
    CHECK_EQ(format_words("48k/16 WAV"), "WAV 48 kHz 16-bit");
    CHECK_EQ(format_words("44.1k/24 stereo WAV"), "WAV 44.1 kHz 24-bit stereo");
    CHECK_EQ(format_words("16k/16 WAV"), "WAV 16 kHz 16-bit");
    CHECK_EQ(format_words("22.05k/16 stereo WAV"), "WAV 22.05 kHz 16-bit stereo");
    CHECK_EQ(tape_format("16k/16 WAV"), "WAV 16k");
    CHECK_EQ(short_format("22.05k/16 stereo WAV"), "22.05k/16");
    CHECK_EQ(frames_to_ms(16000 * 90, 16000), 90000);
    CHECK_EQ(frames_to_ms(8000, 16000), 500);
    CHECK_EQ(ms_to_frames(500, 16000), 8000);
    CHECK_EQ(ms_to_frames(2500, 22050), 55125);

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

    /* idle, nothing loaded: the BLANK cassette (drawn, no name on the label), the strip names the take REC would write next */
    ui::ViewModel vm = map(s, {}, local);
    CHECK(vm.rec.input_present);
    CHECK(vm.rec.transport == ui::Transport::Idle);
    CHECK(vm.rec.tape.loaded);
    CHECK_EQ(vm.rec.tape.label, "");
    CHECK(!vm.rec.tape.format.empty()); /* the blank cassette still prints its format */
    CHECK_EQ(vm.rec.tape.load_seq, 0u);
    CHECK_EQ(vm.rec.info.take, "TAKE 008");
    /* idle with a loaded take: the cassette and the strip name it (a rename keeps it loaded: only the name changes) */
    s.take.name = "take_006_20261108-1105.wav";
    s.take.load_seq = 3;
    vm = map(s, {}, local);
    CHECK(vm.rec.tape.loaded);
    CHECK_EQ(vm.rec.tape.label, "Take 006");
    CHECK_EQ(vm.rec.tape.load_seq, 3u);
    CHECK_EQ(vm.rec.info.take, "TAKE 006");
    CHECK_EQ(vm.rec.tape.side, "SIDE A");
    CHECK_EQ(vm.rec.tape.format, "WAV 48k");
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
    s.take.name.clear();
    vm = map(s);
    CHECK_EQ(vm.rec.info.take, "INTERVIEW 001");
    s.settings.prefix = "take";
    s.take.name = "take_006_20261108-1105.wav"; /* the loaded take again */

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
    s.take.name = "take_006_20261108-1105.wav"; /* the controller names the loaded take until the recording starts */
    s.time.countdown_seconds = 3;
    s.time.countdown_fraction = 0.8f;
    vm = map(s, {}, local);
    CHECK(vm.rec.transport == ui::Transport::Countdown);
    CHECK_EQ(vm.rec.countdown_left, 3);
    CHECK(std::fabs(vm.rec.countdown_fraction - 0.8f) < 1e-6f);
    CHECK(!key(vm, ui::KeyId::Rec).latched);
    CHECK_EQ(vm.rec.counter_ms, 0);
    CHECK_EQ(vm.rec.tape.label, "Take 006"); /* the loaded take: the new take is not created yet */

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

void test_size_text()
{
    auto est = [](SizeKind k, uint64_t bpm) {
        SizeEstimate e;
        e.kind = k;
        e.bytes_per_minute = bpm;
        return size_per_minute_text(e);
    };
    /* MB: one decimal under 10, none from 10 (1 MB = 10^6 bytes) */
    CHECK_EQ(est(SizeKind::Exact, 5760000), "5.8 MB/min");
    CHECK_EQ(est(SizeKind::Exact, 11520000), "12 MB/min");
    CHECK_EQ(est(SizeKind::Exact, 1000000), "1.0 MB/min");
    CHECK_EQ(est(SizeKind::Exact, 9949999), "9.9 MB/min");
    CHECK_EQ(est(SizeKind::Exact, 9950000), "10 MB/min"); /* rounds up into the integer range, no "10.0" */
    CHECK_EQ(est(SizeKind::Exact, 10400000), "10 MB/min");
    CHECK_EQ(est(SizeKind::Exact, 32000000), "32 MB/min");
    /* kB under 1 MB, rounded to 5 kB */
    CHECK_EQ(est(SizeKind::Target, 244600), "245 kB/min");
    CHECK_EQ(est(SizeKind::Target, 182400), "180 kB/min");
    CHECK_EQ(est(SizeKind::Target, 182600), "185 kB/min");
    CHECK_EQ(est(SizeKind::Target, 999999), "1.0 MB/min"); /* 1000 kB is 1.0 MB */
    CHECK_EQ(est(SizeKind::Target, 997400), "995 kB/min");
    CHECK_EQ(est(SizeKind::Target, 1200), "5 kB/min");     /* never 0 */
    /* "about" only for an estimate (FLAC); a bitrate target and an exact size have none */
    CHECK_EQ(est(SizeKind::About, 1730000), "about 1.7 MB/min");
    CHECK_EQ(est(SizeKind::About, 700000), "about 700 kB/min");
    CHECK_EQ(est(SizeKind::Target, 1920000), "1.9 MB/min");
    /* no figure: blank */
    CHECK_EQ(est(SizeKind::Unknown, 5760000), "");
    CHECK_EQ(est(SizeKind::Exact, 0), "");
    /* the footer */
    FormatInputs in;
    CHECK_EQ(formats_footer_text(in), "No sound input: sizes unknown");
    in.state = CaptureState::Known;
    in.capture.rate = 48000;
    in.capture.channels = 1;
    in.capture.bits = 16;
    CHECK_EQ(formats_footer_text(in), "Sizes at 48 kHz mono 16-bit, this card");
    in.capture.rate = 44100;
    in.capture.channels = 2;
    in.capture.bits = 24;
    CHECK_EQ(formats_footer_text(in), "Sizes at 44.1 kHz stereo 24-bit, this card");
}

/* a card with several rates and 24-bit capture (mono) */
Snapshot rich_snapshot()
{
    Snapshot s = base_snapshot();
    s.format_inputs.rates = {16000, 44100, 48000, 96000};
    s.format_inputs.has24 = true;
    return s;
}

void test_codec_picker()
{
    Snapshot s = base_snapshot();
    ParamPicker p = codec_picker(s);
    /* heading, six presets, heading, three codecs */
    CHECK_EQ(p.entries.size(), size_t(11));
    CHECK_EQ(p.items.size(), size_t(11));
    CHECK(p.entries[0].header && p.items[0].header && p.entries[0].name == "Quick presets");
    CHECK(p.entries[7].header && p.entries[7].name == "Codecs");
    const char *names[] = {"WAV, 48 kHz, 16-bit", "FLAC, 48 kHz, 16-bit", "Opus speech 24 kbps",
                           "Opus speech 32 kbps", "Opus music 64 kbps",    "Opus music 128 kbps"};
    for (size_t i = 0; i < 6; ++i) {
        CHECK_EQ(p.entries[1 + i].name, std::string(names[i]));
        CHECK(p.entries[1 + i].compatible && p.items[1 + i].preset);
        CHECK(!p.entries[1 + i].size.empty());
    }
    CHECK(p.items[1].key == (FormatKey{RecFormat::Wav, ""}));
    CHECK(p.items[4].key == (FormatKey{RecFormat::Opus, "speech32"}));
    CHECK_EQ(p.entries[1].size, "5.8 MB/min"); /* 48 kHz mono 16-bit */
    CHECK(p.entries[2].size.compare(0, 6, "about ") == 0);
    CHECK(p.entries[3].size.compare(0, 6, "about ") != 0);
    CHECK_EQ(p.entries[8].name, "WAV");
    CHECK_EQ(p.entries[9].name, "FLAC");
    CHECK_EQ(p.entries[10].name, "Opus");
    CHECK(!p.items[8].preset && !p.items[8].header);
    CHECK_EQ(p.current, 8);
    CHECK_EQ(p.value, "WAV");
    CHECK_EQ(p.footer, "Sizes for 48 kHz, 16-bit, this card");
    /* line 2: the model's detail and the hours that fit in the free space (41 GiB at 5.76 MB/min) */
    CHECK_EQ(p.entries[8].detail, "uncompressed, plays everywhere \xC2\xB7 127 h free");
    CHECK(p.entries[9].detail.find("lossless, a third to half the size \xC2\xB7 ") == 0);
    s.take.free_bytes = 0;
    p = codec_picker(s);
    CHECK_EQ(p.entries[8].detail, "uncompressed, plays everywhere");

    /* the settings rows carry it all */
    s = base_snapshot();
    ui::ViewModel vm = map(s);
    const ui::SettingRow *r = find_row(vm, "codec");
    CHECK(r && r->formats.size() == 11 && r->choice == 8 && r->value == "WAV" && r->label == "Codec");
    CHECK(r && r->formats_footer == "Sizes for 48 kHz, 16-bit, this card");

    /* the current choice: FLAC, Opus (a stored preset, or none: 32 kbps) */
    s.settings.format = RecFormat::Flac;
    p = codec_picker(s);
    CHECK_EQ(p.current, 9);
    CHECK_EQ(p.value, "FLAC");
    s.settings.format = RecFormat::Opus;
    s.settings.format_preset = "music64";
    p = codec_picker(s);
    CHECK_EQ(p.current, 10);
    CHECK_EQ(p.value, "Opus");

    /* a stereo 44.1 kHz card: Opus dimmed (presets and the codec) with the reason, sizes still shown */
    s = base_snapshot();
    s.format_inputs.capture.rate = 44100;
    s.format_inputs.capture.channels = 2;
    p = codec_picker(s);
    CHECK(p.entries[1].compatible && p.entries[2].compatible && p.entries[8].compatible && p.entries[9].compatible);
    for (size_t i : {3u, 4u, 5u, 6u, 10u}) {
        CHECK(!p.entries[i].compatible);
        CHECK_EQ(p.entries[i].reason, "Opus needs 8, 12, 16, 24 or 48 kHz; this card has 44.1 kHz");
        CHECK_EQ(p.entries[i].detail, p.entries[i].reason); /* line 2 is the reason */
        CHECK(!p.entries[i].size.empty());
    }
    CHECK_EQ(p.entries[1].name, "WAV, 44.1 kHz, 16-bit");
    CHECK_EQ(p.entries[1].size, "11 MB/min"); /* 44.1 kHz stereo 16-bit: 10.58 MB */
    CHECK_EQ(p.footer, "Sizes for 44.1 kHz, 16-bit, this card");
    /* a stored Opus choice the card cannot record: the value says WAV and why, the current row is WAV */
    s.settings.format = RecFormat::Opus;
    s.settings.format_preset = "speech32";
    p = codec_picker(s);
    CHECK_EQ(p.value, "WAV (Opus unavailable)");
    CHECK_EQ(p.current, 8);

    /* a missing library */
    s = base_snapshot();
    s.format_inputs.codecs.flac = false;
    s.settings.format = RecFormat::Flac;
    p = codec_picker(s);
    CHECK(!p.entries[9].compatible && !p.entries[2].compatible);
    CHECK_EQ(p.entries[9].detail, "FLAC not installed (libflac14)");
    CHECK_EQ(p.value, "WAV (FLAC unavailable)");
    CHECK_EQ(p.current, 8);
    CHECK(p.entries[3].compatible);

    /* a 16 kHz mono headset: Opus works at its own rate, everything at 16 kHz */
    s = base_snapshot();
    s.format_inputs.capture.rate = 16000;
    p = codec_picker(s);
    CHECK_EQ(p.footer, "Sizes for 16 kHz, 16-bit, this card");
    CHECK_EQ(p.entries[8].size, "1.9 MB/min");
    for (size_t i = 0; i < p.entries.size(); ++i) CHECK(p.entries[i].compatible);

    /* no card: no sizes, the footer says so, WAV is the value */
    s = base_snapshot();
    s.format_inputs.state = CaptureState::NoCard;
    s.card.present = false;
    s.take.format_text = "0k/0 WAV";
    p = codec_picker(s);
    for (const ui::FormatEntry &e : p.entries) CHECK_EQ(e.size, "");
    CHECK_EQ(p.footer, "No sound input: sizes unknown");
    CHECK_EQ(p.value, "WAV");
    CHECK(p.entries[1].compatible && p.entries[6].compatible && p.entries[10].compatible);
    s.format_inputs.codecs.opus = false;
    p = codec_picker(s);
    CHECK(!p.entries[4].compatible);
    CHECK_EQ(p.entries[4].detail, "Opus not installed (libopusenc0)");

    /* while a take runs the rows are dimmed */
    s = base_snapshot();
    s.deck.state = kDeckRecording;
    vm = map(s);
    CHECK(!find_row(vm, "codec")->enabled && !find_row(vm, "rate")->enabled && !find_row(vm, "quality")->enabled);
}

void test_rate_quality_pickers()
{
    Snapshot s = rich_snapshot();
    /* WAV on the rich card: every rate, the current one marked, sizes at the current depth */
    ParamPicker p = rate_picker(s);
    CHECK_EQ(p.label, "Sample rate");
    CHECK_EQ(p.entries.size(), size_t(4));
    CHECK_EQ(p.entries[0].name, "16 kHz");
    CHECK_EQ(p.entries[1].name, "44.1 kHz");
    CHECK_EQ(p.entries[2].name, "48 kHz");
    CHECK_EQ(p.entries[3].name, "96 kHz");
    CHECK_EQ(p.current, 2);
    CHECK_EQ(p.value, "48 kHz");
    CHECK_EQ(p.entries[0].size, "1.9 MB/min");
    CHECK_EQ(p.entries[2].size, "5.8 MB/min");
    CHECK_EQ(p.entries[3].size, "12 MB/min");
    for (const ui::FormatEntry &e : p.entries) CHECK(e.compatible);
    CHECK_EQ(p.footer, "Sizes for WAV, 16-bit, this card");
    CHECK(p.items[1].change.stored.rate == 44100 && p.items[1].change.stored.codec == RecFormat::Wav);
    /* FLAC: the rate rows are "about" figures */
    s.settings.format = RecFormat::Flac;
    p = rate_picker(s);
    CHECK(p.entries[2].size.compare(0, 6, "about ") == 0);
    CHECK_EQ(p.footer, "Sizes for FLAC, 16-bit, this card");
    /* Opus: 44.1 and 96 kHz dimmed with the codec's rule, 16 and 48 kHz open */
    s.settings.format = RecFormat::Opus;
    p = rate_picker(s);
    CHECK(p.entries[0].compatible && !p.entries[1].compatible && p.entries[2].compatible && !p.entries[3].compatible);
    CHECK_EQ(p.entries[1].reason, "Opus needs 8, 12, 16, 24 or 48 kHz");
    CHECK_EQ(p.entries[1].detail, "Opus needs 8, 12, 16, 24 or 48 kHz");
    CHECK_EQ(p.footer, "Sizes for Opus, 32 kbps, this card");
    CHECK_EQ(p.current, 2);
    std::string why;
    CHECK(!selectable_param(p, 3, &why) && why == "Not available: Opus needs 8, 12, 16, 24 or 48 kHz");
    why.clear();
    CHECK(selectable_param(p, 0, &why) && why.empty());
    CHECK(!selectable_param(p, -1, &why) && !selectable_param(p, 4, &why) && why.empty());
    /* a chosen rate shows as the value */
    s = rich_snapshot();
    s.settings.rate = 96000;
    p = rate_picker(s);
    CHECK_EQ(p.current, 3);
    CHECK_EQ(p.value, "96 kHz");
    ui::ViewModel vm = map(s);
    CHECK_EQ(find_row(vm, "rate")->value, "96 kHz");
    CHECK(find_row(vm, "rate")->enabled);

    /* the card has one rate: one row; no card: the row is dimmed and empty */
    s = base_snapshot();
    p = rate_picker(s);
    CHECK_EQ(p.entries.size(), size_t(1));
    s.format_inputs.state = CaptureState::NoCard;
    s.card.present = false;
    p = rate_picker(s);
    CHECK(p.entries.empty());
    CHECK(!find_row(map(s), "rate")->enabled);

    /* bit depth (WAV, FLAC): 24-bit offered by the rich card, dimmed with the reason on a 16-bit card */
    s = rich_snapshot();
    p = quality_picker(s);
    CHECK_EQ(p.label, "Bit depth");
    CHECK_EQ(p.entries.size(), size_t(2));
    CHECK_EQ(p.entries[0].name, "16-bit");
    CHECK_EQ(p.entries[1].name, "24-bit");
    CHECK(p.entries[0].compatible && p.entries[1].compatible);
    CHECK_EQ(p.current, 0);
    CHECK_EQ(p.value, "16-bit");
    CHECK_EQ(p.entries[0].size, "5.8 MB/min");
    CHECK_EQ(p.entries[1].size, "8.6 MB/min");
    CHECK_EQ(p.footer, "Sizes for WAV, 48 kHz, this card");
    CHECK_EQ(find_row(map(s), "quality")->label, "Bit depth");
    s.settings.quality = 24;
    p = quality_picker(s);
    CHECK_EQ(p.current, 1);
    CHECK_EQ(p.value, "24-bit");
    s = base_snapshot();
    p = quality_picker(s);
    CHECK(p.entries[0].compatible && !p.entries[1].compatible);
    CHECK_EQ(p.entries[1].reason, "this card records 16-bit only");
    s.settings.quality = 24; /* stored, but this card has no 24-bit: 16 records */
    p = quality_picker(s);
    CHECK_EQ(p.current, 0);
    /* a 24-bit only card: 16-bit dimmed */
    s = base_snapshot();
    s.format_inputs.has16 = false;
    s.format_inputs.has24 = true;
    s.format_inputs.capture.bits = 24;
    p = quality_picker(s);
    CHECK(!p.entries[0].compatible && p.entries[1].compatible && p.current == 1);
    CHECK_EQ(p.entries[0].reason, "this card records 24-bit only");

    /* bit rate (Opus): the seven rates, the speech and music hints, 32 kbps by default, a preset's own */
    s = rich_snapshot();
    s.settings.format = RecFormat::Opus;
    p = quality_picker(s);
    CHECK_EQ(p.label, "Bit rate");
    CHECK_EQ(p.entries.size(), size_t(7));
    const char *kb[] = {"16 kbps", "24 kbps", "32 kbps", "48 kbps", "64 kbps", "96 kbps", "128 kbps"};
    for (size_t i = 0; i < 7; ++i) CHECK_EQ(p.entries[i].name, std::string(kb[i]));
    CHECK_EQ(p.current, 2);
    CHECK_EQ(p.value, "32 kbps");
    CHECK(p.entries[1].detail.compare(0, 6, "speech") == 0 && p.entries[2].detail.compare(0, 6, "speech") == 0);
    CHECK(p.entries[4].detail.compare(0, 5, "music") == 0 && p.entries[6].detail.compare(0, 5, "music") == 0);
    CHECK(p.entries[0].size.compare(0, 6, "about ") != 0);
    CHECK_EQ(p.footer, "Sizes for Opus, 48 kHz, this card");
    CHECK_EQ(find_row(map(s), "quality")->label, "Bit rate");
    CHECK_EQ(find_row(map(s), "quality")->value, "32 kbps");
    s.settings.format_preset = "music64";
    p = quality_picker(s);
    CHECK_EQ(p.current, 4); /* an old file: the preset's bit rate */
    s.settings.quality = 96; /* the quality key wins over the preset */
    p = quality_picker(s);
    CHECK_EQ(p.current, 5);
    CHECK(p.items[0].change.stored.quality == 16 && p.items[0].change.stored.preset.empty());
    /* a stored Opus choice the card cannot record: the pickers speak for WAV (what records) and build on it */
    s = base_snapshot();
    s.format_inputs.capture.rate = 44100;
    s.settings.format = RecFormat::Opus;
    s.settings.quality = 24; /* 24 kbps: never a bit depth */
    p = quality_picker(s);
    CHECK_EQ(p.label, "Bit depth");
    CHECK_EQ(p.value, "16-bit");
    CHECK(p.items[0].change.stored.codec == RecFormat::Wav && p.items[0].change.stored.quality == 16);
}

void test_param_changes()
{
    /* the codec change re-validates the rate: 44.1 kHz stereo-or-mono card with 48 kHz too */
    Snapshot s = rich_snapshot();
    s.settings.rate = 44100;
    ParamPicker p = codec_picker(s);
    CHECK_EQ(p.current, 8);
    const ParamItem &opus = p.items[10];
    CHECK(opus.compatible);
    CHECK(opus.change.stored.codec == RecFormat::Opus && opus.change.stored.rate == 48000);
    CHECK_EQ(opus.change.message, "Sample rate set to 48 kHz: Opus cannot record at 44.1 kHz");
    std::string why;
    const ParamItem *it = selectable_param(p, 10, &why);
    CHECK(it && why.empty());
    /* WAV and FLAC keep any rate */
    CHECK(p.items[9].change.stored.rate == 44100 && p.items[9].change.message.empty());
    /* presets keep nothing: rate and quality back to the defaults */
    CHECK(p.items[4].preset);
}

void test_settings()
{
    Snapshot s = base_snapshot();
    s.card.auto_gain = true;
    ui::ViewModel vm = map(s);
    std::vector<std::string> ids;
    for (const ui::SettingRow &r : vm.settings.rows) ids.push_back(r.id);
    const std::vector<std::string> want = {"input", "level", "agc", "codec", "rate", "quality", "timer", "rewfwd",
                                           "channels", "lowcut", "limiter", "sync", "folder", "prefix", "takenum", "split"};
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
    r = find_row(vm, "codec");
    CHECK(r && r->value == "WAV" && r->choices.size() == 1 && r->enabled);
    r = find_row(vm, "rate");
    CHECK(r && r->value == "48 kHz" && r->label == "Sample rate" && r->enabled);
    r = find_row(vm, "quality");
    CHECK(r && r->value == "16-bit" && r->label == "Bit depth" && r->enabled);
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
    /* Take number: the next number REC hands out, zero-padded as in the file names; 1 until it is known */
    r = find_row(vm, "takenum");
    CHECK(r && r->kind == ui::SettingKind::Number && r->label == "Take number" && r->enabled);
    CHECK(r && r->value == "Next: 001" && r->min == 1 && r->max == 9999 && r->pos == 1);
    {
        LocalState lc;
        lc.next_take = 12;
        ui::ViewModel tv = map(s, {}, lc);
        r = find_row(tv, "takenum");
        CHECK(r && r->value == "Next: 012" && r->pos == 12);
        lc.next_take = 1234;
        r = find_row(map(s, {}, lc), "takenum");
        CHECK(r && r->value == "Next: 1234" && r->pos == 1234);
        lc.next_take = 20000; /* past the setting's range: the page starts at the top of it */
        r = find_row(map(s, {}, lc), "takenum");
        CHECK(r && r->value == "Next: 20000" && r->pos == 9999);
        Snapshot rec = s;
        rec.deck.state = kDeckRecording;
        r = find_row(map(rec, {}, lc), "takenum");
        CHECK(r && !r->enabled); /* not while a take runs */
    }

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
        const std::vector<std::string> fwant = {"input", "level", "agc", "mute", "codec", "rate", "quality", "timer", "rewfwd",
                                                "channels", "lowcut", "limiter", "sync", "folder", "prefix", "takenum", "split", "output"};
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

    /* the live monitor: two rows after Limiter, only when the present card has a playback side */
    CHECK(!find_row(vm, "monitor") && !find_row(vm, "monitor_level"));
    {
        Snapshot m = base_snapshot();
        m.card.has_playback = true;
        ui::ViewModel mv = map(m);
        std::vector<std::string> mids;
        for (const ui::SettingRow &row : mv.settings.rows) mids.push_back(row.id);
        const std::vector<std::string> mwant = {"input", "level", "agc", "codec", "rate", "quality", "timer", "rewfwd",
                                                "channels", "lowcut", "limiter", "monitor", "monitor_level", "sync", "folder", "prefix", "takenum",
                                                "split"};
        CHECK(mids == mwant);
        const ui::SettingRow *sw = find_row(mv, "monitor");
        CHECK(sw && sw->kind == ui::SettingKind::Toggle && sw->label == "Monitor" && !sw->on && sw->enabled);
        const ui::SettingRow *lv = find_row(mv, "monitor_level");
        CHECK(lv && lv->kind == ui::SettingKind::Slider && lv->label == "Monitor level");
        CHECK(lv && lv->min == 0 && lv->max == 20 && lv->pos == 10 && lv->value_text == "50 %");
        CHECK(lv && lv->value_mul == 5 && lv->value_suffix == " %");
        CHECK(lv && !lv->enabled); /* dimmed, not hidden, while Monitor is off */
        m.settings.monitor = true;
        m.settings.monitor_level = 85;
        mv = map(m);
        CHECK(find_row(mv, "monitor")->on);
        lv = find_row(mv, "monitor_level");
        CHECK(lv && lv->enabled && lv->pos == 17 && lv->value_text == "85 %");
        m.settings.monitor_level = 0;
        CHECK(find_row(map(m), "monitor_level")->value_text == "0 %");
        m.settings.monitor_level = 100;
        CHECK(find_row(map(m), "monitor_level")->pos == 20 && find_row(map(m), "monitor_level")->value_text == "100 %");
        /* the MON lamp: shown with the rows; lit by the controller's REAL running state, not by the switch */
        CHECK(mv.rec.monitor_shown);
        CHECK(!mv.rec.monitor_active); /* switch on, the monitor not running (suspended or failed): dark */
        m.card.monitor_active = true;
        mv = map(m);
        CHECK(mv.rec.monitor_shown && mv.rec.monitor_active);
        {
            Snapshot w = m; /* the switch off but the monitor still running (it stops on the next tick): the real state wins */
            w.settings.monitor = false;
            CHECK(map(w).rec.monitor_active);
        }
        m.card.monitor_active = false;
        CHECK(!map(m).rec.monitor_active);
        {
            ui::ViewModel a = map(m), b = a;
            CHECK(same(a, b));
            b.rec.monitor_active = true; /* a change of the lamp alone is a change of the model */
            CHECK(!same(a, b));
            b = a;
            b.rec.monitor_shown = false;
            CHECK(!same(a, b));
        }
        /* the card is gone: the rows go with it, and the lamp */
        m.card.present = false;
        m.card.monitor_active = true;
        CHECK(!find_row(map(m), "monitor") && !find_row(map(m), "monitor_level"));
        CHECK(!map(m).rec.monitor_shown && !map(m).rec.monitor_active);
    }
    /* a card without a playback side: no lamp */
    CHECK(!vm.rec.monitor_shown && !vm.rec.monitor_active);

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

    /* while recording: Codec, Sample rate, Quality, Folder and Input dimmed */
    s.deck.state = kDeckRecording;
    vm = map(s);
    CHECK(!find_row(vm, "codec")->enabled && !find_row(vm, "folder")->enabled && !find_row(vm, "input")->enabled);
    CHECK(find_row(vm, "timer")->enabled && find_row(vm, "prefix")->enabled);

    /* a card without controls: no Input level, no Auto gain */
    s = base_snapshot();
    s.card.has_level = s.card.has_agc = false;
    vm = map(s);
    CHECK(!find_row(vm, "level") && !find_row(vm, "agc"));
    CHECK(vm.settings.rows.size() == 14);
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
    c = a;
    c.rec.hub_dps[1] = 450.0;
    CHECK(!same(a, c));
}

/* The PLAY/PAUSE key's glyph and latch for every deck state (ui_format.hpp play_key_look), the hubs (both the same way,
 * 3x while REW / FWD, a jump's flash), and the meter's source (input, or the played level while a take plays). */
void test_play_ui()
{
    struct Row {
        int deck;
        ui::Transport t;
        bool pause_glyph, latched;
        double hubs; /* degrees per second, counterclockwise positive */
    };
    const double p = ui::kHubPlayDps;
    const Row rows[] = {
        {kDeckIdle, ui::Transport::Idle, false, false, 0},
        {kDeckCountdown, ui::Transport::Countdown, false, false, 0},
        {kDeckRecording, ui::Transport::Recording, true, false, p},
        {kDeckRecordingPaused, ui::Transport::RecordingPaused, true, true, 0},
        {kDeckPlaying, ui::Transport::Playing, false, true, p},
        {kDeckPlayingPaused, ui::Transport::PlayingPaused, true, true, 0},
        {kDeckScanningForward, ui::Transport::ScanForward, false, true, 3 * p},
        {kDeckScanningReverse, ui::Transport::ScanReverse, false, true, -3 * p},
    };
    for (const Row &r : rows) {
        Snapshot s = base_snapshot();
        s.deck.state = r.deck;
        s.deck.play_pause.enabled = true;
        const ui::ViewModel vm = map(s);
        CHECK(vm.rec.transport == r.t);
        CHECK_EQ(ui::play_key_look(r.t).pause_glyph, r.pause_glyph);
        CHECK_EQ(ui::play_key_look(r.t).latched, r.latched);
        CHECK_EQ(key(vm, ui::KeyId::PlayPause).latched, r.latched);
        CHECK(std::fabs(vm.rec.hub_dps[0] - r.hubs) < 1e-9);
        CHECK(std::fabs(vm.rec.hub_dps[1] - r.hubs) < 1e-9); /* both hubs, in step */
    }

    /* the hubs between states: playing counterclockwise; FWD the same way at 3x; REW the other way at 3x */
    Snapshot s = base_snapshot();
    s.deck.state = kDeckPlaying;
    const ui::ViewModel play = map(s);
    s.deck.state = kDeckScanningForward;
    const ui::ViewModel fwd = map(s);
    s.deck.state = kDeckScanningReverse;
    const ui::ViewModel rew = map(s);
    for (int h = 0; h < 2; ++h) {
        CHECK(play.rec.hub_dps[h] > 0);
        CHECK(fwd.rec.hub_dps[h] > 0);
        CHECK(rew.rec.hub_dps[h] < 0);
        CHECK(std::fabs(fwd.rec.hub_dps[h] / play.rec.hub_dps[h] - 3.0) < 1e-9);
        CHECK(std::fabs(rew.rec.hub_dps[h] / play.rec.hub_dps[h] + 3.0) < 1e-9);
    }
    CHECK(!same(play, fwd) && !same(play, rew) && !same(fwd, rew));

    /* a 5 s jump flashes its direction at the scan speed, from play or pause; not outside the playing states */
    s.deck.state = kDeckPlayingPaused;
    s.scan.jump_dir = -1;
    CHECK(std::fabs(map(s).rec.hub_dps[0] + 3 * p) < 1e-9 && std::fabs(map(s).rec.hub_dps[1] + 3 * p) < 1e-9);
    s.deck.state = kDeckPlaying;
    s.scan.jump_dir = 1;
    CHECK(std::fabs(map(s).rec.hub_dps[0] - 3 * p) < 1e-9 && std::fabs(map(s).rec.hub_dps[1] - 3 * p) < 1e-9);
    s.deck.state = kDeckIdle;
    CHECK_EQ(map(s).rec.hub_dps[0], 0.0);
    s.scan.jump_dir = 0;

    /* the meter: idle shows the input; a mono take playing drives L and R equally from the played level, no PEAK lamp */
    s = base_snapshot();
    s.meter.ch[0].peak_lamp = true;
    s.play_meter.channels = 1;
    s.play_meter.ch[0].bar = 10;
    s.play_meter.ch[0].hold_bar = 11;
    s.play_meter.ch[0].peak_lamp = true;
    ui::ViewModel vm = map(s);
    CHECK_EQ(vm.rec.meter.channels, 1);
    CHECK_EQ(vm.rec.meter.ch[0].bar, 11); /* the input (base snapshot) */
    CHECK(vm.rec.meter.ch[0].peak_lamp);
    s.deck.state = kDeckPlaying;
    vm = map(s);
    CHECK_EQ(vm.rec.meter.channels, 2);
    for (int c = 0; c < 2; ++c) {
        CHECK_EQ(vm.rec.meter.ch[c].bar, 10);
        CHECK_EQ(vm.rec.meter.ch[c].hold_bar, 11);
        CHECK(!vm.rec.meter.ch[c].peak_lamp);
    }
    /* stereo take: each bar its channel; paused: still the played level (falling in the controller) */
    s.play_meter.channels = 2;
    s.play_meter.ch[1].bar = 4;
    s.play_meter.ch[1].hold_bar = 6;
    s.deck.state = kDeckPlayingPaused;
    vm = map(s);
    CHECK_EQ(vm.rec.meter.ch[0].bar, 10);
    CHECK_EQ(vm.rec.meter.ch[1].bar, 4);
    CHECK_EQ(vm.rec.meter.ch[1].hold_bar, 6);
    /* the playback meter not fed yet: the bars at zero (not the input's level) */
    s.play_meter = MeterSnap();
    s.deck.state = kDeckScanningReverse;
    vm = map(s);
    CHECK_EQ(vm.rec.meter.ch[0].bar, 0);
    CHECK_EQ(vm.rec.meter.ch[1].hold_bar, 0);
    /* recording: the input again, with its PEAK lamp */
    s.deck.state = kDeckRecording;
    vm = map(s);
    CHECK_EQ(vm.rec.meter.channels, 1);
    CHECK_EQ(vm.rec.meter.ch[0].bar, 11);
    CHECK(vm.rec.meter.ch[0].peak_lamp);
}

} // namespace

void test_map()
{
    test_helpers();
    test_recorder();
    test_settings();
    test_size_text();
    test_codec_picker();
    test_rate_quality_pickers();
    test_param_changes();
    test_list();
    test_trim();
    test_boxes();
    test_capture_path();
    test_same();
    test_play_ui();
}
