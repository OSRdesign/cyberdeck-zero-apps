/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: what the Local devices, Wi-Fi networks and Wi-Fi walk test pages say and compute (no LVGL, no network:
 * tests/test_ui_model_04.cpp). Like view_model.hpp every text, tone and number of the pages is made here from the core's
 * snapshots, so the pages only place it. Plain ASCII only (the fonts of the small screen have no arrows or dots).
 *
 *   Local devices  the status line, a device row's texts (name, address, vendor, MAC, round trip, the router / this device
 *                  tags), the Device page's info rows, the port scan's status line and rows, the saved Wake-on-LAN machine
 *   Wi-Fi networks the status line and the problems with their fix, a network row (signal bars, dBm, channel, band,
 *                  security words), the detail rows with the one-line meaning of the security, the channels view (bars per
 *                  channel, the busiest, the least crowded as a sentence)
 *   Walk test      the status line, the big reading, the chart's window and scale, the summary card, the weak-zone setting
 */
#pragma once

#include "view_model.hpp"
#include "view_model_03.hpp" /* Box */

#include "devices.hpp"
#include "settings.hpp"
#include "walktest.hpp"
#include "wifiscan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* ================================================================ Local devices */

constexpr const char *kDevicesHint = "Scans only your own network. Use it on networks you own or are allowed to test.";
constexpr const char *kPrivateAddressWords = "Private (random) address";

/* "2 ms", "<1 ms", "14 ms", "120 ms" */
std::string rtt_text(double ms);
/* the name, else the address */
std::string device_title(const Device &d);
/* the vendor, "Private (random) address", "Vendor unknown" (a MAC without a known company) or "" (no MAC) */
std::string device_vendor_text(const Device &d);

/* a row of the list: line 1 the title and the address at the right, line 2 the vendor, the MAC and the round trip */
struct DeviceRowTexts {
    std::string title;   /* line 1, left: the name, else the address */
    std::string ip;      /* line 1, right: "192.168.68.20" ("" when the title is the address) */
    std::string detail;  /* line 2, left: "Raspberry Pi  b8:27:eb:10:20:30" */
    std::string vendor;  /* the detail without the MAC: what a narrow row shows when the whole detail does not fit (portrait) */
    std::string rtt;     /* line 2, right: "2 ms", "" when it did not answer a ping */
    std::string tag;     /* "router", "this device" or "" (a shape and a word) */
    bool router = false, self = false;
};
DeviceRowTexts device_row_texts(const Device &d);

/* the status line: "Press Scan ...", "Scanning 192.168.68.0/24: 112 of 254, 9 found", "Looking up names: 14 devices on
 * 192.168.68.0/24", "Done: 14 devices on 192.168.68.0/24", "Stopped: 9 devices on ...", the core's error in red */
Band devices_band(const DevicesSnapshot &s);
/* the short words of the status line for the core's refusal (the whole words are in the list) */
std::string devices_headline(const std::string &error);
/* what the list says instead of devices: the hint, "Looking for devices...", the error and its fix, "No device answered" */
struct DevicesEmpty {
    std::string text;
    Tone tone = Tone::Normal;
    std::string fix;     /* a second line for an error ("" none) */
};
DevicesEmpty devices_empty(const DevicesSnapshot &s);
/* the fix under the core's refusal ("Connect to a network first ...", "Only your own private network can be scanned.") */
std::string devices_fix(const std::string &error);

/* the Device page: IP, Name (with where it came from), MAC, Vendor, Round trip, Role */
std::vector<RowDef> device_info_rows(const Device &d);
/* "Gateway: the router of this network", "This device", "Another device" */
std::string device_role(const Device &d);

/* the Device page's status line when no port scan is shown: "192.168.68.1, router" */
std::string device_band_text(const Device &d);

/* the actions of the Device page (labels) */
constexpr const char *kActPing = "Ping this device";
constexpr const char *kActPort = "Check a port";
constexpr const char *kActScanCommon = "Scan common ports";
constexpr const char *kActScanAll = "Scan ports 1-1024";
constexpr const char *kActSaveWol = "Save for Wake-on-LAN";
constexpr const char *kActStopScan = "Stop the port scan";

/* the port scan's status line: "Scanning 312 of 1024: 2 open", "Done: 3 open ports", "No open port found among the common
 * ones", "Stopped at 312 of 1024: 2 open", the refusal in red; Idle gives "" */
Band portscan_band(const PortScanSnapshot &s);
/* the list's sentence for a finished scan that found nothing ("No open port found among the common ones"), else "" */
std::string portscan_empty_text(const PortScanSnapshot &s);
/* a row of an open port: label "Port 22  ssh", value "open" */
RowDef open_port_row(const OpenPort &p);

/* the machine "Save for Wake-on-LAN" adds: the device's name (cut to 32 characters, made new with " 2", " 3" ...), its MAC and
 * its address to watch. False when the device has no MAC (then *why says so). */
bool wol_machine_of(const Device &d, const Settings &settings, WolMachine &out, std::string *why);
/* "Saved NAS for Wake-on-LAN", or the core's reason ("At most 20 machines can be saved: remove one first.") */
std::string wol_save_text(const std::string &name);
/* a machine already saved with this MAC: its name, "" when none */
std::string wol_machine_with_mac(const Settings &settings, const std::string &mac);

/* ================================================================ Wi-Fi networks */

/* 1 to 4 bars: from -50 dBm 4, from -60 dBm 3, from -70 dBm 2, else 1 (the words Excellent, Good, Fair, Weak) */
int signal_bars(double dbm);

/* a row of the list: SSID or "(hidden)", the signal at the right, the caption "ch 36, 5 GHz, WPA2" */
struct WifiRowTexts {
    std::string title;
    std::string dbm;       /* "-52 dBm" */
    int bars = 1;
    bool in_use = false;
    std::string connected; /* "connected" for the network in use, else "" */
    std::string caption;   /* "ch 36, 5 GHz" */
    std::string security;  /* "WPA2": the second part of the caption, drawn apart (amber for Open and WEP) */
    bool weak_security = false;
};
WifiRowTexts wifi_row_texts(const WifiNetwork &n);

/* The status line: "14 networks on wlan0, scanned 3 s ago", "Scanning wlan0", "No Wi-Fi adapter on eth0: choose wlan0 under
 * Network". now_ms: the clock of the snapshot's list_at_ms. wifi_hint: the name of a Wi-Fi interface of the board ("" none). */
Band wifi_band(const WifiSnapshot &s, std::int64_t now_ms, const std::string &wifi_hint);
/* the problem in words, for a row under the bar */
struct WifiProblem {
    std::string headline; /* the status line */
    std::string words;    /* the core's words */
    std::string fix;      /* what to do ("" none) */
};
WifiProblem wifi_problem(const std::string &error, const std::string &wifi_hint);
/* "scanned 3 s ago", "scanned just now" */
std::string scanned_ago(std::int64_t now_ms, std::int64_t list_at_ms);

/* the one-line meaning of a security class ("Open: anyone near can read your traffic.") */
std::string security_meaning(WifiSecurity s);
/* the detail page: SSID, BSSID, Vendor, Signal, Channel, Frequency, Band, Bit rate, Security (the meaning is a paragraph under
 * it: security_meaning). gone: the network is not in the last list (its last values), a Status row says so. */
std::vector<RowDef> wifi_detail_rows(const WifiNetwork &n, bool gone);
Band wifi_detail_band(const WifiNetwork &n, bool gone, const WifiSnapshot &s, std::int64_t now_ms, const std::string &wifi_hint);

/* the Channels view */
constexpr const char *kChannelNote149 = "Channels 149 to 165 are not allowed for Wi-Fi in every country.";
constexpr const char *kChannelBarsNote = "Bars show how busy each channel is, neighbouring ones included.";
struct ChannelBar {
    int channel = 0;
    int networks = 0;
    int weight = 0;
    bool best = false;     /* the least crowded one */
    bool busiest = false;
};
struct ChannelsView {
    WifiBand band = WifiBand::Unknown;
    std::string title;     /* "2.4 GHz" */
    std::vector<ChannelBar> bars; /* 2.4 GHz: 1 to 13 (14 when seen); 5 GHz: what the core lists */
    int top = 1;           /* the weight of the tallest bar, at least 1 */
    std::string busiest;   /* "Busiest: channel 6, 4 networks" ("" when nothing was seen) */
    std::string empty;     /* "No network seen on 2.4 GHz" ("" when there is something) */
};
ChannelsView channels_view(const BandChannels &b, WifiBand band);
/* "Least crowded: channel 11 (2.4 GHz), channel 44 (5 GHz)", "Not enough networks seen yet" */
std::string channels_sentence(const ChannelSummary &c);
/* whether a 5 GHz channel list holds one of 149 to 165 (the note about countries) */
bool channels_have_149(const ChannelsView &v);

/* The channels block: the bars of each band (side by side on a wide screen when both have data, else one over the other); the
 * sentence and the notes are rows of the list around it. Boxes in the block's coordinates; lh: a line of caption text, slh: of
 * the small text. portrait (PDA mode, 0.5.0): always one over the other, each band over the whole width. */
struct ChannelsLayout {
    bool side = false;
    Box first, second;
    int total_h = 0;
};
ChannelsLayout channels_layout(int w, int lh, int slh, int bars_h, int pad, bool first_data, bool second_data,
                               bool portrait = false);

/* ================================================================ Wi-Fi walk test */

constexpr const char *kWalkNote = "Walk slowly and keep the deck in your hand; stop at the dead spot to read the numbers.";
constexpr int kWalkWindowS = 150; /* the chart shows the last 2.5 minutes while it walks */

/* "01:23", "12:05", "1:02:03" (the hours only when there are some) */
std::string mmss(double seconds);
/* the weak-zone setting: the label "Weak below" and its value "-75 dBm"; Enter cycles -70, -75, -80 */
std::string weak_value(int dbm);
int next_weak_dbm(int dbm);

/* The live block of the page in a row w wide and h high: the reading at the left and the chart at the right when the area is
 * wide, else the reading over the chart; the legend under the chart. portrait (PDA mode, 0.5.0): always the reading over
 * the chart (also in the shorter block of a stopped walk with its summary). */
struct WalkLayout {
    bool side = false;
    Box reading, chart, legend;
};
WalkLayout walk_layout(int w, int h, int lh, int slh, int big_h, int pad, bool portrait = false);

/* Start / Stop (the page's first cell) */
std::string walk_start_label(const WalkSnapshot &s);

/* "Press Start, then walk", "Walking: 01:23, weakest -78 dBm", "Stopped: 01:23, weakest -78 dBm", the problem */
Band walk_band(const WalkSnapshot &s, const std::string &wifi_hint);

/* the big reading */
struct WalkLive {
    std::string big;       /* "-62", "--" */
    std::string unit;      /* "dBm" ("" when there is no reading) */
    std::string word;      /* "Excellent", "Good", "Fair", "Weak", "No signal", "Ready" */
    Tone tone = Tone::Normal;
    Mark mark = Mark::None;
    std::string zone;      /* "Below -75 dBm: the weak zone" when it is ("" else) */
    std::string where;     /* "HomeNet on wlan0" ("" when not known) */
    std::string elapsed;   /* "01:23" */
};
WalkLive walk_live(const WalkSnapshot &s);

/* The chart's time window in seconds from the start of the walk: the last kWalkWindowS while it walks (growing from 30 s),
 * the whole kept walk (at least 30 s) when stopped. */
struct WalkWindow {
    double t0_s = 0, t1_s = 30;
};
WalkWindow walk_window(const WalkSnapshot &s);
/* the chart's dBm range, in steps of 10: it holds the readings and the threshold, at least 30 dB, inside -100 .. -20 */
struct WalkYScale {
    int top = -40, bottom = -90;
};
WalkYScale walk_y_scale(const std::vector<WalkSample> &samples, int weak_dbm);
/* the legend line under the chart: "Dashed line: -75 dBm. Squares: weak. Ticks: access point changes" */
std::string walk_legend(const WalkSnapshot &s);

/* The summary card: Duration, Samples, Weakest, Average, Strongest, Time in the weak zone, Dips, Access point changes, then a
 * row per change (at most 5). Rows with nothing to say are left out. */
std::vector<RowDef> walk_summary_rows(const WalkSnapshot &s);
/* "no signal" seconds note ("No link for 4 s") or "" */
std::string walk_nolink_note(const WalkSnapshot &s);

} // namespace ui
} // namespace nt
