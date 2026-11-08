/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the texts (ui_format.hpp) and the LED bar steps.
 */
#include "check.hpp"
#include "ui_format.hpp"

using namespace cassette::ui;

namespace {

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

    r.transport = Transport::ScanReverse;
    r.info.take = "TAKE 006";
    r.info.play_pos_ms = 5 * 60000 + 44000;
    r.info.play_len_ms = 12 * 60000 + 8000;
    CHECK_EQ(info_line(r), "PLAYING TAKE 006 \xC2\xB7 05:44 / 12:08 \xC2\xB7 REW 5x");
    CHECK_EQ(info_detail(r), "05:44 / 12:08");
    CHECK_EQ(info_state(r), "PLAY");
    r.transport = Transport::ScanForward;
    CHECK_EQ(info_line(r), "PLAYING TAKE 006 \xC2\xB7 05:44 / 12:08 \xC2\xB7 FWD 5x");
    r.transport = Transport::PlayingPaused;
    CHECK_EQ(info_state(r), "");
    r.info.notice = "Sound card unplugged: take kept";
    CHECK_EQ(info_line(r), "Sound card unplugged: take kept");

    RecorderVm idle;
    idle.info.take = "TAKE 008";
    CHECK_EQ(info_line(idle), "TAKE 008");
    CHECK_EQ(info_state(idle), "");
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

} // namespace

void test_format()
{
    times();
    levels();
    info();
    list_and_trim();
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
