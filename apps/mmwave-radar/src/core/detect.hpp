/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the listen-only port detector. A non-blocking state machine: start() once, then poll(now_ms) every
 * 20-50 ms (an LVGL timer is fine; one poll reads what is there and returns). It never writes to a port (port_sys.hpp
 * has no write call).
 *
 * Per candidate port (ttyUSB*, ttyACM*; the remembered one first):
 *   1. skipped as "busy (held by another app)" when another process has it open (/proc), when the exclusive open fails
 *      (TIOCEXCL held elsewhere) or when its flock is taken (Mesh Hop, Serial Console, another instance);
 *   2. for each speed (the remembered one for this port, then 256000, 9600, 115200, 57600, 38400, 19200, 230400,
 *      460800): raw 8N1, listen 600 ms for the first speed and 400 ms for each next one; the radar is found after 3
 *      consecutive valid tracking frames; GPS (NMEA, UBX) and the LD2410 are named and left; a port that sends nothing
 *      in the first 600 ms is silent and left; 90 bytes without any header move on to the next speed at once;
 *   3. the port is restored (original termios) and released unless it is the radar, which stays open, locked and
 *      configured, and is handed to the caller by take_radar().
 * A total time budget ends the scan; the ports not reached are reported as skipped.
 */
#pragma once

#include "frame.hpp"
#include "port_sys.hpp"
#include "sniff.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace mmw {

using Ms = std::int64_t;

/* What the app remembers to start fast: the port identity (port_id) and the speed it worked at. */
struct Remembered {
    std::string port_id;
    int baud = 0;
    bool empty() const { return port_id.empty() || baud <= 0; }
    std::string serialize() const;                  /* "port=<id>\nbaud=<n>\n" */
    static Remembered parse(const std::string &text); /* tolerant: bad text gives an empty one */
};

enum class Verdict {
    Radar,       /* found: this is the radar */
    Busy,        /* held by another app */
    Denied,      /* permission denied (dialout) */
    Silent,      /* no byte at all */
    NotRadar,    /* something else (GPS, LD2410, text, unknown) */
    Gone,        /* vanished while probing */
    Error,       /* could not be opened or configured */
    Skipped      /* not reached (time budget, cancel) */
};

struct PortReport {
    PortMeta meta;
    std::string id;       /* port_id(meta) */
    std::string title;    /* port_title(meta) */
    Verdict verdict = Verdict::Skipped;
    int baud = 0;         /* the speed the radar or the other device was recognised at (0: none) */
    std::string text;     /* "busy (held by another app)", "not a supported radar: GPS receiver (NMEA)", ... */
};

struct DetectConfig {
    std::vector<int> bauds = {256000, 9600, 115200, 57600, 38400, 19200, 230400, 460800};
    Ms first_listen_ms = 600;
    Ms next_listen_ms = 400;
    Ms total_budget_ms = 8000;
    std::size_t early_garbage_bytes = 90;
    std::uint32_t frames_needed = 3;
};

/* The result handed over when a radar was found: the open, locked, configured port. The caller must close() it
 * through the same Sys. */
struct RadarPort {
    int handle = -1;
    PortMeta meta;
    std::string id;
    std::string title;
    int baud = 0;
};

class Detector {
public:
    Detector(Sys &sys, const DetectConfig &cfg = DetectConfig()) : sys_(sys), cfg_(cfg) {}
    ~Detector();

    /* Begins a scan. `settled` paths (already judged not-a-radar or silent) are not opened again. */
    void start(Ms now, const Remembered &remembered, const std::set<std::string> &settled = std::set<std::string>());
    /* Abandons a running scan: closes the port being probed, the rest are "skipped". */
    void cancel();
    void poll(Ms now);

    bool running() const { return state_ != State::Idle && state_ != State::Done; }
    bool finished() const { return state_ == State::Done; }
    bool found() const { return found_; }
    /* Hands the radar over (once). */
    RadarPort take_radar();
    /* One report per candidate port, in scan order. */
    const std::vector<PortReport> &reports() const { return reports_; }
    /* What is being tried: "Looking at ttyUSB0 (CH340) at 256000 baud". Empty when not running. */
    std::string status() const;

private:
    enum class State { Idle, NextPort, Listen, Done };
    void begin_port(Ms now);
    void begin_baud(Ms now);
    void end_port(Verdict v, const std::string &text, int baud);
    void next_baud(Ms now);
    void finish(Ms now);

    Sys &sys_;
    DetectConfig cfg_;
    State state_ = State::Idle;
    Remembered remembered_;
    std::vector<PortMeta> queue_;
    std::size_t qi_ = 0;
    std::vector<PortReport> reports_;
    Ms start_ms_ = 0;

    /* the port being probed */
    int handle_ = -1;
    std::vector<int> bauds_;
    std::size_t bi_ = 0;
    Ms window_start_ = 0;
    Ms window_len_ = 0;
    bool first_baud_ = true;
    bool any_bytes_ = false;
    Decoder dec_;
    Sniffer sniff_;
    RadarPort radar_;
    bool found_ = false;
};

} // namespace mmw
