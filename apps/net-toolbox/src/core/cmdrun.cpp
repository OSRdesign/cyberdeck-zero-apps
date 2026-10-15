/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: external commands (see cmdrun.hpp).
 */
#include "cmdrun.hpp"

#include "sock.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

extern char **environ;

namespace nt {
namespace {

/* the app's environment with LC_ALL=C (curl's -w numbers and openssl's dates in the C locale) */
std::vector<std::string> child_env()
{
    std::vector<std::string> env;
    for (char **e = environ; e && *e; ++e) {
        if (std::strncmp(*e, "LC_ALL=", 7) == 0) continue;
        env.emplace_back(*e);
    }
    env.emplace_back("LC_ALL=C");
    return env;
}

std::vector<char *> c_array(std::vector<std::string> &v)
{
    std::vector<char *> out;
    out.reserve(v.size() + 1);
    for (std::string &s : v) out.push_back(&s[0]);
    out.push_back(nullptr);
    return out;
}

/* the child ended; it is not reaped, so its process group id stays taken until waitpid() */
bool ended(pid_t pid)
{
    siginfo_t si{};
    return waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid;
}

void nap()
{
    struct timespec ts = {0, 5 * 1000 * 1000};
    nanosleep(&ts, nullptr);
}

bool wait_ended(pid_t pid, int ms)
{
    const std::int64_t end = now_ms() + ms;
    while (!ended(pid)) {
        if (now_ms() >= end) return false;
        nap();
    }
    return true;
}

} // namespace

CommandResult DirectRunner::run(const CommandSpec &spec, const CancelToken &cancel)
{
    CommandResult r;
    const std::int64_t start = now_ms();
    if (spec.argv.empty() || spec.argv[0].empty()) {
        r.exit_code = 127;
        r.started = true;
        return r;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return r;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (const int s : {SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGCHLD}) sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<std::string> args = spec.argv;
    std::vector<std::string> env = child_env();
    std::vector<char *> argv = c_array(args), envp = c_array(env);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        /* as async_run: the command could not be executed */
        r.started = true;
        r.exit_code = 127;
        r.elapsed_ms = static_cast<int>(now_ms() - start);
        return r;
    }
    r.started = true;
    Fd out(fds[0]);
    fcntl(out.get(), F_SETFL, fcntl(out.get(), F_GETFL) | O_NONBLOCK);
    const std::int64_t deadline = spec.timeout_ms > 0 ? start + spec.timeout_ms : 0;
    bool eof = false, exited = false;
    std::int64_t exited_at = 0;
    char buf[4096];
    while (!eof) {
        if (cancel.cancelled()) {
            r.cancelled = true;
            break;
        }
        const std::int64_t now = now_ms();
        if (deadline && now >= deadline) {
            r.timed_out = true;
            break;
        }
        if (!exited && ended(pid)) {
            exited = true;
            exited_at = now;
        }
        if (exited && now - exited_at > 200) break; /* something it left behind holds the pipe: ended below */
        int wait = kSliceMs;
        if (deadline) wait = static_cast<int>(std::min<std::int64_t>(wait, deadline - now));
        struct pollfd p = {out.get(), POLLIN, 0};
        const int pr = poll(&p, 1, std::max(wait, 1));
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        for (;;) {
            const ssize_t n = read(out.get(), buf, sizeof(buf));
            if (n > 0) {
                const std::size_t room = spec.max_output > r.output.size() ? spec.max_output - r.output.size() : 0;
                const std::size_t keep = std::min<std::size_t>(static_cast<std::size_t>(n), room);
                r.output.append(buf, keep);
                if (keep < static_cast<std::size_t>(n)) r.truncated = true;
                continue;
            }
            if (n == 0) eof = true;
            break; /* EAGAIN, EINTR or an error: back to poll */
        }
    }
    /* the output ended: the command ends too, within its time */
    while (!exited && !r.cancelled && !r.timed_out) {
        if (ended(pid)) {
            exited = true;
        } else if (cancel.cancelled()) {
            r.cancelled = true;
        } else if (deadline && now_ms() >= deadline) {
            r.timed_out = true;
        } else {
            nap();
        }
    }
    if (!exited) {
        killpg(pid, SIGTERM);
        if (!wait_ended(pid, std::max(0, spec.grace_ms))) killpg(pid, SIGKILL);
    }
    killpg(pid, SIGKILL); /* anything left in its group; the leader still holds the group id until reaped */
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) r.exit_code = r.cancelled || r.timed_out ? -1 : WEXITSTATUS(status);
    else if (WIFSIGNALED(status) && !r.cancelled && !r.timed_out) r.term_signal = WTERMSIG(status);
    r.elapsed_ms = static_cast<int>(now_ms() - start);
    return r;
}

bool run_flow(CommandFlow &flow, CommandRunner &runner, const CancelToken &cancel, const std::function<void()> &after_step)
{
    CommandSpec cmd;
    while (flow.next(cmd)) {
        CommandResult r;
        if (cancel.cancelled()) r.cancelled = true;
        else r = runner.run(cmd, cancel);
        flow.feed(r);
        if (after_step) after_step();
        if (r.cancelled || cancel.cancelled()) return false;
        cmd = CommandSpec();
    }
    return !cancel.cancelled();
}

} // namespace nt
