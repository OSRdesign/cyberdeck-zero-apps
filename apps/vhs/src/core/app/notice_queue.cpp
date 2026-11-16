/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the toast line (see notice_queue.hpp).
 */
#include "notice_queue.hpp"

namespace vhs {

void NoticeQueue::push(const Notice &n)
{
    if (n.text.empty() || shown_.text == n.text) return;
    for (const Notice &q : queue_)
        if (q.text == n.text) return;
    queue_.push_back(n);
}

void NoticeQueue::set_standing(const Notice &n)
{
    if (n == standing_) return;
    standing_ = n;
    standing_hidden_ = false;
}

void NoticeQueue::dismiss(std::int64_t now_ms)
{
    if (!shown_.text.empty()) {
        shown_ = Notice();
        since_ = now_ms;
        return;
    }
    if (!standing_.text.empty()) standing_hidden_ = true;
}

Notice NoticeQueue::current(std::int64_t now_ms)
{
    if (!shown_.text.empty() && now_ms - since_ >= show_ms_) shown_ = Notice();
    if (shown_.text.empty() && !queue_.empty()) {
        shown_ = queue_.front();
        queue_.pop_front();
        since_ = now_ms;
    }
    if (!shown_.text.empty()) return shown_;
    if (!standing_.text.empty() && !standing_hidden_) return standing_;
    return Notice();
}

} // namespace vhs
