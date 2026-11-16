/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the presenter (see presenter.hpp).
 */
#include "presenter.hpp"

namespace vhs {

bool Presenter::full() const
{
    std::lock_guard<std::mutex> lock(m_);
    return q_.size() >= cap_;
}

std::size_t Presenter::size() const
{
    std::lock_guard<std::mutex> lock(m_);
    return q_.size();
}

void Presenter::push(TimedPicture p)
{
    std::lock_guard<std::mutex> lock(m_);
    q_.push_back(std::move(p));
}

std::shared_ptr<Picture> Presenter::pick(std::int64_t clock_ms, std::int64_t *pts_ms)
{
    std::lock_guard<std::mutex> lock(m_);
    std::shared_ptr<Picture> out;
    std::int64_t pts = -1;
    /* too late to show while a newer one is due: never shown */
    while (q_.size() >= 2 && q_[1].pts_ms <= clock_ms && q_.front().pts_ms < clock_ms - late_ms_) {
        q_.pop_front();
        ++dropped_;
    }
    if (!q_.empty() && q_.front().pts_ms <= clock_ms) { /* the oldest due: a little late is still shown */
        out = q_.front().pic;
        pts = q_.front().pts_ms;
        q_.pop_front();
    }
    if (pts_ms) *pts_ms = pts;
    return out;
}

std::int64_t Presenter::next_pts() const
{
    std::lock_guard<std::mutex> lock(m_);
    return q_.empty() ? -1 : q_.front().pts_ms;
}

void Presenter::clear()
{
    std::lock_guard<std::mutex> lock(m_);
    q_.clear();
}

std::uint64_t Presenter::dropped() const
{
    std::lock_guard<std::mutex> lock(m_);
    return dropped_;
}

} // namespace vhs
