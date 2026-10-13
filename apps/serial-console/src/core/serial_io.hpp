/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the port I/O, on worker threads (no LVGL; tests/test_serial_pty.cpp runs it on pseudo-terminals).
 *
 * Session: a connection. Its worker thread owns the port: it opens and configures it (raw, the speed and frame, no
 * flow control, input flushed), reads with poll() and writes what send() queued, all non-blocking. Every byte read is
 * written to the log file (when on) at once and never dropped; the same bytes go to a queue the UI drains with take()
 * on its own timer (at most ui_cap bytes wait there: if the UI cannot keep up with a flood, the oldest bytes of the
 * queue are dropped from the display only and counted). When the port goes (unplugged: read error or hang-up), the
 * status turns to Lost and the worker looks for the same port id every retry_ms (resolve_port: a board that comes back
 * as another ttyUSB number is found), then opens it again with the same settings. An open refused for a reason that
 * waiting will not fix (no permission, busy) ends the session as Failed with its errno.
 *
 * SpeedFinder: the auto-baud detection (baud.hpp Detector) on its own thread: each candidate speed is set, the input
 * flushed, then it listens listen_ms (sends one CR first in the second pass) and scores what came. Cancel stops it
 * within about 50 ms.
 *
 * reset_board(): DTR off and RTS on for 100 ms, then the lines as they were: the reset of ESP32 / ESP8266 boards with
 * the usual two-transistor auto-reset circuit (RTS pulls EN low while DTR is off), and of Arduino boards (DTR coupled
 * to RESET: the DTR edge back on resets them). A port without modem lines (a pseudo-terminal, some CDC devices) says so
 * in the status note.
 *
 * All methods are thread-safe; the UI thread calls them, the worker never calls back.
 */
#pragma once

#include "baud.hpp"
#include "ports.hpp"
#include "settings.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace sc {

/* ---------------------------------------------------------------- port primitives (any thread) */

/* open() of a serial port: read-write, no controlling tty, non-blocking, close-on-exec, exclusive (TIOCEXCL). -1 and
 * *err = errno on failure. */
int open_port(const std::string &path, int *err);
/* TIOCNXCL, then close(): the exclusive flag does not outlive us when another process holds the port open. */
void close_port(int fd);
/* Raw mode, the speed (any rate: termios2 for one without a B constant, 74880 for instance), the frame, CLOCAL and
 * CREAD, no flow control; the input queue flushed. false and errno on failure. */
bool configure_port(int fd, int baud, const Frame &frame);
/* The output speed the port has now (-1 when unknown). Works on a pseudo-terminal master too (it reports its slave). */
int port_speed(int fd);
/* The DTR / RTS pulse; false when the port has no modem lines. */
bool pulse_reset(int fd);

/* ---------------------------------------------------------------- a connection */

enum class LinkState { Opening, Open, Lost, Failed, Closed };
const char *link_state_name(LinkState state); /* "Opening", "Connected", "Port lost", "Failed", "Closed" */

struct SessionOptions {
    std::string id;         /* the port id: resolve_port() finds it again after an unplug */
    std::string path;       /* the /dev path to open first */
    int baud = kDefaultBaud;
    Frame frame;
    Roots roots;
    bool log = false;
    std::string log_path;   /* the log file (its folder is created) */
    std::size_t ui_cap = 512 * 1024;
    int retry_ms = 500;
};

struct SessionStatus {
    LinkState state = LinkState::Opening;
    int error = 0;               /* Failed: the errno of the open */
    std::uint64_t rx = 0, tx = 0;
    std::uint64_t ui_dropped = 0; /* bytes the display skipped (the log has them) */
    bool log_on = false;
    bool log_error = false;      /* the log file could not be opened or written */
    std::string log_path;
    std::string path;            /* the path in use */
    int reconnects = 0;
    std::string note;            /* the last remark: "No DTR/RTS lines on this port" */
};

class Session {
public:
    explicit Session(SessionOptions options);
    ~Session(); /* stop() */
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    void start();
    void stop(); /* ends the worker and closes the port and the log; idempotent */

    void send(const std::string &bytes);
    void set_log(bool on, const std::string &path);
    void reset_board();

    /* Moves at most max received bytes (oldest first) to out; returns how many. */
    std::size_t take(std::string &out, std::size_t max);
    SessionStatus status() const;

private:
    void run();
    bool try_open(const std::string &path, int *err);
    void lose_port();
    void apply_log();
    void wait_ms(int ms);
    void wake();

    SessionOptions options_;
    mutable std::mutex mutex_;
    std::string rx_queue_, tx_queue_;
    SessionStatus status_;
    bool reset_wanted_ = false;
    bool log_wanted_ = false;
    std::string log_path_wanted_;
    bool log_changed_ = false;
    std::atomic<bool> stop_{false};
    int wake_[2] = {-1, -1};
    int fd_ = -1;
    int log_fd_ = -1;
    std::thread thread_;
};

/* ---------------------------------------------------------------- auto-baud */

struct FinderProgress {
    bool running = false;
    bool done = false;
    int error = 0;           /* the open failed (errno), or the port went (ENODEV) */
    std::string text;        /* Detector::progress_text() */
    int index = 0, count = 0, rate = 0, pass = 1;
    DetectResult result;     /* when done */
};

class SpeedFinder {
public:
    SpeedFinder(std::string path, Frame frame, int recommended, bool enter_pass, int listen_ms = 700);
    ~SpeedFinder(); /* cancel + join */
    SpeedFinder(const SpeedFinder &) = delete;
    SpeedFinder &operator=(const SpeedFinder &) = delete;

    void start();
    void cancel();
    FinderProgress progress() const;

private:
    void run();

    std::string path_;
    Frame frame_;
    int recommended_;
    bool enter_pass_;
    int listen_ms_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    FinderProgress progress_;
    std::thread thread_;
};

/* CLOCK_MONOTONIC milliseconds. */
std::int64_t now_ms();

} // namespace sc
