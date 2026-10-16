/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the Wi-Fi walk test, the core (no LVGL; tests/test_walktest.cpp on /proc/net/wireless and `iw` output
 * recorded on a Pi 3A+ and on scripted walks).
 *
 * The signal of the link the chosen interface is connected to, once a second, while the user walks with the board:
 *   - the level: /proc/net/wireless (any user, no command, a fresh reading at every read: on the Pi 3A+ standing still
 *     -62, -64, -65, -65, -66 dBm over five seconds); when that file lacks the interface (or it is not associated), the
 *     `signal:` line of `/usr/sbin/iw dev <if> link` (interfaces.hpp: parse_iw_link);
 *   - the access point: `iw dev <if> link` every sample when iw is installed (a short process a second through the
 *     command runner, 1.5 s timeout); its BSSID tells when the board moves to another access point (roaming). Without iw
 *     the walk runs on the level only and says that roaming is not known.
 * Nothing is sent: both only read what the adapter knows.
 *
 * The walk: start, stop (the samples and the summary stay), reset (back to nothing). The last 10 minutes of samples are
 * kept for the chart (a ring of kWalkMaxSamples); the summary covers the whole walk:
 *   duration (first sample to last, plus one interval), samples, weakest dBm and when (seconds from the start), the
 *   average (mean of the dBm readings), the strongest and when; the time in the weak zone (samples below the threshold,
 *   default -75 dBm, setting `walk_weak_dbm`; a sample without link counts as weak too, and is counted apart) in seconds
 *   (weak samples x the interval) and in % of the samples; the dips: a dip starts with the first weak sample and ends once
 *   the signal has been back at or above the threshold for 2 s (hysteresis: a short rise does not end it, so a signal
 *   that hovers at the threshold makes one dip, not ten); the access point changes (roaming) with their time.
 */
#pragma once

#include "cmdrun.hpp"
#include "source.hpp"
#include "wifiscan.hpp"
#include "worker.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

constexpr int kWalkWeakDbm = -75, kWalkWeakMinDbm = -95, kWalkWeakMaxDbm = -40;
constexpr int kWalkIntervalMs = 1000;
constexpr int kWalkHysteresisMs = 2000;
constexpr std::size_t kWalkMaxSamples = 600; /* 10 minutes at 1 Hz */
constexpr std::size_t kWalkMaxEvents = 64;   /* dips and access point changes kept in the lists (all are counted) */

/* ---------------------------------------------------------------- readings (pure) */

/* The level of `iface` in /proc/net/wireless (parse_net_wireless: an interface that is not associated has none). */
bool proc_wireless_dbm(const std::string &text, const std::string &iface, int &dbm);

struct WalkReading {
    bool linked = false;        /* a signal was read */
    int dbm = 0;
    bool from_iw = false;       /* the level came from iw (the proc file had none for the interface) */
    bool has_link = false;      /* iw answered this time: bssid and ssid are its word ("" = not connected) */
    std::string bssid, ssid;
};

/* One reading: the proc file's level, then (ask_iw) iw's answer for the access point and, when the proc file had no
 * level, for the level. *iw_missing is set when iw is not installed (the caller stops asking). */
WalkReading read_walk_signal(const Source &src, CommandRunner &cmd, const std::string &iface, bool ask_iw,
                             const CancelToken &cancel, bool *iw_missing);

/* ---------------------------------------------------------------- the walk (pure, times in ms from the start) */

struct WalkSample {
    std::int64_t t_ms = 0;
    bool linked = false;
    int dbm = 0;                /* when linked */
    bool weak = false;          /* below the threshold, or no link */
};

struct WalkDip {
    std::int64_t start_ms = 0;  /* the first weak sample */
    std::int64_t end_ms = -1;   /* the first sample of the 2 s back above the threshold; -1: the walk is (or ended) in it */
    bool has_dbm = false;
    int lowest_dbm = 0;         /* the weakest reading in it */
    bool lost_link = false;     /* a sample without link in it */
};

struct WalkRoam {
    std::int64_t t_ms = 0;
    std::string from, to;       /* BSSIDs */
    std::string ssid;           /* the network's name after the change */
    bool has_dbm = false;
    int dbm = 0;                /* the level right after */
};

struct WalkSummary {
    int samples = 0;            /* the whole walk (the ring may have dropped the oldest) */
    int linked_samples = 0;
    double duration_s = 0;
    bool has_signal = false;    /* at least one reading with a level */
    int weakest_dbm = 0;
    double weakest_at_s = 0;    /* the first time it was that weak */
    int strongest_dbm = 0;
    double strongest_at_s = 0;
    double average_dbm = 0;     /* mean of the readings with a level */
    int weak_samples = 0;       /* below the threshold or no link */
    double weak_s = 0;          /* weak_samples x the interval */
    double weak_pct = 0;        /* of the samples */
    int nolink_samples = 0;
    double nolink_s = 0;
    int dips = 0;
    int roams = 0;
    bool roaming_known = false; /* iw gave a BSSID at least once */
};

class WalkRecorder {
public:
    explicit WalkRecorder(int weak_dbm = kWalkWeakDbm, int interval_ms = kWalkIntervalMs,
                          int hysteresis_ms = kWalkHysteresisMs, std::size_t max_samples = kWalkMaxSamples);
    /* one reading at t_ms (from the start of the walk; increasing) */
    void add(std::int64_t t_ms, const WalkReading &r);

    const std::deque<WalkSample> &samples() const { return ring_; }
    std::uint64_t dropped() const { return dropped_; } /* samples that left the ring */
    const WalkSummary &summary() const { return sum_; }
    const std::vector<WalkDip> &dips() const { return dips_; }
    const std::vector<WalkRoam> &roams() const { return roams_; }
    bool in_dip() const { return in_dip_; }   /* the hysteresis state: a dip is open */
    const std::string &bssid() const { return bssid_; }
    const std::string &ssid() const { return ssid_; }
    int weak_dbm() const { return weak_dbm_; }

private:
    int weak_dbm_, interval_ms_, hysteresis_ms_;
    std::size_t max_samples_;
    std::deque<WalkSample> ring_;
    std::uint64_t dropped_ = 0;
    WalkSummary sum_;
    double dbm_total_ = 0;
    std::int64_t first_t_ = -1;
    bool in_dip_ = false;
    std::int64_t above_since_ = -1;
    int open_dip_ = -1;         /* its index in dips_, -1 when it was not kept (the list was full) */
    std::vector<WalkDip> dips_;
    std::vector<WalkRoam> roams_;
    std::string bssid_, ssid_;
};

/* ---------------------------------------------------------------- the sampler */

struct WalkConfig {
    std::string iface;          /* wifi_scan_iface(): the chosen network, or the main Wi-Fi interface */
    int weak_dbm = kWalkWeakDbm;        /* kWalkWeakMinDbm .. kWalkWeakMaxDbm */
    int interval_ms = kWalkIntervalMs;  /* 500 .. 5000 */
    int hysteresis_ms = kWalkHysteresisMs; /* 0 .. 10000 */
    std::size_t max_samples = kWalkMaxSamples; /* 10 .. 3600 */
    bool ask_iw = true;
    int max_samples_taken = 0;  /* tests: stop after that many (0 = until stopped) */
    std::vector<std::string> iw = {kIwPath}; /* tests: a fake */
};

struct WalkSnapshot {
    enum class State { Idle, Running, Stopped, Failed };
    State state = State::Idle;
    std::string iface;
    std::string error;          /* Failed: "No Wi-Fi adapter on wlan1" */
    int weak_dbm = kWalkWeakDbm;
    int interval_ms = kWalkIntervalMs;
    int hysteresis_ms = kWalkHysteresisMs;
    std::vector<WalkSample> samples; /* the ring, oldest first */
    std::uint64_t dropped = 0;
    WalkSummary summary;
    std::vector<WalkDip> dips;
    std::vector<WalkRoam> roams;
    bool in_dip = false;
    /* the latest reading */
    bool linked = false;
    int dbm = 0;
    std::string bssid, ssid;
    std::string source;         /* "/proc/net/wireless", "iw", "" (no link) */
    bool iw_missing = false;    /* iw is not installed: no access point changes */
    double elapsed_s = 0;       /* since start */
    bool running() const { return state == State::Running; }
};

/* The whole walk on the calling thread (the session's worker; tests call it with a fake clock). A sample every interval
 * on a fixed schedule from the start (a late one does not make the next ones bunch up: missed ticks are skipped).
 * publish() after every sample. Returns the last snapshot (Stopped when cancelled). */
WalkSnapshot run_walk(const WalkConfig &cfg, const Source &src, CommandRunner &cmd, const LoopClock &clock,
                      const CancelToken &cancel, const std::function<void(const WalkSnapshot &)> &publish);

class WalkSession {
public:
    WalkSession(); /* the system's /proc and /sys, DirectRunner, the real clock */
    WalkSession(std::shared_ptr<const Source> src, std::shared_ptr<CommandRunner> cmd, LoopClock clock);
    bool start(const WalkConfig &cfg); /* LVGL thread: a new walk (a running one is stopped and forgotten) */
    void stop();                       /* the samples and the summary stay (state Stopped) */
    void reset();                      /* stops and forgets: an Idle snapshot */
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const WalkSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<const Source> src_;
    std::shared_ptr<CommandRunner> cmd_;
    LoopClock clock_;
    WalkConfig last_;
    Channel<WalkSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
