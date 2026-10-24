// Report 033 (Mesh Hop CLI), phase P5: the child on a pseudo-terminal that runs meshcore-cli (core/pty_process.*), with /bin/sh
// as the child. Included by test_core.cpp.

#include "pty_process.hpp"

#include <chrono>
#include <csignal>
#include <thread>

static uint64_t pty_now()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/* Reads the child's output until it exited and its pty gave EOF (or timeout_ms); returns what it wrote. */
static std::string pty_run_out(PtyProcess &p, int timeout_ms)
{
    std::string out;
    const uint64_t end = pty_now() + static_cast<uint64_t>(timeout_ms);
    while (pty_now() < end) {
        p.poll(pty_now());
        p.read(out);
        if (p.exited() && p.eof()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return out;
}

static PtyProcess::Spec sh_spec(const std::string &script)
{
    PtyProcess::Spec s;
    s.argv = {"/bin/sh", "-c", script};
    return s;
}

static void test_pty_process()
{
    {   // output (the pty turns \n into \r\n), the exit code, the environment set and unset
        ::setenv("MESHHOP_PTY_GONE", "still here", 1);
        PtyProcess p;
        PtyProcess::Spec s = sh_spec("printf 'hello\\n'; echo \"$TERM:$MESHHOP_PTY_X:${MESHHOP_PTY_GONE-unset}\"; exit 7");
        s.env = {"TERM=dumb", "MESHHOP_PTY_X=1"};
        s.unset = {"MESHHOP_PTY_GONE"};
        CHECK(p.start(s) && p.running() && p.pid() > 0);
        const std::string out = pty_run_out(p, 3000);
        CHECK(out == "hello\r\ndumb:1:unset\r\n");
        CHECK(p.exited() && p.exit_code() == 7 && p.term_signal() == 0 && !p.running());
        ::unsetenv("MESHHOP_PTY_GONE");
    }
    {   // the pty's echo is off (prompt_toolkit echoes itself): what is written comes back once, from the child only
        PtyProcess p;
        CHECK(p.start(sh_spec("read l; echo \"got:$l\"")));
        p.write("abc\n");
        CHECK(pty_run_out(p, 3000) == "got:abc\r\n");
        CHECK(p.exit_code() == 0);
    }
    {   // the window size of the spec (stty reads it from the terminal)
        PtyProcess p;
        PtyProcess::Spec s = sh_spec("stty size");
        s.cols = 101;
        s.rows = 33;
        CHECK(p.start(s));
        CHECK(pty_run_out(p, 3000) == "33 101\r\n");
    }
    {   // a controlling terminal of its own, in a new session (its own process group), none of Mesh Hop's descriptors
        int fds[2];
        CHECK(::pipe(fds) == 0);                                    // (no CLOEXEC: the child must close it itself)
        PtyProcess p;
        CHECK(p.start(sh_spec("test -t 0 && test -t 1 && echo tty; ls /proc/self/fd | wc -l; ps -o pgid= -p $$ | tr -d ' '")));
        const std::string out = pty_run_out(p, 3000);
        CHECK(out.rfind("tty\r\n", 0) == 0);
        const size_t nl = out.find("\r\n", 5);
        CHECK(nl != std::string::npos && std::atoi(out.substr(5, nl - 5).c_str()) <= 4);   // 0, 1, 2 and ls's own directory
        CHECK(nl != std::string::npos && std::atoi(out.substr(nl + 2).c_str()) == static_cast<int>(p.pid()));
        ::close(fds[0]);
        ::close(fds[1]);
    }
    {   // an argv[0] that cannot run: the reason on the pty, exit 127
        PtyProcess p;
        PtyProcess::Spec s;
        s.argv = {"/nonexistent/mccli", "-v"};
        CHECK(p.start(s));
        const std::string out = pty_run_out(p, 3000);
        CHECK(out.find("cannot start /nonexistent/mccli: ") != std::string::npos);
        CHECK(p.exit_code() == 127);
    }
    {   // stop(): SIGHUP and SIGTERM to the whole group (a child of the child too); the shell ignores both: SIGKILL after the grace
        PtyProcess p;
        CHECK(p.start(sh_spec("trap '' HUP TERM; sleep 30 & echo started; wait")));
        std::string out;
        for (int i = 0; i < 300 && out.find("started") == std::string::npos; ++i) {
            p.read(out);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const pid_t group = p.pid();
        const uint64_t t0 = pty_now();
        p.stop(t0, 300);
        CHECK(p.stopping());
        pty_run_out(p, 3000);
        CHECK(p.exited() && p.term_signal() == SIGKILL && pty_now() - t0 >= 300);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(::kill(-group, 0) != 0 && errno == ESRCH);           // the background sleep went with it
    }
    {   // a plain stop: SIGHUP ends a shell at once
        PtyProcess p;
        CHECK(p.start(sh_spec("sleep 30")));
        p.stop(pty_now(), 2000);
        pty_run_out(p, 3000);
        CHECK(p.exited() && (p.term_signal() == SIGHUP || p.term_signal() == SIGTERM));
    }
    {   // kill_now(): the app's end, nothing left; the destructor of a running one does the same
        PtyProcess p;
        CHECK(p.start(sh_spec("sleep 30")));
        const pid_t pid = p.pid();
        p.kill_now();
        CHECK(p.exited() && p.term_signal() == SIGKILL && ::kill(pid, 0) != 0);
        pid_t pid2 = 0;
        {
            PtyProcess q;
            CHECK(q.start(sh_spec("sleep 30")));
            pid2 = q.pid();
        }
        CHECK(::kill(pid2, 0) != 0);
        // nothing to run, twice started
        PtyProcess r;
        CHECK(!r.start(PtyProcess::Spec{}) && !r.error().empty());
        CHECK(r.start(sh_spec("exit 0")) && !r.start(sh_spec("exit 0")));
        pty_run_out(r, 3000);
        r.write("after the end");                                  // nothing happens once it is gone
        CHECK(r.pending_input() == 0);
    }
}
