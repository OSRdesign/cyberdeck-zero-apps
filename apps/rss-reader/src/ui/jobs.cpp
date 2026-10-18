/*
 * SPDX-License-Identifier: MIT
 */
#include "jobs.hpp"

namespace rssui {

Jobs::~Jobs()
{
    cancel_all();
    join_all();
    clear_posted();
}

Jobs::Ptr Jobs::start(std::function<void(Token &)> work)
{
    reap();
    Ptr token = std::make_shared<Token>();
    Entry e;
    e.token = token;
    e.thread = std::thread([token, work = std::move(work)]() {
        work(*token);
        token->finished.store(true);
    });
    jobs_.push_back(std::move(e));
    return token;
}

void Jobs::post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> g(mu_);
    posted_.push_back(std::move(fn));
}

std::size_t Jobs::run_posted()
{
    std::size_t n = 0;
    for (;;) {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard<std::mutex> g(mu_);
            now.swap(posted_);
        }
        if (now.empty()) break;
        for (auto &fn : now) {
            fn();
            ++n;
        }
    }
    reap();
    return n;
}

void Jobs::reap()
{
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->token->finished.load()) {
            if (it->thread.joinable()) it->thread.join();
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

void Jobs::cancel_all()
{
    for (Entry &e : jobs_) e.token->cancel.store(true);
}

void Jobs::join_all()
{
    for (Entry &e : jobs_)
        if (e.thread.joinable()) e.thread.join();
    jobs_.clear();
}

void Jobs::clear_posted()
{
    std::lock_guard<std::mutex> g(mu_);
    posted_.clear();
}

std::size_t Jobs::running() const
{
    std::size_t n = 0;
    for (const Entry &e : jobs_)
        if (!e.token->finished.load()) ++n;
    return n;
}

} // namespace rssui
