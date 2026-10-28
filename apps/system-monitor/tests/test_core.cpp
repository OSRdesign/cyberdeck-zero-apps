/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the System Monitor's core/ (no LVGL, no screen) and of its package files (.desktop, app.json).
 * Built and run by ../../_common/build_app.sh host (make -C _common/headless): this file has main(), the navigation and
 * the page texts; the other .cpp files of tests/ the groups of check.hpp.
 */
#include "check.hpp"

#include "grid.hpp"
#include "monitor_core.hpp"
#include "text.hpp"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

int g_pass = 0, g_fail = 0;

using namespace sm;

static std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string fixture_dir(const char *board) { return std::string(CDK_APP_DIR) + "/tests/fixtures/" + board; }

std::string fixture(const char *board, const char *path) { return read_file(fixture_dir(board) + "/" + path); }

std::string test_tmpdir(const char *tag)
{
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/sm-test-" + tag + "-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string("/tmp");
}

static void test_nav()
{
    Nav nav;
    CHECK(nav.current() == View::Overview);
    nav.open(View::Cpu, 2);
    nav.open(View::Processes, 9);
    CHECK_EQ(nav.depth(), 3);
    int row = -1;
    CHECK(nav.back(&row) == Back::Popped && nav.current() == View::Cpu);
    CHECK_EQ(row, 9);
    CHECK(nav.back(&row) == Back::Popped && nav.current() == View::Overview);
    CHECK_EQ(row, 2);
    CHECK(nav.back(&row) == Back::Quit); /* a short Esc at the overview ends the app */
    CHECK_EQ(nav.depth(), 1);
    CHECK(tile_target(kTilePower) == View::Thermal);
    CHECK(tile_target(kTileCpu) == View::Cpu);
    CHECK(tile_target(kTileTemp) == View::Thermal);
    CHECK(tile_target(kTileMem) == View::Memory);
    CHECK(tile_target(kTileStorage) == View::Storage);
    CHECK(tile_target(kTileNet) == View::Network);
    CHECK(footer_target(kFootProcs) == View::Processes);
    CHECK(footer_target(kFootSystem) == View::About);
    CHECK(footer_target(kFootPeriod) == View::Period);
    CHECK(view_wants_procs(View::Processes) && view_wants_procs(View::Cpu) && view_wants_procs(View::Memory));
    CHECK(!view_wants_procs(View::Overview) && !view_wants_procs(View::Network));
    CHECK(view_wants_link(View::Network) && !view_wants_link(View::Overview));
    CHECK_EQ(std::string(view_title(View::Thermal)), std::string("Temp & power"));
}

/* The overview's grid at the five sizes, with the tokens the layout service gives there (area under the header,
 * margins SPACE_M, gaps SPACE_S, footer ROW, the smallest tile the fonts need), and the focus moves. */
static void test_grid()
{
    // 640x480: 2 x 3, nothing across
    GridLayout g = grid_layout(608, 408, 8, 79, kFootCount, kTileCount, 90);
    CHECK_EQ(g.cols, 2);
    CHECK_EQ(g.rows, 3);
    CHECK(!g.wide);
    CHECK_EQ(g.tiles.size(), static_cast<std::size_t>(6));
    CHECK_EQ(g.tiles[0].w, 300);
    CHECK_EQ(g.tiles[0].h, 101);
    CHECK_EQ(g.tiles[1].x, 308);
    CHECK_EQ(g.tiles[1].x + g.tiles[1].w, 608);
    CHECK_EQ(g.tiles[5].y + g.tiles[5].h <= g.footer[0].y - 8, true);
    CHECK_EQ(g.footer.size(), static_cast<std::size_t>(3));
    CHECK_EQ(g.footer[0].y, 408 - 79);
    CHECK_EQ(g.footer[2].x + g.footer[2].w, 608);
    // 800x480, 1024x600, 1280x720: 3 x 2
    g = grid_layout(772, 408, 8, 64, kFootCount, kTileCount, 90);
    CHECK(g.cols == 3 && g.rows == 2 && !g.wide);
    g = grid_layout(996, 528, 8, 79, kFootCount, kTileCount, 94);
    CHECK(g.cols == 3 && g.rows == 2 && !g.wide);
    g = grid_layout(1248, 640, 10, 81, kFootCount, kTileCount, 100);
    CHECK(g.cols == 3 && g.rows == 2 && !g.wide);
    // 720x720: Power and Network across, 2 x 2 between them
    const GridLayout sq = grid_layout(688, 640, 8, 70, kFootCount, kTileCount, 96);
    CHECK(sq.cols == 2 && sq.rows == 4 && sq.wide);
    CHECK_EQ(sq.tiles[0].w, 688);
    CHECK_EQ(sq.tiles[5].w, 688);
    CHECK_EQ(sq.tiles[1].y, sq.tiles[2].y);
    CHECK_EQ(sq.tiles[5].y, sq.tiles[3].y + sq.tiles[3].h + 8);
    // too little room for any: the tallest cells (3 columns), still every tile placed
    g = grid_layout(608, 300, 8, 79, kFootCount, kTileCount, 400);
    CHECK(g.cols == 3 && g.tiles.size() == 6);
    // no footer
    g = grid_layout(608, 408, 8, 79, 0, kTileCount, 90);
    CHECK(g.footer.empty() && g.tiles[5].y + g.tiles[5].h <= 408);

    // portrait (PDA mode): 480x800, a column 448 px wide under the 60 mm cap (511 px at 8.5 px/mm): six cards across
    g = grid_layout(448, 728, 8, 60, kFootCount, kTileCount, 90, 511);
    CHECK(g.cols == 1 && g.rows == 6 && !g.wide);
    CHECK_EQ(g.tiles.size(), static_cast<std::size_t>(6));
    for (const Rect &t : g.tiles) CHECK(t.x == 0 && t.w == 448 && t.h == 103);
    CHECK_EQ(g.tiles[1].y, 103 + 8);
    CHECK(g.tiles[5].y + g.tiles[5].h <= g.footer[0].y - 8);
    CHECK(g.footer.size() == 3 && g.footer[0].y == 728 - 60 && g.footer[2].x + g.footer[2].w == 448);
    {
        std::vector<Rect> col = g.tiles;
        col.insert(col.end(), g.footer.begin(), g.footer.end());
        CHECK_EQ(next_focus(col, 0, Dir::Down), 1);
        CHECK_EQ(next_focus(col, 4, Dir::Down), 5);
        CHECK_EQ(next_focus(col, 3, Dir::Up), 2);
        CHECK_EQ(next_focus(col, 2, Dir::Left), -1); /* one card per row: Left / Right keep the ring */
        CHECK_EQ(next_focus(col, 2, Dir::Right), -1);
        const int foot = next_focus(col, 5, Dir::Down); /* Network down to the footer */
        CHECK(foot >= 6 && foot <= 8);
        CHECK_EQ(next_focus(col, 6, Dir::Up), 5);
        CHECK_EQ(next_focus(col, 6, Dir::Right), 7);
    }
    // 480x640: six cards would be 76 px, under the 92 px a card needs: the two-column rule (Power and Network across)
    g = grid_layout(448, 568, 8, 63, kFootCount, kTileCount, 92, 541);
    CHECK(g.cols == 2 && g.rows == 4 && g.wide);
    CHECK(g.tiles[0].w == 448 && g.tiles[5].w == 448 && g.tiles[1].w < 448);
    // 800x1280: a column 768 px wide is over the cap (514 px at 8.6 px/mm): two columns, Power and Network across
    g = grid_layout(768, 1208, 8, 60, kFootCount, kTileCount, 90, 514);
    CHECK(g.cols == 2 && g.rows == 4 && g.wide);
    // no cap (0): a portrait area takes the landscape rule; a landscape area ignores the cap
    g = grid_layout(448, 728, 8, 60, kFootCount, kTileCount, 90, 0);
    CHECK(g.cols == 2);
    g = grid_layout(608, 408, 8, 79, kFootCount, kTileCount, 90, 10000);
    CHECK(g.cols == 2 && g.rows == 3 && !g.wide && g.tiles[0].w == 300 && g.tiles[0].h == 101);

    // focus on the 640x480 grid: tiles 0-5 (2 x 3), footer 6-8
    g = grid_layout(608, 408, 8, 79, kFootCount, kTileCount, 90);
    std::vector<Rect> r = g.tiles;
    r.insert(r.end(), g.footer.begin(), g.footer.end());
    CHECK_EQ(next_focus(r, 0, Dir::Right), 1);
    CHECK_EQ(next_focus(r, 1, Dir::Left), 0);
    CHECK_EQ(next_focus(r, 0, Dir::Down), 2);
    CHECK_EQ(next_focus(r, 2, Dir::Down), 4);
    CHECK_EQ(next_focus(r, 3, Dir::Up), 1);
    CHECK_EQ(next_focus(r, 4, Dir::Down), 6);  /* Storage -> Processes (below it) */
    CHECK_EQ(next_focus(r, 5, Dir::Down), 8);  /* Network -> Sampling (below it) */
    CHECK_EQ(next_focus(r, 6, Dir::Right), 7);
    CHECK_EQ(next_focus(r, 8, Dir::Up), 5);
    CHECK_EQ(next_focus(r, 6, Dir::Up), 4);
    CHECK_EQ(next_focus(r, 1, Dir::Right), -1); /* the edge: the focus stays */
    CHECK_EQ(next_focus(r, 0, Dir::Up), -1);
    CHECK_EQ(next_focus(r, 7, Dir::Down), -1);
    CHECK_EQ(next_focus(r, 99, Dir::Down), 0);
    // on the square grid: from Power (across) down to the first of the row under it, and back up
    r = sq.tiles;
    r.insert(r.end(), sq.footer.begin(), sq.footer.end());
    CHECK_EQ(next_focus(r, 0, Dir::Down), 1);
    CHECK_EQ(next_focus(r, 2, Dir::Up), 0);
    CHECK_EQ(next_focus(r, 3, Dir::Down), 5);
    CHECK_EQ(next_focus(r, 4, Dir::Down), 5);
    CHECK_EQ(next_focus(r, 1, Dir::Right), 2);
    // on the 3 x 2 grid: Power, CPU, Temperature over Memory, Storage, Network
    const GridLayout wide = grid_layout(772, 408, 8, 64, kFootCount, kTileCount, 90);
    r = wide.tiles;
    r.insert(r.end(), wide.footer.begin(), wide.footer.end());
    CHECK_EQ(next_focus(r, 0, Dir::Down), 3);
    CHECK_EQ(next_focus(r, 2, Dir::Right), -1);
    CHECK_EQ(next_focus(r, 4, Dir::Down), 7);
}

static void test_pages()
{
    // before the first sample: every tile is there and waits
    const std::vector<TileText> ov = overview_tiles(nullptr);
    CHECK_EQ(ov.size(), static_cast<std::size_t>(kTileCount));
    CHECK_EQ(ov[kTilePower].title, std::string("Power"));
    CHECK_EQ(ov[kTileNet].title, std::string("Network"));
    CHECK_EQ(ov[kTileCpu].value[0], std::string("..."));
    CHECK(ov[kTileCpu].value_dim);
    CHECK(ov[kTileCpu].spark == Spark::Cpu && ov[kTileStorage].spark == Spark::Bar);
    CHECK(ov[kTilePower].frame);
    const std::vector<std::vector<std::string>> foot = overview_footer(nullptr, 2);
    CHECK_EQ(foot.size(), static_cast<std::size_t>(kFootCount));
    CHECK_EQ(foot[kFootPeriod][0], std::string("Every 2 s"));
    CHECK_EQ(foot[kFootProcs][0], std::string("Processes"));
    // no sample yet: no row, the list says what it waits for
    CHECK(cpu_rows(nullptr).empty() && memory_rows(nullptr).empty() && thermal_rows(nullptr).empty());
    CHECK(storage_rows(nullptr).empty() && network_rows(nullptr).empty());
    CHECK(process_rows(nullptr, SortBy::Cpu).empty());
    CHECK_EQ(std::string(view_empty_text(View::Cpu)), std::string("Reading..."));
    CHECK_EQ(std::string(view_empty_text(View::Processes)), std::string("Reading processes..."));
    CHECK(view_read_only(View::Thermal) && view_read_only(View::Storage) && view_read_only(View::Network));
    CHECK(view_read_only(View::About));
    CHECK(!view_read_only(View::Cpu) && !view_read_only(View::Memory) && !view_read_only(View::Processes));

    // the period page
    const std::vector<Row> per = period_rows(5);
    CHECK_EQ(per.size(), static_cast<std::size_t>(3));
    CHECK(per[2].on && !per[0].on);
    CHECK_EQ(per[0].label, std::string("Every second"));
    CHECK_EQ(per[2].id, 5);

    // a snapshot made by hand
    Snapshot s;
    s.period_s = 1;
    s.cpu.ok = true;
    s.cpu.total = 12.4;
    s.cpu.cores = {10, 20, 5, 15};
    s.cpu.cur_mhz = 1000;
    s.cpu.max_mhz = 1000;
    s.temp.ok = true;
    s.temp.c = 82.4;
    s.mem.ok = true;
    s.mem.total = 427168;
    s.mem.available = 226760;
    s.mem.has_available = true;
    s.procs_valid = true;
    ProcRow p;
    p.pid = 1203;
    p.name = "chromium";
    p.state = 'R';
    p.cpu_pct = 31.26;
    p.rss_bytes = 86016000;
    s.top_cpu = {p};
    s.top_mem = {p};
    for (int i = 0; i < 120; ++i) s.hist.temp.push(70.0f + i * 0.1f);
    const std::vector<TileText> ov2 = overview_tiles(&s);
    CHECK(ov2[kTileTemp].value_tone == Tone::Error); /* hot */
    CHECK_EQ(ov2[kTileTemp].value[1], fmt_temp_short(82.4));
    CHECK(ov2[kTileNet].value_tone == Tone::Warn); /* no interface */
    CHECK_EQ(ov2[kTileNet].value[0], std::string("No network"));
    CHECK(ov2[kTileStorage].spark == Spark::None);
    s.proc_count = 143;
    CHECK_EQ(overview_footer(&s, 1)[kFootProcs][0], std::string("Processes 143"));
    s.self_ok = true;
    s.self_cpu = 0.84;
    s.self_rss = 14 * 1048576ull;
    CHECK_EQ(self_cost_values(s)[0], std::string("0.8% CPU, 14 MB"));
    const std::vector<Row> cpu = cpu_rows(&s);
    CHECK_EQ(cpu[0].values[0], std::string("12%"));
    CHECK(cpu[0].type == RowType::Item && cpu[0].tone == Tone::Normal);
    CHECK_EQ(cpu[1].label, std::string("Core 0"));
    CHECK_EQ(cpu[2].values[0], std::string("20%"));
    bool top = false;
    for (const Row &r : cpu) top |= r.label == "chromium" && r.values[0] == "31.3%";
    CHECK(top);
    CHECK_EQ(cpu.back().id, kRowAllProcs);
    CHECK_EQ(cpu.back().label, std::string("All processes"));
    CHECK(cpu.back().aff == Aff::Open);                    /* opens the Processes page: a chevron */
    for (const Row &r : cpu) CHECK(r.id == kRowAllProcs || r.aff == Aff::Info); /* every other row only shows */
    std::size_t heading = 0;
    while (heading < cpu.size() && cpu[heading].type != RowType::Section) ++heading;
    CHECK(heading < cpu.size() && cpu[heading].label == "Top processes by CPU" && cpu[heading].values.empty());
    CHECK(heading + 1 < cpu.size() && cpu[heading + 1].label == "chromium");
    // a busy CPU: amber from 80 %, red from 95 % (the total and each core)
    s.cpu.total = 85;
    s.cpu.cores = {99, 20, 80, 79.9};
    const std::vector<Row> busy_cpu = cpu_rows(&s);
    CHECK(busy_cpu[0].tone == Tone::Warn);
    CHECK(busy_cpu[1].tone == Tone::Error && busy_cpu[2].tone == Tone::Normal);
    CHECK(busy_cpu[3].tone == Tone::Warn && busy_cpu[4].tone == Tone::Normal);
    s.cpu.total = 12.4;
    s.cpu.cores = {10, 20, 5, 15};
    const std::vector<Row> mem = memory_rows(&s);
    CHECK_EQ(mem[0].values[0], std::string("196 MB of 417 MB (47%)"));
    CHECK_EQ(mem[0].values.back(), std::string("196 MB"));
    CHECK(mem[0].tone == Tone::Normal);
    bool mem_heading = false;
    for (const Row &r : mem) mem_heading |= r.type == RowType::Section && r.label == "Top processes by memory";
    CHECK(mem_heading);
    s.mem.available = 30000; /* 93 % used: amber */
    CHECK(memory_rows(&s)[0].tone == Tone::Warn);
    s.mem.available = 10000; /* 98 %: red */
    CHECK(memory_rows(&s)[0].tone == Tone::Error);
    s.mem.available = 226760;
    const std::vector<Row> th = thermal_rows(&s);
    CHECK_EQ(th[0].label, std::string("CPU temperature")); /* no power source: no Power row */
    CHECK(th[0].tone == Tone::Error);                      /* 82.4 °C: hot, red */
    CHECK_EQ(th[1].values[0], std::string("Hot: the Pi slows down"));
    CHECK(th[1].tone == Tone::Error);
    CHECK_EQ(th[2].values[0], std::string("81.9 \xC2\xB0" "C in 2 min"));
    CHECK(th[2].tone == Tone::Error);
    s.temp.c = 72;
    CHECK(thermal_rows(&s)[0].tone == Tone::Warn);
    s.temp.c = 52;
    CHECK(thermal_rows(&s)[0].tone == Tone::Normal && thermal_rows(&s)[1].tone == Tone::Normal);
    s.temp.c = 82.4;
    const std::vector<Row> pr = process_rows(&s, SortBy::Memory);
    CHECK_EQ(pr[0].values[0], std::string("Memory"));
    CHECK_EQ(pr[0].id, kRowSort);
    CHECK(pr[0].aff == Aff::Edit);                         /* Sort by changes its value in place */
    for (std::size_t i = 1; i < pr.size(); ++i) CHECK(pr[i].aff == Aff::Info); /* a process row only shows */
    for (const Row &r : thermal_rows(&s)) CHECK(r.aff == Aff::Info);
    for (const Row &r : storage_rows(&s)) CHECK(r.aff == Aff::Info);
    for (const Row &r : network_rows(&s)) CHECK(r.aff == Aff::Info);
    CHECK_EQ(pr[1].label, std::string("chromium"));
    CHECK_EQ(pr[1].values[0], std::string("31.3%  82 MB"));
    CHECK_EQ(pr[1].values[1], std::string("82 MB"));
    CHECK_EQ(pr[1].id, 0);
    CHECK_EQ(process_rows(&s, SortBy::Cpu)[1].values[1], std::string("31.3%"));
    CHECK_EQ(process_caption(p), std::string("pid 1203, running, 82 MB, 31.3% CPU"));
    CHECK_EQ(process_summary(s), std::string("Top 1 of 143 processes, %CPU of one core"));
    const std::vector<Row> ab = about_rows(&s, {{"Screen", {"640x480 standard", "640x480"}}});
    CHECK_EQ(ab.back().label, std::string("Screen"));
    for (const Row &r : ab) CHECK(r.type == RowType::Item);

    // the charts' texts
    CHECK_EQ(cpu_chart(s).title[0], std::string("CPU 12%"));
    CHECK_EQ(cpu_chart(s).range, std::string("0-100%, last 2 min"));
    int lo = 0, hi = 0;
    temp_range(s.hist.temp, lo, hi);
    const ChartText tc = thermal_chart(s, lo, hi);
    CHECK_EQ(tc.title[0], std::string("CPU 82.4 \xC2\xB0" "C"));
    CHECK(tc.tone == Tone::Error);
    CHECK_EQ(tc.range, std::string("20-90 \xC2\xB0" "C, last 2 min"));
    s.period_s = 5;
    CHECK_EQ(memory_chart(s).range, std::string("0-100%, last 10 min"));
    CHECK_EQ(memory_chart(s).title[1], std::string("Swap off"));
    CHECK_EQ(storage_chart(s, 20480).range, std::string("0-20 kB/s, last 10 min"));
}

/* What the list shows: the value variant that leaves the label whole, sections without a value, the cache of a live
 * refresh (rows that did not change are not measured again). Widths: 15 px per label character, 13 per value
 * character (about the deck's compact list: 28 px bold labels, 26 px values), a 528 px room, an 8 px gap. */
static void test_list_rows()
{
    int measured = 0;
    const TextWidth width = [&measured](const std::string &t, bool value) {
        ++measured;
        return static_cast<int>(t.size()) * (value ? 13 : 15);
    };
    const int room = 528, gap = 8;
    // "Power" (75) + gap + 41 chars (533): too wide; "Under-voltage now" (221): fits
    CHECK_EQ(fit_value("Power", {"Under-voltage now: check the power supply", "Under-voltage now", "UV now"}, room, gap,
                       width),
             std::string("Under-voltage now"));
    // nothing fits beside a long label: the shortest; no variant: no value
    CHECK_EQ(fit_value(std::string(40, 'x'), {"a long value", "short"}, room, gap, width), std::string("short"));
    CHECK_EQ(fit_value("Label", {}, room, gap, width), std::string());
    // the whole first variant when there is room (a wide screen)
    CHECK_EQ(fit_value("Power", {"Under-voltage now: check the power supply", "UV now"}, 1000, gap, width),
             std::string("Under-voltage now: check the power supply"));
    // exactly the room: fits (label + gap + value <= room)
    CHECK_EQ(fit_value("ab", {std::string(38, 'v'), "v"}, 30 + 8 + 38 * 13, gap, width), std::string(38, 'v'));
    CHECK_EQ(fit_value("ab", {std::string(38, 'v'), "v"}, 30 + 8 + 38 * 13 - 1, gap, width), std::string("v"));

    // rows -> items
    Row sec;
    sec.type = RowType::Section;
    sec.label = "Top processes by CPU";
    Row item;
    item.label = "Used";
    item.values = {"196 MB of 417 MB (47%)", "196 / 417 MB", "196 MB"};
    item.tone = Tone::Warn;
    item.id = 7;
    Row choice;
    choice.type = RowType::Choice;
    choice.label = "Every second";
    choice.values = {"ignored"};
    const std::vector<Row> rows{sec, item, choice};
    measured = 0;
    const std::vector<ListItem> items = list_items(rows, room, gap, width);
    CHECK_EQ(items.size(), static_cast<std::size_t>(3));
    CHECK(items[0].section && items[0].label == "Top processes by CPU" && items[0].value.empty());
    CHECK(!items[1].section && items[1].value == "196 MB of 417 MB (47%)");
    CHECK(items[1].tone == Tone::Warn && items[1].id == 7);
    CHECK(!items[2].section && items[2].value.empty());
    CHECK(measured >= 2); /* the label and one value at least */
    // a live refresh: unchanged rows keep their items unmeasured; a changed value is measured again
    measured = 0;
    CHECK(list_items(rows, rows, items, room, gap, width) == items);
    CHECK_EQ(measured, 0);
    std::vector<Row> next = rows;
    next[1].values = {"300 MB of 417 MB (72%)", "300 / 417 MB", "300 MB"};
    const std::vector<ListItem> after = list_items(next, rows, items, room, gap, width);
    CHECK_EQ(after[1].value, std::string("300 MB of 417 MB (72%)"));
    CHECK(after[0] == items[0] && after[2] == items[2]);
    CHECK(measured >= 2);
    // fewer rows than before, more rows than before: the extra ones are new items
    CHECK_EQ(list_items({sec}, rows, items, room, gap, width).size(), static_cast<std::size_t>(1));
    std::vector<Row> more = rows;
    more.push_back(item);
    const std::vector<ListItem> more_items = list_items(more, rows, items, room, gap, width);
    CHECK(more_items.size() == 4 && more_items[3] == items[1]);
    // the use tones
    CHECK(use_tone(79.9, 80, 95) == Tone::Normal && use_tone(80, 80, 95) == Tone::Warn);
    CHECK(use_tone(95, 80, 95) == Tone::Error && use_tone(0, 85, 95) == Tone::Normal);
}

/* The package: X-MinSize equal to the app's minimum, Exec and Icon in root/, app.json complete (a draft or published: the flag is not checked). */
static void test_package()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_file(app + "/root/usr/share/APPLaunch/applications/system-monitor.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nName=Monitor\n") != std::string::npos); /* one line under the home tile's icon */
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    const std::string min = "\nX-MinSize=" + std::to_string(kMinW) + "x" + std::to_string(kMinH);
    CHECK(desktop.find(min) != std::string::npos);
    /* PDA mode: shown upright on a portrait launcher (APP-CONTRACT section 6), the same minimum as the AppSpec's */
    const std::string portrait = "\nX-Portrait=" + std::to_string(kPortraitW) + "x" + std::to_string(kPortraitH) + "\n";
    CHECK(desktop.find(portrait) != std::string::npos);
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/system-monitor\n") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/system-monitor.png") != std::string::npos);
    CHECK(desktop.find("\nTerminal=false") != std::string::npos);
    CHECK(desktop.find("\nX-Background=freeze\n") != std::string::npos); /* multitasking: stopped while hidden */
    CHECK(!read_file(app + "/root/usr/share/APPLaunch/share/images/system-monitor.png").empty());
    CHECK(read_file(app + "/root/usr/share/APPLaunch/share/images/system-monitor.png") == read_file(app + "/icon.png"));
    const std::string copyright = read_file(app + "/root/usr/share/doc/system-monitor/copyright");
    CHECK(copyright.find("License: MIT") != std::string::npos);
    const std::string manifest = read_file(app + "/app.json");
    CHECK(manifest.find("\"share_code\": \"system-monitor\"") != std::string::npos);
    CHECK(manifest.find("\"package\": \"system-monitor\"") != std::string::npos);
    CHECK(manifest.find("\"version\": \"0.3.0\"") != std::string::npos);
    CHECK(manifest.find("\"title\": \"System Monitor\"") != std::string::npos);
    CHECK(manifest.find("\"license\": \"MIT\"") != std::string::npos);
    CHECK(manifest.find("\"depends\": \"libfreetype6, libinput10, libxkbcommon0\"") != std::string::npos);
    CHECK(manifest.find("libc6") == std::string::npos);
    CHECK(std::string(kAppId) == "system-monitor");
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    test_nav();
    test_grid();
    test_pages();
    test_list_rows();
    test_parsers();
    test_format();
    test_stats();
    test_power();
    test_settings();
    test_command();
    test_sampler();
    test_package();
    std::printf("test-system-monitor: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
