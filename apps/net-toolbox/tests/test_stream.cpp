/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): the stream runner on real commands (sh): stdout counted and never kept, the lines of
 * stderr, zero bytes on stdin (all of them, or ended early), text in and out, the farewell on a cancel, the client's
 * stop (the process group ended), a command that ignores SIGTERM, the timeout, a missing program, a command that does
 * not read its stdin (no SIGPIPE), a long line in pieces.
 */
#include "check.hpp"

#include "cmdrun.hpp"
#include "text.hpp"

#include <signal.h>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

using namespace nt;

namespace {

StreamSpec sh(const std::string &script, int timeout_ms = 10000)
{
    StreamSpec s;
    s.argv = {"/bin/sh", "-c", script};
    s.timeout_ms = timeout_ms;
    return s;
}

struct Collect : StreamClient {
    std::vector<std::string> lines;
    std::vector<StreamTick> ticks;
    std::function<void(const std::string &, StreamOrders &)> line_fn;
    std::function<void(const StreamTick &, StreamOrders &)> tick_fn;
    void on_line(const std::string &line, std::int64_t, StreamOrders &o) override
    {
        lines.push_back(line);
        if (line_fn) line_fn(line, o);
    }
    void on_tick(const StreamTick &t, StreamOrders &o) override
    {
        ticks.push_back(t);
        if (tick_fn) tick_fn(t, o);
    }
};

bool gone(pid_t pid)
{
    for (int i = 0; i < 100; ++i) {
        if (kill(pid, 0) != 0 && errno == ESRCH) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string slurp(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

void test_stream_runner()
{
    DirectStreamRunner run;
    CancelToken none;
    // stdout counted and dropped, stderr as lines
    {
        StreamSpec s = sh("head -c 3000000 /dev/zero; echo report >&2; printf 'last' >&2");
        s.stdout_is_data = true;
        Collect c;
        const StreamResult r = run.run(s, none, c);
        CHECK(r.ok() && !r.stopped);
        CHECK_EQ(r.out_bytes, static_cast<std::uint64_t>(3000000));
        CHECK(c.lines.size() == 2 && c.lines[0] == "report" && c.lines[1] == "last"); /* a last line without '\n' too */
        CHECK_EQ(r.output, std::string("report\nlast")); /* the body is never kept */
        bool monotonic = !c.ticks.empty();
        for (std::size_t i = 1; i < c.ticks.size(); ++i)
            if (c.ticks[i].out_bytes < c.ticks[i - 1].out_bytes || c.ticks[i].at_us < c.ticks[i - 1].at_us) monotonic = false;
        CHECK(monotonic);
    }
    // zero bytes on stdin, every one of them, then end of input
    {
        StreamSpec s = sh("wc -c >&2");
        s.stdout_is_data = true;
        s.input = StreamSpec::Input::Zeros;
        s.zero_bytes = 5000000;
        Collect c;
        const StreamResult r = run.run(s, none, c);
        std::uint64_t counted = 0;
        CHECK(r.ok() && c.lines.size() == 1 && parse_u64(c.lines.empty() ? "" : c.lines[0], counted));
        CHECK_EQ(counted, static_cast<std::uint64_t>(5000000));
        CHECK_EQ(r.in_bytes, static_cast<std::uint64_t>(5000000));
    }
    // the client ends stdin early (the speed test's time target): the command gets what was written, no more
    {
        StreamSpec s = sh("wc -c >&2");
        s.stdout_is_data = true;
        s.input = StreamSpec::Input::Zeros;
        s.zero_bytes = 500000000;
        Collect c;
        c.tick_fn = [](const StreamTick &t, StreamOrders &o) {
            if (t.in_bytes >= 1000000) o.end_input = true;
        };
        const StreamResult r = run.run(s, none, c);
        std::uint64_t counted = 0;
        CHECK(r.ok() && !c.lines.empty() && parse_u64(c.lines.empty() ? "" : c.lines[0], counted));
        CHECK(counted >= 1000000 && counted < 500000000);
        CHECK_EQ(counted, r.in_bytes);
    }
    // text in, more sent after a line, the command ends by itself
    {
        StreamSpec s = sh("while read l; do echo \"got $l\"; [ \"$l\" = quit ] && exit 0; done; exit 3");
        s.input = StreamSpec::Input::Text;
        s.text = "hello\n";
        Collect c;
        c.line_fn = [](const std::string &l, StreamOrders &o) {
            if (l == "got hello") o.send = "quit\n";
        };
        const StreamResult r = run.run(s, none, c);
        CHECK(r.ok());
        CHECK(c.lines.size() == 2 && c.lines[0] == "got hello" && c.lines[1] == "got quit");
    }
    // a cancel from another thread: the farewell first (the command quits by itself), within the grace
    {
        const std::string dir = test_tmpdir("stream");
        StreamSpec s = sh("while read l; do echo \"$l\" >> '" + dir + "/log'; [ \"$l\" = quit ] && exit 0; done");
        s.input = StreamSpec::Input::Text;
        s.text = "scan on\n";
        s.farewell = "scan off\nquit\n";
        s.grace_ms = 1000;
        CancelToken cancel;
        Collect c;
        StreamResult r;
        std::thread t([&] { r = run.run(s, cancel, c); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::int64_t t0 = now_ms();
        cancel.cancel();
        t.join();
        CHECK(r.cancelled && !r.ok() && now_ms() - t0 < 1500); /* it quit on its own words, before SIGTERM */
        CHECK_EQ(slurp(dir + "/log"), std::string("scan on\nscan off\nquit\n"));
        remove_dir(dir);
    }
    // the client's stop ends the whole group (the shell and its child)
    {
        StreamSpec s = sh("sleep 30 & echo $! >&2; while :; do head -c 1000 /dev/zero; sleep 0.02; done");
        s.stdout_is_data = true;
        int bg = 0;
        Collect c;
        c.line_fn = [&](const std::string &l, StreamOrders &) { parse_int(l, bg); };
        c.tick_fn = [&](const StreamTick &t, StreamOrders &o) {
            if (t.out_bytes >= 3000) o.stop = true;
        };
        const std::int64_t t0 = now_ms();
        const StreamResult r = run.run(s, none, c);
        CHECK(r.stopped && !r.cancelled && !r.timed_out && now_ms() - t0 < 3000);
        CHECK(r.term_signal == SIGTERM || r.exit_code >= 0);
        CHECK(bg > 1);
        if (bg > 1) CHECK(gone(bg));
    }
    // a command that ignores the farewell and SIGTERM: SIGKILL after the grace
    {
        StreamSpec s = sh("trap '' TERM; while :; do sleep 0.05; done", 400);
        s.input = StreamSpec::Input::Text;
        s.farewell = "quit\n";
        s.grace_ms = 200;
        Collect c;
        const std::int64_t t0 = now_ms();
        const StreamResult r = run.run(s, none, c);
        CHECK(r.timed_out && now_ms() - t0 < 2500);
    }
    // a missing program
    {
        StreamSpec s;
        s.argv = {"net-toolbox-no-such-program"};
        Collect c;
        const StreamResult r = run.run(s, none, c);
        CHECK(r.started && r.exit_code == 127);
        StreamSpec empty;
        CHECK(run.run(empty, none, c).exit_code == 127);
    }
    // a command that does not read its stdin: the write fails (EPIPE), nothing kills the tests
    {
        StreamSpec s = sh("exit 0");
        s.input = StreamSpec::Input::Zeros;
        s.zero_bytes = 50000000;
        Collect c;
        const StreamResult r = run.run(s, none, c);
        CHECK(r.ok() && r.in_bytes < 50000000);
    }
    // a long line comes in pieces of max_line
    {
        StreamSpec s = sh("head -c 10000 /dev/zero | tr '\\0' a; echo");
        s.max_line = 4096;
        Collect c;
        const StreamResult r = run.run(s, none, c);
        CHECK(r.ok() && c.lines.size() == 3);
        if (c.lines.size() == 3) CHECK(c.lines[0].size() == 4096 && c.lines[1].size() == 4096 && c.lines[2].size() == 1808);
    }
    // the kept text is capped; every line still reaches the client
    {
        StreamSpec s = sh("i=0; while [ $i -lt 200 ]; do echo line-$i; i=$((i+1)); done");
        s.max_text = 100;
        Collect c;
        const StreamResult r = run.run(s, none, c);
        CHECK(r.ok() && c.lines.size() == 200 && r.output.size() <= 100 && r.truncated);
    }
}
