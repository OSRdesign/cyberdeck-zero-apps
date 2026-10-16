/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the pages without LVGL (tests/test_core.cpp): navigation and every row and chart text, made from a
 * Snapshot. A row carries its value as a list of variants, longest first: list_items() keeps the first one that leaves
 * the whole label visible beside it (so nothing is cut at 640x480), the shortest one otherwise. A page's rows are
 * items (label, value, the value's tone) and section headings: what cp0::ui::CompactList draws (ui/monitor_app).
 *
 *   Overview     six tiles (grid.hpp): Power, CPU, Temperature, Memory, Storage, Network, each a title, a big value,
 *                a small line and a history line or bar; a footer: Processes, System, Sampling. Each opens its page
 *   CPU          total, each core, frequency, governor, load, tasks, uptime, the section "Top processes by CPU" with
 *                the top 5, All processes
 *   Memory       used, available, cache, buffers, free, shared, swap, "Top processes by memory" with the top 5, All
 *                processes
 *   Thermal      "Temp & power" (temperature and power): the power state, temperature, state, highest, fan, each
 *                power flag with the time since boot of its last event, supply / core voltage, the raw flags
 *   Storage      each real filesystem (used / size, %), the root disk's read and write rates and totals
 *   Network      per interface (a section): state, address, Wi-Fi name / signal / bit rate, in and out rates and
 *                totals; Bluetooth
 *   Processes    Sort by (CPU / Memory), then the top 15 (name, %CPU of one core, memory); the status line gives
 *                the highlighted one's pid and state
 *   About        model, kernel, OS, hostname, serial, firmware, CPU, RAM, screen, launcher, app version
 *   Period       every 1, 2 or 5 seconds
 *
 * Value tones: amber (Warn) and red (Error) for a warm / hot CPU, a high use (CPU from 80 / 95 %, memory, swap and
 * filesystems from 85 / 95 %) and the power flags (now: red for under-voltage and throttling, amber for the others;
 * since boot: amber); light green (Good) for a power state or a flag that is fine.
 */
#pragma once

#include "power.hpp"
#include "sampler.hpp"
#include "stats.hpp"

#include <functional>
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

/* Item: label and value (a list row); Section: a heading over the items under it (grey, upper case on the screen,
 * never selected); Choice: the Sampling page's rows (a check mark on the current one). */
enum class RowType { Item, Section, Choice };

/* What activating a row does (the launcher's row affordance rule): Info: a read-out, nothing happens (grey value, no
 * marker); Open: opens another page (chevron); Edit: changes the value in place (value, then chevron). */
enum class Aff { Info, Open, Edit };

struct Row {
    RowType type = RowType::Item;
    std::string label;
    std::vector<std::string> values; /* longest first; empty: no value (the label takes the row) */
    Tone tone = Tone::Normal;        /* the value's colour */
    int id = -1;                     /* what the row is: an action (kRowAllProcs, kRowSort), a process, a period */
    bool on = false;                 /* Choice: the current one */
    Aff aff = Aff::Info;             /* what a tap does (Item rows) */

    bool operator==(const Row &o) const
    {
        return type == o.type && label == o.label && values == o.values && tone == o.tone && id == o.id && on == o.on &&
               aff == o.aff;
    }
    bool operator!=(const Row &o) const { return !(*this == o); }
};

/* action ids on the detail pages */
constexpr int kRowAllProcs = 100;
constexpr int kRowSort = 101;

/* A row as the list shows it: one value, the variant that fits. */
struct ListItem {
    bool section = false;
    std::string label;
    std::string value;
    Tone tone = Tone::Normal;
    int id = -1;
    Aff aff = Aff::Info;

    bool operator==(const ListItem &o) const
    {
        return section == o.section && label == o.label && value == o.value && tone == o.tone && id == o.id &&
               aff == o.aff;
    }
    bool operator!=(const ListItem &o) const { return !(*this == o); }
};

/* A text's width in px on the screen: an item's label (value false; the selected row's bold face) or a value. */
using TextWidth = std::function<int(const std::string &text, bool value)>;

/* The value variant to show beside label in a row whose text room (its width minus both insets) is room, with gap
 * between the label and the value: the first (longest) one with which the whole label fits, else the last (shortest).
 * "" when there is none. */
std::string fit_value(const std::string &label, const std::vector<std::string> &values, int room, int gap,
                      const TextWidth &width);
/* The page's rows as the list shows them (Choice rows: items without a value). */
std::vector<ListItem> list_items(const std::vector<Row> &rows, int room, int gap, const TextWidth &width);
/* The same, reusing the items of rows that did not change since the last call (no text measuring for them): a live
 * refresh measures only the values that moved. */
std::vector<ListItem> list_items(const std::vector<Row> &rows, const std::vector<Row> &old_rows,
                                 const std::vector<ListItem> &old_items, int room, int gap, const TextWidth &width);

/* The tone of a use in % (CPU: warn 80, error 95; memory and disks: 85, 95). */
Tone use_tone(double pct, double warn, double error);

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
/* The detail pages' rows. s null (no sample yet): none, the list shows view_empty_text(). */
std::vector<Row> cpu_rows(const Snapshot *s);
std::vector<Row> memory_rows(const Snapshot *s);
std::vector<Row> thermal_rows(const Snapshot *s);
std::vector<Row> storage_rows(const Snapshot *s);
std::vector<Row> network_rows(const Snapshot *s);
/* Sort by, then the top 15 (id: the index in the snapshot's top list); none before the first process table */
std::vector<Row> process_rows(const Snapshot *s, SortBy by);
/* extra: screen, launcher, app version (the screen side knows them) */
std::vector<Row> about_rows(const Snapshot *s,
                            const std::vector<std::pair<std::string, std::vector<std::string>>> &extra);
std::vector<Row> period_rows(int period_s);
/* The grey line of a page's list while it has no row: "Reading..." ("Reading processes..." on Processes). */
const char *view_empty_text(View view);
/* Pages whose list only shows (no highlight: Up / Down scroll): Temp & power, Storage, Network, System. */
bool view_read_only(View view);

/* the Processes page's status line for its highlighted row: "pid 1234, sleeping, 45 MB, 12.3% CPU" */
std::string process_caption(const ProcRow &p);
/* the same line while Sort by is highlighted: "Top 15 of 143 processes, %CPU of one core" */
std::string process_summary(const Snapshot &s);
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
