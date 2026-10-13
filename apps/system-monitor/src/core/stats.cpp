/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: rates, the history ring, the process table (see stats.hpp).
 */
#include "stats.hpp"

#include <algorithm>
#include <cmath>

namespace sm {

void Ring::push(float v)
{
    data_[head_] = v;
    head_ = (head_ + 1) % data_.size();
    if (size_ < data_.size()) ++size_;
}

float Ring::at(std::size_t i) const
{
    if (i >= size_) return 0.0f;
    const std::size_t first = (head_ + data_.size() - size_) % data_.size();
    return data_[(first + i) % data_.size()];
}

float Ring::max() const
{
    float m = 0.0f;
    for (std::size_t i = 0; i < size_; ++i) m = i == 0 ? at(i) : std::max(m, at(i));
    return m;
}

float Ring::min() const
{
    float m = 0.0f;
    for (std::size_t i = 0; i < size_; ++i) m = i == 0 ? at(i) : std::min(m, at(i));
    return m;
}

std::vector<float> Ring::values() const
{
    std::vector<float> out;
    out.reserve(size_);
    for (std::size_t i = 0; i < size_; ++i) out.push_back(at(i));
    return out;
}

double counter_rate(std::uint64_t before, std::uint64_t after, double seconds)
{
    if (!(seconds > 0) || after < before) return 0.0;
    return static_cast<double>(after - before) / seconds;
}

ProcSample proc_sample(const ProcStat &stat, long page_size)
{
    ProcSample s;
    s.pid = stat.pid;
    s.name = stat.comm;
    s.state = stat.state;
    s.ticks = stat.utime + stat.stime;
    s.rss_bytes = static_cast<std::uint64_t>(stat.rss_pages) * static_cast<std::uint64_t>(page_size > 0 ? page_size : 4096);
    return s;
}

std::vector<ProcRow> proc_rows(const std::map<int, std::uint64_t> &before, const std::vector<ProcSample> &now,
                               double ticks_per_core)
{
    std::vector<ProcRow> rows;
    rows.reserve(now.size());
    for (const ProcSample &s : now) {
        ProcRow r;
        r.pid = s.pid;
        r.name = s.name;
        r.state = s.state;
        r.rss_bytes = s.rss_bytes;
        const auto it = before.find(s.pid);
        if (it != before.end() && ticks_per_core > 0 && s.ticks >= it->second)
            r.cpu_pct = static_cast<double>(s.ticks - it->second) / ticks_per_core * 100.0;
        rows.push_back(r);
    }
    return rows;
}

std::vector<ProcRow> top_n(std::vector<ProcRow> rows, SortBy by, std::size_t n)
{
    const auto cmp = [by](const ProcRow &a, const ProcRow &b) {
        if (by == SortBy::Cpu) {
            if (a.cpu_pct != b.cpu_pct) return a.cpu_pct > b.cpu_pct;
            if (a.rss_bytes != b.rss_bytes) return a.rss_bytes > b.rss_bytes;
        } else {
            if (a.rss_bytes != b.rss_bytes) return a.rss_bytes > b.rss_bytes;
            if (a.cpu_pct != b.cpu_pct) return a.cpu_pct > b.cpu_pct;
        }
        return a.pid < b.pid;
    };
    if (rows.size() > n) {
        std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(n), rows.end(), cmp);
        rows.resize(n);
    } else {
        std::sort(rows.begin(), rows.end(), cmp);
    }
    return rows;
}

double nice_ceiling(double v, double floor)
{
    double target = std::max(v, floor);
    if (!(target > 0)) return 1.0;
    double base = std::pow(10.0, std::floor(std::log10(target)));
    for (int guard = 0; guard < 4; ++guard) {
        for (const double m : {1.0, 2.0, 5.0}) {
            if (m * base >= target * (1.0 - 1e-9)) return m * base;
        }
        base *= 10.0;
    }
    return target;
}

} // namespace sm
