/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the pages without LVGL (tests/test_core.cpp): navigation and every row and chart text, made from a
 * Snapshot. A row carries its value as a list of variants, longest first: the screen shows the first one that fits
 * beside the label (so nothing is cut at 640x480), the shortest one otherwise.
 *
 *   Overview     CPU, Temperature, Frequency, Memory, Swap, Load, Uptime, Storage, Power (only when the system
 *                tells it), Network, Processes, System, Sampling: each opens its page
 *   CPU          total, each core, frequency, governor, load, tasks, the top 5 processes by CPU, All processes
 *   Memory       used, available, cache, buffers, free, shared, swap, the top 5 by memory, All processes
 *   Thermal      "Temp & power" (temperature and power): the power sentence, temperature, state, highest, fan, each power flag
 *                with the time since boot of its last event, supply / core voltage, the raw flags
 *   Storage      each real filesystem (used / size, %), the root disk's read and write rates and totals
 *   Network      per interface: state, address, Wi-Fi name / signal / bit rate, in and out rates and totals;
 *                Bluetooth
 *   Processes    Sort by (CPU / Memory), then the top 15 (name, %CPU of one core, memory); the status line gives
 *                the highlighted one's pid and state
 *   About        model, kernel, OS, hostname, serial, firmware, CPU, RAM, screen, launcher, app version
 *   Period       every 1, 2 or 5 seconds
 */
#pragma once

#include "power.hpp"
#include "sampler.hpp"
#include "stats.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sm {

/* The smallest screen: X-MinSize of the .desktop file (tests/test_core.cpp compares them). */
constexpr int kMinW = 640;
constexpr int kMinH = 480;
constexpr const char *kAppTitle = "System Monitor";
constexpr const char *kAppId = "system-monitor";
constexpr int kTopProcs = 15;   /* the Processes page */
constexpr int kTopDetail = 5;   /* the CPU and Memory pages */

/* ---------------------------------------------------------------- navigation */

enum class View { Overview, Cpu, Memory, Thermal, Storage, Network, Processes, About, Period };
enum class Back { Quit, Popped };

const char *view_title(View view); /* "CPU", "Temp & power", ... ("" for the overview): fits the header at 640x480 */

class Nav {
public:
    View current() const { return stack_.back().first; }
    int depth() const { return static_cast<int>(stack_.size()); }
    /* `from_row`: the row of the page underneath that opened it (the highlight goes back there) */
    void open(View view, int from_row) { stack_.emplace_back(view, from_row); }
    /* Esc / Left / the header's Esc: one level up (from_row: the row to highlight there); at the overview: Quit. */
    Back back(int *from_row = nullptr);

private:
    std::vector<std::pair<View, int>> stack_{{View::Overview, 0}};
};

/* Which pages read the process table, and the Wi-Fi link (the sampler does it only for them). */
bool view_wants_procs(View view);
bool view_wants_link(View view);

/* ---------------------------------------------------------------- rows */

enum class RowType { Nav, Info, Choice, Busy };

struct Row {
    RowType type = RowType::Info;
    std::string label;
    std::vector<std::string> values; /* longest first; empty: no value (an Info heading across the row) */
    Tone tone = Tone::Normal;        /* Nav values: the colour */
    int id = -1;                     /* what the row is (an overview row, an action, a period) */
    bool on = false;                 /* Choice: the current one */
};

/* overview row ids */
enum OvRow { kOvCpu, kOvTemp, kOvFreq, kOvMem, kOvSwap, kOvLoad, kOvUptime, kOvStorage, kOvPower, kOvNet, kOvProcs,
             kOvSystem, kOvPeriod };
/* action ids on the detail pages */
constexpr int kRowAllProcs = 100;
constexpr int kRowSort = 101;

View overview_target(int id);

/* s may be null (no sample yet): the values say "..." */
std::vector<Row> overview_rows(const Snapshot *s, int period_s);
std::vector<Row> cpu_rows(const Snapshot *s);
std::vector<Row> memory_rows(const Snapshot *s);
std::vector<Row> thermal_rows(const Snapshot *s);
std::vector<Row> storage_rows(const Snapshot *s);
std::vector<Row> network_rows(const Snapshot *s);
std::vector<Row> process_rows(const Snapshot *s, SortBy by);
/* extra: screen, launcher, app version (the screen side knows them) */
std::vector<Row> about_rows(const Snapshot *s,
                            const std::vector<std::pair<std::string, std::vector<std::string>>> &extra);
std::vector<Row> period_rows(int period_s);

/* the Processes page's status line for its highlighted row: "pid 1234, sleeping, 45 MB, 12.3% CPU" */
std::string process_caption(const ProcRow &p);
std::string fmt_proc_cpu(double pct); /* "12.3%" */

/* ---------------------------------------------------------------- charts */

struct ChartText {
    std::string title[2];  /* one per series ("" when the chart has one) */
    Tone tone = Tone::Normal; /* the first title's colour (the temperature's state) */
    std::string range;     /* "0-100%, last 2 min" */
};

ChartText cpu_chart(const Snapshot &s);
ChartText memory_chart(const Snapshot &s);
ChartText thermal_chart(const Snapshot &s, int lo, int hi);
ChartText storage_chart(const Snapshot &s, double top_bps);
ChartText network_chart(const Snapshot &s, double top_bps);

/* The temperature chart's range in °C: 20..90, wider when the history leaves it (to the next 10). */
void temp_range(const Ring &temps, int &lo, int &hi);
/* The top of a rate chart: a round number of kB/s (at least 10 kB/s) or MB/s at or above the highest value. */
double rate_ceiling(double max_bps);

} // namespace sm
