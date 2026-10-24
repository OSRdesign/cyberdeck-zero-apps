/*
 * SPDX-License-Identifier: MIT
 *
 * A child process on a pseudo-terminal (see pty_process.hpp).
 */

#include "pty_process.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

namespace meshzero {

namespace {

/* The child's environment: Mesh Hop's, without the names of `unset` and of `set`, then `set`. Built before fork (no allocation in the child). */
std::vector<std::string> child_env(const std::vector<std::string> &set, const std::vector<std::string> &unset)
{
    std::vector<std::string> out;
    auto name_of = [](const std::string &kv) { return kv.substr(0, kv.find('=')); };
    for (char **e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const std::string n = name_of(kv);
        bool drop = false;
        for (const std::string &u : unset) drop = drop || u == n;
        for (const std::string &s : set) drop = drop || name_of(s) == n;
        if (!drop) out.push_back(kv);
    }
    for (const std::string &s : set) out.push_back(s);
    return out;
}

void say(int fd, const char *a, const char *b = "", const char *c = "")
{
    for (const char *s : {a, b, c})
        if (s && *s && ::write(fd, s, std::strlen(s)) < 0) return;
}

} // namespace

PtyProcess::~PtyProcess()
{
    if (running()) kill_now();
    close_master();
}

bool PtyProcess::start(const Spec &spec)
{
    error_.clear();
    if (pid_ > 0 || spec.argv.empty()) {
        error_ = pid_ > 0 ? "already started" : "nothing to run";
        return false;
    }
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        error_ = std::string("no pseudo-terminal: ") + std::strerror(errno);
        if (master >= 0) ::close(master);
        return false;
    }
    char name[128] = {};
    if (::ptsname_r(master, name, sizeof(name)) != 0) {
        error_ = std::string("no pseudo-terminal name: ") + std::strerror(errno);
        ::close(master);
        return false;
    }
    // the slave opened here, before the fork (as openpty does): the master never reads a pty nobody opened yet
    const int slave = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        error_ = std::string("cannot open ") + name + ": " + std::strerror(errno);
        ::close(master);
        return false;
    }
    {   // the echo off (prompt_toolkit echoes what it reads), the window size
        struct termios t {};
        if (::tcgetattr(slave, &t) == 0) {
            t.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL);
            ::tcsetattr(slave, TCSANOW, &t);
        }
        struct winsize ws {};
        ws.ws_col = static_cast<unsigned short>(spec.cols > 0 ? spec.cols : 80);
        ws.ws_row = static_cast<unsigned short>(spec.rows > 0 ? spec.rows : 24);
        ::ioctl(slave, TIOCSWINSZ, &ws);
    }
    // everything the child needs, prepared before the fork: the child only calls async-signal-safe functions
    const std::vector<std::string> env = child_env(spec.env, spec.unset);
    std::vector<char *> argv, envp;
    for (const std::string &a : spec.argv) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    for (const std::string &e : env) envp.push_back(const_cast<char *>(e.c_str()));
    envp.push_back(nullptr);
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    const pid_t parent = ::getpid();

    const pid_t pid = ::fork();
    if (pid < 0) {
        error_ = std::string("cannot fork: ") + std::strerror(errno);
        ::close(slave);
        ::close(master);
        return false;
    }
    if (pid == 0) {
        // the child: a new session on the pty, Mesh Hop's descriptors closed, the default signals, then the program
        ::setsid();
        ::ioctl(slave, TIOCSCTTY, 0);
        ::dup2(slave, 0);
        ::dup2(slave, 1);
        ::dup2(slave, 2);
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (long fd = 3; fd < max_fd; ++fd) ::close(static_cast<int>(fd));
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        for (int s = 1; s < NSIG; ++s)
            if (s != SIGKILL && s != SIGSTOP) ::sigaction(s, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) ::_exit(127);          // Mesh Hop is already gone
        ::execve(argv[0], argv.data(), envp.data());
        say(2, "cannot start ", argv[0], ": ");
        say(2, std::strerror(errno), "\r\n");
        ::_exit(127);
    }
    ::close(slave);
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    master_ = master;
    pid_ = pid;
    reaped_ = eof_ = killed_ = false;
    exit_code_ = -1;
    term_signal_ = 0;
    stop_at_ = kill_at_ = 0;
    in_.clear();
    return true;
}

size_t PtyProcess::read(std::string &out, size_t max)
{
    if (master_ < 0 || eof_) return 0;
    size_t got = 0;
    char buf[4096];
    while (got < max) {
        const ssize_t n = ::read(master_, buf, std::min(sizeof(buf), max - got));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            got += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        eof_ = true;                                         // EIO (or 0): every holder of the pty closed it
        break;
    }
    return got;
}

void PtyProcess::write(const std::string &bytes)
{
    if (master_ < 0 || eof_ || reaped_ || bytes.empty()) return;
    in_ += bytes;
    poll(0);
}

void PtyProcess::poll(uint64_t now)
{
    while (master_ >= 0 && !in_.empty() && !eof_) {
        const ssize_t n = ::write(master_, in_.data(), in_.size());
        if (n > 0) {
            in_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        in_.clear();                                         // the pty refuses: nobody will read it
        break;
    }
    if (pid_ > 0 && !reaped_) {
        reap(false);
        if (!reaped_ && kill_at_ && now >= kill_at_ && !killed_) {
            ::kill(-pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
            killed_ = true;
        }
    }
}

void PtyProcess::stop(uint64_t now, uint32_t grace_ms)
{
    if (pid_ <= 0 || reaped_ || stop_at_) return;
    stop_at_ = now ? now : 1;
    kill_at_ = stop_at_ + grace_ms;
    ::kill(-pid_, SIGHUP);
    ::kill(-pid_, SIGTERM);
    ::kill(pid_, SIGTERM);                                   // (in case it left its group)
}

void PtyProcess::kill_now()
{
    if (pid_ > 0 && !reaped_) {
        ::kill(-pid_, SIGKILL);
        ::kill(pid_, SIGKILL);
        killed_ = true;
        reap(true);
    }
    close_master();
}

void PtyProcess::resize(int cols, int rows)
{
    if (master_ < 0 || cols <= 0 || rows <= 0) return;
    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ::ioctl(master_, TIOCSWINSZ, &ws);
}

void PtyProcess::reap(bool wait)
{
    int st = 0;
    pid_t r;
    do {
        r = ::waitpid(pid_, &st, wait ? 0 : WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == pid_) {
        reaped_ = true;
        if (WIFEXITED(st)) exit_code_ = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) term_signal_ = WTERMSIG(st);
    } else if (r < 0 && errno == ECHILD) {
        reaped_ = true;                                      // reaped by someone else: how it ended is unknown
    }
}

void PtyProcess::close_master()
{
    if (master_ >= 0) ::close(master_);
    master_ = -1;
}

} // namespace meshzero
