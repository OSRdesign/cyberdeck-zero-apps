/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the one background thread that does everything slow or that touches the disk (the engine's listing,
 * imports, removals, the cache statistics and clearing, key and switch writes, folder listings of the file browser).
 * A job runs on the worker and returns a closure; the closure runs on the LVGL thread at the next poll() (the app's
 * LVGL timer), so no LVGL call is ever made off the LVGL thread and nothing slow runs on it. Jobs run one at a time,
 * in order. shutdown() lets the running job end (set its cancel flag first) and drops the rest.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace atlasui {

class Worker {
public:
    using Done = std::function<void()>;
    using Job = std::function<Done()>;

    Worker();
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void submit(Job job);
    /* LVGL thread: runs the closures of the jobs that ended, in order. */
    void poll();
    /* Jobs queued or running. */
    int pending() const { return pending_.load(); }
    void shutdown();

private:
    void run();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    bool stop_ = false;
    std::atomic<int> pending_{0};
    std::thread thread_; /* last: started in the constructor's body, after the members above */
};

} // namespace atlasui
