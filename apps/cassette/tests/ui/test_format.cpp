/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the texts (ui_format.hpp) and the LED bar steps.
 */
#include "check.hpp"
#include "ui_format.hpp"

#include <cmath>

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
    /* a paused take reads PAUSED, not PLAYING (the strip only; the portrait box is unchanged); scanning keeps PLAYING */
    CHECK_EQ(info_line(r), "PAUSED TAKE 006 \xC2\xB7 05:44 / 12:08");
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

/* The info box beside the counter: line 1 the codec (never the take's name), line 2 the time left as before */
void info_box()
{
    RecorderVm r;
    r.info.take = "TAKE 007 WITH A LONG NAME";
    r.info.time_left = "955 h";
    r.info.format = "48k/16 WAV";
    r.info.codec = "WAV 48k";
    for (Transport t : {Transport::Idle, Transport::Countdown, Transport::Recording, Transport::RecordingPaused}) {
        r.transport = t;
        CHECK_EQ(info_codec(r), "WAV 48k");
        CHECK_EQ(info_detail(r), "955 h left");
    }
    r.info.codec = "FLAC 44.1k";
    CHECK_EQ(info_codec(r), "FLAC 44.1k");
    r.info.codec = "OPUS 48k";
    CHECK_EQ(info_codec(r), "OPUS 48k"); /* no bit rate known: none shown */
    for (int kbps : {16, 24, 32, 48, 64, 96, 128}) {
        r.info.kbps = kbps;
        CHECK_EQ(info_codec(r), "OPUS 48k \xC2\xB7 " + std::to_string(kbps) + " kbps");
    }
    r.transport = Transport::Recording;
    r.info.lost_ms = 300;
    CHECK_EQ(info_codec(r), "OPUS 48k \xC2\xB7 128 kbps"); /* lost audio only changes line 2 */
    CHECK_EQ(info_detail(r), "lost 0.3 s");
    r.info.lost_ms = 0;
    r.info.server = true;
    CHECK_EQ(info_detail(r), "(server) 955 h");
    r.info.server = false;
    /* a take plays: the codec and rate of the take in the deck (the tape's), line 2 the position over the length */
    r.transport = Transport::Playing;
    r.tape.format = "FLAC 44.1k";
    r.info.play_pos_ms = 5000;
    r.info.play_len_ms = 60000;
    CHECK_EQ(info_codec(r), "FLAC 44.1k");
    CHECK_EQ(info_detail(r), "00:05 / 01:00");
    r.transport = Transport::PlayingPaused;
    CHECK_EQ(info_codec(r), "FLAC 44.1k");
    r.transport = Transport::Idle;
    CHECK_EQ(info_codec(r), "OPUS 48k \xC2\xB7 128 kbps"); /* idle again: the Settings choice, not the loaded take */
    /* no input: the codec of the Settings, line 2 as it was (the notice, else the time left) */
    RecorderVm n;
    n.info.codec = "WAV";
    n.info.notice = "No sound input found";
    CHECK_EQ(info_codec(n), "WAV");
    CHECK_EQ(info_detail(n), "No sound input found");
    n.info.notice.clear();
    CHECK_EQ(info_detail(n), "");
}

/* The font rule: the largest size at which the text fits one line */
void font_fit_rule()
{
    auto width = [](int px) { return px * 10; }; /* a text 10 px wide per px of size */
    CHECK_EQ(largest_fit_px(15, 6, 1000, width), 15); /* fits at the top: the top */
    CHECK_EQ(largest_fit_px(15, 6, 150, width), 15);
    CHECK_EQ(largest_fit_px(15, 6, 149, width), 14);
    CHECK_EQ(largest_fit_px(15, 6, 100, width), 10);
    CHECK_EQ(largest_fit_px(15, 6, 61, width), 6);
    CHECK_EQ(largest_fit_px(15, 6, 10, width), 6); /* nothing fits: the smallest */
    CHECK_EQ(largest_fit_px(6, 6, 10, width), 6);
    CHECK_EQ(largest_fit_px(4, 6, 1000, width), 4); /* a maximum under the minimum stays the maximum */
    /* a width that grows with the size, the longest box line: the result fits and one more would not */
    const std::string longest = "OPUS 48k \xC2\xB7 128 kbps";
    auto w2 = [&](int px) { return static_cast<int>(longest.size()) * px * 6 / 10 - 2 * px / 10; };
    for (int box = 40; box <= 400; box += 7) {
        const int p = largest_fit_px(30, 6, box, w2);
        if (p > 6) CHECK(w2(p) <= box);
        if (p < 30) CHECK(w2(p + 1) > box);
    }
}

/* the cassette label's name: 8 px per character, a writing area of 160 px (20 characters on a line) */
void label_wrap()
{
    auto width = [](const std::string &s) {
        int n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) ++n;
        return n * 8;
    };
    const int W = 160;
    const std::string base = "Interview with Mary Ann 2026 ok";
    for (std::size_t n = 1; n <= 24; ++n) {
        std::string name = base.substr(0, n); /* every cut: with and without spaces in it */
        while (!name.empty() && name.back() == ' ') name.pop_back();
        const NameWrap w = wrap_name(name, W, 2, width);
        CHECK(w.fits);
        CHECK(w.lines.size() >= 1 && w.lines.size() <= 2);
        for (const std::string &l : w.lines) {
            CHECK(width(l) <= W);
            CHECK(!l.empty());
        }
        std::string joined = w.lines[0];
        if (w.lines.size() == 2) joined += " " + w.lines[1];
        CHECK_EQ(joined, name);
        CHECK_EQ(w.lines.size() == 1, width(name) <= W); /* one line exactly when it fits */
    }
    /* 25 characters: two lines, split at the space that balances them */
    NameWrap w = wrap_name("Interview with Mary Ann 2", W, 2, width);
    CHECK_EQ(w.lines.size(), 2u);
    CHECK_EQ(w.lines[0], "Interview with");
    CHECK_EQ(w.lines[1], "Mary Ann 2");
    w = wrap_name("aaaa bbbb cccc dddd eeee ffff", 170, 2, width);
    CHECK(w.fits);
    CHECK_EQ(w.lines[0], "aaaa bbbb cccc");
    CHECK_EQ(w.lines[1], "dddd eeee ffff");
    /* a word wider than a line is broken inside it */
    w = wrap_name("Supercalifragilisticexpialidocious", W, 2, width); /* 34 characters: 20 + 14 */
    CHECK(w.fits);
    CHECK_EQ(w.lines.size(), 2u);
    CHECK_EQ(w.lines[0].size() + w.lines[1].size(), 34u);
    CHECK(width(w.lines[0]) <= W && width(w.lines[1]) <= W);
    /* too long for two lines: the last ends with dots and fits */
    w = wrap_name(std::string(60, 'x'), W, 2, width);
    CHECK(!w.fits);
    CHECK_EQ(w.lines.size(), 2u);
    CHECK(width(w.lines[1]) <= W);
    CHECK_EQ(w.lines[1].substr(w.lines[1].size() - 3), "...");
    /* multi-byte characters are not cut in the middle */
    std::string uml;
    for (int i = 0; i < 24; ++i) uml += "\xC3\xA4";
    w = wrap_name(uml, W, 2, width);
    CHECK(w.fits);
    CHECK_EQ(w.lines[0].size(), 40u); /* 20 two-byte characters */
    CHECK_EQ(w.lines[1].size(), 8u);

    /* the rung: the largest at which the name fits the area, one line before two */
    const std::vector<int> rungs = {31, 29, 22, 15};
    auto rw = [](int r, const std::string &s) { return static_cast<int>(s.size()) * r * 4 / 10; }; /* 0.4 em per character */
    auto lh = [](int r) { return r * 5 / 4; };
    NameLayout l = layout_name_text("Take 007", rungs, 22, 160, 36, rw, lh);
    CHECK_EQ(l.rung, 22);
    CHECK_EQ(l.wrap.lines.size(), 1u);
    CHECK_EQ(l.text_h, lh(22));
    l = layout_name_text("Interview with Mary Ann 2", rungs, 22, 160, 36, rw, lh); /* one line needs 220 px at 22 */
    CHECK(l.wrap.fits);
    CHECK(l.text_h <= 36);
    CHECK(l.rung < 22 || l.wrap.lines.size() == 1);
    for (const std::string &s : l.wrap.lines) CHECK(rw(l.rung, s) <= 160);
    /* a wanted size above the ladder's top is the top, below it the smallest rung */
    l = layout_name_text("Take 007", rungs, 40, 400, 100, rw, lh);
    CHECK_EQ(l.rung, 31);
    l = layout_name_text("Take 007", rungs, 10, 400, 100, rw, lh);
    CHECK_EQ(l.rung, 15);
    /* a pitch of rung + 1, the second line adds one pitch */
    l = layout_name_text("abcdefghij klmnopqrstu", rungs, 15, 100, 36, rw, lh);
    CHECK_EQ(l.wrap.lines.size(), 2u);
    CHECK_EQ(l.pitch, 16);
    CHECK_EQ(l.text_h, 16 + lh(15));
    CHECK(l.text_h <= 36);
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
    /* merge mode: a reason that wraps to two lines (34 px against the 16 px detail box) grows the row by 18, the name on top */
    g = list_row_geom(57, 20, 16, 22, 22, 34);
    CHECK_EQ(g.h, 57 + 18);
    CHECK_EQ(g.name_box, 20);
    CHECK_EQ(g.detail_y, (g.h - 2 - (20 + 34)) / 2 + 20);
    CHECK(g.detail_y + 34 <= g.h - 2);
    /* a reason of one line (or none) changes nothing; with a wrapped name both add up */
    CHECK_EQ(list_row_geom(57, 20, 16, 22, 22, 16).h, 57);
    CHECK_EQ(list_row_geom(57, 20, 16, 22, 22, 0).h, 57);
    CHECK_EQ(list_row_geom(57, 20, 16, 44, 22, 34).h, 57 + 24 + 18);
}

/* the list's row keys: five keys are exactly the trim page's five; merge mode's two keys share the same span */
void list_keys()
{
    for (int i = 0; i < 5; ++i) {
        const KeyBox l = list_key_box(i, 5, false), p = list_key_box(i, 5, true);
        CHECK(std::fabs(l.x - (16.0 + 91 * i)) < 1e-9 && std::fabs(l.w - 84) < 1e-9 && l.y == 222 && l.h == 56);
        CHECK(std::fabs(p.x - (16.0 + 59 * i)) < 1e-9 && std::fabs(p.w - 52) < 1e-9 && p.y == 396 && p.h == 64);
    }
    const KeyBox c = list_key_box(0, 2, false), m = list_key_box(1, 2, false);
    CHECK(std::fabs(c.x - 16) < 1e-9 && std::fabs(m.x + m.w - 464) < 1e-9 && std::fabs(m.x - (c.x + c.w) - 7) < 1e-9);
    const KeyBox pc = list_key_box(0, 2, true), pm = list_key_box(1, 2, true);
    CHECK(std::fabs(pc.w - 140.5) < 1e-9 && std::fabs(pm.x + pm.w - 304) < 1e-9);
    /* every key keeps the toolkit's minimum on the 320 x 480 board (44 px wide and high at scale 1) */
    for (int n : {2, 5}) {
        CHECK(list_key_box(0, n, true).w >= 44);
        CHECK(list_key_box(0, n, true).h >= 44);
    }
    /* out of range indexes stay on the row */
    CHECK(std::fabs(list_key_box(9, 5, false).x - list_key_box(4, 5, false).x) < 1e-9);
    /* merge mode's hint */
    ListVm l;
    l.merge_mode = true;
    CHECK_EQ(list_hint(l), "Choose the takes in order, then MERGE. Esc cancels.");
}

void test_format()
{
    list_row_heights();
    list_keys();
    times();
    name_counter();
    levels();
    info();
    info_box();
    font_fit_rule();
    label_wrap();
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
