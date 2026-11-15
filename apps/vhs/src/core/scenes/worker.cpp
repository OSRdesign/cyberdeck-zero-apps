/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the scenes worker (see worker.hpp).
 */
#include "worker.hpp"

namespace vhs {

Worker::~Worker()
{
    stop();
}

void Worker::start()
{
    std::lock_guard<std::mutex> lock(m_);
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void Worker::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!thread_.joinable()) return;
        stop_ = true; /* the jobs queued still run: a photo taken is never dropped */
    }
    cv_.notify_all();
    thread_.join();
}

void Worker::post(Job job)
{
    {
        std::lock_guard<std::mutex> lock(m_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

int Worker::drain()
{
    std::deque<std::function<void()>> done;
    {
        std::lock_guard<std::mutex> lock(m_);
        done.swap(done_);
    }
    for (auto &f : done)
        if (f) f();
    return static_cast<int>(done.size());
}

int Worker::pending() const
{
    std::lock_guard<std::mutex> lock(m_);
    return static_cast<int>(jobs_.size()) + running_ + static_cast<int>(done_.size());
}

void Worker::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return; /* stopped, nothing left */
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }
        std::function<void()> done = job ? job() : std::function<void()>();
        std::lock_guard<std::mutex> lock(m_);
        --running_;
        done_.push_back(std::move(done));
    }
}

} // namespace vhs
