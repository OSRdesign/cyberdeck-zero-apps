/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the numbers between two samples (no LVGL; tests/test_stats.cpp): counter rates, the history ring,
 * the process table and its top-N, the chart's scale.
 */
#pragma once

#include "parsers.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sm {

/* The history of one value: the last `capacity` samples, oldest first. */
class Ring {
public:
    explicit Ring(std::size_t capacity = 120) : data_(capacity > 0 ? capacity : 1) {}
    void push(float v);
    void clear() { size_ = head_ = 0; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return data_.size(); }
    float at(std::size_t i) const;  /* 0 = oldest */
    float last() const { return size_ ? at(size_ - 1) : 0.0f; }
    float max() const;              /* 0 when empty */
    float min() const;              /* 0 when empty */
    std::vector<float> values() const;

private:
    std::vector<float> data_;
    std::size_t size_ = 0;
    std::size_t head_ = 0; /* where the next value goes */
};

/* Bytes (or anything) per second between two readings of a counter; 0 when the counter went back (a reset, an
 * interface that came again) or no time passed. */
double counter_rate(std::uint64_t before, std::uint64_t after, double seconds);

/* ---------------------------------------------------------------- processes */

struct ProcSample {
    int pid = 0;
    std::string name;
    char state = '?';
    std::uint64_t ticks = 0;     /* utime + stime */
    std::uint64_t rss_bytes = 0;
};

ProcSample proc_sample(const ProcStat &stat, long page_size);

struct ProcRow {
    int pid = 0;
    std::string name;
    char state = '?';
    double cpu_pct = 0;          /* of one core, as top shows it (a process using two cores shows 200%) */
    std::uint64_t rss_bytes = 0;
};

/* The table between two scans: cpu_pct = ticks used since the last scan / ticks one core had meanwhile x 100.
 * ticks_per_core: the /proc/stat total delta of all cores divided by the number of cores (0: no previous scan, every
 * process 0%). A process not in `before` (just started) shows 0%. */
std::vector<ProcRow> proc_rows(const std::map<int, std::uint64_t> &before, const std::vector<ProcSample> &now,
                               double ticks_per_core);

enum class SortBy { Cpu, Memory };

/* The first n by CPU (then memory, then pid) or by memory (then CPU, then pid). */
std::vector<ProcRow> top_n(std::vector<ProcRow> rows, SortBy by, std::size_t n);

/* ---------------------------------------------------------------- chart scale */

/* The smallest 1, 2 or 5 x 10^k that is >= v and >= floor ("nice" top of a rate chart). */
double nice_ceiling(double v, double floor);

} // namespace sm
