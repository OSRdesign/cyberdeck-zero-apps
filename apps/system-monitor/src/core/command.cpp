/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: a command with a timeout (see command.hpp).
 */
#include "command.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

extern char **environ;

namespace sm {
namespace {

long long mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

CmdResult run_command(const std::vector<std::string> &argv, int timeout_ms, std::size_t max_out)
{
    CmdResult r;
    if (argv.empty()) {
        r.error = EINVAL;
        return r;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        r.error = errno;
        return r;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    // the environment with LC_ALL=C (parsed output is the C locale's)
    std::vector<std::string> env_strings;
    for (char **e = environ; e && *e; ++e)
        if (std::strncmp(*e, "LC_ALL=", 7) != 0) env_strings.emplace_back(*e);
    env_strings.emplace_back("LC_ALL=C");
    std::vector<char *> envp;
    for (std::string &s : env_strings) envp.push_back(&s[0]);
    envp.push_back(nullptr);
    std::vector<std::string> args = argv;
    std::vector<char *> argp;
    for (std::string &s : args) argp.push_back(&s[0]);
    argp.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argp[0], &actions, &attr, argp.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        r.error = rc;
        return r;
    }
    r.started = true;

    const long long deadline = mono_ms() + (timeout_ms > 0 ? timeout_ms : 1000);
    char buf[1024];
    for (;;) {
        const long long left = deadline - mono_ms();
        if (left <= 0) {
            r.timed_out = true;
            break;
        }
        struct pollfd p = {fds[0], POLLIN, 0};
        const int n = poll(&p, 1, static_cast<int>(left));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;
        const ssize_t got = read(fds[0], buf, sizeof(buf));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (got == 0) break; /* every writer closed: the command ended (or closed its output) */
        if (r.output.size() < max_out)
            r.output.append(buf, static_cast<std::size_t>(got) < max_out - r.output.size()
                                     ? static_cast<std::size_t>(got)
                                     : max_out - r.output.size());
    }
    close(fds[0]);
    if (r.timed_out) kill(-pid, SIGKILL);
    // reap it: a command that closed its output but goes on is waited for until the deadline, then killed
    int status = 0;
    bool reaped = false;
    for (;;) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0 && errno != EINTR) break; /* ECHILD: SIGCHLD ignored elsewhere, the child was reaped for us */
        if (mono_ms() >= deadline && !r.timed_out) {
            r.timed_out = true;
            kill(-pid, SIGKILL);
        }
        if (r.timed_out) {
            reaped = waitpid(pid, &status, 0) == pid;
            break;
        }
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    if (!reaped) r.exit_code = r.timed_out ? -1 : 0; /* status unknown: the output decides */
    else if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.signal = WTERMSIG(status);
    return r;
}

} // namespace sm
