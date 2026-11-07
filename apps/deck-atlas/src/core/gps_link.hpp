/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS reader's state machine over a GpsPortSys. Driven by tick(now_ms) from the app's 100 ms LVGL
 * timer: every call is non-blocking (open, read and list are quick system calls; nothing sleeps, no thread), reads at
 * most a few KB, and does nothing at all while the GPS is off.
 *
 * Find: every 2 s (and at once when the list of ports changed) the candidate ports are listed: in Auto only ports whose
 * USB id is a known GPS receiver (gps_ids.cpp: u-blox first; a CH340 radar, an ESP32 or any other serial device is never
 * opened, "No GPS receiver found"), or just the one the user chose (opened whatever it is). The first free one is opened (read-only, locked:
 * a port another app holds is skipped). Speed: a fixed one, or Auto: the last good one, then 9600, 4800, 38400 and
 * 115200, each listened to for 2.5 s; the port is the receiver only when two sentences with a valid NMEA checksum
 * arrived (a strict sniff; line noise at a wrong speed never passes). A port that does not talk NMEA, is busy or
 * denied is left alone for 2, 4, 8, 15, then 30 s (per port; a port that left the list starts over). A receiver that
 * goes silent for 10 s or vanishes is closed and found again the same way.
 *
 * Listen-only: the port interface has no send call (gps_port.hpp).
 */
#pragma once

#include "gps_fix.hpp"
#include "gps_port.hpp"
#include "nmea.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace atlas {

struct GpsConfig {
    bool on = false;
    std::string port;       /* "" = Auto, else a device path ("/dev/ttyACM0") */
    int baud = 0;           /* 0 = Auto, else 4800, 9600, 38400 or 115200 */
    bool operator==(const GpsConfig &o) const { return on == o.on && port == o.port && baud == o.baud; }
    bool operator!=(const GpsConfig &o) const { return !(*this == o); }
};

struct GpsTiming {
    std::int64_t sniff_ms = 2500;     /* per speed */
    std::int64_t scan_ms = 2000;      /* between looks at the port list */
    std::int64_t silence_ms = 10000;  /* a locked receiver quiet this long is lost */
};

class GpsLink {
public:
    explicit GpsLink(GpsPortSys &sys, GpsTiming timing = GpsTiming()) : sys_(sys), timing_(timing) {}
    ~GpsLink();
    GpsLink(const GpsLink &) = delete;
    GpsLink &operator=(const GpsLink &) = delete;

    /* A new setting (no change: nothing happens). Off closes the port at once. */
    void configure(const GpsConfig &cfg);
    const GpsConfig &config() const { return cfg_; }
    void tick(std::int64_t now_ms);

    GpsLinkState state() const { return state_; }
    const FixTracker &tracker() const { return tracker_; }
    GpsStatus status(std::int64_t now_ms) const { return make_status(state_, tracker_, now_ms); }
    /* The receiver in use (empty while none), its speed (0 while none), and the speed that locked last. */
    const PortInfo &port() const { return port_; }
    int baud() const { return baud_; }
    int last_good_baud() const { return last_good_baud_; }

    /* The events worth a log line ("opened", "locked at 9600", "lost: ..."). */
    void set_log(std::function<void(const std::string &)> fn) { log_ = std::move(fn); }

    /* The pause after the n-th failure in a row (0-based): 2, 4, 8, 15, 30, 30... s. */
    static std::int64_t backoff_ms(int failures);
    /* The order Auto tries the speeds in, `last` (0 = none) first. */
    static std::vector<int> baud_order(int last);

private:
    struct PortFail {
        int count = 0;
        std::int64_t retry_at = 0;
        PortErr err = PortErr::None;
    };

    void scan(std::int64_t now);
    bool begin(const PortInfo &p, std::int64_t now, PortErr &err);
    void read_some(std::int64_t now);
    void next_baud_or_fail(std::int64_t now);
    void fail_port(const std::string &path, PortErr err, std::int64_t now);
    void lose(const char *why, std::int64_t now);
    void close_port();
    void say(const std::string &s) const { if (log_) log_(s); }

    GpsPortSys &sys_;
    GpsTiming timing_;
    GpsConfig cfg_;
    GpsLinkState state_ = GpsLinkState::Off;
    int handle_ = -1;
    PortInfo port_;
    int baud_ = 0;
    std::vector<int> bauds_;
    std::size_t baud_idx_ = 0;
    std::int64_t sniff_start_ = 0, last_valid_ms_ = 0, next_scan_ms_ = 0;
    int valid_count_ = 0;
    int last_good_baud_ = 0;
    std::vector<NmeaMsg> pending_;      /* what the sniff heard, handed to the tracker at the lock */
    std::map<std::string, PortFail> fails_;
    std::vector<std::string> last_listing_;
    NmeaSplitter splitter_;
    FixTracker tracker_;
    std::function<void(const std::string &)> log_;
};

} // namespace atlas
