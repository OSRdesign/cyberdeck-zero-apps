/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: what the speed test, Wake-on-LAN and Bluetooth radar pages say and compute (no LVGL, no network:
 * tests/test_ui_model_03.cpp). Like view_model.hpp, every text, tone and number of the pages is made here from the core's
 * snapshots, so the pages only place it. Plain ASCII only (the fonts of the small screen have no arrows or dots).
 *
 *   Speed test   the gauge's scale steps, the needle's angle, the tick labels, the easing, the grade lines, the result rows,
 *                the status line, the layout of the live block (gauge, chart, progress) for a width and a height
 *   Wake-on-LAN  the wake status (word, detail, shape), the one-second strip, the list rows' last-status text
 *   Bluetooth    the status line and the plain-words problems with their one-line fix, the radar's blip angle, radius and
 *                brightness, the sweep angle, the list rows, the device details, the radar's layout
 */
#pragma once

#include "view_model.hpp"

#include "ble.hpp"
#include "settings.hpp"
#include "speedtest.hpp"
#include "wol.hpp"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace nt {
namespace ui {

/* ================================================================ speed test */

/* the dial: 220 degrees of arc, from -110 to +110 degrees around straight up (clockwise positive) */
constexpr double kGaugeSweepDeg = 220.0;
constexpr double kGaugeHalfSweepDeg = kGaugeSweepDeg / 2.0;
/* the steps of the dial's scale in Mbit/s: 10, 25, 50, 100, 250, 500, 1000 */
const std::vector<double> &gauge_steps();
/* the smallest step that holds `mbps` with a little room (the value stays under 92 % of the scale); 1000 at most */
double gauge_scale_for(double mbps);

/* The dial's scale during a run: it steps up with the highest value seen and never comes back down until reset(). */
struct GaugeScaler {
    double scale = 10;
    double max_seen = 0;
    void reset();
    /* a new value; true when the scale stepped up */
    bool feed(double mbps);
};

/* value / scale, clamped to 0..1 */
double gauge_fraction(double value, double scale);
/* the needle's angle in degrees around straight up: -110 at 0, +110 at the full scale */
double gauge_needle_deg(double value, double scale);

struct GaugeTick {
    double value = 0;
    std::string label; /* "" for a minor tick */
    bool major = false;
};
/* 6 numbered ticks (0 to the scale in 5 equal steps) with one minor tick between each pair */
std::vector<GaugeTick> gauge_ticks(double scale);
/* "0", "20", "2.5" */
std::string gauge_number(double value);

/* the needle's value on its way from `from` to `to`: ease-out over dur_ms; elapsed_ms 0 gives from, dur_ms and more give to */
double ease_out(double from, double to, double elapsed_ms, double dur_ms);

/* "23.4", "234", "0.40": what the big number of the dial and of the result card says; "--" for a negative value */
std::string speed_value_text(double mbps);
/* "18 ms", "3.2 ms" */
std::string ms_text(double ms);
/* "+34 ms", "-2 ms" */
std::string ms_delta_text(double ms);
/* "Uses up to about 150 MB each way. Start it yourself; on a phone hotspot that is mobile data." */
std::string speed_note(std::uint64_t max_bytes);
/* "Latency", "Download", "Upload"; "Ready" when idle, "Finished", "Stopped", "Failed" */
std::string speed_phase_label(const SpeedSnapshot &s);

/* the elapsed fraction of the phase in progress (0 in the latency phase, phase_s / target_s clamped) and "4 s of 8 s" */
double speed_progress(const SpeedSnapshot &s, double target_s);
std::string speed_progress_text(const SpeedSnapshot &s, double target_s);

/* The grade: letter, one plain-words line, tone and a shape (never the colour alone). */
std::string grade_line(BloatGrade g);
Tone grade_tone(BloatGrade g);
Mark grade_mark(BloatGrade g);
/* the grade of the whole result, and the line shown with it ("F: latency explodes under load: heavy buffering in the router",
 * for an unknown grade the core's note: "Not measured: ...") */
struct SpeedGrade {
    BloatGrade grade = BloatGrade::Unknown;
    std::string letter;  /* "A" .. "F", "-" */
    std::string line;
    Tone tone = Tone::Normal;
    Mark mark = Mark::None;
};
SpeedGrade speed_grade(const SpeedResult &r);

/* The result card's rows under the two big values: Latency, Jitter, Latency while downloading / uploading (value and
 * increase), Server. A row with nothing to say is left out. */
std::vector<RowDef> speed_rows(const SpeedSnapshot &s);
/* "Cloudflare, CDG" */
std::string speed_server_text(const SpeedResult &r);
/* the plain-words notes under the card: the ping note, a direction's note ("Measured over 3.2 s only: ..."), "" none */
std::vector<std::string> speed_notes(const SpeedSnapshot &s);

/* the page's status line (never ends with dots). target_s: the seconds each direction lasts. */
Band speed_band(const SpeedSnapshot &s, double target_s);
/* the short head of an error: "No connection", "Secure connection failed", "Cloudflare refused the test", else
 * "Speed test failed"; the whole words are in the card */
std::string speed_error_title(const std::string &error);

/* a rectangle */
struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

/* The live block of the speed page (gauge, chart, progress) in a row w wide and h high: side by side when that gives the
 * bigger dial, else stacked. lh: a line of caption text. */
struct SpeedLayout {
    bool side = false;
    Box gauge;     /* the dial's box (the dial is centred in it) */
    Box chart;
    Box progress;  /* the thin bar and its text */
    int radius = 0; /* the dial's outer radius */
    int cx = 0, cy = 0; /* the dial's centre, in the block's coordinates */
};
SpeedLayout speed_layout(int w, int h, int lh, int big_h, int pad);
/* the dial's radii, from its outer radius (the numbers, the ticks, the needle's tip) */
struct GaugeRadii {
    int outer = 0, track = 0, inner = 0, tick_major = 0, tick_minor = 0, label = 0, needle = 0, hub = 0;
};
GaugeRadii gauge_radii(int outer);
/* the point at `deg` degrees around straight up, `radius` from (cx, cy) */
void polar_point(int cx, int cy, double radius, double deg, double &x, double &y);

/* ================================================================ Wake-on-LAN */

/* "Packet sent on wlan0, wlan1" from the core's send result; failures in words ("... Failed on wlan1: network unreachable") */
std::string wol_sent_text(const WolSendResult &r);

enum class WakeKind { None, Sending, Sent, Waking, Awake, NoAnswer, Stopped, Failed };
/* what the wake watch's badge says: a shape and a word, never colour alone */
struct WakeStatus {
    WakeKind kind = WakeKind::None;
    std::string word;   /* "Sent", "Waking... 12 s", "Awake after 22 s", "No answer yet" */
    std::string detail; /* "Packet sent on wlan0, wlan1", the hint for no answer */
    Tone tone = Tone::Normal;
    Mark mark = Mark::None;
    int seconds = 0;    /* waited, or the time to the answer */
    bool spinning = false;
};
WakeStatus wake_status(const WolSnapshot &s);

/* the strip of one-second cells: grey while waiting, green from the first answer on, empty for the seconds to come */
enum class StripCell { Future, Waiting, Awake };
constexpr int kWakeStripCells = 120;
std::vector<StripCell> wake_strip(const WolSnapshot &s, int cells = kWakeStripCells);

/* the last wake of a machine, kept while the app runs, for the list ("Sent 14:02", "Awake after 22 s", "No answer yet") */
struct WakeLast {
    WakeKind kind = WakeKind::None;
    std::time_t sent_at = 0;
    int seconds = 0;
};
WakeLast wake_last_of(const WolSnapshot &s, std::time_t sent_at);
std::string wake_last_text(const WakeLast &last);
Tone wake_last_tone(const WakeLast &last);

/* the page's status line */
Band wol_band(const WolSnapshot &s, const std::string &machine);
/* why a typed value is not accepted: "" when it is. The name must be new (compared without case) unless `own` is it. */
std::string wol_name_problem(const std::string &name, const Settings &settings, const std::string &own);
std::string wol_mac_problem(const std::string &text);
std::string wol_host_problem(const std::string &text); /* "" (nothing to watch) is accepted */
constexpr const char *kWolFullText = "You can save up to 20 machines: delete one first.";
constexpr const char *kWolNote = "The target must have Wake-on-LAN switched on; it usually works over Ethernet, rarely over Wi-Fi.";

/* ================================================================ Bluetooth radar */

constexpr const char *kBleWarnText = "Wi-Fi and Bluetooth share one radio: a Bluetooth keyboard may stutter while scanning.";
constexpr const char *kBleLegend = "Signal strength, not distance";
constexpr const char *kBleLegendLine = "Rings show signal strength, not distance.";
constexpr const char *kBleFirstUse =
    "Press Start to listen for Bluetooth devices around you. The scan lasts up to a minute and stops by itself. Nothing "
    "is connected or paired.";
constexpr double kRadarTurnMs = 3000.0; /* one turn of the sweep */

/* "Scanning, 14 devices, 37 s left", "Stopped, 23 devices seen", "Finished, 23 devices seen", the problem's headline */
Band ble_band(const BleSnapshot &s);

struct BlePlain {
    std::string headline; /* "Bluetooth is switched off" */
    std::string words;    /* the core's words */
    std::string fix;      /* "Turn Bluetooth on in Settings > Bluetooth" ("" when there is none) */
};
BlePlain ble_problem(const std::string &error);

/* the blip of a device: its angle is a stable hash of the address (degrees clockwise from straight up, 0 up to 360) */
double blip_angle_deg(const std::string &address);
/* its distance from the centre as a fraction of the radar's radius, from the smoothed RSSI: the near class lies inside the
 * inner ring (up to 1/3), medium between the rings (up to 2/3), far outside (up to 1); a stronger signal is nearer inside
 * its class; never 0 and never over 0.95 */
double blip_radius_frac(double smooth_dbm);
/* the blip's brightness, 0 .. 255: it fades with the time since it was last heard (gone after expiry_ms: 0), and pulses
 * when the sweep has just passed it (sweep_deg < 0: no sweep, a steady middle brightness) */
int blip_alpha(std::int64_t age_ms, int expiry_ms, double sweep_deg, double angle_deg);
/* the sweep's angle: one turn per kRadarTurnMs */
double sweep_deg_at(std::int64_t ms);

/* "-72 dBm, near" */
std::string ble_row_value(const BleDevice &d);
/* the list row's name: the name, else the address */
std::string ble_row_label(const BleDevice &d);
/* "just now", "2 s ago", "1 min 5 s ago" */
std::string ago_text(std::int64_t ms);
/* the device's details as rows: Address, Type, Vendor, Signal, Smoothed signal, TX power, Last seen, Heard */
std::vector<RowDef> ble_detail_rows(const BleDevice &d, std::int64_t now_ms);

/* The page's layout: the radar's box and the list's, side by side or stacked, in an area w x h. */
struct BleLayout {
    bool side = false;
    Box radar;
    Box list;
};
BleLayout ble_layout(int w, int h, int row_h, int gap);

} // namespace ui
} // namespace nt
