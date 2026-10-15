/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: external commands (see cmdrun.hpp).
 */
#include "cmdrun.hpp"

#include "sock.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
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

std::vector<std::string> with_curl_interface(std::vector<std::string> argv, const std::string &iface)
{
    if (argv.empty() || iface.empty() || !valid_iface_name(iface)) return argv;
    const std::string &cmd = argv[0];
    const bool is_curl = cmd == "curl" || (cmd.size() > 5 && cmd.compare(cmd.size() - 5, 5, "/curl") == 0);
    if (!is_curl) return argv;
    for (const std::string &a : argv)
        if (a == "--interface") return argv;
    argv.insert(argv.begin() + 1, {"--interface", iface});
    return argv;
}

std::vector<std::string> scoped_argv(std::vector<std::string> argv)
{
    return with_curl_interface(std::move(argv), active_interface());
}

namespace {

/* posix_spawnp in its own process group with the default signals and LC_ALL=C; stdin from in_fd (-1: /dev/null).
 * The pid, or -1 when it could not be executed. */
pid_t spawn_child(const std::vector<std::string> &argv_in, int in_fd, int out_fd, int err_fd)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    if (in_fd < 0) posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    else posix_spawn_file_actions_adddup2(&fa, in_fd, 0);
    posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    posix_spawn_file_actions_adddup2(&fa, err_fd, 2);
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (const int s : {SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGCHLD}) sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<std::string> args = scoped_argv(argv_in); /* curl bound to the chosen network */
    std::vector<std::string> env = child_env();
    std::vector<char *> argv = c_array(args), envp = c_array(env);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
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
    const pid_t pid = spawn_child(spec.argv, -1, fds[1], fds[1]);
    const int rc = pid > 0 ? 0 : -1;
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

/* ---------------------------------------------------------------- streams */

namespace {

/* SIGPIPE blocked in this thread while it writes to a command's stdin; one raised meanwhile is taken off before the old
 * mask comes back (a write to a command that is gone then fails with EPIPE, nothing else) */
class SigpipeGuard {
public:
    SigpipeGuard()
    {
        sigemptyset(&pipe_);
        sigaddset(&pipe_, SIGPIPE);
        sigset_t pending;
        sigemptyset(&pending);
        sigpending(&pending);
        was_pending_ = sigismember(&pending, SIGPIPE) == 1;
        pthread_sigmask(SIG_BLOCK, &pipe_, &old_);
    }
    ~SigpipeGuard()
    {
        if (!was_pending_) {
            struct timespec zero = {0, 0};
            while (sigtimedwait(&pipe_, nullptr, &zero) == SIGPIPE) {
            }
        }
        pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }
    SigpipeGuard(const SigpipeGuard &) = delete;
    SigpipeGuard &operator=(const SigpipeGuard &) = delete;

private:
    sigset_t pipe_{};
    sigset_t old_{};
    bool was_pending_ = false;
};

/* one text pipe cut into lines */
struct LineSplit {
    std::string buf;
    void feed(const char *p, std::size_t n, std::size_t max_line, std::vector<std::string> &out)
    {
        buf.append(p, n);
        std::size_t from = 0;
        for (;;) {
            const std::size_t nl = buf.find('\n', from);
            if (nl == std::string::npos) break;
            out.push_back(buf.substr(from, nl - from));
            from = nl + 1;
        }
        buf.erase(0, from);
        while (max_line && buf.size() >= max_line) {
            out.push_back(buf.substr(0, max_line));
            buf.erase(0, max_line);
        }
    }
    void flush(std::vector<std::string> &out)
    {
        if (!buf.empty()) out.push_back(buf);
        buf.clear();
    }
};

void set_nonblock(int fd)
{
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* reads what is there and drops it (the command must not block on a full pipe while it ends) */
void drain(Fd &fd)
{
    if (!fd.valid()) return;
    char buf[4096];
    for (int i = 0; i < 64; ++i) {
        const ssize_t n = read(fd.get(), buf, sizeof(buf));
        if (n > 0) continue;
        if (n == 0) fd.reset();
        break;
    }
}

bool wait_ended_draining(pid_t pid, int ms, Fd &a, Fd &b)
{
    const std::int64_t end = now_ms() + ms;
    for (;;) {
        if (ended(pid)) return true;
        if (now_ms() >= end) return false;
        struct pollfd p[2];
        nfds_t n = 0;
        if (a.valid()) p[n++] = {a.get(), POLLIN, 0};
        if (b.valid()) p[n++] = {b.get(), POLLIN, 0};
        if (n) poll(p, n, 5);
        else nap();
        drain(a);
        drain(b);
    }
}

const char kZeros[16384] = {};

} // namespace

StreamResult DirectStreamRunner::run(const StreamSpec &spec, const CancelToken &cancel, StreamClient &client)
{
    StreamResult r;
    const std::int64_t start = now_ms();
    if (spec.argv.empty() || spec.argv[0].empty()) {
        r.exit_code = 127;
        r.started = true;
        return r;
    }
    SigpipeGuard guard;
    int out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, in_p[2] = {-1, -1};
    const auto close_all = [&] {
        for (int *p : {out_p, err_p, in_p})
            for (int i = 0; i < 2; ++i)
                if (p[i] >= 0) close(p[i]);
    };
    if (pipe2(out_p, O_CLOEXEC) != 0 || (spec.stdout_is_data && pipe2(err_p, O_CLOEXEC) != 0) ||
        (spec.input != StreamSpec::Input::Null && pipe2(in_p, O_CLOEXEC) != 0)) {
        close_all();
        return r;
    }
    const pid_t pid = spawn_child(spec.argv, in_p[0], out_p[1], spec.stdout_is_data ? err_p[1] : out_p[1]);
    for (int *fd : {&out_p[1], &err_p[1], &in_p[0]}) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
    }
    if (pid <= 0) {
        close_all();
        r.started = true; /* as async_run: the command could not be executed */
        r.exit_code = 127;
        r.elapsed_ms = static_cast<int>(now_ms() - start);
        return r;
    }
    r.started = true;
    Fd out(out_p[0]), err(err_p[0]), in(in_p[1]);
    set_nonblock(out.get());
    set_nonblock(err.get());
    set_nonblock(in.get());

    const std::int64_t deadline = spec.timeout_ms > 0 ? start + spec.timeout_ms : 0;
    const bool text_in = spec.input == StreamSpec::Input::Text;
    std::uint64_t zeros_left = spec.input == StreamSpec::Input::Zeros ? spec.zero_bytes : 0;
    std::string pending = text_in ? spec.text : std::string();
    std::size_t pending_at = 0;
    bool end_input = false, exited = false;
    std::int64_t exited_at = 0;
    LineSplit out_lines, err_lines;
    std::vector<std::string> lines;
    std::vector<char> buf(65536);

    const auto keep_text = [&](const std::string &line) {
        const std::size_t need = line.size() + (r.output.empty() ? 0 : 1);
        if (r.output.size() + need > spec.max_text) {
            r.truncated = true;
            return;
        }
        if (!r.output.empty()) r.output += '\n';
        r.output += line;
    };
    const auto obey = [&](StreamOrders &o) {
        if (!o.send.empty() && text_in && in.valid()) pending += o.send;
        if (o.end_input) end_input = true;
        if (o.stop) r.stopped = true;
        o = StreamOrders();
    };
    /* stdin: what waits, as far as the pipe takes it; closed when there is nothing more to give */
    const auto feed_input = [&] {
        if (in.valid() && end_input && !text_in) in.reset(); /* not one byte more than the client saw */
        while (in.valid()) {
            const char *p = nullptr;
            std::size_t n = 0;
            if (text_in && pending_at < pending.size()) {
                p = pending.data() + pending_at;
                n = pending.size() - pending_at;
            } else if (zeros_left) {
                p = kZeros;
                n = static_cast<std::size_t>(std::min<std::uint64_t>(zeros_left, sizeof(kZeros)));
            }
            if (!n) break;
            const ssize_t w = write(in.get(), p, n);
            if (w > 0) {
                r.in_bytes += static_cast<std::uint64_t>(w);
                if (text_in) pending_at += static_cast<std::size_t>(w);
                else zeros_left -= static_cast<std::uint64_t>(w);
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            in.reset(); /* EPIPE: it stopped reading */
        }
        if (text_in && pending_at >= pending.size()) {
            pending.clear();
            pending_at = 0;
        }
        const bool nothing_left = text_in ? pending.empty() : zeros_left == 0;
        if (in.valid() && nothing_left && (end_input || spec.input == StreamSpec::Input::Zeros)) in.reset();
        if (in.valid() && end_input && !text_in) in.reset();
    };

    feed_input();
    while (!r.stopped) {
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
        if (exited && !out.valid() && !err.valid()) break;
        if (exited && now - exited_at > 200) break; /* something it left behind holds a pipe: ended below */
        struct pollfd p[3];
        nfds_t n = 0;
        int i_out = -1, i_err = -1, i_in = -1;
        if (out.valid()) {
            i_out = static_cast<int>(n);
            p[n++] = {out.get(), POLLIN, 0};
        }
        if (err.valid()) {
            i_err = static_cast<int>(n);
            p[n++] = {err.get(), POLLIN, 0};
        }
        if (in.valid() && ((text_in && pending_at < pending.size()) || zeros_left)) {
            i_in = static_cast<int>(n);
            p[n++] = {in.get(), POLLOUT, 0};
        }
        int wait = kSliceMs;
        if (deadline) wait = static_cast<int>(std::min<std::int64_t>(wait, deadline - now));
        const int pr = n ? poll(p, n, std::max(wait, 1)) : 0;
        if (!n) nap();
        if (pr < 0 && errno != EINTR) break;
        /* stdout */
        if (i_out >= 0 && p[i_out].revents) {
            for (int k = 0; k < 16 && out.valid(); ++k) {
                const ssize_t got = read(out.get(), buf.data(), buf.size());
                if (got > 0) {
                    if (spec.stdout_is_data) r.out_bytes += static_cast<std::uint64_t>(got);
                    else out_lines.feed(buf.data(), static_cast<std::size_t>(got), spec.max_line, lines);
                    continue;
                }
                if (got == 0) {
                    out_lines.flush(lines);
                    out.reset();
                }
                break; /* EAGAIN, EINTR, an error: back to poll */
            }
        }
        if (i_err >= 0 && p[i_err].revents) {
            for (int k = 0; k < 16 && err.valid(); ++k) {
                const ssize_t got = read(err.get(), buf.data(), buf.size());
                if (got > 0) {
                    err_lines.feed(buf.data(), static_cast<std::size_t>(got), spec.max_line, lines);
                    continue;
                }
                if (got == 0) {
                    err_lines.flush(lines);
                    err.reset();
                }
                break;
            }
        }
        if (i_in >= 0 && p[i_in].revents) feed_input();
        StreamOrders orders;
        for (const std::string &line : lines) {
            keep_text(line);
            client.on_line(line, now_us(), orders);
            obey(orders);
        }
        lines.clear();
        StreamTick tick;
        tick.at_us = now_us();
        tick.out_bytes = r.out_bytes;
        tick.in_bytes = r.in_bytes;
        tick.input_open = in.valid();
        client.on_tick(tick, orders);
        obey(orders);
        feed_input();
    }
    if (!r.cancelled) { /* a last line without its '\n' */
        out_lines.flush(lines);
        err_lines.flush(lines);
        StreamOrders late;
        for (const std::string &line : lines) {
            keep_text(line);
            client.on_line(line, now_us(), late);
        }
        lines.clear();
    }

    if (!exited && !ended(pid)) {
        bool gone = false;
        if (text_in && in.valid() && !spec.farewell.empty()) {
            /* the polite way first: the command's own words to stop (bluetoothctl: scan off, quit) */
            const ssize_t w = write(in.get(), spec.farewell.data(), spec.farewell.size());
            if (w > 0) r.in_bytes += static_cast<std::uint64_t>(w);
            in.reset();
            gone = wait_ended_draining(pid, std::max(0, spec.grace_ms), out, err);
        }
        in.reset();
        if (!gone) {
            killpg(pid, SIGTERM);
            if (!wait_ended_draining(pid, std::max(0, spec.grace_ms), out, err)) killpg(pid, SIGKILL);
        }
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

} // namespace nt
