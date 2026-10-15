/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the tool threads (see worker.hpp).
 */
#include "worker.hpp"

#include <csignal>
#include <ctime>

#include <algorithm>
#include <chrono>
#include <thread>

namespace nt {

std::int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::int64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool CancelToken::sleep_ms(int ms) const
{
    const std::int64_t end = now_ms() + ms;
    while (!cancelled()) {
        const std::int64_t left = end - now_ms();
        if (left <= 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(left, kSliceMs)));
    }
    return false;
}

Worker::~Worker()
{
    cancel();
    for (auto &r : retired_) r->cancel->store(true);
    if (current_) {
        pthread_join(current_->thread, nullptr);
        current_.reset();
    }
    for (auto &r : retired_) pthread_join(r->thread, nullptr);
    retired_.clear();
}

void *Worker::trampoline(void *arg)
{
    Run *run = static_cast<Run *>(arg);
    run->fn(CancelToken(run->cancel));
    run->fn = nullptr; /* what the function captured goes now, on its own thread */
    run->done.store(true);
    return nullptr;
}

void Worker::reap(bool block)
{
    for (auto it = retired_.begin(); it != retired_.end();) {
        if ((*it)->done.load() || block) {
            pthread_join((*it)->thread, nullptr);
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

std::uint64_t Worker::start(Fn fn)
{
    if (current_) {
        current_->cancel->store(true);
        retired_.push_back(std::move(current_));
    }
    reap(false);
    if (retired_.size() > 3) reap(true); /* runs that do not end: bounded (each ends within a slice of its cancel) */

    auto run = std::make_unique<Run>();
    run->fn = std::move(fn);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackBytes);
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    const int rc = pthread_create(&run->thread, &attr, trampoline, run.get());
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    pthread_attr_destroy(&attr);
    ++gen_;
    if (rc != 0) return 0;
    current_ = std::move(run);
    return gen_;
}

void Worker::cancel()
{
    if (current_) current_->cancel->store(true);
}

bool Worker::running() const
{
    return current_ && !current_->done.load();
}

bool Worker::wait(int timeout_ms) const
{
    const std::int64_t end = now_ms() + timeout_ms;
    while (running()) {
        if (now_ms() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

} // namespace nt
