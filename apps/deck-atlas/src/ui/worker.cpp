/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the background thread (see worker.hpp).
 */
#include "worker.hpp"

namespace atlasui {

/* the thread starts in the body: every member it uses (the mutex, the condition, the queues) exists by then */
Worker::Worker() { thread_ = std::thread([this] { run(); }); }

Worker::~Worker() { shutdown(); }

void Worker::submit(Job job)
{
    if (!job) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        jobs_.push_back(std::move(job));
        ++pending_;
    }
    cv_.notify_one();
}

void Worker::poll()
{
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        done.swap(done_);
    }
    for (Done &d : done) {
        if (d) d();
        --pending_;
    }
}

void Worker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_ && !thread_.joinable()) return;
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mu_);
    done_.clear();
}

void Worker::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done d = job();
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        done_.push_back(std::move(d));
    }
}

} // namespace atlasui
