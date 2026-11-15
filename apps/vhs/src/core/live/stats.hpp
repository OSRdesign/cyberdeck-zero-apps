/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the measurements. Durations are collected per stage (SampleSet, any thread) and summarised once a second
 * (mean, p95, max); the system side reads the process (/proc/self/stat utime + stime, per thread from
 * /proc/self/task/<tid>/stat, VmRSS), the board temperature (/sys/class/thermal, the "cpu" zone when there is one) and
 * the Raspberry Pi firmware's throttling flags (the get_throttled node under /sys/devices/platform, else
 * `vcgencmd get_throttled`, at most every 5 s). Everything here is LVGL-free; the parsers are pure (unit-tested).
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vhs {

struct Summary {
    int n = 0;
    double mean = 0, p95 = 0, max = 0;
};
/* mean, 95th percentile (the value at rank ceil(0.95 n)), max; all 0 when v is empty */
Summary summarize(std::vector<double> v);

/* Durations (ms) added by one thread, taken by another. */
class SampleSet {
public:
    void add(double ms);
    std::vector<double> take(); /* the samples since the last take() */

private:
    std::mutex m_;
    std::vector<double> v_;
};

/* ---------------------------------------------------------------------------------------- parsers (pure) */

/* /proc/<pid>/stat or /proc/<pid>/task/<tid>/stat: utime + stime in clock ticks (fields 14 and 15, counted after the
 * command's closing parenthesis, which may hold spaces). */
bool parse_stat_ticks(const std::string &text, std::uint64_t &ticks);
/* /proc/self/status: VmRSS in kB. */
bool parse_vmrss_kb(const std::string &text, long &kb);
/* A thermal zone's temp (millidegrees) -> degrees C. */
bool parse_millideg(const std::string &text, double &c);
/* "throttled=0x50005" (vcgencmd get_throttled), "0x50005" or "50005" (the firmware's sysfs node): hex bits. */
bool parse_throttled(const std::string &text, std::uint32_t &bits);
/* CPU seconds per wall second between two readings (ticks at hz, wall in us); -1 when the interval is empty. */
double cores_between(std::uint64_t ticks0, std::uint64_t ticks1, std::int64_t us0, std::int64_t us1, long hz);
/* The firmware flags in words: "ok" for 0, else the set bits ("under-voltage now, throttled, temp-limit now ..."). */
std::string throttled_text(std::uint32_t bits);

std::string read_text(const std::string &path, std::size_t max = 65536);

/* ---------------------------------------------------------------------------------------- the system */

class SystemProbe {
public:
    /* root "" = the real system; a folder = a fake root holding proc/... and sys/... (tests). */
    explicit SystemProbe(std::string root = "");
    bool process_ticks(std::uint64_t &ticks) const;
    bool thread_ticks(int tid, std::uint64_t &ticks) const;
    double rss_mb() const; /* -1 when unknown */
    bool temp_c(double &c) const;
    /* The firmware flags: the sysfs node when there is one, else vcgencmd (run at most every 5 s; the last value in
     * between). False when neither exists (not a Raspberry Pi). */
    bool throttled(std::uint32_t &bits, std::int64_t now_us);
    /* The rpi_volt hwmon's in0_lcrit_alarm (under-voltage now, brief 5.6). False when the board has none (amd64). */
    bool undervolt_alarm(bool &on) const;
    const std::string &thermal_path() const { return thermal_; }
    const std::string &throttle_source() const { return throttle_src_; }
    const std::string &undervolt_path() const { return uv_alarm_; }
    long hz() const { return hz_; }

private:
    std::string root_, thermal_, throttle_node_, vcgencmd_, throttle_src_, uv_alarm_;
    long hz_ = 100;
    std::int64_t next_vcgencmd_us_ = 0;
    bool have_last_ = false;
    std::uint32_t last_bits_ = 0;
    int vcgencmd_failures_ = 0;
};

/* Runs argv with a time limit; its stdout (at most 4 kB). False when it could not run, timed out or failed. */
bool run_capture_output(const std::vector<std::string> &argv, int timeout_ms, std::string &out);

/* ---------------------------------------------------------------------------------------- one interval */

struct Interval {
    std::uint64_t index = 0; /* 1, 2, 3 ... */
    double secs = 0;
    std::string state;       /* the camera state ("streaming", "no-camera" ...) */
    std::string mode;        /* "MJPG 1280x720 30 fps" */
    std::string scale;       /* "auto 1/2 640x360" */
    std::string fmt;         /* "RGB565" */
    std::string box;         /* "352x198" */
    bool paused = false;
    double cap_fps = 0, dec_fps = 0, shown_fps = 0;
    std::uint64_t drop_dec = 0, drop_ui = 0, gaps = 0, errors = 0, dec_fail = 0;
    Summary dec_ms, conv_ms, ui_ms, render_ms, lat_ms;
    double cores = -1, cap_cores = -1, dec_cores = -1, ui_cores = -1;
    double rss_mb = -1;
    bool have_temp = false;
    double temp_c = 0;
    bool have_thr = false;
    std::uint32_t thr = 0;
    bool have_uv = false;    /* the rpi_volt alarm */
    bool uv = false;
    /* under-voltage now: the firmware's bit 0 or the hwmon alarm */
    bool under_voltage() const { return (have_thr && (thr & 1u)) || (have_uv && uv); }
};

/* One log line: "[vhs] stats ..." (key=value pairs, one interval). */
std::string format_log(const Interval &iv);
/* The overlay's lines (short, for a 350 px wide picture at the caption size). */
std::vector<std::string> format_overlay(const Interval &iv);

} // namespace vhs
