/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: the command worker. The UI thread posts every controller command here and returns at once; one
 * thread runs them in the order posted (a REW down is always followed by its up, a stepper's taps add up). A command
 * that waits (STOP closing a take and syncing it, a card being opened, a folder listed) therefore never holds up a frame.
 *
 * Inline mode (tests): post() runs the job on the calling thread. No LVGL.
 */
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace cassette {
namespace bind {

class Worker {
public:
    explicit Worker(bool threaded = true);
    ~Worker(); /* finish() */
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    /* Queues a job (ignored after finish()). Any thread but the worker's own. */
    void post(std::function<void()> job);
    /* Runs what is queued, then stops the thread; later posts are dropped. Idempotent. */
    void finish();
    /* Waits until the queue is empty and no job runs (tests). */
    void wait_idle();
    /* Jobs run so far. */
    unsigned long done() const;

private:
    void loop();
    const bool threaded_;
    mutable std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false, busy_ = false;
    unsigned long done_ = 0;
    std::thread th_;
};

} // namespace bind
} // namespace cassette
