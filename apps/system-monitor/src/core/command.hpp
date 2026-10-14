/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: one short command with a timeout, for the sampler thread (vcgencmd, iw). Never called on the LVGL
 * thread: the sampler is a thread of its own, so waiting here never blocks the screen.
 *
 * posix_spawnp (no shell), the child in its own process group, stdin /dev/null, stdout and stderr into one pipe,
 * LC_ALL=C. At the timeout the whole group gets SIGKILL and is reaped; the output is capped.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sm {

struct CmdResult {
    bool started = false;  /* the program ran (not found: false, error ENOENT) */
    int error = 0;         /* errno of a start that failed */
    int exit_code = -1;    /* when it exited */
    int signal = 0;        /* the signal that ended it */
    bool timed_out = false;
    std::string output;    /* stdout and stderr, at most max_out bytes */
    bool ok() const { return started && !timed_out && signal == 0 && exit_code == 0; }
    bool not_found() const { return !started || exit_code == 127; }
};

CmdResult run_command(const std::vector<std::string> &argv, int timeout_ms, std::size_t max_out = 16384);

} // namespace sm
