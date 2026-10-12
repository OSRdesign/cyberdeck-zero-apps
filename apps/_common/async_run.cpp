/*
 * SPDX-License-Identifier: MIT
 *
 * Background commands for an LVGL app (see async_run.hpp).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "async_run.hpp"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace cdk {
namespace async_run {
namespace {

using Clock = std::chrono::steady_clock;

struct Event {
    bool done = false;
    std::string line;
    Result result;
};

struct Job {
    JobId id = 0;
    pid_t pid = -1;
    int fd = -1;
    Spec spec;                       /* callbacks: LVGL thread only */
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::mutex mu;                   /* queue, armed, exited */
    std::condition_variable exited_cv;
    std::deque<Event> queue;
    bool armed = false;              /* a drain is scheduled */
    bool exited = false;             /* the worker returned */
};

std::mutex g_mu; /* g_jobs, g_next */
std::map<JobId, std::shared_ptr<Job>> g_jobs;
JobId g_next = 1;
std::atomic<bool> g_shutting{false};

std::uint64_t ms_since(Clock::time_point t0)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

std::shared_ptr<Job> find(JobId id)
{
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_jobs.find(id);
    return it == g_jobs.end() ? nullptr : it->second;
}

void join_or_detach(const std::shared_ptr<Job> &job, int wait_ms)
{
    if (!job->worker.joinable() || job->worker.get_id() == std::this_thread::get_id()) return;
    std::unique_lock<std::mutex> lock(job->mu);
    const bool exited = job->exited_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return job->exited; });
    lock.unlock();
    if (exited) job->worker.join();
    else job->worker.detach(); /* it may be waiting for lv_lock (shutdown inside an LVGL callback): it exits alone */
}

void drain_cb(void *data)
{
    const JobId id = static_cast<JobId>(reinterpret_cast<std::uintptr_t>(data));
    std::shared_ptr<Job> job = find(id);
    if (!job) return;
    std::deque<Event> events;
    {
        std::lock_guard<std::mutex> lock(job->mu);
        events.swap(job->queue);
        job->armed = false;
    }
    for (Event &event : events) {
        if (g_shutting.load()) return;
        if (!event.done) {
            if (job->spec.on_line) job->spec.on_line(event.line);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_jobs.erase(id);
        }
        join_or_detach(job, 1500);
        if (job->spec.on_done) job->spec.on_done(event.result);
        return; /* done is the last event */
    }
}

/* Worker side (or run() for a start failure): queue events, schedule one drain on the LVGL thread. */
void post(const std::shared_ptr<Job> &job, std::vector<Event> &&events)
{
    if (events.empty() || g_shutting.load()) return;
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(job->mu);
        for (Event &e : events) job->queue.push_back(std::move(e));
        if (!job->armed) job->armed = schedule = true;
    }
    if (!schedule) return;
    lv_lock(); /* lv_async_call creates an LVGL timer: never without the LVGL lock from another thread */
    const bool ok = !g_shutting.load() &&
                    lv_async_call(drain_cb, reinterpret_cast<void *>(static_cast<std::uintptr_t>(job->id))) == LV_RESULT_OK;
    lv_unlock();
    if (!ok) {
        std::lock_guard<std::mutex> lock(job->mu);
        job->armed = false;
    }
}

void close_from_3()
{
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    for (int fd = 3; fd < 4096; ++fd) close(fd);
}

void worker_main(std::shared_ptr<Job> job)
{
    const Spec &spec = job->spec;
    const Clock::time_point start = Clock::now();
    std::string buffer;
    char chunk[4096];
    Result result;
    result.started = true;
    bool eof = false, reaped = false, term_sent = false, kill_sent = false;
    int status = 0;
    std::uint64_t kill_at = 0, giveup_at = 0, reaped_at = 0, straggler_kill_at = 0;

    while (true) {
        const std::uint64_t now = ms_since(start);
        const bool timeout = spec.timeout_ms > 0 && now >= static_cast<std::uint64_t>(spec.timeout_ms);
        if (!term_sent && (job->cancel.load() || g_shutting.load() || timeout)) {
            if (timeout && !job->cancel.load()) result.timed_out = true;
            else result.cancelled = true;
            killpg(job->pid, SIGTERM);
            term_sent = true;
            kill_at = now + static_cast<std::uint64_t>(spec.grace_ms > 0 ? spec.grace_ms : 0);
        }
        if (term_sent && !kill_sent && now >= kill_at) {
            killpg(job->pid, SIGKILL);
            kill_sent = true;
            giveup_at = now + 1000;
        }
        if (!reaped) {
            const pid_t r = waitpid(job->pid, &status, WNOHANG);
            if (r == job->pid || (r < 0 && errno == ECHILD)) {
                reaped = true;
                reaped_at = now;
            }
        }
        if (!eof) {
            struct pollfd pfd = {job->fd, POLLIN, 0};
            const int pr = poll(&pfd, 1, 50);
            if (pr > 0) {
                while (true) {
                    const ssize_t n = read(job->fd, chunk, sizeof(chunk));
                    if (n > 0) {
                        buffer.append(chunk, static_cast<std::size_t>(n));
                        if (buffer.size() > 64 * 1024) break; /* deliver before reading more */
                        continue;
                    }
                    if (n == 0) eof = true;
                    else if (errno == EINTR) continue;
                    else if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
                    break;
                }
            }
            std::vector<std::string> lines;
            split_lines(buffer, spec.max_line, lines);
            if (!lines.empty()) {
                std::vector<Event> events;
                events.reserve(lines.size());
                for (std::string &line : lines) {
                    Event e;
                    e.line = std::move(line);
                    events.push_back(std::move(e));
                }
                result.lines += events.size();
                post(job, std::move(events));
            }
        } else if (!reaped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (eof && reaped) break;
        /* the leader is gone but something it started still holds the pipe: end the group, then stop waiting */
        if (reaped && !eof) {
            if (!straggler_kill_at) straggler_kill_at = reaped_at + 500;
            if (now >= straggler_kill_at && !kill_sent) {
                killpg(job->pid, SIGKILL);
                kill_sent = true;
                giveup_at = now + 1000;
            }
        }
        if (kill_sent && now >= giveup_at) {
            if (!reaped && waitpid(job->pid, &status, 0) == job->pid) reaped = true; /* SIGKILLed: it ends */
            break;
        }
    }
    if (!buffer.empty()) { /* a last line without its newline */
        Event e;
        e.line = buffer.substr(0, spec.max_line);
        ++result.lines;
        std::vector<Event> events;
        events.push_back(std::move(e));
        post(job, std::move(events));
    }
    close(job->fd);
    job->fd = -1;
    if (reaped) {
        if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) result.term_signal = WTERMSIG(status);
    }
    result.elapsed_ms = ms_since(start);
    Event done;
    done.done = true;
    done.result = result;
    std::vector<Event> events;
    events.push_back(std::move(done));
    post(job, std::move(events));
    {
        std::lock_guard<std::mutex> lock(job->mu);
        job->exited = true;
    }
    job->exited_cv.notify_all();
}

std::vector<std::string> child_environment(const std::vector<std::string> &extra)
{
    std::vector<std::string> env;
    bool lc_all = false;
    for (const std::string &kv : extra)
        if (kv.rfind("LC_ALL=", 0) == 0) lc_all = true;
    for (char **e = environ; e && *e; ++e) {
        const std::string kv(*e);
        const std::string key = kv.substr(0, kv.find('='));
        bool replaced = key == "LC_ALL";
        for (const std::string &x : extra)
            if (x.substr(0, x.find('=')) == key) replaced = true;
        if (!replaced) env.push_back(kv);
    }
    for (const std::string &kv : extra) env.push_back(kv);
    if (!lc_all) env.push_back("LC_ALL=C"); /* parse-stable output (report 024 section 3) */
    return env;
}

} // namespace

void split_lines(std::string &buffer, std::size_t max_line, std::vector<std::string> &out)
{
    if (max_line == 0) max_line = 4096;
    std::size_t start = 0;
    while (start < buffer.size()) {
        const std::size_t nl = buffer.find('\n', start);
        if (nl == std::string::npos) {
            /* no newline yet: only cut a piece when the line is already too long */
            while (buffer.size() - start >= max_line) {
                out.push_back(buffer.substr(start, max_line));
                start += max_line;
            }
            break;
        }
        std::size_t end = nl;
        if (end > start && buffer[end - 1] == '\r') --end;
        std::size_t pos = start;
        do {
            const std::size_t len = std::min(max_line, end - pos);
            out.push_back(buffer.substr(pos, len));
            pos += len;
        } while (pos < end);
        start = nl + 1;
    }
    buffer.erase(0, start);
}

JobId run(Spec spec)
{
    auto job = std::make_shared<Job>();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        job->id = g_next++;
    }
    job->spec = std::move(spec);
    const auto fail = [&]() -> JobId {
        Event done;
        done.done = true; /* started = false */
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_jobs[job->id] = job;
        }
        std::vector<Event> events;
        events.push_back(std::move(done));
        post(job, std::move(events));
        return 0;
    };
    if (job->spec.argv.empty() || g_shutting.load()) return fail();

    /* everything the child needs is built before fork: after it only async-signal-safe calls */
    std::vector<std::string> env = child_environment(job->spec.env);
    std::vector<char *> argv, envp;
    for (std::string &a : job->spec.argv) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    for (std::string &e : env) envp.push_back(&e[0]);
    envp.push_back(nullptr);

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return fail();
    const pid_t parent = getpid();
    const bool merge = job->spec.merge_stderr;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return fail();
    }
    if (pid == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(127); /* the app died between fork and prctl */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        struct sigaction dfl;
        std::memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &dfl, nullptr);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(merge ? fds[1] : (devnull >= 0 ? devnull : fds[1]), 2);
        close_from_3(); /* the framebuffer, input devices and sockets of the app stay with the app */
        execvpe(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid); /* both sides: no window where a kill would miss the group */
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    job->pid = pid;
    job->fd = fds[0];
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_jobs[job->id] = job;
    }
    /* the worker must not take the app's quit signals: they belong to the LVGL thread */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    job->worker = std::thread(worker_main, job);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    return job->id;
}

void cancel(JobId id)
{
    if (std::shared_ptr<Job> job = find(id)) job->cancel.store(true);
}

std::size_t running()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_jobs.size();
}

void shutdown()
{
    std::map<JobId, std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        jobs.swap(g_jobs);
    }
    if (jobs.empty()) return;
    g_shutting.store(true);
    int wait_ms = 0;
    for (auto &kv : jobs) {
        kv.second->cancel.store(true);
        if (kv.second->pid > 0) killpg(kv.second->pid, SIGTERM);
        wait_ms = std::max(wait_ms, kv.second->spec.grace_ms + 1500);
    }
    for (auto &kv : jobs) join_or_detach(kv.second, wait_ms);
    /* drains already scheduled find no job (g_jobs is empty) and return; cancel them anyway, under the LVGL lock */
    lv_lock();
    for (auto &kv : jobs)
        lv_async_call_cancel(drain_cb, reinterpret_cast<void *>(static_cast<std::uintptr_t>(kv.first)));
    lv_unlock();
    /* g_shutting stays set: the app is ending, a worker that had to be detached must never post again */
}

} // namespace async_run
} // namespace cdk
