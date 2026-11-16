/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the latest-item mailbox between two threads (a triple buffer). The producer fills back() and publish()es
 * it; the consumer take()s the newest item into front() and reads it there until its next take(). An item published
 * while the previous one was still waiting replaces it: the older one is DROPPED (counted), so a slow consumer always
 * gets the newest item and the producer never waits for it. The lock is held only for a pointer swap: neither side
 * ever blocks on the other's work. The three items are reused (no allocation per item once their buffers have grown).
 *
 *   producer thread:  T &b = box.back(); ...fill b...; box.publish();
 *   consumer thread:  if (box.take()) use(box.front());      or box.wait_take(100) to sleep until one arrives
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace vhs {

template <typename T> class LatestMailbox {
public:
    LatestMailbox() : back_(new T()), mid_(new T()), front_(new T()) {}
    LatestMailbox(const LatestMailbox &) = delete;
    LatestMailbox &operator=(const LatestMailbox &) = delete;

    /* Producer side: the item being filled (never seen by the consumer until publish()). */
    T &back() { return *back_; }

    /* Producer side: back() becomes the waiting item. A waiting item not taken yet is dropped (and counted). */
    void publish()
    {
        bool dropped;
        {
            std::lock_guard<std::mutex> lock(m_);
            std::swap(back_, mid_);
            dropped = fresh_;
            fresh_ = true;
        }
        published_.fetch_add(1, std::memory_order_relaxed);
        if (dropped) drops_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
    }

    /* Consumer side: true when a new item moved into front(). */
    bool take()
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!fresh_) return false;
        std::swap(front_, mid_);
        fresh_ = false;
        return true;
    }

    /* Consumer side: take(), waiting up to timeout_ms for an item. */
    bool wait_take(int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(m_);
        if (!fresh_) cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return fresh_ || woken_; });
        woken_ = false;
        if (!fresh_) return false;
        std::swap(front_, mid_);
        fresh_ = false;
        return true;
    }

    /* Wakes a consumer sleeping in wait_take() (it returns false when nothing waits). Any thread. */
    void wake()
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            woken_ = true;
        }
        cv_.notify_all();
    }

    /* Consumer side: the item of the last successful take(). */
    T &front() { return *front_; }
    const T &front() const { return *front_; }

    /* A waiting item is thrown away (not counted as a drop): a mode change, a pause. Any thread. */
    void clear()
    {
        std::lock_guard<std::mutex> lock(m_);
        fresh_ = false;
    }

    std::uint64_t published() const { return published_.load(std::memory_order_relaxed); }
    std::uint64_t drops() const { return drops_.load(std::memory_order_relaxed); }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::unique_ptr<T> back_, mid_, front_;
    bool fresh_ = false;
    bool woken_ = false;
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> drops_{0};
};

} // namespace vhs
