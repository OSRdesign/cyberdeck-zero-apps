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
 *   StreamRunner    0.3: a command whose output is a stream (curl's body counted and dropped, bluetoothctl's lines), with
 *                   stdin fed by the core (zero bytes, or text); see below. DirectStreamRunner is the real one.
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

/* ---------------------------------------------------------------- the chosen network (0.3)
 *
 * curl is the one external command that sends the tools' traffic (HTTP check, public address, speed test): `--interface
 * <name>` binds its sockets to the chosen interface (sock.hpp has the process-wide choice). It is added in the one place
 * that starts a command (spawn_child, and the UI's FlowDriver for async_run), never in each tool's argv builder, so the
 * builders stay as they are. The name is validated ([A-Za-z0-9_.:-]{1,15}); the argv stays an array, never a shell.
 * openssl s_client (the certificate read after curl refused it) has no such option and is not bound. */

/* {"curl", "--interface", iface, ...rest}. Unchanged: another command than curl, "" (Automatic), an invalid name, an
 * argv that already has --interface. */
std::vector<std::string> with_curl_interface(std::vector<std::string> argv, const std::string &iface);
/* with_curl_interface(argv, active_interface()) */
std::vector<std::string> scoped_argv(std::vector<std::string> argv);

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

/* ---------------------------------------------------------------- streams (0.3)
 *
 * A command whose output is a stream rather than a result, on a worker thread (the shared async_run delivers lines to
 * the LVGL thread and cannot count a body):
 *   - the speed test: curl's body on stdout is counted and dropped (stdout_is_data: never kept, the memory stays flat),
 *     its -w report and its error message come as lines on stderr; an upload reads generated zero bytes from stdin;
 *   - the BLE scan: bluetoothctl reads its commands on stdin (kept open), its stdout and stderr are lines.
 * The client sees every line and a tick after each wake-up (data moved, or kSliceMs at most) and can send text, end
 * stdin or stop the command. A stop, a cancel or the timeout first writes `farewell` to stdin (best effort: what the
 * pipe takes), waits up to grace_ms for the command to end by itself, then sends SIGTERM to its process group and
 * SIGKILL after another grace_ms. Writing to a command that is gone never raises SIGPIPE (blocked and drained in the
 * calling thread). Same process rules as DirectRunner (own process group, LC_ALL=C, no shell). */

struct StreamSpec {
    enum class Input { Null, Zeros, Text };
    std::vector<std::string> argv;
    int timeout_ms = 30000;              /* 0 = none (the cancel and the client's stop still work) */
    int grace_ms = 500;
    Input input = Input::Null;           /* Null: /dev/null */
    std::uint64_t zero_bytes = 0;        /* Zeros: at most this many zero bytes, then stdin is closed */
    std::string text;                    /* Text: written first; stdin stays open for StreamOrders::send */
    std::string farewell;                /* Text: written before a stop, a cancel or the timeout ends the command */
    bool stdout_is_data = false;         /* true: stdout counted and dropped, stderr as lines; false: both as lines */
    std::size_t max_text = 64 * 1024;    /* the text kept in the result (every line still reaches on_line) */
    std::size_t max_line = 4096;         /* a longer line comes in pieces */
};

struct StreamTick {
    std::int64_t at_us = 0;              /* the runner's clock (now_us() for the real one) */
    std::uint64_t out_bytes = 0;         /* stdout bytes so far (stdout_is_data) */
    std::uint64_t in_bytes = 0;          /* bytes written to stdin so far */
    bool input_open = false;
};

/* what the client wants after a line or a tick */
struct StreamOrders {
    std::string send;                    /* Text: more for stdin, after what waits */
    bool end_input = false;              /* close stdin (after the text that waits) */
    bool stop = false;                   /* end the command (farewell, SIGTERM, SIGKILL) */
};

class StreamClient {
public:
    virtual ~StreamClient() = default;
    /* a line without its '\n' ('\r' kept: bluetoothctl redraws its prompt with it) */
    virtual void on_line(const std::string &line, std::int64_t at_us, StreamOrders &orders) {}
    virtual void on_tick(const StreamTick &tick, StreamOrders &orders) {}
};

struct StreamResult : CommandResult {    /* output = the text lines, joined with '\n', capped at max_text */
    std::uint64_t out_bytes = 0;
    std::uint64_t in_bytes = 0;
    bool stopped = false;                /* the client asked for the stop (not an error) */
};

class StreamRunner {
public:
    virtual ~StreamRunner() = default;
    /* Blocking (a worker thread). Never throws. */
    virtual StreamResult run(const StreamSpec &spec, const CancelToken &cancel, StreamClient &client) = 0;
};

class DirectStreamRunner : public StreamRunner {
public:
    StreamResult run(const StreamSpec &spec, const CancelToken &cancel, StreamClient &client) override;
};

} // namespace nt
