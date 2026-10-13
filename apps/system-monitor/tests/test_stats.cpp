/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor tests: the history ring, rates, the process table, the chart scales, the power wording, the
 * settings file and the command runner.
 */
#include "check.hpp"

#include "command.hpp"
#include "monitor_core.hpp"
#include "parsers.hpp"
#include "power.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "text.hpp"

#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

using namespace sm;

static std::vector<ProcSample> table(const char *file)
{
    std::vector<ProcSample> out;
    for (const std::string &line : split_lines(fixture("pi-zero-2w", file))) {
        ProcStat st;
        if (parse_proc_stat(line, st)) out.push_back(proc_sample(st, 4096));
    }
    return out;
}

static void test_ring()
{
    Ring r(4);
    CHECK_EQ(r.size(), static_cast<std::size_t>(0));
    CHECK_EQ(r.max(), 0.0f);
    r.push(1);
    r.push(5);
    r.push(3);
    CHECK_EQ(r.size(), static_cast<std::size_t>(3));
    CHECK_EQ(r.at(0), 1.0f);
    CHECK_EQ(r.last(), 3.0f);
    r.push(4);
    r.push(2); /* the oldest (1) goes */
    CHECK_EQ(r.size(), static_cast<std::size_t>(4));
    CHECK_EQ(r.at(0), 5.0f);
    CHECK_EQ(r.at(3), 2.0f);
    CHECK_EQ(r.max(), 5.0f);
    CHECK_EQ(r.min(), 2.0f);
    CHECK_EQ(r.values().size(), static_cast<std::size_t>(4));
    CHECK_EQ(r.at(9), 0.0f);
    r.clear();
    CHECK_EQ(r.size(), static_cast<std::size_t>(0));
    Ring big(120);
    for (int i = 0; i < 300; ++i) big.push(static_cast<float>(i));
    CHECK_EQ(big.size(), static_cast<std::size_t>(120));
    CHECK_EQ(big.at(0), 180.0f);
    CHECK_EQ(big.last(), 299.0f);
}

static void test_rates()
{
    CHECK_NEAR(counter_rate(1000, 3000, 2.0), 1000.0, 1e-9);
    CHECK_EQ(counter_rate(3000, 1000, 1.0), 0.0); /* reset */
    CHECK_EQ(counter_rate(1000, 3000, 0.0), 0.0);
    CHECK_EQ(nice_ceiling(0.3, 1), 1.0);
    CHECK_EQ(nice_ceiling(14.6, 10), 20.0);
    CHECK_EQ(nice_ceiling(20, 10), 20.0);
    CHECK_EQ(nice_ceiling(21, 10), 50.0);
    CHECK_EQ(nice_ceiling(600, 10), 1000.0);
    CHECK_EQ(rate_ceiling(0), 10.0 * 1024);
    CHECK_EQ(rate_ceiling(15000), 20.0 * 1024);
    CHECK_EQ(rate_ceiling(3.0 * 1048576), 5.0 * 1048576);
    CHECK_EQ(fmt_rate(rate_ceiling(15000)), std::string("20 kB/s"));
    int lo = 0, hi = 0;
    Ring t(10);
    temp_range(t, lo, hi);
    CHECK(lo == 20 && hi == 90);
    t.push(45);
    t.push(83);
    temp_range(t, lo, hi);
    CHECK(lo == 20 && hi == 90);
    t.push(89.5f);
    t.push(19.0f);
    temp_range(t, lo, hi);
    CHECK(lo == 10 && hi == 100);
}

static void test_process_table()
{
    const std::vector<ProcSample> a = table("proc-table-0s.txt");
    const std::vector<ProcSample> b = table("proc-table-1s.txt");
    CHECK_EQ(a.size(), static_cast<std::size_t>(6));
    CHECK_EQ(b.size(), static_cast<std::size_t>(7));
    CHECK_EQ(b[3].name, std::string("chromium (gpu)"));
    CHECK_EQ(b[3].rss_bytes, static_cast<std::uint64_t>(21500) * 4096);
    std::map<int, std::uint64_t> before;
    for (const ProcSample &p : a) before[p.pid] = p.ticks;
    // the /proc/stat delta of the same second: 400 ticks over 4 cores = 100 per core
    const StatInfo s0 = parse_stat(fixture("pi-zero-2w", "proc/stat"));
    const StatInfo s1 = parse_stat(fixture("pi-zero-2w", "proc/stat-1s"));
    const double per_core = static_cast<double>(s1.all.total() - s0.all.total()) / 4.0;
    CHECK_NEAR(per_core, 100.0, 1e-9);
    const std::vector<ProcRow> rows = proc_rows(before, b, per_core);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(7));
    const std::vector<ProcRow> cpu = top_n(rows, SortBy::Cpu, 15);
    CHECK_EQ(cpu[0].name, std::string("chromium (gpu)"));
    CHECK_NEAR(cpu[0].cpu_pct, 40.0, 1e-9);
    CHECK_EQ(cpu[1].name, std::string("applaunch"));
    CHECK_NEAR(cpu[1].cpu_pct, 10.0, 1e-9);
    CHECK_EQ(cpu[2].name, std::string("system-monitor"));
    CHECK_NEAR(cpu[2].cpu_pct, 2.0, 1e-9);
    CHECK_EQ(cpu[3].name, std::string("NetworkManager"));
    CHECK_EQ(cpu[4].name, std::string("python3")); /* 0 %: by memory then */
    CHECK_EQ(cpu[6].name, std::string("sh"));      /* new since the last scan: 0 % */
    CHECK_EQ(cpu[6].cpu_pct, 0.0);
    const std::vector<ProcRow> mem = top_n(rows, SortBy::Memory, 3);
    CHECK_EQ(mem.size(), static_cast<std::size_t>(3));
    CHECK_EQ(mem[0].name, std::string("chromium (gpu)"));
    CHECK_EQ(mem[1].name, std::string("applaunch"));
    CHECK_EQ(mem[2].name, std::string("system-monitor"));
    // no previous scan: every process 0 %
    const std::vector<ProcRow> first = proc_rows({}, b, 0.0);
    CHECK_EQ(top_n(first, SortBy::Cpu, 1)[0].name, std::string("chromium (gpu)")); /* by memory */
    CHECK_EQ(top_n(first, SortBy::Cpu, 1)[0].cpu_pct, 0.0);
    CHECK_EQ(process_caption(cpu[0]), std::string("pid 1203, running, 84 MB, 40.0% CPU"));
    // the 16 kB pages of a Pi 5 kernel
    ProcStat st;
    CHECK(parse_proc_stat("7 (x) S 1 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 1 1 100", st));
    CHECK_EQ(proc_sample(st, 16384).rss_bytes, static_cast<std::uint64_t>(1638400));
}

void test_stats()
{
    test_ring();
    test_rates();
    test_process_table();
}

void test_power()
{
    CHECK(temp_state(69.9) == TempState::Ok);
    CHECK(temp_state(70.0) == TempState::Warm);
    CHECK(temp_state(79.9) == TempState::Warm);
    CHECK(temp_state(80.0) == TempState::Hot);
    CHECK(temp_tone(TempState::Hot) == Tone::Error);
    CHECK(temp_tone(TempState::Warm) == Tone::Warn);

    // 0x50005: under-voltage now, throttled now, both since boot
    ThrottleTracker t;
    t.update(0x50005, 2471.0);
    PowerInfo p;
    p.source = PowerSource::Firmware;
    t.fill(p);
    CHECK(p.flags[kUnderVoltage].now && p.flags[kUnderVoltage].occurred);
    CHECK(p.flags[kThrottled].now);
    CHECK(!p.flags[kFreqCapped].now && !p.flags[kFreqCapped].occurred);
    CHECK_EQ(power_status(p, false).text, std::string("Under-voltage now: check the power supply"));
    CHECK(power_status(p, false).tone == Tone::Error);
    Tone tone = Tone::Normal;
    std::vector<std::string> v = power_values(p, tone);
    CHECK_EQ(v[0], std::string("Under-voltage now"));
    CHECK(tone == Tone::Error);
    CHECK_EQ(flag_values(p.flags[kUnderVoltage], false)[0], std::string("now"));

    // the supply recovers: the sticky bit stays, the time of the last event is the last sample that showed it
    t.update(0x50000, 2480.0);
    t.fill(p);
    CHECK(!p.flags[kUnderVoltage].now && p.flags[kUnderVoltage].occurred);
    CHECK_EQ(power_status(p, false).text, std::string("Under-voltage happened since boot"));
    CHECK(power_status(p, false).tone == Tone::Warn);
    v = power_values(p, tone);
    CHECK_EQ(v[0], std::string("Under-voltage since boot"));
    CHECK_EQ(v.back(), std::string("UV earlier"));
    CHECK_EQ(flag_values(p.flags[kUnderVoltage], false)[0], std::string("last 0:41:11 after boot"));
    CHECK_EQ(flag_values(p.flags[kSoftTempLimit], false)[0], std::string("never since boot"));

    // the app started after the event: occurred, never seen
    ThrottleTracker late;
    late.update(0x50000, 100.0);
    late.fill(p);
    CHECK_EQ(flag_values(p.flags[kUnderVoltage], false)[0], std::string("before the app started"));
    // an event between two samples: the sticky bit appears
    late.update(0x70000, 105.0);
    late.fill(p);
    CHECK_EQ(flag_values(p.flags[kFreqCapped], false)[2], std::string("0:01:45"));

    // all well
    ThrottleTracker ok;
    ok.update(0, 50);
    ok.fill(p);
    CHECK_EQ(power_status(p, false).text, std::string("Power OK"));
    CHECK_EQ(power_values(p, tone)[0], std::string("OK"));
    CHECK(tone == Tone::Normal);
    // soft limit and throttling on a hot Pi 5
    ThrottleTracker hot;
    hot.update(0xC000C, 900);
    hot.fill(p);
    CHECK_EQ(power_status(p, true).text, std::string("Throttled now: the CPU is too hot"));
    // the hwmon alarm only
    ThrottleTracker alarm;
    alarm.update_alarm(true, 60);
    PowerInfo a;
    a.source = PowerSource::Alarm;
    alarm.fill(a);
    CHECK_EQ(power_status(a, false).text, std::string("Under-voltage now: check the power supply"));
    alarm.update_alarm(false, 70);
    alarm.fill(a);
    CHECK_EQ(power_status(a, false).text, std::string("Under-voltage happened since boot"));
    CHECK_EQ(flag_values(a.flags[kUnderVoltage], true)[1], std::string("last at 0:01:00"));
    // no access, no source
    PowerInfo denied;
    denied.source = PowerSource::NoAccess;
    CHECK(power_status(denied, false).text.find("video group") != std::string::npos);
    PowerInfo none;
    CHECK(power_status(none, false).text.empty());

    // the overview's status line: power first, then a hot CPU, then a warm one
    CHECK_EQ(overview_status(none, true, 52).text, std::string(""));
    CHECK(overview_status(none, true, 82).tone == Tone::Error);
    CHECK_EQ(overview_status(none, true, 82).text, std::string("CPU hot: 82 \xC2\xB0" "C, the Pi slows down from 80 to 85 \xC2\xB0" "C"));
    CHECK(overview_status(none, true, 72).tone == Tone::Warn);
    ThrottleTracker uv;
    uv.update(0x50005, 10);
    PowerInfo u;
    u.source = PowerSource::Firmware;
    uv.fill(u);
    CHECK_EQ(overview_status(u, true, 82).text, std::string("Under-voltage now: check the power supply"));
    ok.fill(u);
    CHECK_EQ(overview_status(u, true, 52).text, std::string("Power OK"));
}

void test_settings()
{
    CHECK_EQ(Settings::parse("v=1\nperiod=5\n").period_s, 5);
    CHECK_EQ(Settings::parse("period=3\n").period_s, 1); /* not offered: the default */
    CHECK_EQ(Settings::parse("# c\nperiod = 2\r\nnoise\n").period_s, 2);
    CHECK_EQ(Settings::parse("").period_s, 1);
    Settings s;
    s.period_s = 2;
    CHECK_EQ(Settings::parse(s.serialize()).period_s, 2);
    const std::string dir = test_tmpdir("settings");
    const std::string path = dir + "/a/b/system-monitor.conf";
    CHECK(save_settings(path, s));
    CHECK_EQ(load_settings(path).period_s, 2);
    CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
    CHECK_EQ(load_settings(dir + "/missing.conf").period_s, 1);
    setenv("SYSTEM_MONITOR_CONFIG", path.c_str(), 1);
    CHECK_EQ(settings_path(), path);
    unsetenv("SYSTEM_MONITOR_CONFIG");
    const char *home = std::getenv("HOME");
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/deck", 1);
    CHECK_EQ(settings_path(), std::string("/home/deck/.config/cyberdeckzero/system-monitor.conf"));
    setenv("XDG_CONFIG_HOME", "/x", 1);
    CHECK_EQ(settings_path(), std::string("/x/cyberdeckzero/system-monitor.conf"));
    if (home) setenv("HOME", home, 1);
    if (xdg) setenv("XDG_CONFIG_HOME", xdg, 1);
    else unsetenv("XDG_CONFIG_HOME");
    std::system(("rm -rf '" + dir + "'").c_str());
}

void test_command()
{
    CmdResult r = run_command({"sh", "-c", "echo hi; echo err >&2"}, 2000);
    CHECK(r.ok());
    CHECK_EQ(r.output, std::string("hi\nerr\n"));
    r = run_command({"sh", "-c", "exit 3"}, 2000);
    CHECK(r.started && r.exit_code == 3 && !r.ok());
    r = run_command({"sh", "-c", "echo $LC_ALL"}, 2000);
    CHECK_EQ(r.output, std::string("C\n"));
    r = run_command({"system-monitor-no-such-command"}, 2000);
    CHECK(!r.started && r.not_found());
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    r = run_command({"sh", "-c", "sleep 5; echo late"}, 300);
    clock_gettime(CLOCK_MONOTONIC, &b);
    const double took = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(r.timed_out && !r.ok());
    CHECK(took < 1.5);
    CHECK(r.output.empty());
    // output capped
    r = run_command({"sh", "-c", "yes | head -c 100000"}, 3000, 1000);
    CHECK_EQ(r.output.size(), static_cast<std::size_t>(1000));
}
