/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the direct command runner (output, exit codes, a missing program, the timeout, the cancel, the
 * whole process group ended, the output cap, LC_ALL=C) and run_flow() with a scripted flow.
 */
#include "check.hpp"

#include "cmdrun.hpp"
#include "text.hpp"

#include <signal.h>

#include <cerrno>
#include <chrono>
#include <thread>

using namespace nt;

namespace {

CommandSpec sh(const std::string &script, int timeout_ms = 5000)
{
    CommandSpec s;
    s.argv = {"/bin/sh", "-c", script};
    s.timeout_ms = timeout_ms;
    return s;
}

bool gone(pid_t pid)
{
    for (int i = 0; i < 100; ++i) {
        if (kill(pid, 0) != 0 && errno == ESRCH) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

class TwoSteps : public CommandFlow {
public:
    int step = 0;
    std::vector<CommandResult> got;
    bool next(CommandSpec &cmd) override
    {
        if (step >= 2) return false;
        cmd = sh(step == 0 ? "echo one" : "echo two; exit 4");
        return true;
    }
    void feed(const CommandResult &r) override
    {
        got.push_back(r);
        ++step;
    }
};

} // namespace

void test_cmdrun()
{
    DirectRunner run;
    CancelToken none;
    {
        const CommandResult r = run.run(sh("echo hello; echo oops >&2; exit 3"), none);
        CHECK(r.started && r.exit_code == 3 && !r.timed_out && !r.cancelled && !r.ok());
        CHECK(r.output.find("hello\n") != std::string::npos && r.output.find("oops\n") != std::string::npos);
    }
    {
        const CommandResult r = run.run(sh("printf '%s' \"$LC_ALL\"; exit 0"), none);
        CHECK(r.ok() && r.output == "C"); /* the C locale: curl's numbers and openssl's dates in a known form */
    }
    {
        CommandSpec s;
        s.argv = {"net-toolbox-no-such-program"};
        const CommandResult r = run.run(s, none);
        CHECK(r.started && r.exit_code == 127 && !r.ok()); /* as async_run reports it */
        CommandSpec empty;
        CHECK(run.run(empty, none).exit_code == 127);
    }
    {
        const CommandResult r = run.run(sh("read x; echo \"got [$x]\""), none);
        CHECK(r.ok() && r.output == "got []\n"); /* stdin is /dev/null: openssl s_client ends after the handshake */
    }
    // the timeout ends the whole group: the shell and what it started
    {
        const std::int64_t t0 = now_ms();
        const CommandResult r = run.run(sh("sleep 30 & echo $!; wait", 400), none);
        const std::int64_t took = now_ms() - t0;
        CHECK(r.timed_out && !r.ok() && took >= 390 && took < 2000);
        int bg = 0;
        CHECK(parse_int(r.output, bg) && bg > 1);
        if (bg > 1) CHECK(gone(bg));
    }
    // a cancel from another thread, within a slice
    {
        CancelToken cancel;
        CommandResult r;
        std::thread t([&] { r = run.run(sh("sleep 30 & echo $!; wait"), cancel); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::int64_t t0 = now_ms();
        cancel.cancel();
        t.join();
        CHECK(r.cancelled && !r.ok() && now_ms() - t0 < 1500);
        int bg = 0;
        if (parse_int(r.output, bg) && bg > 1) CHECK(gone(bg));
        else CHECK(false);
    }
    // a process that ignores SIGTERM gets SIGKILL after the grace
    {
        CommandSpec s = sh("trap '' TERM; sleep 30 & wait; sleep 30", 300);
        s.grace_ms = 200;
        const std::int64_t t0 = now_ms();
        const CommandResult r = run.run(s, none);
        CHECK(r.timed_out && now_ms() - t0 < 3000);
    }
    // the output cap: the rest is read and dropped, the command still ends normally
    {
        CommandSpec s = sh("i=0; while [ $i -lt 2000 ]; do echo 0123456789abcdef0123456789abcdef; i=$((i+1)); done");
        s.max_output = 1000;
        const CommandResult r = run.run(s, none);
        CHECK(r.ok() && r.truncated && r.output.size() == 1000);
    }
    // a signal ends it
    {
        const CommandResult r = run.run(sh("kill -9 $$"), none);
        CHECK(r.started && r.term_signal == 9 && !r.ok());
    }
    // run_flow: every step in order; a cancelled token feeds a cancelled result and stops
    {
        TwoSteps flow;
        int after = 0;
        CHECK(run_flow(flow, run, none, [&] { ++after; }));
        CHECK(flow.got.size() == 2 && after == 2);
        CHECK(flow.got[0].ok() && flow.got[0].output == "one\n");
        CHECK(flow.got[1].exit_code == 4 && flow.got[1].output == "two\n");
        TwoSteps stopped;
        CancelToken c;
        c.cancel();
        CHECK(!run_flow(stopped, run, c));
        CHECK(stopped.got.size() == 1 && stopped.got[0].cancelled && !stopped.got[0].started);
    }
}
