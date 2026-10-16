/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the Wi-Fi walk test. /proc/net/wireless and `iw dev wlan0 link` recorded on the Pi 3A+
 * (tests/fixtures/wifi, and the boards' folders); the reading (the proc file first, iw for the access point and as the
 * fallback for the level, iw missing, the adapter gone); the walk's state machine on scripted readings (the weak zone
 * with its 2 s hysteresis, the summary's numbers, a signal that hovers at the threshold, the ring bound, the lists'
 * bound, the access point changes); the sampler on a fake clock (the schedule, skipped ticks, cancel, no adapter, iw
 * missing); the session on its thread (stop keeps the walk, reset forgets it and drops the stopping run's last
 * snapshot); the saved text; the two 0.4 settings.
 */
#include "check.hpp"
#include "wifi_fakes.hpp"

#include "interfaces.hpp"
#include "netlog_wifi.hpp"
#include "settings.hpp"
#include "text.hpp"
#include "walktest.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace nt;

namespace {

bool has(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

template <typename F> bool eventually(F f, int ms)
{
    const std::int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

WalkReading level(int dbm)
{
    WalkReading r;
    r.linked = true;
    r.dbm = dbm;
    return r;
}

WalkReading nolink()
{
    return WalkReading();
}

WalkReading at_ap(const char *bssid, int dbm, const char *ssid = "Net")
{
    WalkReading r = level(dbm);
    r.has_link = true;
    r.bssid = bssid;
    r.ssid = ssid;
    return r;
}

/* the 1 Hz recording's data lines, each as a whole /proc/net/wireless */
std::vector<std::string> recorded_walk()
{
    std::vector<std::string> out;
    const std::string head = "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
                             " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n";
    for (const std::string &line : split_lines(fixture("wifi", "proc-wireless-3a-plus-1hz.txt")))
        if (starts_with(trim(line), "wlan0:")) out.push_back(head + line + "\n");
    return out;
}

void test_readings()
{
    int dbm = 0;
    CHECK(proc_wireless_dbm(fixture("wifi", "proc-wireless-3a-plus.txt"), "wlan0", dbm) && dbm == -63);
    CHECK(!proc_wireless_dbm(fixture("wifi", "proc-wireless-3a-plus.txt"), "wlan1", dbm));
    CHECK(!proc_wireless_dbm(fixture("deck-offline", "proc/net/wireless"), "wlan0", dbm)); /* not associated: 0 */
    CHECK(proc_wireless_dbm(fixture("pi-3a-plus", "proc/net/wireless"), "wlan0", dbm) && dbm == -67);
    const std::vector<std::string> walk = recorded_walk();
    CHECK_EQ(walk.size(), static_cast<std::size_t>(5));
    const int want[] = {-62, -64, -65, -65, -66};
    for (std::size_t i = 0; i < walk.size() && i < 5; ++i) CHECK(proc_wireless_dbm(walk[i], "wlan0", dbm) && dbm == want[i]);
    CHECK(proc_wireless_dbm(wfake::proc_wireless({{"wlan0", -50}, {"wlan1", -70}}), "wlan1", dbm) && dbm == -70);
    CHECK(!proc_wireless_dbm("garbage", "wlan0", dbm));
    CHECK(!proc_wireless_dbm(" wlan0: 0000   47.  -300.  -256  0 0 0 0 0 0\n", "wlan0", dbm));
    CHECK(!proc_wireless_dbm(" wlan0: 0000   47.  abc  -256\n", "wlan0", dbm));
    CHECK(!proc_wireless_dbm("", "wlan0", dbm));

    const IwLink link = parse_iw_link(fixture("wifi", "iw-link-3a-plus.txt"));
    CHECK(link.connected && link.bssid == "b0:4e:26:00:00:01" && link.ssid == "HomeNet" && link.signal_dbm == -65);
    CHECK(link.freq_mhz == 2452 && link.has_bitrate && link.tx_mbit > 72 && link.tx_mbit < 73);
    CHECK(!parse_iw_link(fixture("wifi", "iw-nodev-3a-plus.txt")).connected);
    CHECK(!parse_iw_link("Not connected.\n").connected);

    const std::string iw = fixture("wifi", "iw-link-3a-plus.txt");
    const std::string dir = fixture_dir("pi-3a-plus");
    CancelToken cancel;
    // the proc file's level; iw for the access point
    {
        wfake::ScriptSource src(dir, {wfake::proc_wireless({{"wlan0", -63}})});
        wfake::Runner runner;
        runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        bool missing = false;
        const WalkReading r = read_walk_signal(src, runner, "wlan0", true, cancel, &missing);
        CHECK(r.linked && r.dbm == -63 && !r.from_iw && r.has_link && r.bssid == "b0:4e:26:00:00:01" && r.ssid == "HomeNet");
        CHECK(runner.calls() == 1 && runner.spec(0).argv == iw_link_argv("wlan0") && runner.spec(0).timeout_ms == kIwTimeoutMs);
        CHECK(!missing);
    }
    // the proc file without the interface: iw's level
    {
        wfake::ScriptSource src(dir, {wfake::proc_wireless({{"wlan1", -50}})});
        wfake::Runner runner;
        runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        const WalkReading r = read_walk_signal(src, runner, "wlan0", true, cancel, nullptr);
        CHECK(r.linked && r.dbm == -65 && r.from_iw);
        runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, "Not connected.\n"); };
        const WalkReading off = read_walk_signal(src, runner, "wlan0", true, cancel, nullptr);
        CHECK(!off.linked && off.has_link && off.bssid.empty());
        runner.answer = [&](const CommandSpec &, int) { return wfake::result(237, fixture("wifi", "iw-nodev-3a-plus.txt")); };
        const WalkReading gone = read_walk_signal(src, runner, "wlan0", true, cancel, nullptr);
        CHECK(!gone.linked && !gone.has_link);
    }
    // iw not installed: said once, not asked again; ask_iw off: never asked
    {
        wfake::ScriptSource src(dir, {wfake::proc_wireless({{"wlan0", -70}})});
        wfake::Runner runner;
        runner.answer = [&](const CommandSpec &, int) { return wfake::not_installed(); };
        bool missing = false;
        const WalkReading r = read_walk_signal(src, runner, "wlan0", true, cancel, &missing);
        CHECK(missing && r.linked && r.dbm == -70 && !r.has_link);
        read_walk_signal(src, runner, "wlan0", true, cancel, &missing);
        CHECK_EQ(runner.calls(), 1);
        wfake::Runner none;
        const WalkReading q = read_walk_signal(src, none, "wlan0", false, cancel, nullptr);
        CHECK(q.linked && none.calls() == 0);
    }
}

void test_recorder()
{
    // a walk into a weak zone and out, a short rise inside a dip, a lost link
    WalkRecorder rec(-75, 1000, 2000, 600);
    const WalkReading script[] = {level(-60), level(-70), level(-76), level(-80), level(-74), level(-77), level(-70),
                                  level(-68), level(-76), nolink(),   level(-60), level(-60), level(-50)};
    const bool in_dip_after[] = {false, false, true, true, true, true, true, false, true, true, true, false, false};
    bool states_ok = true;
    for (int i = 0; i < 13; ++i) {
        rec.add(i * 1000, script[i]);
        if (rec.in_dip() != in_dip_after[i]) states_ok = false;
    }
    CHECK(states_ok);
    const WalkSummary &m = rec.summary();
    CHECK(m.samples == 13 && m.linked_samples == 12 && m.nolink_samples == 1);
    CHECK_NEAR(m.duration_s, 13.0, 1e-9);
    CHECK(m.has_signal && m.weakest_dbm == -80 && m.strongest_dbm == -50);
    CHECK_NEAR(m.weakest_at_s, 3.0, 1e-9);
    CHECK_NEAR(m.strongest_at_s, 12.0, 1e-9);
    CHECK_NEAR(m.average_dbm, -821.0 / 12.0, 1e-9);
    CHECK_EQ(m.weak_samples, 5);
    CHECK_NEAR(m.weak_s, 5.0, 1e-9);
    CHECK_NEAR(m.weak_pct, 500.0 / 13.0, 1e-9);
    CHECK_NEAR(m.nolink_s, 1.0, 1e-9);
    CHECK_EQ(m.dips, 2);
    CHECK(!m.roaming_known && m.roams == 0);
    CHECK_EQ(rec.dips().size(), static_cast<std::size_t>(2));
    if (rec.dips().size() == 2) {
        const WalkDip &a = rec.dips()[0], &b = rec.dips()[1];
        CHECK(a.start_ms == 2000 && a.end_ms == 6000 && a.has_dbm && a.lowest_dbm == -80 && !a.lost_link);
        CHECK(b.start_ms == 8000 && b.end_ms == 10000 && b.has_dbm && b.lowest_dbm == -76 && b.lost_link);
    }
    CHECK(rec.samples().size() == 13 && rec.dropped() == 0);
    CHECK(rec.samples()[9].weak && !rec.samples()[9].linked && rec.samples()[4].dbm == -74 && !rec.samples()[4].weak);

    // the threshold itself is not weak
    WalkRecorder edge(-75, 1000, 2000, 600);
    edge.add(0, level(-75));
    CHECK(edge.summary().weak_samples == 0 && edge.summary().dips == 0);

    // a signal hovering at the threshold: one dip with the hysteresis, one per crossing without
    WalkRecorder hover(-75, 1000, 2000, 600), raw(-75, 1000, 0, 600);
    for (int i = 0; i < 20; ++i) {
        hover.add(i * 1000, level(i % 2 ? -74 : -76));
        raw.add(i * 1000, level(i % 2 ? -74 : -76));
    }
    CHECK(hover.summary().dips == 1 && hover.in_dip() && hover.summary().weak_samples == 10);
    CHECK(hover.dips().size() == 1 && hover.dips()[0].end_ms == -1);
    CHECK(raw.summary().dips == 10 && !raw.in_dip() && raw.dips()[9].end_ms == 19000);

    // a half-second schedule: the 2 s are four samples back above
    WalkRecorder fast(-75, 500, 2000, 600);
    const int fast_dbm[] = {-80, -70, -70, -70, -70, -60};
    for (int i = 0; i < 6; ++i) fast.add(i * 500, level(fast_dbm[i]));
    CHECK(fast.dips().size() == 1 && fast.dips()[0].end_ms == 500 && !fast.in_dip());
    CHECK_NEAR(fast.summary().weak_s, 0.5, 1e-9);
    CHECK_NEAR(fast.summary().duration_s, 3.0, 1e-9);

    // the ring keeps the last samples; the summary covers the whole walk
    WalkRecorder ring(-75, 1000, 2000, 10);
    for (int i = 0; i < 25; ++i) ring.add(i * 1000, level(-60 - i));
    CHECK(ring.samples().size() == 10 && ring.dropped() == 15 && ring.samples().front().t_ms == 15000);
    CHECK(ring.summary().samples == 25 && ring.summary().strongest_dbm == -60 && ring.summary().weakest_dbm == -84);
    CHECK_NEAR(ring.summary().strongest_at_s, 0.0, 1e-9);

    // the first time it was that weak (or strong) is the one kept
    WalkRecorder first(-90, 1000, 2000, 600);
    first.add(0, level(-70));
    first.add(1000, level(-80));
    first.add(2000, level(-50));
    first.add(3000, level(-80));
    first.add(4000, level(-50));
    CHECK_NEAR(first.summary().weakest_at_s, 1.0, 1e-9);
    CHECK_NEAR(first.summary().strongest_at_s, 2.0, 1e-9);

    // access point changes (roaming), with iw's answers
    WalkRecorder roam(-75, 1000, 2000, 600);
    roam.add(0, at_ap("aa:00:00:00:00:01", -60));
    roam.add(1000, at_ap("aa:00:00:00:00:01", -70));
    roam.add(2000, at_ap("aa:00:00:00:00:02", -61));
    WalkReading off = nolink();
    off.has_link = true; /* iw: not connected */
    roam.add(3000, off);
    roam.add(4000, at_ap("aa:00:00:00:00:02", -62)); /* back on the same one: not a change */
    roam.add(5000, at_ap("aa:00:00:00:00:03", -63, "Net-5G"));
    WalkReading stale = level(-64);
    stale.bssid = "aa:00:00:00:00:09"; /* iw did not answer this time: ignored */
    roam.add(6000, stale);
    roam.add(7000, at_ap("aa:00:00:00:00:03", -65, "Net-5G"));
    CHECK(roam.summary().roaming_known && roam.summary().roams == 2 && roam.roams().size() == 2);
    if (roam.roams().size() == 2) {
        const WalkRoam &r1 = roam.roams()[0], &r2 = roam.roams()[1];
        CHECK(r1.t_ms == 2000 && r1.from == "aa:00:00:00:00:01" && r1.to == "aa:00:00:00:00:02" && r1.ssid == "Net");
        CHECK(r1.has_dbm && r1.dbm == -61);
        CHECK(r2.t_ms == 5000 && r2.from == "aa:00:00:00:00:02" && r2.to == "aa:00:00:00:00:03" && r2.ssid == "Net-5G");
    }
    CHECK(roam.bssid() == "aa:00:00:00:00:03" && roam.ssid() == "Net-5G");

    // the lists are bounded, the counts are not
    WalkRecorder many(-75, 1000, 0, 600);
    for (int i = 0; i < 140; ++i) many.add(i * 1000, level(i % 2 ? -60 : -80));
    CHECK(many.summary().dips == 70 && many.dips().size() == kWalkMaxEvents);
    bool closed = true;
    for (const WalkDip &d : many.dips())
        if (d.end_ms < 0) closed = false;
    CHECK(closed);
    WalkRecorder hops(-75, 1000, 2000, 600);
    for (int i = 0; i < 100; ++i) {
        char b[24];
        std::snprintf(b, sizeof(b), "aa:00:00:00:00:%02x", i % 2);
        hops.add(i * 1000, at_ap(b, -60));
    }
    CHECK(hops.summary().roams == 99 && hops.roams().size() == kWalkMaxEvents);
}

struct WalkRun {
    std::shared_ptr<wfake::Time> time = std::make_shared<wfake::Time>();
    wfake::Runner runner;
    std::unique_ptr<wfake::ScriptSource> src;
    std::vector<WalkSnapshot> published;
    CancelToken cancel;
    WalkConfig cfg;
    explicit WalkRun(std::vector<std::string> proc, const char *board = "pi-3a-plus")
        : src(new wfake::ScriptSource(fixture_dir(board), std::move(proc)))
    {
        runner.time = time;
        cfg.iface = "wlan0";
    }
    WalkSnapshot go()
    {
        return run_walk(cfg, *src, runner, wfake::clock_of(time), cancel,
                        [this](const WalkSnapshot &s) { published.push_back(s); });
    }
};

void test_sampler()
{
    const std::string iw = fixture("wifi", "iw-link-3a-plus.txt");
    // the recording replayed: a sample a second on the schedule, iw each time
    {
        WalkRun t(recorded_walk());
        t.cfg.max_samples_taken = 5;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        const WalkSnapshot end = t.go();
        CHECK_EQ(t.published.size(), static_cast<std::size_t>(7)); /* Running, five samples, Stopped */
        CHECK(t.published[0].state == WalkSnapshot::State::Running && t.published[0].samples.empty());
        CHECK(end.state == WalkSnapshot::State::Stopped && end.samples.size() == 5);
        const int want[] = {-62, -64, -65, -65, -66};
        bool ok = end.samples.size() == 5;
        for (std::size_t i = 0; ok && i < 5; ++i)
            if (end.samples[i].t_ms != static_cast<std::int64_t>(i) * 1000 || end.samples[i].dbm != want[i] ||
                !end.samples[i].linked || end.samples[i].weak)
                ok = false;
        CHECK(ok);
        CHECK(t.runner.calls() == 5 && t.runner.at(0) == 1000 && t.runner.at(4) == 5000);
        CHECK_NEAR(end.summary.average_dbm, -64.4, 1e-9);
        CHECK(end.summary.weakest_dbm == -66 && end.summary.strongest_dbm == -62 && end.summary.dips == 0);
        CHECK(end.summary.roaming_known && end.summary.roams == 0 && !end.iw_missing);
        CHECK(end.bssid == "b0:4e:26:00:00:01" && end.ssid == "HomeNet" && end.source == "/proc/net/wireless");
        CHECK(end.linked && end.dbm == -66 && end.weak_dbm == -75 && end.interval_ms == 1000 && end.hysteresis_ms == 2000);
        CHECK_NEAR(end.elapsed_s, 4.0, 1e-9);
        CHECK_EQ(t.src->proc_reads(), 5);
    }
    // a threshold of the user's: the end of the recording is a dip still open
    {
        WalkRun t(recorded_walk());
        t.cfg.max_samples_taken = 5;
        t.cfg.weak_dbm = -64;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        const WalkSnapshot end = t.go();
        CHECK(end.summary.weak_samples == 3 && end.summary.dips == 1 && end.in_dip && end.dips.size() == 1);
        CHECK(end.dips.size() == 1 && end.dips[0].start_ms == 2000 && end.dips[0].end_ms == -1 && end.dips[0].lowest_dbm == -66);
        CHECK_NEAR(end.summary.weak_pct, 60.0, 1e-9);
    }
    // a slow iw: the ticks it overran are skipped, the next ones stay on the schedule
    {
        WalkRun t({wfake::proc_wireless({{"wlan0", -60}})});
        t.cfg.max_samples_taken = 4;
        t.runner.on_call = [&](int i) {
            if (i == 1) t.time->t += 2500;
        };
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        const WalkSnapshot end = t.go();
        CHECK(end.samples.size() == 4 && end.samples[1].t_ms == 1000 && end.samples[2].t_ms == 4000 && end.samples[3].t_ms == 5000);
        CHECK(t.runner.at(2) == 5000 && t.runner.at(3) == 6000);
    }
    // not a Wi-Fi adapter, or none
    {
        WalkRun t({}, "eth-wifi");
        t.cfg.iface = "end0";
        const WalkSnapshot end = t.go();
        CHECK(end.state == WalkSnapshot::State::Failed && end.error == "No Wi-Fi adapter on end0" && t.runner.calls() == 0);
        CHECK_EQ(t.published.size(), static_cast<std::size_t>(1));
        WalkRun n({});
        n.cfg.iface = "";
        CHECK_EQ(n.go().error, std::string("No Wi-Fi adapter on this board"));
    }
    // cancel while waiting for the next tick
    {
        WalkRun t({wfake::proc_wireless({{"wlan0", -60}})});
        t.time->on_sleep = [&](std::int64_t now) {
            if (now >= 3500) t.cancel.cancel();
        };
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        const WalkSnapshot end = t.go();
        CHECK(end.state == WalkSnapshot::State::Stopped && end.samples.size() == 3 && t.runner.calls() == 3);
        CHECK(t.time->t.load() <= 3600);
    }
    // iw not installed: the level only, said in the snapshot
    {
        WalkRun t({wfake::proc_wireless({{"wlan0", -60}})});
        t.cfg.max_samples_taken = 3;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::not_installed(); };
        const WalkSnapshot end = t.go();
        CHECK(t.runner.calls() == 1 && end.iw_missing && !end.summary.roaming_known && end.samples.size() == 3);
        CHECK(end.bssid.empty() && end.source == "/proc/net/wireless");
    }
    // the proc file lacks the interface: iw's level; then not connected: no link, weak
    {
        WalkRun t({""});
        t.cfg.max_samples_taken = 3;
        t.runner.answer = [&](const CommandSpec &, int i) {
            return i == 0 ? wfake::result(0, iw) : wfake::result(0, "Not connected.\n");
        };
        const WalkSnapshot end = t.go();
        CHECK(end.samples.size() == 3 && end.samples[0].linked && end.samples[0].dbm == -65 && !end.samples[1].linked);
        CHECK(t.published.size() >= 2 && t.published[1].source == "iw");
        CHECK(end.source.empty() && !end.linked && end.summary.nolink_samples == 2 && end.summary.dips == 1);
        CHECK(end.bssid.empty()); /* iw said: not connected */
    }
    // the settings kept within bounds; a program in front of iw's arguments
    {
        WalkRun t({wfake::proc_wireless({{"wlan0", -60}})});
        t.cfg.max_samples_taken = 2;
        t.cfg.weak_dbm = -20;
        t.cfg.interval_ms = 10;
        t.cfg.hysteresis_ms = -5;
        t.cfg.iw = {"sh", "/fake/iw"};
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
        const WalkSnapshot end = t.go();
        CHECK(end.weak_dbm == kWalkWeakMaxDbm && end.interval_ms == 500 && end.hysteresis_ms == 0);
        CHECK(end.samples.size() == 2 && end.samples[1].t_ms == 500 && t.runner.at(1) - t.runner.at(0) == 500);
        const CommandSpec spec = t.runner.spec(0);
        CHECK(spec.argv.size() == 5 && spec.argv[0] == "sh" && spec.argv[1] == "/fake/iw" && spec.argv[2] == "dev" &&
              spec.argv[3] == "wlan0" && spec.argv[4] == "link");
    }
}

void test_walk_session()
{
    const std::string iw = fixture("wifi", "iw-link-3a-plus.txt");
    auto time = std::make_shared<wfake::Time>();
    time->real_us = 200;
    auto runner = std::make_shared<wfake::Runner>();
    runner->time = time;
    runner->answer = [&](const CommandSpec &, int) { return wfake::result(0, iw); };
    std::shared_ptr<const Source> src(
        new wfake::ScriptSource(fixture_dir("pi-3a-plus"), {wfake::proc_wireless({{"wlan0", -80}})}));
    {
        WalkSession session(src, runner, wfake::clock_of(time));
        CHECK(!session.latest()); /* nothing before the first start */
        WalkConfig cfg;
        cfg.iface = "wlan0";
        CHECK(session.start(cfg));
        CHECK(session.latest() && session.latest()->state != WalkSnapshot::State::Idle);
        CHECK(eventually([&] { return session.latest()->samples.size() >= 3; }, 5000));
        session.stop();
        CHECK(session.wait(2000));
        const auto stopped = session.latest();
        CHECK(stopped->state == WalkSnapshot::State::Stopped && stopped->samples.size() >= 3);
        CHECK(stopped->summary.weak_samples == stopped->summary.samples && stopped->in_dip); /* -80: all weak */
        // reset: nothing left, and it stays so
        session.reset();
        CHECK(session.latest()->state == WalkSnapshot::State::Idle && session.latest()->samples.empty());
        CHECK(session.latest()->weak_dbm == -75 && session.latest()->iface == "wlan0");
        // a new walk starts from zero
        CHECK(session.start(cfg));
        CHECK(eventually([&] { return session.latest()->samples.size() >= 2; }, 5000));
        CHECK_EQ(session.latest()->samples.front().t_ms, static_cast<std::int64_t>(0));
        // reset while it runs: the run's last snapshot (Stopped) is dropped
        session.reset();
        CHECK(session.wait(2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(session.latest()->state == WalkSnapshot::State::Idle && session.latest()->samples.empty());
        CHECK(session.start(cfg));
        CHECK(eventually([&] { return session.latest()->samples.size() >= 1; }, 5000));
    } /* the destructor stops and joins the run */
}

void test_walk_text()
{
    WalkRecorder rec(-75, 1000, 2000, 600);
    const WalkReading script[] = {level(-60), level(-70), level(-76), level(-80), level(-74), level(-77), level(-70),
                                  level(-68), level(-76), nolink(),   level(-60), level(-60), level(-50)};
    for (int i = 0; i < 13; ++i) rec.add(i * 1000, script[i]);
    WalkSnapshot s;
    s.state = WalkSnapshot::State::Stopped;
    s.iface = "wlan0";
    s.samples.assign(rec.samples().begin(), rec.samples().end());
    s.summary = rec.summary();
    s.dips = rec.dips();
    s.roams = rec.roams();
    s.iw_missing = true;
    const std::string text = format_walktest(s);
    CHECK(text.find("Interface: wlan0\nState: stopped\n") == 0);
    CHECK(has(text, "Weak zone: below -75 dBm or no link (a dip ends after 2 s back above)\n"));
    CHECK(has(text, "Duration: 13 s, 13 samples, one every 1 s\n"));
    CHECK(has(text, "Signal: average -68.4 dBm, strongest -50 dBm at 12 s, weakest -80 dBm at 3 s\n"));
    CHECK(has(text, "Weak zone: 5 s (38%) in 2 dips; no link 1 s\n"));
    CHECK(has(text, "Dips:\n  2 s to 6 s, lowest -80 dBm\n  8 s to 10 s, lowest -76 dBm, link lost\n"));
    CHECK(has(text, "Access point changes: not known (iw is not installed)\n"));
    CHECK(has(text, "Samples (s dBm, - = no link):\n  0 -60, 1 -70, 2 -76, 3 -80, 4 -74, 5 -77, 6 -70, 7 -68, 8 -76, 9 -\n"
                    "  10 -60, 11 -60, 12 -50\n"));
    // roaming known, a dip still open, a ring that dropped samples, a half-second schedule
    WalkRecorder r2(-75, 500, 2000, 4);
    r2.add(0, at_ap("aa:00:00:00:00:01", -60, "Home"));
    r2.add(500, at_ap("aa:00:00:00:00:02", -62, "Home"));
    r2.add(1000, at_ap("aa:00:00:00:00:02", -79, "Home"));
    r2.add(1500, nolink());
    r2.add(2000, nolink());
    WalkSnapshot s2;
    s2.state = WalkSnapshot::State::Running;
    s2.iface = "wlan1";
    s2.interval_ms = 500;
    s2.samples.assign(r2.samples().begin(), r2.samples().end());
    s2.dropped = r2.dropped();
    s2.summary = r2.summary();
    s2.dips = r2.dips();
    s2.roams = r2.roams();
    const std::string t2 = format_walktest(s2);
    CHECK(has(t2, "State: running\n") && has(t2, "Duration: 2.5 s, 5 samples, one every 0.5 s\n"));
    CHECK(has(t2, "Weak zone: 1.5 s (60%) in 1 dip; no link 1 s\n"));
    CHECK(has(t2, "Dips:\n  1 s to the end, lowest -79 dBm, link lost\n"));
    CHECK(has(t2, "Access point changes: 1\n  at 0.5 s: aa:00:00:00:00:01 -> aa:00:00:00:00:02 (Home), -62 dBm\n"));
    CHECK(has(t2, "Samples (s dBm, - = no link; the last 4):\n  0.5 -62, 1 -79, 1.5 -, 2 -\n"));
    WalkSnapshot empty;
    empty.error = "No Wi-Fi adapter on end0";
    empty.state = WalkSnapshot::State::Failed;
    const std::string t3 = format_walktest(empty);
    CHECK(has(t3, "State: failed\nError: No Wi-Fi adapter on end0\n") && has(t3, "Signal: no reading\n"));
    CHECK(has(t3, "Access point changes: not known\n") && !has(t3, "Samples"));
}

void test_walk_settings()
{
    CHECK(Settings().wifi_rescan_ms == kWifiIntervalMs && Settings().walk_weak_dbm == kWalkWeakDbm);
    const Settings s = Settings::parse("v=1\nwifi_rescan_ms=10000\nwalk_weak_dbm=-80\n");
    CHECK(s.wifi_rescan_ms == 10000 && s.walk_weak_dbm == -80);
    const Settings bad = Settings::parse("wifi_rescan_ms=100\nwalk_weak_dbm=-20\nwifi_rescan_ms=x\nwalk_weak_dbm=-75dBm\n");
    CHECK(bad.wifi_rescan_ms == 5000 && bad.walk_weak_dbm == -75);
    const Settings edge = Settings::parse("wifi_rescan_ms=60000\nwalk_weak_dbm=-95\n");
    CHECK(edge.wifi_rescan_ms == 60000 && edge.walk_weak_dbm == -95);
    const Settings back = Settings::parse(s.serialize());
    CHECK(back.wifi_rescan_ms == 10000 && back.walk_weak_dbm == -80);
    CHECK(has(Settings().serialize(), "wifi_rescan_ms=5000\nwalk_weak_dbm=-75\n"));
    const Settings old = Settings::parse("v=1\nping_target=8.8.8.8\nble_seconds=30\n"); /* a 0.3 file */
    CHECK(old.wifi_rescan_ms == 5000 && old.walk_weak_dbm == -75 && old.ble_seconds == 30);
}

} // namespace

void test_walktest()
{
    test_readings();
    test_recorder();
    test_sampler();
    test_walk_session();
    test_walk_text();
    test_walk_settings();
}
