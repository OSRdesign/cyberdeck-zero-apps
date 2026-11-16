/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the scenes worker (brief 3.2): one thread for the file jobs (in M2: saving a photo, listing the photos, decoding
 * one for the review, deleting one). Jobs go in a queue; a job's completion is queued back and run by drain() on the
 * owner's thread (the LVGL timer), so no LVGL call and no shared state happen on the worker. LVGL-free.
 */
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace vhs {

class Worker {
public:
    Worker() = default;
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void start();
    /* The jobs queued and running finish (a photo taken is never dropped), then the thread is joined; their completions
     * stay queued for a drain(). */
    void stop();

    /* A job for the worker thread; it returns the completion to run on the owner's thread (may be empty). */
    using Job = std::function<std::function<void()>()>;
    void post(Job job);
    /* Runs the completions that arrived (owner's thread); the number run. */
    int drain();
    /* Jobs queued or running. */
    int pending() const;

private:
    void run();
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<std::function<void()>> done_;
    std::thread thread_;
    bool stop_ = false;
    int running_ = 0;
};

} // namespace vhs
