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
    CHECK(cpu_rows(nullptr)[0].type == RowType::Busy);
    CHECK(process_rows(nullptr, SortBy::Cpu)[1].type == RowType::Busy);

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
    CHECK_EQ(cpu[1].label, std::string("Core 0"));
    CHECK_EQ(cpu[2].values[0], std::string("20%"));
    bool top = false;
    for (const Row &r : cpu) top |= r.label == "chromium" && r.values[0] == "31.3%";
    CHECK(top);
    CHECK_EQ(cpu.back().id, kRowAllProcs);
    const std::vector<Row> mem = memory_rows(&s);
    CHECK_EQ(mem[0].values[0], std::string("196 MB of 417 MB (47%)"));
    CHECK_EQ(mem[0].values.back(), std::string("196 MB"));
    const std::vector<Row> th = thermal_rows(&s);
    CHECK_EQ(th[0].label, std::string("CPU temperature")); /* no power source: no sentence */
    CHECK_EQ(th[1].values[0], std::string("Hot: the Pi slows down"));
    CHECK_EQ(th[2].values[0], std::string("81.9 \xC2\xB0" "C in 2 min"));
    const std::vector<Row> pr = process_rows(&s, SortBy::Memory);
    CHECK_EQ(pr[0].values[0], std::string("Memory"));
    CHECK_EQ(pr[1].label, std::string("chromium"));
    CHECK_EQ(pr[1].values[0], std::string("31.3%  82 MB"));
    CHECK_EQ(pr[1].values[1], std::string("82 MB"));
    CHECK_EQ(process_rows(&s, SortBy::Cpu)[1].values[1], std::string("31.3%"));
    const std::vector<Row> ab = about_rows(&s, {{"Screen", {"640x480 standard", "640x480"}}});
    CHECK_EQ(ab.back().label, std::string("Screen"));

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

/* The package: X-MinSize equal to the app's minimum, Exec and Icon in root/, app.json complete and a draft. */
static void test_package()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_file(app + "/root/usr/share/APPLaunch/applications/system-monitor.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nName=Monitor\n") != std::string::npos); /* one line under the home tile's icon */
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    const std::string min = "\nX-MinSize=" + std::to_string(kMinW) + "x" + std::to_string(kMinH);
    CHECK(desktop.find(min) != std::string::npos);
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/system-monitor\n") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/system-monitor.png") != std::string::npos);
    CHECK(desktop.find("\nTerminal=false") != std::string::npos);
    CHECK(!read_file(app + "/root/usr/share/APPLaunch/share/images/system-monitor.png").empty());
    CHECK(read_file(app + "/root/usr/share/APPLaunch/share/images/system-monitor.png") == read_file(app + "/icon.png"));
    const std::string copyright = read_file(app + "/root/usr/share/doc/system-monitor/copyright");
    CHECK(copyright.find("License: MIT") != std::string::npos);
    const std::string manifest = read_file(app + "/app.json");
    CHECK(manifest.find("\"share_code\": \"system-monitor\"") != std::string::npos);
    CHECK(manifest.find("\"package\": \"system-monitor\"") != std::string::npos);
    CHECK(manifest.find("\"version\": \"0.1.0\"") != std::string::npos);
    CHECK(manifest.find("\"title\": \"System Monitor\"") != std::string::npos);
    CHECK(manifest.find("\"draft\": true") != std::string::npos);
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
