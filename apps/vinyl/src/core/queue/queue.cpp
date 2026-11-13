/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl queue (see queue.hpp).
 */
#include "queue.hpp"

#include <algorithm>
#include <numeric>

namespace vinyl {

const char *repeat_name(Repeat r) {
    switch (r) {
    case Repeat::All: return "all";
    case Repeat::One: return "one";
    default: return "off";
    }
}

Repeat repeat_from(const std::string &s) {
    if (s == "all") return Repeat::All;
    if (s == "one") return Repeat::One;
    return Repeat::Off;
}

void Queue::linear() {
    order_.resize(items_.size());
    std::iota(order_.begin(), order_.end(), 0);
}

void Queue::reshuffle() {
    const int cur_item = (cur_ >= 0 && cur_ < size()) ? order_[static_cast<size_t>(cur_)] : -1;
    linear();
    std::shuffle(order_.begin(), order_.end(), rng_);
    if (cur_item >= 0) {
        auto it = std::find(order_.begin(), order_.end(), cur_item);
        std::rotate(order_.begin(), it, it + 1); /* the current track first, the rest shuffled after it */
        cur_ = 0;
    }
}

void Queue::set(std::vector<std::string> paths, int start, const std::string &source) {
    items_ = std::move(paths);
    source_ = source;
    linear();
    cur_ = (start >= 0 && start < size()) ? start : (items_.empty() ? -1 : -1);
    if (shuffle_) reshuffle();
}

void Queue::clear() {
    items_.clear();
    order_.clear();
    cur_ = -1;
    source_.clear();
}

const std::string &Queue::at(int pos) const {
    static const std::string none;
    if (pos < 0 || pos >= size()) return none;
    return items_[static_cast<size_t>(order_[static_cast<size_t>(pos)])];
}

const std::string *Queue::current_path() const {
    if (cur_ < 0 || cur_ >= size()) return nullptr;
    return &at(cur_);
}

void Queue::set_current(int pos) { cur_ = (pos >= 0 && pos < size()) ? pos : -1; }

void Queue::set_shuffle(bool on) {
    if (on == shuffle_) return;
    shuffle_ = on;
    if (on) {
        reshuffle();
    } else {
        const int cur_item = (cur_ >= 0 && cur_ < size()) ? order_[static_cast<size_t>(cur_)] : -1;
        linear();
        cur_ = cur_item;
    }
}

Repeat Queue::cycle_repeat() {
    repeat_ = repeat_ == Repeat::Off ? Repeat::All : repeat_ == Repeat::All ? Repeat::One : Repeat::Off;
    return repeat_;
}

int Queue::next_after(int pos, bool user) const {
    if (items_.empty()) return -1;
    if (pos < 0 || pos >= size()) return 0;
    if (!user && repeat_ == Repeat::One) return pos;
    if (pos + 1 < size()) return pos + 1;
    if (repeat_ == Repeat::All || (user && repeat_ == Repeat::One)) return 0;
    return -1;
}

int Queue::prev_pos() const {
    if (items_.empty() || cur_ < 0) return -1;
    if (cur_ > 0) return cur_ - 1;
    if (repeat_ == Repeat::All) return size() - 1;
    return -1;
}

bool Queue::move_up(int pos) {
    if (pos <= 0 || pos >= size()) return false;
    if (shuffle_) {
        std::swap(order_[static_cast<size_t>(pos)], order_[static_cast<size_t>(pos - 1)]);
    } else {
        std::swap(items_[static_cast<size_t>(pos)], items_[static_cast<size_t>(pos - 1)]);
    }
    if (cur_ == pos) cur_ = pos - 1;
    else if (cur_ == pos - 1) cur_ = pos;
    return true;
}

bool Queue::move_down(int pos) {
    if (pos < 0 || pos + 1 >= size()) return false;
    return move_up(pos + 1);
}

void Queue::remove(int pos) {
    if (pos < 0 || pos >= size()) return;
    const int item = order_[static_cast<size_t>(pos)];
    items_.erase(items_.begin() + item);
    order_.erase(order_.begin() + pos);
    for (int &o : order_)
        if (o > item) --o;
    if (cur_ > pos) --cur_;
    else if (cur_ == pos && cur_ >= size()) cur_ = -1;
}

void Queue::clear_except_current() {
    if (cur_ < 0 || cur_ >= size()) {
        const std::string src = source_;
        clear();
        source_ = src;
        return;
    }
    std::string keep = at(cur_);
    items_.assign(1, keep);
    linear();
    cur_ = 0;
}

void Queue::restore(std::vector<std::string> items, std::vector<int> order, int current, bool shuffle, Repeat r,
                    const std::string &source) {
    items_ = std::move(items);
    source_ = source;
    shuffle_ = shuffle;
    repeat_ = r;
    std::vector<int> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    bool perm = sorted.size() == items_.size();
    for (size_t i = 0; perm && i < sorted.size(); ++i) perm = sorted[i] == static_cast<int>(i);
    if (perm) order_ = std::move(order);
    else linear();
    cur_ = (current >= 0 && current < size()) ? current : -1;
}

}  // namespace vinyl
