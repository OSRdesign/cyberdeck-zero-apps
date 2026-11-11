/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the Bluetooth texts (task T4, ui_format.hpp): the pickers' two-line rows, the rate, the Input picker's
 * footer, the monitor delay line, the switching line and the strip / box with it. Synthetic names only.
 */
#include "check.hpp"
#include "meter_lamps.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <cmath>

using namespace cassette::ui;

namespace {

const std::string kDot = " \xC2\xB7 ";

BtDeviceFacts dev(const char *label, const char *kind, std::uint32_t rate, const char *profile, bool connected = true,
                  bool usable = true, const char *reason = "")
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

void rates()
{
    CHECK_EQ(bt_rate_text(16000), "16 kHz");
    CHECK_EQ(bt_rate_text(8000), "8 kHz");
    CHECK_EQ(bt_rate_text(44100), "44.1 kHz"); /* the Format picker's formatting */
    CHECK_EQ(bt_rate_text(0), "");
}

void input_rows()
{
    /* a connected headset: selectable, the rate on the right, the kind and the quality under the name */
    FormatEntry e = bt_input_entry(dev("Evolve 65", "headset", 16000, "call"));
    CHECK_EQ(e.name, "Evolve 65");
    CHECK_EQ(e.size, "16 kHz");
    CHECK_EQ(e.detail, "Bluetooth headset" + kDot + "speech quality");
    CHECK(e.compatible);
    CHECK_EQ(e.reason, "");
    CHECK(!e.header);
    /* CVSD */
    CHECK_EQ(bt_input_entry(dev("Old Headset", "headset", 8000, "call")).size, "8 kHz");
    /* a speaker or another kind: "Bluetooth device" */
    CHECK_EQ(bt_input_entry(dev("Car Kit", "speaker", 16000, "call")).detail, "Bluetooth device");
    CHECK_EQ(bt_input_entry(dev("Thing", "other", 16000, "unknown")).detail, "Bluetooth device");
    CHECK_EQ(bt_input_entry(dev("Cans", "headphones", 16000, "music")).detail, "Bluetooth device");
    /* the microphone is off: dimmed, the reason replaces line 2 (the page says "Not available: <reason>") */
    e = bt_input_entry(dev("Beats Fit Pro", "headset", 16000, "music", true, false, "The headset microphone is off"));
    CHECK(!e.compatible);
    CHECK_EQ(e.detail, "The headset microphone is off");
    CHECK_EQ(e.reason, "The headset microphone is off");
    CHECK_EQ(e.size, "16 kHz"); /* kept, dimmed with the name */
    /* chosen but absent: "Not connected" (given or not) */
    e = bt_input_entry(dev("Gone Headset", "other", 0, "", false, false, "Not connected"));
    CHECK(!e.compatible);
    CHECK_EQ(e.detail, "Not connected");
    CHECK_EQ(e.size, "");
    e = bt_input_entry(dev("Gone Headset", "headset", 16000, "", false, true, ""));
    CHECK(!e.compatible);
    CHECK_EQ(e.reason, "Not connected");
}

void output_rows()
{
    FormatEntry e = bt_output_entry(dev("XWF-M18 Speaker", "speaker", 48000, "music"));
    CHECK_EQ(e.name, "XWF-M18 Speaker");
    CHECK_EQ(e.size, ""); /* no right-hand detail */
    CHECK_EQ(e.detail, "Music quality");
    CHECK(e.compatible);
    CHECK_EQ(bt_output_entry(dev("Evolve 65", "headset", 16000, "call")).detail, "Call quality (16 kHz)");
    CHECK_EQ(bt_output_entry(dev("Odd", "other", 0, "unknown")).detail, "Bluetooth");
    CHECK_EQ(bt_output_entry(dev("Odd", "other", 0, "")).detail, "Bluetooth");
    e = bt_output_entry(dev("Beats Fit Pro", "other", 0, "", false, false, "Not connected"));
    CHECK(!e.compatible);
    CHECK_EQ(e.detail, "Not connected");
    CHECK_EQ(e.reason, "Not connected");
    e = bt_output_entry(dev("Busy", "speaker", 48000, "music", true, false, "The speaker is busy"));
    CHECK(!e.compatible);
    CHECK_EQ(e.detail, "The speaker is busy");
}

void footer()
{
    CHECK_EQ(bt_input_footer(true, false), "Bluetooth headsets record speech quality (16 kHz)");
    CHECK_EQ(bt_input_footer(true, true), "Bluetooth headsets record speech quality (16 kHz)");
    CHECK_EQ(bt_input_footer(false, true), "Bluetooth audio is not set up on this board");
    CHECK_EQ(bt_input_footer(false, false), "");
}

void delay_line()
{
    /* plain: to 10 ms under 1 s, one decimal of a second from 1 s */
    CHECK_EQ(monitor_delay_text(90, false), "Delay about 90 ms");
    CHECK_EQ(monitor_delay_text(87.4, false), "Delay about 90 ms");
    CHECK_EQ(monitor_delay_text(250, false), "Delay about 250 ms");
    CHECK_EQ(monitor_delay_text(950, false), "Delay about 950 ms");
    CHECK_EQ(monitor_delay_text(994, false), "Delay about 990 ms");
    CHECK_EQ(monitor_delay_text(996, false), "Delay about 1.0 s"); /* rounds to 1000 ms */
    CHECK_EQ(monitor_delay_text(1000, false), "Delay about 1.0 s");
    CHECK_EQ(monitor_delay_text(1200, false), "Delay about 1.2 s");
    CHECK_EQ(monitor_delay_text(3, false), "Delay about 10 ms");
    CHECK_EQ(monitor_delay_text(0, false), ""); /* no figure, no notice: nothing */
    CHECK_EQ(monitor_delay_text(-5, false), "");
    /* the echo notice */
    CHECK_EQ(monitor_delay_text(250, true), "Delay about 0.25 s: you will hear an echo");
    CHECK_EQ(monitor_delay_text(90, true), "Delay about 0.09 s: you will hear an echo");
    CHECK_EQ(monitor_delay_text(950, true), "Delay about 0.95 s: you will hear an echo");
    CHECK_EQ(monitor_delay_text(1200, true), "Delay about 1.2 s: you will hear an echo");
    CHECK_EQ(monitor_delay_text(0, true), "Delay about 0.2-0.3 s: you will hear an echo");
}

void switching_line()
{
    CHECK_EQ(switching_text(0.0, 4.0), "Switching to music mode... 0 s");
    CHECK_EQ(switching_text(1.3, 4.0), "Switching to music mode... 1 s");
    CHECK_EQ(switching_text(1.99, 4.0), "Switching to music mode... 1 s");
    CHECK_EQ(switching_text(3.999, 4.0), "Switching to music mode... 3 s");
    CHECK_EQ(switching_text(6.2, 4.0), "Switching to music mode... 4 s"); /* capped at the timeout */
    CHECK_EQ(switching_text(-1.0, 4.0), "Switching to music mode... 0 s");

    /* the info box's second line and the strip's time slot */
    RecorderVm r;
    r.transport = Transport::Playing;
    r.info.take = "TAKE 006";
    r.info.play_pos_ms = 0;
    r.info.play_len_ms = 12 * 60000 + 8000;
    CHECK_EQ(info_detail(r), "00:00 / 12:08");
    r.info.play_status = switching_text(1.3, 4.0);
    CHECK_EQ(info_detail(r), "Switching to music mode... 1 s");
    CHECK_EQ(info_line(r), "PLAYING TAKE 006" + kDot + "Switching to music mode... 1 s");
    r.transport = Transport::PlayingPaused;
    CHECK_EQ(info_line(r), "PAUSED TAKE 006" + kDot + "Switching to music mode... 1 s");
    /* a standing message still wins (the card's reason) */
    r.info.notice = "No sound input";
    CHECK_EQ(info_detail(r), "No sound input");
    /* idle: play_status is not shown */
    r.info.notice.clear();
    r.transport = Transport::Idle;
    r.info.time_left = "12 h 40";
    CHECK_EQ(info_detail(r), "12 h 40 left");
}

void notices()
{
    CHECK_EQ(std::string(kCallQualityNotice), "Playing in call quality: this headset has no music mode.");
    CHECK_EQ(std::string(kBtConnectingNotice), "Connecting the headset...");
    CHECK_EQ(std::string(kBtNoSoundNotice), "The headset is connected but sends no sound.");
    /* the view model's equality sees the new fields */
    SettingRow a, b;
    b.detail = "Delay about 90 ms";
    CHECK(!(a == b));
    b = a;
    b.detail_warn = true;
    CHECK(!(a == b));
    b = a;
    b.dimmed = true;
    CHECK(!(a == b));
    b = a;
    b.choice_entries.push_back(FormatEntry());
    CHECK(!(a == b));
}

/* The landscape BT label ends at least kLandInLabelGap board px before the bar. Widths from the 480x320 render (scale 1,
 * Montserrat Bold, letter spacing 1, 2026-11-11): at 13 px "BT" is 17 px from the label's x to the end of its ink, "IN" 15;
 * they scale with the size. At IN's place and size BT left 1 px (the T touched the first step): the reason for the change. */
void bt_label_gap()
{
    auto width = [](const InLabelGeo &g) { return (std::string(g.text) == "BT" ? 17.0 : 15.0) * g.px / 13.0; };
    const InLabelGeo in = landscape_in_label(false), bt = landscape_in_label(true);
    CHECK_EQ(std::string(in.text), "IN");
    CHECK_EQ(std::string(bt.text), "BT");
    CHECK(in.x == 268 && in.px == 13); /* IN unchanged */
    CHECK(bt.px == in.px - 1);          /* one font step down */
    CHECK(bt.x == in.x - 2);            /* 2 px further left */
    CHECK(kLandBarX - (bt.x + width(bt)) >= kLandInLabelGap);
    CHECK(kLandBarX - (in.x + width(in)) >= kLandInLabelGap);
    const InLabelGeo old{268, 13, 1, "BT"};
    CHECK(kLandBarX - (old.x + width(old)) < kLandInLabelGap);
    CHECK(bt.x >= 261); /* inside the panel's 3 px frame (258 + 3) */
}

/* The echo line under Monitor (T5a): kAmber (the dark panels' amber) read 1.4:1 on the cream row and 1.09:1 on the selected
 * row. kAmberText is the same hue darkened to the lightest step with at least 4.5:1 on the row and 3:1 on the selected row. */
void text_contrast()
{
    using namespace cassette::ui::col;
    CHECK(contrast_ratio(0xFFFFFF, 0x000000) > 20.99 && contrast_ratio(0xFFFFFF, 0x000000) < 21.01);
    CHECK(contrast_ratio(kRow, kRow) == 1.0);
    CHECK(contrast_ratio(kAmber, kRow) == contrast_ratio(kRow, kAmber));
    CHECK(contrast_ratio(kAmber, kRow) < 1.5);     /* 1.40: the finding */
    CHECK(contrast_ratio(kAmber, kStrip) < 1.1);   /* 1.09 */
    CHECK(contrast_ratio(kAmberText, kRow) >= 4.5);   /* 4.61 */
    CHECK(contrast_ratio(kAmberText, kStrip) >= 3.0); /* 3.00 */
    CHECK(contrast_ratio(kAmberText, kRow) < 4.7);    /* the lightest such step, not just any dark colour */
    /* the same hue: G / R = 0xB4 / 0xF0 = 0.75, no blue */
    CHECK((kAmberText & 0xFF) == 0);
    CHECK(std::abs(double((kAmberText >> 8) & 0xFF) / double((kAmberText >> 16) & 0xFF) - 0.75) < 0.01);
    CHECK_EQ(monitor_detail_color(true), kAmberText);
    CHECK_EQ(monitor_detail_color(false), kInkMuted);
    CHECK(contrast_ratio(kInkMuted, kRow) >= 4.5);   /* the plain delay line: 7.10 */
    CHECK(contrast_ratio(kInkMuted, kStrip) >= 4.5); /* 4.63 */
    /* The name box's counter (amber from 20 of 24) is on the toolkit's dark dialog box (#1C1C1C, cp0_ui_dialog_widget.cpp),
     * not on cream: kAmber stays there (9.1:1); muted and red read too. */
    const std::uint32_t kDialogBox = 0x1C1C1C;
    CHECK_EQ(name_count_color(CountTone::Amber), kAmber);
    CHECK(contrast_ratio(kAmber, kDialogBox) >= 4.5);
    CHECK(contrast_ratio(name_count_color(CountTone::Muted), kDialogBox) >= 4.5);
    CHECK(contrast_ratio(name_count_color(CountTone::Red), kDialogBox) >= 3.0);
}

/* T9 (B27): the chosen headset not connected. The window's two lines (never the USB text); the info box (portrait, square) shows
 * the short form, the strip keeps the controller's full text. The full text (39 characters) wrapped to two lines in the box at
 * 720x720 and ran over its border (the box's line is a one-line label fitted down to its floor size; a longer text grows the label
 * downwards). The real-font fit is measured by the scenes' layout check (bt13-rec-notconnected, the seven sizes). */
void missing_texts()
{
    CHECK_EQ(std::string(kBtMissingMessage), "The Bluetooth headset is not connected.");
    CHECK_EQ(std::string(kBtMissingHint), "Found by itself when it connects.");
    CHECK_EQ(std::string(kNoInputMessage), "No sound input found. Plug in a USB sound card or headset.");
    CHECK_EQ(std::string(kNoInputHint), "Found by itself when plugged in.");
    CHECK_EQ(std::string(kBtMissingShort), "Not connected");
    RecorderVm r;
    r.input_present = false;
    r.bt_missing = true;
    r.info.notice = kBtMissingMessage;
    r.info.box_notice = kBtMissingShort;
    CHECK_EQ(info_detail(r), "Not connected");
    CHECK_EQ(info_line(r), "The Bluetooth headset is not connected."); /* the landscape strip: the whole text, one fitted line */
    CHECK(std::string(kBtMissingShort).size() * 2 < std::string(kBtMissingMessage).size()); /* under half the full text */
    /* without the short form the box shows the notice (USB reasons are unchanged) */
    r.info.box_notice.clear();
    CHECK_EQ(info_detail(r), "The Bluetooth headset is not connected.");
}

/* T9: "Not connected" on ONE line in the info box at the four layouts that have the box. The geometry is the recorder page's: the box's
 * second line is x 212, 69 wide on the portrait board (fitted from 13 down to the floor 9, scaled), x 444, 210 wide on the square one
 * (24 down to 16); s = 1 at 320x480 and 720x720, 1.5 at 480x800 (480 / 320; its body is taller), 2/3 at 480x480; positions round,
 * sizes are round(v * s). Text widths: Montserrat Medium (the TTF the boards and the scenes draw with, unitsPerEm 1000) advance per
 * glyph, each rounded to whole px as FreeType's hinted advances are: "Not connected" is 109 px at 14 px, the figure the scenes' layout
 * check measured at 480x800, where the box is 104 px. */
int not_connected_width(int px)
{
    int w = 0;
    for (char c : std::string("Not connected")) {
        int adv = 0;
        switch (c) {
        case 'N': adv = 812; break;
        case 'o': adv = 635; break;
        case 't': adv = 414; break;
        case ' ': adv = 269; break;
        case 'c': adv = 571; break;
        case 'n': adv = 681; break;
        case 'e': adv = 612; break;
        case 'd': adv = 682; break;
        default: break;
        }
        w += static_cast<int>(std::lround(adv * px / 1000.0));
    }
    return w;
}

void box_geometry()
{
    CHECK_EQ(not_connected_width(14), 109); /* the layout check's measurement at 480x800 */
    CHECK_EQ(not_connected_width(13), 97);
    struct L {
        const char *name;
        double s, x, w, max_px, floor_px;
    };
    const L layouts[] = {{"320x480", 1.0, 212, 69, 13, 9},
                         {"480x800", 1.5, 212, 69, 13, 9},
                         {"480x480", 480.0 / 720.0, 444, 210, 24, 16},
                         {"720x720", 1.0, 444, 210, 24, 16}};
    for (const L &l : layouts) {
        const int box = static_cast<int>(std::lround((l.x + l.w) * l.s) - std::lround(l.x * l.s));
        const int top = std::max(1, static_cast<int>(std::lround(l.max_px * l.s)));
        const int floor_px = std::max(9, static_cast<int>(std::lround(l.floor_px * l.s)));
        const int with_rule = largest_fit_px(top, box_detail_min_px(floor_px, true), box, not_connected_width);
        const int without = largest_fit_px(top, floor_px, box, not_connected_width);
        const bool one_line = not_connected_width(with_rule) <= box + 1; /* the layout check's tolerance */
        if (!one_line) std::printf("  %s: \"Not connected\" %d px at %d px, the box %d px\n", l.name, not_connected_width(with_rule), with_rule, box);
        CHECK(one_line);
        const bool needs_rule = not_connected_width(without) > box + 1;
        CHECK_EQ(needs_rule, std::string(l.name) == "480x800"); /* one px under the floor is needed there only */
        if (std::string(l.name) == "480x800") {
            CHECK_EQ(box, 104);
            CHECK_EQ(floor_px, 14);
            CHECK_EQ(with_rule, 13);
        } else {
            CHECK_EQ(with_rule, without); /* elsewhere the size it already had */
        }
    }
    /* the rule itself: one px under the floor for the short form only, never under 9 */
    CHECK_EQ(box_detail_min_px(14, true), 13);
    CHECK_EQ(box_detail_min_px(14, false), 14);
    CHECK_EQ(box_detail_min_px(9, true), 9);
    CHECK_EQ(box_detail_min_px(16, true), 15);
}

} // namespace

void test_bt()
{
    missing_texts();
    box_geometry();
    text_contrast();
    rates();
    input_rows();
    output_rows();
    footer();
    delay_line();
    switching_line();
    notices();
    bt_label_gap();
}
