/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.3: the speed test (no LVGL; tests/test_speedtest.cpp).
 *
 * Cloudflare's open speed endpoints (no account, no key), through the system's curl as an argv, never a shell:
 *   https://speed.cloudflare.com/__down?bytes=N   N bytes of body (N < 100 000 000: 100 000 000 and more get 403)
 *   https://speed.cloudflare.com/__up             a POST; the answer's cf-meta-upload-bytes says what arrived
 * Measured on the deck and the Pi 3A+ on 2026-10-15 (report of task 0.3): one HTTP/1.1 connection fills the link (two at
 * once gave no more on either board); TLS 1.3 with ChaCha20-Poly1305 costs these CPUs (no AES instructions) about half
 * the CPU of AES-256-GCM, so it is asked first (--tls13-ciphers, AES kept as a fallback).
 *
 * Phases, one after the other, on one worker thread (the session), one curl at a time:
 *   Latency   one curl with latency_requests + 1 requests of __down?bytes=0 on one kept-alive connection; the first sets
 *             the connection up (its time is setup_ms), the others give round trips: first byte - request sent, less the
 *             server's own time from its Server-Timing header. Median and jitter (as ping: the mean difference of
 *             successive values). Beside it, idle_pings pings of the ping target (the idle latency).
 *   Download  one curl, several __down requests of chunk_bytes on the same connection whose sizes add up to max_bytes:
 *             the body comes on curl's stdout and is counted and dropped by the stream runner (never kept). The phase
 *             ends target_ms after the first byte (curl is stopped) or when every byte came (`capped`).
 *   Upload    one curl, a chunked POST of zero bytes generated on its stdin (`-T -`, at most max_bytes; curl streams it,
 *             its memory stays flat); target_ms after curl took the first byte, stdin is closed: curl ends the body and
 *             reads the answer, which carries the bytes the server received.
 * Throughput: a sample every sample_ms (the live figure and the chart: Mbit/s = bytes x 8 / seconds / 10^6); the result
 * leaves out the first ramp_ms (TCP's slow start): (bytes at the end - bytes at the ramp) / (time between), or the whole
 * when the phase lasted less than two ramps. Both ends of an upload are measured as bytes handed to curl, so the bytes
 * that wait in the buffers count the same at both ends and drop out of the difference.
 * Latency under load (bufferbloat): the ping module pings the ping target (1.1.1.1 by default, the gateway as an
 * option: the UI turns the word into an address) every ping_interval_ms from the first byte of each direction to its
 * end; loaded median - idle median = the increase, graded A < 30 ms, B < 60 ms, C < 200 ms, D < 400 ms, F from 400 ms
 * (the steps of the Waveform bufferbloat test, without its A+); the overall grade is the worse direction. No ping
 * (not allowed, no target): the speeds are measured, the grade is not.
 * Cancel: the runner ends curl's process group (SIGTERM, SIGKILL after the grace) within a poll slice; the ping beside it
 * is stopped and joined.
 */
#pragma once

#include "cmdrun.hpp"
#include "ping.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

constexpr const char *kSpeedHost = "speed.cloudflare.com";
constexpr const char *kSpeedDownUrl = "https://speed.cloudflare.com/__down?bytes=";
constexpr const char *kSpeedUpUrl = "https://speed.cloudflare.com/__up";
constexpr const char *kSpeedCiphers = "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384";
constexpr std::uint64_t kSpeedMaxRequest = 99999999; /* __down refuses 100 000 000 bytes and more (403) */
constexpr const char *kSpeedTag = "NTSPEED";         /* curl's -w lines start with it */

struct SpeedConfig {
    int latency_requests = 10;            /* after one warm-up request (3 .. 30) */
    int target_ms = 8000;                 /* per direction, from the first byte (2000 .. 20000) */
    std::uint64_t max_bytes = 150000000;  /* per direction, never more (1 MB .. 500 MB) */
    std::uint64_t chunk_bytes = 50000000; /* one download request (1 MB .. kSpeedMaxRequest) */
    int sample_ms = 500;                  /* the live figure and the chart (200 .. 2000) */
    int ramp_ms = 1000;                   /* left out of the result (0 .. 4000) */
    std::string ping_target = "1.1.1.1";  /* the latency under load; "" = not measured */
    int ping_interval_ms = 200;           /* 200 .. 1000 */
    int idle_pings = 10;                  /* 3 .. 30 */
    ResolveOptions resolve;               /* for a ping target that is a name */
    bool download = true;
    bool upload = true;
};
SpeedConfig clamp_speed_config(const SpeedConfig &cfg);

/* curl's -w report of one transfer: one line per URL on stderr, fields split by '|' */
struct SpeedTransfer {
    char kind = 0;                        /* 'L' latency, 'D' download, 'U' upload */
    int http_code = 0;                    /* 0: no answer */
    int exit_code = -1;                   /* curl's result for this transfer */
    int connects = 0;                     /* new connections for this transfer (0: the kept-alive one) */
    double connect_s = 0, tls_s = 0, pretransfer_s = 0, starttransfer_s = 0, total_s = 0;
    std::uint64_t size_download = 0, size_upload = 0;
    std::string colo;                     /* the Cloudflare site, an airport code ("CDG") */
    bool has_server_bytes = false;
    std::uint64_t server_bytes = 0;       /* __up: cf-meta-upload-bytes */
    double server_ms = -1;                /* Server-Timing: the server's own time, -1 when not given */
    /* request sent -> first byte, less the server's time (never below 0); -1 when curl gave no first byte */
    double rtt_ms() const;
};
bool parse_speed_transfer(const std::string &line, SpeedTransfer &out);
/* "cfSpeedEdge;dur=4, cfSpeedWorker;dur=26" -> 30 (the sum of the dur= values); -1 when there is none */
double parse_server_timing(const std::string &header);

/* the command lines (curl as an argv) */
std::vector<std::string> speed_latency_argv(int requests);
std::vector<std::string> speed_download_argv(std::uint64_t max_bytes, std::uint64_t chunk_bytes);
std::vector<std::string> speed_upload_argv();
/* what the __down URLs of an argv ask for, in all */
std::uint64_t speed_download_total(const std::vector<std::string> &argv);

/* plain words for a phase that failed; http_code 0 when no answer came */
std::string speed_error_words(const CommandResult &r, int http_code, const std::string &curl_message);

/* ---------------------------------------------------------------- numbers (pure) */

struct SpeedSample {
    double t_s = 0;                       /* since the first byte of the direction */
    double mbps = 0;
};

/* one direction's throughput from (time, bytes so far) points */
class ThroughputMeter {
public:
    explicit ThroughputMeter(int sample_ms = 500, int ramp_ms = 1000, std::size_t max_history = 120);
    /* bytes so far at that time; true when a sample was closed. The first point with bytes is the start. */
    bool add(std::int64_t at_us, std::uint64_t bytes);
    bool started() const { return started_; }
    std::int64_t start_us() const { return t0_; }
    double elapsed_s() const { return started_ ? static_cast<double>(last_t_ - t0_) / 1e6 : 0; }
    std::uint64_t bytes() const { return last_b_; }
    double current_mbps() const { return current_; }
    const std::vector<SpeedSample> &history() const { return history_; }
    /* the steady state (after the ramp), or the whole when the phase was shorter than two ramps; 0 before data */
    double result_mbps() const;
    static double mbps(std::uint64_t bytes, double seconds);

private:
    std::int64_t sample_us_, ramp_us_;
    std::size_t max_history_;
    bool started_ = false;
    std::int64_t t0_ = 0, last_t_ = 0, sample_t_ = 0, ramp_t_ = 0;
    std::uint64_t b0_ = 0, last_b_ = 0, sample_b_ = 0, ramp_b_ = 0;
    bool ramp_set_ = false;
    double current_ = 0;
    std::vector<SpeedSample> history_;
};

enum class BloatGrade { Unknown, A, B, C, D, F };
/* the latency increase under load: A < 30 ms, B < 60, C < 200, D < 400, F from 400 (negative: A) */
BloatGrade bufferbloat_grade(double increase_ms);
const char *grade_letter(BloatGrade g);   /* "A" .. "F", "-" */
const char *grade_words(BloatGrade g);    /* one plain sentence */
BloatGrade worse_grade(BloatGrade a, BloatGrade b);

double median_of(std::vector<double> v); /* 0 for none */
double jitter_of(const std::vector<double> &v); /* mean |difference| of successive values; 0 for fewer than 2 */

struct LatencyStats {
    bool ok = false;                      /* at least one reply */
    int sent = 0, received = 0, lost = 0;
    double median_ms = 0, min_ms = 0, max_ms = 0, jitter_ms = 0;
    double loss_pct() const { return received + lost ? 100.0 * lost / (received + lost) : 0; }
};
/* the replies kept in a ping snapshot (packets still waiting are neither) */
LatencyStats latency_from_ping(const PingSnapshot &s);
LatencyStats latency_from_values(const std::vector<double> &rtts, int lost = 0);

/* ---------------------------------------------------------------- results */

struct SpeedDirection {
    bool tried = false;
    bool measured = false;                /* a throughput exists */
    double mbps = 0;                      /* the result (steady state) */
    std::uint64_t bytes = 0;              /* moved while measured (download: received; upload: handed to curl) */
    double seconds = 0;                   /* first byte to the end */
    bool capped = false;                  /* every byte allowed moved before target_ms */
    bool has_server_bytes = false;        /* upload: the server's count */
    std::uint64_t server_bytes = 0;
    std::string note;                     /* "the connection broke after 3.2 s: ..." */
    std::string error;                    /* not measured: why */
    LatencyStats loaded;                  /* ping beside this direction */
    bool has_increase = false;
    double increase_ms = 0;               /* loaded median - idle median */
    BloatGrade grade = BloatGrade::Unknown;
    std::string grade_note;               /* "every ping was lost under load" ... */
    std::vector<SpeedSample> history;
};

struct SpeedResult {
    bool latency_ok = false;
    double latency_ms = 0, jitter_ms = 0; /* HTTP round trips to the server, the server's time taken off */
    double latency_min_ms = 0, latency_max_ms = 0;
    int latency_samples = 0;
    double setup_ms = 0;                  /* the first request: DNS, TCP and TLS */
    std::string colo;                     /* the Cloudflare site that answered */
    std::string ping_target;              /* as configured */
    std::string ping_address;
    std::string ping_note;                /* why the latency under load is not measured */
    LatencyStats idle;                    /* ping, idle */
    SpeedDirection down, up;
    BloatGrade grade = BloatGrade::Unknown; /* the worse direction */
};

struct SpeedSnapshot {
    enum class State { Idle, Running, Done, Failed, Cancelled };
    enum class Phase { None, Latency, Download, Upload };
    State state = State::Idle;
    Phase phase = Phase::None;
    double now_mbps = 0;                  /* the direction in progress: its last sample */
    double phase_s = 0;                   /* since its first byte */
    std::uint64_t phase_bytes = 0;
    double ping_now_ms = -1;              /* the last reply of the ping beside the load, -1 none */
    SpeedResult result;
    std::string error;                    /* Failed: plain words */
    std::string detail;                   /* curl's own last message when a phase did not go well (for the log) */
    bool running() const { return state == State::Running; }
};
const char *speed_phase_name(SpeedSnapshot::Phase p); /* "latency", "download", "upload", "" */

/* ---------------------------------------------------------------- the ping beside the load */

class LoadPing {
public:
    virtual ~LoadPing() = default;
    virtual bool start(const PingConfig &cfg) = 0;
    virtual void stop() = 0;                      /* returns at once */
    virtual bool wait(int timeout_ms) = 0;        /* the run ended */
    virtual std::shared_ptr<const PingSnapshot> latest() const = 0;
};
using LoadPingFactory = std::function<std::unique_ptr<LoadPing>()>;
/* the ping module's PingSession (its own 256 kB thread, joined when the LoadPing goes); factory = nullptr: ICMP */
LoadPingFactory session_load_ping(EchoFactory factory = nullptr);

/* ---------------------------------------------------------------- the engine and the session */

/* The whole test on the calling thread (the session's worker; tests call it directly). publish gets a snapshot at each
 * phase change and each sample; the last one is returned. */
SpeedSnapshot run_speedtest(const SpeedConfig &cfg, StreamRunner &runner, const LoadPingFactory &ping,
                            const CancelToken &cancel, const std::function<void(const SpeedSnapshot &)> &publish);

class SpeedTestSession {
public:
    SpeedTestSession(); /* DirectStreamRunner and the ICMP ping */
    SpeedTestSession(std::shared_ptr<StreamRunner> runner, LoadPingFactory ping);
    bool start(const SpeedConfig &cfg); /* LVGL thread; a running test is stopped first */
    void stop();                        /* curl ends within a poll slice (and its grace) */
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const SpeedSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<StreamRunner> runner_;
    LoadPingFactory ping_;
    Channel<SpeedSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
