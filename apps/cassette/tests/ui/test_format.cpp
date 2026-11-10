/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the texts (ui_format.hpp) and the LED bar steps.
 */
#include "check.hpp"
#include "ui_format.hpp"

using namespace cassette::ui;

namespace {

void name_counter()
{
    CHECK_EQ(name_count_text(0), "0/24");
    CHECK_EQ(name_count_text(9), "9/24");
    CHECK_EQ(name_count_text(24), "24/24");
    for (std::size_t n = 0; n <= 19; ++n) {
        CHECK(name_count_tone(n) == CountTone::Muted);
        CHECK_EQ(name_count_color(name_count_tone(n)), 0x9B9FA3u);
    }
    for (std::size_t n = 20; n <= 23; ++n) {
        CHECK(name_count_tone(n) == CountTone::Amber);
        CHECK_EQ(name_count_color(name_count_tone(n)), 0xF0B400u);
    }
    CHECK(name_count_tone(24) == CountTone::Red);
    CHECK_EQ(name_count_color(name_count_tone(24)), 0xE5372Bu);
    CHECK(name_count_tone(30) == CountTone::Red); /* never more than the limit in practice; still red */
}

void times()
{
    CHECK_EQ(fmt_counter(0), "00:00");
    CHECK_EQ(fmt_counter(42999), "00:42");
    CHECK_EQ(fmt_counter(-5), "00:00");
    CHECK_EQ(fmt_counter(59 * 60000 + 59999), "59:59");
    CHECK(!counter_long(3599999));
    CHECK(counter_long(3600000));
    CHECK_EQ(fmt_counter(3600000), "1:00:00");
    CHECK_EQ(fmt_counter(3600000 + 2 * 60000 + 3000), "1:02:03");
    CHECK_EQ(fmt_counter(10LL * 3600000 + 5000), "10:00:05");

    CHECK_EQ(fmt_mmss(5 * 60000 + 44200), "05:44");
    CHECK_EQ(fmt_mmss(12 * 60000 + 8000), "12:08");

    CHECK_EQ(fmt_tenths(2 * 60000 + 58700), "02:58.7");
    CHECK_EQ(fmt_tenths(10 * 60000 + 28799), "10:28.7");
    CHECK_EQ(fmt_tenths(7 * 60000 + 30000), "07:30.0");
    CHECK_EQ(fmt_tenths(3600000 + 58700), "1:00:58.7");
    CHECK_EQ(fmt_tenths_short(5 * 60000 + 44200), "5:44.2");
    CHECK_EQ(fmt_tenths_short(3600000 + 44200), "1:00:44.2");

    CHECK_EQ(fmt_short(2 * 60000 + 58700), "2:59");
    CHECK_EQ(fmt_short(10 * 60000 + 28700), "10:29");
    CHECK_EQ(fmt_short(7 * 60000 + 30000), "7:30");
    CHECK_EQ(fmt_short(3600000 + 59600), "1:01:00");

    CHECK_EQ(fmt_ruler(5 * 60000 + 14200), "5:14");
    CHECK_EQ(fmt_ruler(-1), "0:00");
    CHECK_EQ(fmt_ruler(3600000 + 61000), "1:01:01");

    CHECK_EQ(fmt_zoom(60000), "60 s");
    CHECK_EQ(fmt_zoom(2000), "2 s");
    CHECK_EQ(fmt_zoom(1500), "1.5 s");
    CHECK_EQ(fmt_zoom(99000), "99 s");
    CHECK_EQ(fmt_zoom(12 * 60000 + 8000), "12 min");
}

void levels()
{
    CHECK_EQ(fmt_db(13.0, true), "+13.0 dB");
    CHECK_EQ(fmt_db(13.0, false), "+13 dB");
    CHECK_EQ(fmt_db(6.0, true), "+6.0 dB");
    CHECK_EQ(fmt_db(23.8, true), "+23.8 dB");
    CHECK_EQ(fmt_db(23.8, false), "+24 dB");
    CHECK_EQ(fmt_db(0.0, true), "0.0 dB");
    CHECK_EQ(fmt_db(0.04, true), "0.0 dB");
    CHECK_EQ(fmt_db(-3.0, true), "-3.0 dB");
}

void info()
{
    RecorderVm r;
    r.transport = Transport::Recording;
    r.info.take = "TAKE 007";
    r.info.format = "48k/16 WAV";
    r.info.time_left = "12 h 40";
    CHECK_EQ(info_line(r), "TAKE 007 \xC2\xB7 48k/16 WAV \xC2\xB7 12 h 40 left");
    CHECK_EQ(info_title(r), "TAKE 007");
    CHECK_EQ(info_detail(r), "12 h 40 left");
    CHECK_EQ(info_state(r), "REC");
    /* the square layout's wide detail: the format in front, while recording or idle */
    CHECK_EQ(info_detail_wide(r), "48k/16 WAV \xC2\xB7 12 h 40 left");
    r.info.lost_ms = 300;
    CHECK_EQ(info_detail_wide(r), "48k/16 WAV \xC2\xB7 lost 0.3 s");
    r.info.lost_ms = 0;
    r.info.notice = "Saved";
    CHECK_EQ(info_detail_wide(r), "Saved");
    r.info.notice.clear();
    r.info.format.clear();
    CHECK_EQ(info_detail_wide(r), "12 h 40 left");
    r.info.format = "48k/16 WAV";

    r.transport = Transport::ScanReverse;
    r.info.take = "TAKE 006";
    r.info.play_pos_ms = 5 * 60000 + 44000;
    r.info.play_len_ms = 12 * 60000 + 8000;
    CHECK_EQ(info_line(r), "PLAYING TAKE 006 \xC2\xB7 05:44 / 12:08 \xC2\xB7 REW 5x");
    CHECK_EQ(info_detail(r), "05:44 / 12:08");
    CHECK_EQ(info_detail_wide(r), "05:44 / 12:08");
    CHECK_EQ(info_state(r), "PLAY");
    r.transport = Transport::ScanForward;
    CHECK_EQ(info_line(r), "PLAYING TAKE 006 \xC2\xB7 05:44 / 12:08 \xC2\xB7 FWD 5x");
    r.transport = Transport::PlayingPaused;
    CHECK_EQ(info_state(r), "");
    /* a paused take reads PAUSED, not PLAYING (the strip only; the portrait box is unchanged); scanning keeps PLAYING */
    CHECK_EQ(info_line(r), "PAUSED TAKE 006 \xC2\xB7 05:44 / 12:08");
    CHECK_EQ(info_title(r), "TAKE 006");
    CHECK_EQ(info_detail(r), "05:44 / 12:08");
    r.transport = Transport::Playing;
    CHECK_EQ(info_line(r), "PLAYING TAKE 006 \xC2\xB7 05:44 / 12:08");
    r.info.take.clear();
    CHECK_EQ(info_line(r), "PLAYING \xC2\xB7 05:44 / 12:08");
    r.transport = Transport::PlayingPaused;
    CHECK_EQ(info_line(r), "PAUSED \xC2\xB7 05:44 / 12:08");
    r.info.take = "TAKE 006";
    r.info.notice = "Sound card unplugged: take kept";
    CHECK_EQ(info_line(r), "Sound card unplugged: take kept");

    RecorderVm idle;
    idle.info.take = "TAKE 008";
    CHECK_EQ(info_line(idle), "TAKE 008");
    CHECK_EQ(info_state(idle), "");

    /* through the sound server: recording marks the format; idle leads with the card */
    RecorderVm sv;
    sv.transport = Transport::Recording;
    sv.info.take = "TAKE 007";
    sv.info.format = "48k/16 WAV";
    sv.info.time_left = "12 h 40";
    sv.info.server = true;
    CHECK_EQ(info_line(sv), "TAKE 007 \xC2\xB7 48k/16 WAV (server) \xC2\xB7 12 h 40 left");
    CHECK_EQ(info_title(sv), "TAKE 007");
    CHECK_EQ(info_detail(sv), "(server) 12 h 40");
    sv.transport = Transport::Idle;
    sv.info.card = "USB AUDIO CODEC";
    CHECK_EQ(info_line(sv), "USB AUDIO CODEC (server) \xC2\xB7 TAKE 007 \xC2\xB7 12 h 40 left");
    CHECK_EQ(info_detail(sv), "(server) 12 h 40");
    sv.info.server = false;
    sv.info.card.clear();
    CHECK_EQ(info_line(sv), "TAKE 007 \xC2\xB7 48k/16 WAV \xC2\xB7 12 h 40 left"); /* direct: as before */

    /* lost audio: one decimal, last on the strip, alone in the box, only while recording */
    RecorderVm lo;
    lo.transport = Transport::Recording;
    lo.info.take = "TAKE 007";
    lo.info.format = "48k/16 WAV";
    lo.info.time_left = "12 h 40";
    CHECK_EQ(info_lost(lo), "");
    lo.info.lost_ms = 300;
    CHECK_EQ(info_lost(lo), "lost 0.3 s");
    CHECK_EQ(info_line(lo), "TAKE 007 \xC2\xB7 48k/16 WAV \xC2\xB7 lost 0.3 s");
    CHECK_EQ(info_detail(lo), "lost 0.3 s");
    lo.info.lost_ms = 12345;
    CHECK_EQ(info_lost(lo), "lost 12.3 s");
    lo.info.lost_ms = 20;
    CHECK_EQ(info_lost(lo), "lost <0.1 s");
    lo.info.lost_ms = 300;
    lo.info.server = true;
    CHECK_EQ(info_line(lo), "TAKE 007 \xC2\xB7 48k/16 WAV (server) \xC2\xB7 lost 0.3 s");
    lo.transport = Transport::Idle;
    CHECK_EQ(info_lost(lo), ""); /* the count belongs to a take */
}

void list_and_trim()
{
    ListVm l;
    l.take_count = 12;
    l.folder = "~/Recordings";
    l.free_space = "41 GB";
    l.free_hours = "117 h";
    l.format_short = "48k/16";
    CHECK_EQ(list_strip_left(l, false), "12 TAKES \xC2\xB7 ~/Recordings");
    CHECK_EQ(list_strip_left(l, true), "12 TAKES");
    CHECK_EQ(list_strip_right(l, false), "41 GB FREE \xC2\xB7 117 h at 48k/16");
    CHECK_EQ(list_strip_right(l, true), "41 GB FREE \xC2\xB7 117 h");
    l.take_count = 1;
    CHECK_EQ(list_strip_left(l, true), "1 TAKE");
    TakeRow t;
    t.name = "Take 006";
    l.takes = {t};
    l.selected = 0;
    CHECK_EQ(list_hint(l), "Take 006 selected. Esc returns to the recorder.");
    l.selected = -1;
    CHECK_EQ(list_hint(l), "Esc returns to the recorder.");

    TrimVm tr;
    tr.in_ms = 2 * 60000 + 58700;
    tr.out_ms = 10 * 60000 + 28700;
    tr.splits = {320000, 345000};
    CHECK_EQ(trim_strip_wide(tr), "IN 02:58.7 \xC2\xB7 OUT 10:28.7 \xC2\xB7 LEN 07:30.0 \xC2\xB7 2 splits");
    CHECK_EQ(trim_strip_narrow_top(tr), "2 splits \xC2\xB7 LEN 7:30");
    CHECK_EQ(trim_strip_narrow_left(tr), "IN 2:59 \xC2\xB7 OUT 10:29");
    CHECK_EQ(trim_strip_narrow_right(tr), "");
    tr.selected_split = 1;
    CHECK_EQ(trim_strip_narrow_right(tr), "SPLIT 2 SELECTED");
    CHECK_EQ(save_summary(tr, 3, false), "Keep 02:58.7 to 10:28.7, cut at 2 splits: 3 files.");
    CHECK_EQ(save_summary(tr, 3, true), "Keep 2:59 to 10:29, cut at 2 splits: 3 files.");
    tr.splits.clear();
    CHECK_EQ(splits_text(0), "no splits");
    CHECK_EQ(splits_text(1), "1 split");
    CHECK_EQ(save_summary(tr, 1, false), "Keep 02:58.7 to 10:28.7: 1 file.");
    CHECK_EQ(strip_wav("take_006_20261108-1105-1.wav"), "take_006_20261108-1105-1");
    CHECK_EQ(strip_wav("x.flac"), "x.flac");
    CHECK_EQ(more_below({}), "");
    CHECK_EQ(more_below({"REW / FWD", "Channels"}), "more below: REW / FWD, Channels");
    auto upto = [](std::size_t n) { return [n](const std::string &s) { return s.size() <= n; }; };
    CHECK_EQ(more_below_fit({"REW / FWD", "Channels"}, upto(100)), "more below: REW / FWD, Channels");
    CHECK_EQ(more_below_fit({"REW / FWD", "Channels", "Limiter"}, upto(30)), "more below: REW / FWD, ...");
    CHECK_EQ(more_below_fit({"A very long setting name"}, upto(10)), "more below: ...");
    CHECK_EQ(more_below_fit({}, upto(10)), "");
}

/* The PLAY / PAUSE key shows the current state; the hubs turn together, 3x and signed for FWD / REW. */
void play_key_and_hubs()
{
    struct Row {
        Transport t;
        bool glyph_pause, latched;
        double dps;
    };
    const Row rows[] = {
        {Transport::Idle, false, false, 0},
        {Transport::Countdown, false, false, 0},
        {Transport::Recording, true, false, kHubPlayDps},
        {Transport::RecordingPaused, true, true, 0},
        {Transport::Playing, false, true, kHubPlayDps},
        {Transport::PlayingPaused, true, true, 0},
        {Transport::ScanForward, false, true, 3 * kHubPlayDps},
        {Transport::ScanReverse, false, true, -3 * kHubPlayDps},
    };
    for (const Row &r : rows) {
        CHECK_EQ(play_key_look(r.t).pause_glyph, r.glyph_pause);
        CHECK_EQ(play_key_look(r.t).latched, r.latched);
        CHECK(hub_speed_dps(r.t) == r.dps);
    }
    CHECK(kHubPlayDps > 0); /* counterclockwise while playing */
    /* a jump flash wins: forward 3x counterclockwise, backward 3x clockwise, even from a paused take */
    CHECK(hub_speed_dps(Transport::PlayingPaused, 1) == 3 * kHubPlayDps);
    CHECK(hub_speed_dps(Transport::Playing, -1) == -3 * kHubPlayDps);
}

} // namespace

void list_row_heights()
{
    /* the deck: 57 px row, name box 20, detail box 16, a 22 px name line: one line leaves the row as it was */
    ListRowGeom g = list_row_geom(57, 20, 16, 22, 22);
    CHECK_EQ(g.h, 57);
    CHECK_EQ(g.name_box, 20);
    CHECK_EQ(g.detail_y, (57 - 2 - 36) / 2 + 20);
    CHECK_EQ(g.name_y, (57 - 2 - 36) / 2 + (20 - 22) / 2);
    /* two lines (44 px of name): the row grows by the name box's growth, the detail sits under the name, the block stays centred */
    g = list_row_geom(57, 20, 16, 44, 22);
    CHECK_EQ(g.name_box, 44);
    CHECK_EQ(g.h, 57 + 24);
    CHECK_EQ(g.name_y, (g.h - 2 - 60) / 2);
    CHECK_EQ(g.detail_y, g.name_y + 44);
    CHECK(g.name_y + 44 + 16 <= g.h - 2); /* nothing runs out of the row */
    /* three lines grow it further, monotonic */
    const ListRowGeom g3 = list_row_geom(57, 20, 16, 66, 22);
    CHECK(g3.h > g.h);
    CHECK_EQ(g3.h - g.h, 22);
    CHECK(g3.name_y >= 0);
    /* a small row (the 320 x 170 style 42 px row) keeps its one-line geometry too */
    g = list_row_geom(42, 20, 16, 22, 22);
    CHECK_EQ(g.h, 42);
    CHECK_EQ(g.detail_y, (42 - 2 - 36) / 2 + 20);
    /* a text height equal to the line is one line, never a growth */
    CHECK_EQ(list_row_geom(42, 20, 16, 19, 19).h, 42);
    CHECK_EQ(list_row_geom(42, 20, 16, 38, 19).h, 42 + 18);
}

void test_format()
{
    list_row_heights();
    times();
    name_counter();
    levels();
    info();
    list_and_trim();
    play_key_and_hubs();
}

void test_meter_steps()
{
    /* the core meter's rule: step i (0..14) lit when db >= -40 + i * 40 / 14 */
    CHECK_EQ(bar_steps(-120.0f), 0);
    CHECK_EQ(bar_steps(-40.01f), 0);
    CHECK_EQ(bar_steps(-40.0f), 1);
    CHECK_EQ(bar_steps(-37.2f), 1);
    CHECK_EQ(bar_steps(-37.1f), 2);
    CHECK_EQ(bar_steps(-18.0f), 8);
    CHECK_EQ(bar_steps(-6.0f), 12);
    CHECK_EQ(bar_steps(-0.01f), 14);
    CHECK_EQ(bar_steps(0.0f), 15);
    CHECK_EQ(bar_steps(3.0f), 15);
    for (int i = 0; i < 15; ++i) CHECK_EQ(bar_steps(-40.0f + float(i) * (40.0f / 14.0f)), i + 1);
    CHECK(led_band(0) == LedBand::Green);
    CHECK(led_band(9) == LedBand::Green);
    CHECK(led_band(10) == LedBand::Amber);
    CHECK(led_band(12) == LedBand::Amber);
    CHECK(led_band(13) == LedBand::Red);
    CHECK(led_band(14) == LedBand::Red);
}
