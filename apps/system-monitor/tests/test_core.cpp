/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the System Monitor's core/ (no LVGL, no screen) and of its package files (.desktop, app.json).
 * Built and run by ../../_common/build_app.sh host (make -C _common/headless): this file has main(), the navigation and
 * the page texts; the other .cpp files of tests/ the groups of check.hpp.
 */
#include "check.hpp"

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
    CHECK(overview_target(kOvCpu) == View::Cpu);
    CHECK(overview_target(kOvFreq) == View::Cpu);
    CHECK(overview_target(kOvLoad) == View::Cpu);
    CHECK(overview_target(kOvTemp) == View::Thermal);
    CHECK(overview_target(kOvPower) == View::Thermal);
    CHECK(overview_target(kOvMem) == View::Memory);
    CHECK(overview_target(kOvSwap) == View::Memory);
    CHECK(overview_target(kOvStorage) == View::Storage);
    CHECK(overview_target(kOvNet) == View::Network);
    CHECK(overview_target(kOvProcs) == View::Processes);
    CHECK(overview_target(kOvSystem) == View::About);
    CHECK(overview_target(kOvUptime) == View::About);
    CHECK(overview_target(kOvPeriod) == View::Period);
    CHECK(view_wants_procs(View::Processes) && view_wants_procs(View::Cpu) && view_wants_procs(View::Memory));
    CHECK(!view_wants_procs(View::Overview) && !view_wants_procs(View::Network));
    CHECK(view_wants_link(View::Network) && !view_wants_link(View::Overview));
    CHECK_EQ(std::string(view_title(View::Thermal)), std::string("Temp & power"));
}

static void test_pages()
{
    // before the first sample: every value waits, every row is there
    const std::vector<Row> ov = overview_rows(nullptr, 2);
    CHECK(ov.size() >= 12);
    CHECK_EQ(ov[0].label, std::string("CPU"));
    CHECK_EQ(ov[0].values[0], std::string("..."));
    CHECK(ov[0].type == RowType::Nav);
    CHECK_EQ(ov.back().label, std::string("Sampling"));
    CHECK_EQ(ov.back().values[0], std::string("Every 2 s"));
    CHECK(ov.back().id == kOvPeriod);
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
    const std::vector<Row> ov2 = overview_rows(&s, 1);
    CHECK(ov2[1].tone == Tone::Error); /* hot */
    CHECK_EQ(ov2[1].values[1], std::string("82 \xC2\xB0" "C"));
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
    CHECK(desktop.find("\nName=System Monitor\n") != std::string::npos);
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
