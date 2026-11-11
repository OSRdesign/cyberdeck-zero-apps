/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: the Bluetooth texts (task T4, ui_format.hpp): the pickers' two-line rows, the rate, the Input picker's
 * footer, the monitor delay line, the switching line and the strip / box with it. Synthetic names only.
 */
#include "check.hpp"
#include "meter_lamps.hpp"
#include "ui_format.hpp"

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

} // namespace

void test_bt()
{
    rates();
    input_rows();
    output_rows();
    footer();
    delay_line();
    switching_line();
    notices();
    bt_label_gap();
}
