/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: the command worker (see worker.hpp).
 */
#include "worker.hpp"

namespace cassette {
namespace bind {

Worker::Worker(bool threaded) : threaded_(threaded)
{
    if (threaded_) th_ = std::thread([this] { loop(); });
}

Worker::~Worker()
{
    finish();
}

void Worker::post(std::function<void()> job)
{
    if (!job) return;
    if (!threaded_) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stop_) return;
        }
        job();
        std::lock_guard<std::mutex> g(mu_);
        ++done_;
        return;
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stop_) return;
        q_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void Worker::loop()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return; /* stop_ and nothing left */
            job = std::move(q_.front());
            q_.pop_front();
            busy_ = true;
        }
        job();
        {
            std::lock_guard<std::mutex> g(mu_);
            busy_ = false;
            ++done_;
        }
        idle_cv_.notify_all();
    }
}

void Worker::finish()
{
    {
        std::lock_guard<std::mutex> g(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    idle_cv_.notify_all();
}

void Worker::wait_idle()
{
    if (!threaded_) return;
    std::unique_lock<std::mutex> l(mu_);
    idle_cv_.wait(l, [this] { return q_.empty() && !busy_; });
}

unsigned long Worker::done() const
{
    std::lock_guard<std::mutex> g(mu_);
    return done_;
}

} // namespace bind
} // namespace cassette
