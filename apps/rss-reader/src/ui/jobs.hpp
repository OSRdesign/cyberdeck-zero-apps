/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the background work of the pages (a refresh, finding a feed, loading the articles of a feed). Each job
 * runs on its own thread with a cancel flag; what it has to tell the UI it post()s as a closure, and the UI thread runs
 * the closures in order from its tick (run_posted). Nothing here touches LVGL: unit tested on the PC.
 *
 * The rule of the guidelines ("no blocking UI; every background job has a cancel and a timeout"): the network calls
 * have the curl client's timeouts (15 s per request, fetch.hpp), the cancel flag is checked by the client every
 * 100 ms, and the destructor cancels and joins every job.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rssui {

class Jobs {
public:
    struct Token {
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
    };
    using Ptr = std::shared_ptr<Token>;

    Jobs() = default;
    ~Jobs();
    Jobs(const Jobs &) = delete;
    Jobs &operator=(const Jobs &) = delete;

    /* Runs work on a new thread. The token is the job's cancel flag (and says when it ended). */
    Ptr start(std::function<void(Token &)> work);
    /* Any thread: fn runs on the UI thread at the next run_posted(), after the ones posted before it. */
    void post(std::function<void()> fn);
    /* UI thread: runs what was posted (also what those closures post), joins the threads that ended. Returns the
     * number of closures run. */
    std::size_t run_posted();
    /* Asks every job to stop / waits for every thread (the closures they posted are dropped by clear_posted()). */
    void cancel_all();
    void join_all();
    void clear_posted();
    std::size_t running() const;

private:
    struct Entry {
        Ptr token;
        std::thread thread;
    };
    void reap();

    mutable std::mutex mu_;
    std::vector<std::function<void()>> posted_;
    std::vector<Entry> jobs_;     /* UI thread only */
};

} // namespace rssui
