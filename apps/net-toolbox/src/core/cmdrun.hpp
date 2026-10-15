/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: external commands (curl, openssl) for the tools that use them (no LVGL; tests/test_cmdrun.cpp).
 *
 * The core never runs a shell: a command is an argv. It only builds the argv and parses what the command printed; who
 * runs it is injectable:
 *   CommandRunner   blocking, for a worker thread (a session): run(spec, cancel) returns when the command ended, timed
 *                   out or was cancelled.
 *   DirectRunner    the core's own runner (posix_spawn, stdout and stderr merged into one pipe, stdin /dev/null,
 *                   LC_ALL=C, its own process group; a timeout or a cancel sends SIGTERM to the group, SIGKILL after
 *                   the grace). Used by the sessions and the host tests.
 *   CommandFlow     the steps of a tool as a state machine (next command, feed its result), so the UI can drive the
 *                   same tool with the shared async_run on the LVGL thread instead of a worker (as wifi_link does with
 *                   `iw`): async_run's on_line pieces joined with '\n' are CommandResult::output, its Result maps one to
 *                   one (exit_code, term_signal, started, timed_out, cancelled); set Spec::merge_stderr = true and
 *                   Spec::max_line = CommandSpec::max_line.
 */
#pragma once

#include "worker.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

struct CommandSpec {
    std::vector<std::string> argv;    /* argv[0] is searched in PATH */
    int timeout_ms = 20000;           /* 0 = none (the cancel still works) */
    int grace_ms = 500;               /* SIGTERM -> SIGKILL */
    std::size_t max_output = 256 * 1024; /* bytes kept; the rest is read and dropped (truncated = true) */
    std::size_t max_line = 256 * 1024;   /* for async_run: a JSON line must not be cut in pieces */
};

struct CommandResult {
    bool started = false;    /* the command ran (exec failure: started, exit_code 127) */
    int exit_code = -1;      /* when it exited */
    int term_signal = 0;     /* the signal that ended it */
    bool timed_out = false;
    bool cancelled = false;
    bool truncated = false;  /* more than max_output bytes came */
    std::string output;      /* stdout and stderr, merged */
    int elapsed_ms = 0;
    bool ok() const { return started && !timed_out && !cancelled && term_signal == 0 && exit_code == 0; }
};

class CommandRunner {
public:
    virtual ~CommandRunner() = default;
    /* Blocking (a worker thread). Never throws. */
    virtual CommandResult run(const CommandSpec &spec, const CancelToken &cancel) = 0;
};

class DirectRunner : public CommandRunner {
public:
    CommandResult run(const CommandSpec &spec, const CancelToken &cancel) override;
};

/* A tool's steps. next() gives the next command (false: finished); feed() hands its result back. */
class CommandFlow {
public:
    virtual ~CommandFlow() = default;
    virtual bool next(CommandSpec &cmd) = 0;
    virtual void feed(const CommandResult &result) = 0;
};

/* Runs a flow to its end with a blocking runner (a worker thread); after_step runs after each feed(). False when it
 * was cancelled (the flow got the cancelled result of the step in progress). */
bool run_flow(CommandFlow &flow, CommandRunner &runner, const CancelToken &cancel,
              const std::function<void()> &after_step = nullptr);

} // namespace nt
