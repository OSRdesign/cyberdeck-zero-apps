/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: how a tool runs without blocking the screen (no LVGL).
 *
 *   Worker      ONE thread per running tool (a pthread with a 256 kB stack and the app's quit signals blocked: they
 *               belong to the LVGL thread). start() never waits: a run that is still ending is asked to stop and joined
 *               later (at the next start(), at most a few runs kept, or in the destructor). Every wait inside a tool is
 *               a poll() of at most kSliceMs, so a cancelled run ends within about that time.
 *   CancelToken what a run checks: its own flag, set by cancel(), a new start() or the destructor.
 *   Channel<T>  the results: the run publishes immutable snapshots under a mutex, tagged with its generation; a stale
 *               run's snapshots are dropped. The UI polls seq() (a timer, as System Monitor's sampler) and takes
 *               latest() when it moved. Nothing here waits on the UI thread except the destructor's joins.
 *
 * Worker, Channel::begin() and the session classes that use them are called from one thread (the LVGL thread).
 */
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace nt {

constexpr int kSliceMs = 50; /* the longest wait between two looks at the cancel flag */

std::int64_t now_ms(); /* CLOCK_MONOTONIC */
std::int64_t now_us();

class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    explicit CancelToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}
    bool cancelled() const { return flag_ && flag_->load(); }
    void cancel() const
    {
        if (flag_) flag_->store(true);
    }
    /* sleeps ms in slices; false when cancelled meanwhile */
    bool sleep_ms(int ms) const;

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class Worker {
public:
    using Fn = std::function<void(const CancelToken &cancel)>;
    Worker() = default;
    ~Worker(); /* cancels every run and joins them */
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    /* Cancels the current run (if any) and starts fn on a new thread. Returns the new run's generation (> 0), or 0
     * when no thread could be started (fn did not run). */
    std::uint64_t start(Fn fn);
    /* Asks the current run to stop; returns at once. */
    void cancel();
    /* The current run has not returned yet. */
    bool running() const;
    /* the latest run's generation; the next start() gives generation() + 1 (a session begins its Channel with that
     * value before start(), so the run's first publish is never taken for a stale one) */
    std::uint64_t generation() const { return gen_; }
    /* Tests: waits for the current run to return (true) or the time to pass (false). */
    bool wait(int timeout_ms) const;

    static constexpr std::size_t kStackBytes = 256 * 1024;

private:
    struct Run {
        pthread_t thread{};
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> done{false};
        Fn fn;
    };
    static void *trampoline(void *arg);
    void reap(bool block);

    std::unique_ptr<Run> current_;
    std::vector<std::unique_ptr<Run>> retired_;
    std::uint64_t gen_ = 0;
};

/* The snapshots of one tool. T is copied once per publish: keep it small (the ping ring is 120 entries). */
template <typename T> class Channel {
public:
    /* LVGL thread, at start: the new run's generation and its first snapshot (published at once) */
    void begin(std::uint64_t gen, T first)
    {
        std::lock_guard<std::mutex> lock(mu_);
        gen_ = gen;
        latest_ = std::make_shared<const T>(std::move(first));
        seq_.fetch_add(1);
    }
    /* the run's thread: false (dropped) when a newer run began */
    bool publish(std::uint64_t gen, T value)
    {
        auto p = std::make_shared<const T>(std::move(value));
        std::lock_guard<std::mutex> lock(mu_);
        if (gen != gen_) return false;
        latest_ = std::move(p);
        seq_.fetch_add(1);
        return true;
    }
    std::shared_ptr<const T> latest() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return latest_;
    }
    std::uint64_t seq() const { return seq_.load(); }

private:
    mutable std::mutex mu_;
    std::uint64_t gen_ = 0;
    std::shared_ptr<const T> latest_;
    std::atomic<std::uint64_t> seq_{0};
};

} // namespace nt
