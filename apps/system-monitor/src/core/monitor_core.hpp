/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the pages without LVGL (tests/test_core.cpp): navigation and every row and chart text, made from a
 * Snapshot. A row carries its value as a list of variants, longest first: the screen shows the first one that fits
 * beside the label (so nothing is cut at 640x480), the shortest one otherwise.
 *
 *   Overview     six tiles (grid.hpp): Power, CPU, Temperature, Memory, Storage, Network, each a title, a big value,
 *                a small line and a history line or bar; a footer: Processes, System, Sampling. Each opens its page
 *   CPU          total, each core, frequency, governor, load, tasks, uptime, the top 5 processes by CPU, All
 *                processes
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

/* action ids on the detail pages */
constexpr int kRowAllProcs = 100;
constexpr int kRowSort = 101;

/* ---------------------------------------------------------------- the overview: tiles and footer */

/* The tiles in grid order (grid.hpp: Power first, Network last, the two that may span the width). */
enum TileId { kTilePower, kTileCpu, kTileTemp, kTileMem, kTileStorage, kTileNet, kTileCount };
/* The footer row under them. */
enum FootId { kFootProcs, kFootSystem, kFootPeriod, kFootCount };

/* What a tile draws beside its big value (when the tile has room): a history line, or a fill bar. */
enum class Spark { None, Cpu, Temp, Mem, Rx, Bar };

struct TileText {
    int id = 0;
    std::string title;               /* top left, small */
    std::string hint;                /* top right, small, dim ("wlan0 -52 dBm"); "" none */
    std::vector<std::string> value;  /* the big value, longest first */
    Tone value_tone = Tone::Normal;  /* Normal white, Good green, Warn amber, Error red */
    bool value_dim = false;          /* not known ("...", "Unknown"): grey */
    std::vector<std::string> detail; /* the small line under it, longest first */
    Tone detail_tone = Tone::Normal;
    Spark spark = Spark::None;
    double bar_pct = 0;              /* Spark::Bar */
    Tone bar_tone = Tone::Normal;
    bool frame = false;              /* Power: the tile's frame and ground in its state colour */
    Tone frame_tone = Tone::Normal;
};

View tile_target(int id);
View footer_target(int id);

/* s may be null (no sample yet): the values say "..." */
std::vector<TileText> overview_tiles(const Snapshot *s);
/* the footer items' texts, longest first: "Processes 143", "System", "Every 1 s" */
std::vector<std::vector<std::string>> overview_footer(const Snapshot *s, int period_s);
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
/* the System page's "This app" value: "1.2% CPU, 14 MB" (CPU of one core over the last 10 s, resident memory) */
std::vector<std::string> self_cost_values(const Snapshot &s);

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
