/*
 * SPDX-License-Identifier: MIT
 *
 * A child process on a pseudo-terminal (report 033, P5: meshcore-cli for the CLI page). meshcore-cli's prompt (prompt_toolkit)
 * needs a terminal on its stdin and stdout; with TERM=dumb it prints a plain prompt and echoes what it reads itself.
 *
 *   start()     posix_openpt + fork: the child gets a new session (its own process group, the pty as its controlling
 *               terminal on 0, 1 and 2), the pty's echo off (prompt_toolkit echoes; the kernel must not echo a second
 *               time), the window size, its signals back to the defaults, PR_SET_PDEATHSIG(SIGTERM), every other file
 *               descriptor of Mesh Hop closed (the serial port, the relay's sockets...), then execv of an absolute path
 *               with the environment given. A failed exec says why on the pty and exits 127.
 *   read()      what the child wrote (non-blocking): the master's buffer drains after the child ended, then EIO (eof()).
 *   write()     queued and sent without blocking by poll().
 *   poll()      flushes the queue and reaps the child (waitpid WNOHANG): exited() / exit_code() / term_signal().
 *   stop()      SIGHUP + SIGTERM to the child's process group, SIGKILL after the grace (from poll()); kill_now() at once and
 *               waits (the app's end). The destructor kills what is left.
 * libc only, no thread: the app's tick calls read() and poll().
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace meshzero {

class PtyProcess {
public:
    struct Spec {
        std::vector<std::string> argv;            // argv[0]: an absolute path (no PATH search)
        std::vector<std::string> env;             // "KEY=value": set in the child (over Mesh Hop's environment)
        std::vector<std::string> unset;           // names removed from the child's environment
        int cols = 80, rows = 24;                 // the window size (TIOCSWINSZ)
    };

    PtyProcess() = default;
    ~PtyProcess();
    PtyProcess(const PtyProcess &) = delete;
    PtyProcess &operator=(const PtyProcess &) = delete;

    /* Starts the child. False (error() says why) when no pty or no fork: nothing runs then. */
    bool start(const Spec &spec);
    /* Appends at most max bytes of the child's output to out; returns the count. */
    size_t read(std::string &out, size_t max = 64 * 1024);
    /* Queues bytes for the child's input (nothing once it is gone). */
    void write(const std::string &bytes);
    /* Flushes the input queue, reaps the child, sends SIGKILL when a stop() grace ran out. now: a monotonic ms clock. */
    void poll(uint64_t now);
    /* SIGHUP and SIGTERM to the child's group; SIGKILL from poll() after grace_ms. */
    void stop(uint64_t now, uint32_t grace_ms = 1500);
    /* SIGKILL to the child's group and waits for it (the app is ending); closes the pty. */
    void kill_now();
    /* A new window size (the view changed). */
    void resize(int cols, int rows);

    bool started() const { return pid_ > 0; }
    bool running() const { return pid_ > 0 && !reaped_; }     // started and not reaped yet
    bool exited() const { return reaped_; }                   // reaped: exit_code() or term_signal() say how
    bool eof() const { return eof_; }                         // the pty gave EIO (the child and its children closed it)
    int exit_code() const { return exit_code_; }              // -1 unless it exited
    int term_signal() const { return term_signal_; }          // 0 unless a signal ended it
    bool stopping() const { return stop_at_ != 0; }
    pid_t pid() const { return pid_; }
    size_t pending_input() const { return in_.size(); }
    const std::string &error() const { return error_; }

private:
    void reap(bool wait);
    void close_master();

    int master_ = -1;
    pid_t pid_ = -1;
    bool reaped_ = false, eof_ = false;
    int exit_code_ = -1, term_signal_ = 0;
    uint64_t stop_at_ = 0, kill_at_ = 0;
    bool killed_ = false;
    std::string in_;
    std::string error_;
};

} // namespace meshzero
