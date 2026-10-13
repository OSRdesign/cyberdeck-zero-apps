/*
 * SPDX-License-Identifier: MIT
 *
 * Background commands for an LVGL app (report 024 section 2): fork + exec of an argv (no shell unless the argv asks
 * for one), the output read line by line on a worker thread, every line and the end delivered ON THE LVGL THREAD, in
 * order (one lv_async_call drains a per-job queue, under lv_lock()), with an optional timeout.
 *
 * Process hygiene:
 *   - the child gets its own process group (setpgid), so a timeout, cancel() or shutdown() ends the whole pipeline
 *     (killpg SIGTERM, then SIGKILL after the grace);
 *   - PR_SET_PDEATHSIG(SIGTERM): the child is told when the app dies without running shutdown() (SIGKILL from the
 *     launcher's grace timeout); run() must therefore be called from the LVGL thread, which lives as long as the app;
 *   - stdin is /dev/null, the signal mask and SIGPIPE are reset, LC_ALL=C unless the spec sets env itself;
 *   - cdk::stop() (app.hpp) calls shutdown() before LVGL goes: callbacks never run after it.
 *
 * Note: the launcher's hold-Esc watchdog signals the app's process group only; the commands run in their own groups,
 * which is why the app's SIGTERM path ends them (shutdown) and PDEATHSIG covers a SIGKILL of the app.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cdk {
namespace async_run {

struct Result {
    int exit_code = -1;          /* exit status when the command exited, else -1 */
    int term_signal = 0;         /* the signal that ended it, else 0 */
    bool started = false;        /* fork / exec worked (exec failure: exit_code 127, started true) */
    bool timed_out = false;      /* the timeout fired (it was then ended) */
    bool cancelled = false;      /* cancel() ended it */
    std::uint64_t lines = 0;     /* lines delivered */
    std::uint64_t elapsed_ms = 0;
    bool ok() const { return started && !timed_out && !cancelled && term_signal == 0 && exit_code == 0; }
};

struct Spec {
    std::vector<std::string> argv;            /* argv[0] is searched in PATH */
    std::vector<std::string> env;             /* extra "KEY=value" entries (added to the app's environment) */
    int timeout_ms = 0;                       /* 0 = none */
    int grace_ms = 500;                       /* SIGTERM -> SIGKILL */
    bool merge_stderr = true;                 /* false: stderr goes to /dev/null */
    std::size_t max_line = 4096;              /* a longer line is delivered in pieces */
    std::function<void(const std::string &line)> on_line; /* LVGL thread; the line without its '\n' */
    std::function<void(const Result &result)> on_done;     /* LVGL thread; last call for the job */
};

using JobId = std::uint64_t;

/* Starts the command. LVGL thread. Returns its id (> 0); on a failure to start (no pipe, no fork) it returns 0 and
 * on_done still runs (started = false), on the LVGL thread, later. */
JobId run(Spec spec);
/* Ends a running job (SIGTERM to its group, SIGKILL after the grace); its on_done reports cancelled. LVGL thread. */
void cancel(JobId id);
/* Jobs still running. */
std::size_t running();
/* Ends every job and joins the workers (at most grace + 1.5 s each); no callback runs after it, and run() refuses
 * new jobs (the app is ending). LVGL thread, outside lv_lock() if possible (a worker waiting for the lock is
 * detached after the wait instead of joined). */
void shutdown();

/* Splits buffered output into lines (pure, unit-tested): complete lines are appended to `out` and removed from
 * `buffer`; a line longer than max_line is cut into pieces of max_line bytes; "\r\n" counts as "\n". */
void split_lines(std::string &buffer, std::size_t max_line, std::vector<std::string> &out);

} // namespace async_run
} // namespace cdk
