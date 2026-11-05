/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the link to the sensor, one object for the UI. A poll loop, no thread: call poll(now_ms) from an LVGL
 * timer every 20-50 ms (each call reads what is waiting, at most about 4 KB, and returns at once; the port is
 * non-blocking, so nothing waits). A thread buys nothing here: at 9600 baud the radar sends 580 bytes a second (19
 * frames), at 256000 about 25 KB/s at most. The detector's listen windows are driven by the same poll, so a scan
 * never blocks the screen. It writes nothing to the sensor.
 *
 * States:  Searching  a scan is running, or nothing was found and we wait for a plug-in
 *          Found      streaming frames
 *          Lost       it was found and the frames stopped (2 s without a frame, or the port went away): re-detecting
 *          Busy       no radar, and a port was skipped because another app holds it
 *          Error      no radar, and a port could not be opened or configured (permission, ...)
 * Re-detection: frames stop for lost_ms (2 s) or a read fails -> the port is released, state Lost, a scan starts with
 * the remembered port and speed first. While nothing is found, the port list is checked every rescan_ms (2 s): a new or
 * changed list starts a scan (hot-plug); busy, denied and failed ports are tried again every retry_ms (4 s); ports
 * judged "not a radar" or silent are not opened again until the list changes or rescan() is called.
 * The time stamp of a frame is the poll time (frames read in one poll share it).
 */
#pragma once

#include "detect.hpp"
#include "tracker.hpp"

#include <deque>
#include <set>
#include <string>
#include <vector>

namespace mmw {

enum class LinkState { Searching, Found, Lost, Busy, Error };
const char *link_state_name(LinkState s);

struct LinkConfig {
    DetectConfig detect;
    TrackerConfig tracker;
    Ms lost_ms = 2000;
    Ms rescan_ms = 2000;
    Ms retry_ms = 4000;
};

struct LinkStats {
    double fps = 0.0;                  /* frames per second over the last second */
    std::uint32_t frames = 0;          /* good frames since found */
    std::uint32_t bad_frames = 0;      /* bad + cut since found */
    std::uint32_t answers = 0;         /* command answers seen (not sent by us) */
    std::uint64_t bytes = 0;
    Ms last_frame_age_ms = -1;         /* -1: no frame yet */
};

class Link {
public:
    Link(Sys &sys, const LinkConfig &cfg = LinkConfig());
    ~Link();

    /* Starts (or restarts) the search; `remembered` makes it fast. */
    void start(Ms now, const Remembered &remembered = Remembered());
    /* Stops everything and releases the port (restores its settings). Safe to call twice. */
    void close();
    /* The user's "retry": forget what was judged and scan again now. */
    void rescan(Ms now);
    void poll(Ms now);

    LinkState state() const { return state_; }
    const LinkStats &stats() const { return stats_; }
    LinkStats stats(Ms now) const; /* with the last frame age at `now` */
    /* The port (when Found, Lost keeps the last one) and the speed. */
    const PortMeta &port() const { return port_.meta; }
    const std::string &port_title() const { return port_.title; }
    int baud() const { return port_.baud; }
    /* Results of the latest scan, for the "not found" page. */
    const std::vector<PortReport> &reports() const { return reports_; }
    /* "Looking at ttyUSB0 (CH340) at 256000 baud" while scanning. */
    std::string status() const;
    Tracker &tracker() { return tracker_; }
    const Tracker &tracker() const { return tracker_; }
    /* The latest decoded frame (valid when stats().frames > 0). */
    const Frame &last_frame() const { return last_; }
    /* What to store for the next fast start; true once after it changed. */
    bool take_remembered_changed();
    const Remembered &remembered() const { return remembered_; }
    /* Sensor-model line for the sensor page. */
    static const char *model() { return "LD2450-family tracking radar (LD2450, LD2452)"; }

private:
    void begin_scan(Ms now);
    void lose(Ms now);
    void after_scan(Ms now);
    void read_frames(Ms now);

    Sys &sys_;
    LinkConfig cfg_;
    Detector det_;
    Tracker tracker_;
    LinkState state_ = LinkState::Searching;
    bool started_ = false;
    bool ever_found_ = false;
    RadarPort port_;
    bool open_ = false;
    Decoder dec_;
    LinkStats stats_;
    Frame last_;
    Ms last_frame_ms_ = -1;
    Ms found_ms_ = 0;
    std::deque<Ms> arrivals_;
    std::vector<PortReport> reports_;
    std::set<std::string> settled_;
    std::set<std::string> listed_;     /* the ports of the last scan */
    Ms idle_check_ms_ = 0;
    Ms last_scan_ms_ = 0;
    bool scanning_ = false;
    Remembered remembered_;
    bool remembered_changed_ = false;
};

} // namespace mmw
