/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): the speed test. curl's report lines recorded on the deck (tests/fixtures/speedtest); the
 * Server-Timing reader; the command lines (the byte cap never exceeded); the throughput meter (Mbit/s, the ramp left
 * out, a short phase, the history bound); the bufferbloat grades; the error words; the whole test on a scripted curl
 * runner and a scripted ping on a fake clock (the phases in order, the live figures, the cap, the time target, an upload
 * the server does not confirm, every failure point, cancel, no ping); the real ping module beside a scripted load (a
 * fake echo socket answering slower under load); the session on fake curl programs run by the real stream runner (the
 * zero bytes counted by the "server", Stop ends curl's process group).
 */
#include "check.hpp"
#include "stream_fakes.hpp"

#include "speedtest.hpp"
#include "text.hpp"

#include <signal.h>
#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>

using namespace nt;

namespace {

std::vector<std::string> fixture_lines(char kind)
{
    std::vector<std::string> out;
    for (const std::string &l : split_lines(fixture("speedtest", "curl-deck.txt")))
        if (starts_with(l, std::string("NTSPEED|") + kind + "|")) out.push_back(l);
    return out;
}

const std::int64_t kSec = 1000000;

/* the latency phase: the four L lines recorded on the deck */
fake::Script latency_script()
{
    fake::Script s;
    fake::Step st;
    st.at_us = 1 * kSec;
    st.lines = fixture_lines('L');
    s.steps.push_back(st);
    return s;
}

/* a direction at a steady rate, a step every 100 ms from t0; first_second_factor slows the first second (the ramp) */
fake::Script direction_script(bool upload, double bytes_per_s, double seconds, double first_second_factor = 1.0,
                              std::uint64_t cap = 0, const std::vector<std::string> &end_lines = {})
{
    fake::Script s;
    const std::int64_t t0 = 2 * kSec;
    double bytes = 0;
    for (int i = 1; i <= static_cast<int>(seconds * 10); ++i) {
        const double rate = i <= 10 ? bytes_per_s * first_second_factor : bytes_per_s;
        bytes += rate / 10.0;
        fake::Step st;
        st.at_us = t0 + i * 100000;
        std::uint64_t b = static_cast<std::uint64_t>(bytes);
        if (cap && b >= cap) {
            b = cap;
            st.lines = end_lines; /* curl reports at the end of its last request */
        }
        (upload ? st.in : st.out) = b;
        s.steps.push_back(st);
        if (cap && b >= cap) break;
    }
    return s;
}

std::string upload_line(std::uint64_t server_bytes)
{
    return "NTSPEED|U|200|0|1|0.029372|0.155534|0.156995|0.990846|8.2|0|" + std::to_string(server_bytes + 396) +
           "||CDG|" + std::to_string(server_bytes) + "|cfSpeedEdge;dur=5, cfSpeedWorker;dur=810";
}

struct Run {
    std::shared_ptr<fake::StreamRunner> runner = std::make_shared<fake::StreamRunner>();
    std::shared_ptr<fake::PingPlan> ping = std::make_shared<fake::PingPlan>();
    std::vector<SpeedSnapshot> seen;
    SpeedSnapshot last;
    void go(const SpeedConfig &cfg, const CancelToken &cancel = CancelToken())
    {
        last = run_speedtest(cfg, *runner, fake::scripted_ping(ping), cancel,
                             [this](const SpeedSnapshot &s) { seen.push_back(s); });
    }
};

void test_speed_parse()
{
    const std::vector<std::string> L = fixture_lines('L'), D = fixture_lines('D'), U = fixture_lines('U');
    CHECK(L.size() == 5 && D.size() == 3 && U.size() == 1);
    SpeedTransfer t;
    CHECK(parse_speed_transfer(L[0], t));
    CHECK(t.kind == 'L' && t.http_code == 200 && t.exit_code == 0 && t.connects == 1 && t.colo == "CDG");
    CHECK_NEAR(t.pretransfer_s, 0.189160, 1e-9);
    CHECK_NEAR(t.server_ms, 32, 1e-9);
    CHECK(parse_speed_transfer(L[1], t));
    CHECK(t.connects == 0);
    CHECK_NEAR(t.rtt_ms(), (0.070425 - 0.000543) * 1000 - 28, 1e-6); /* the server's 28 ms taken off */
    CHECK(parse_speed_transfer(L[4], t));                             /* the DNS failure */
    CHECK(t.http_code == 0 && t.exit_code == 6 && t.server_ms < 0 && t.rtt_ms() < 0 && t.colo.empty());
    CHECK(parse_speed_transfer(D[2], t));
    CHECK(t.kind == 'D' && t.http_code == 403 && t.size_download == 1 && t.colo.empty());
    CHECK_NEAR(t.server_ms, 1, 1e-9);
    CHECK(parse_speed_transfer(U[0], t));
    CHECK(t.kind == 'U' && t.has_server_bytes && t.server_bytes == 3000000 && t.size_upload == 3000396 && t.colo == "CDG");
    CHECK(parse_speed_transfer(D[0], t) && !t.has_server_bytes && t.size_download == 2000000);
    // hostile or foreign lines
    CHECK(!parse_speed_transfer("curl: (6) Could not resolve host: speed.cloudflare.invalid", t));
    CHECK(!parse_speed_transfer("NTSPEED|X|200|0|0|0|0|0|0|0|0|0||||", t));
    CHECK(!parse_speed_transfer("NTSPEED|L|200|0", t));
    CHECK(!parse_speed_transfer("NTSPEEDY|L|200|0|0|0|0|0|0|0|0|0||||", t));
    CHECK(!parse_speed_transfer("", t));
    CHECK(parse_speed_transfer("NTSPEED|L|abc|x|-4|-1|nan|1e400|0.5|0.6|99999999999999999999999|-5|<b>|ZZZ|-3|dur=1|x|y", t));
    CHECK(t.http_code == 0 && t.exit_code == -1 && t.connects == 0 && t.pretransfer_s == 0 && t.starttransfer_s == 0.5);
    CHECK(t.colo.empty() && !t.has_server_bytes && t.size_download == 0); /* "<b>": not a site code */
    CHECK(parse_speed_transfer("NTSPEED|L|200|0|0|0|0|0|0|0|0|0|CDG-1|||", t) && t.colo.empty()); /* not a site code */
    // Server-Timing
    CHECK_NEAR(parse_server_timing("cfSpeedEdge;dur=4, cfSpeedWorker;dur=26"), 30, 1e-9);
    CHECK_NEAR(parse_server_timing("a;dur=1.5"), 1.5, 1e-9);
    CHECK(parse_server_timing("") < 0);
    CHECK(parse_server_timing("cfL4;desc=\"?proto=TCP&rtt=9263&min_rtt=5735\"") < 0);
    CHECK(parse_server_timing("x;dur=") < 0);
    CHECK(parse_server_timing("x;dur=99999999999999999") < 0);
    // the round trip never goes below 0
    SpeedTransfer f;
    f.pretransfer_s = 0.001;
    f.starttransfer_s = 0.010;
    f.server_ms = 50;
    CHECK_NEAR(f.rtt_ms(), 0, 1e-9);
}

void test_speed_argv()
{
    const std::vector<std::string> lat = speed_latency_argv(10);
    int urls = 0;
    for (const std::string &a : lat)
        if (a == "--url") ++urls;
    CHECK_EQ(urls, 11); /* + the warm-up */
    CHECK(lat[0] == "curl" && std::find(lat.begin(), lat.end(), std::string(kSpeedCiphers)) != lat.end());
    CHECK(std::find(lat.begin(), lat.end(), std::string("--http1.1")) != lat.end());
    CHECK(std::find(lat.begin(), lat.end(), std::string("=https")) != lat.end());
    // the byte cap: the URLs never ask for more, no URL asks for what the server refuses
    struct Case {
        std::uint64_t cap, chunk;
        int urls;
    } cases[] = {{150000000, 50000000, 3},  {150000000, 99999999, 2}, {1000000, 50000000, 1},
                 {150000001, 50000000, 4},  {150000000, 500000000, 2}, {7, 3, 3}};
    for (const Case &c : cases) {
        const std::vector<std::string> a = speed_download_argv(c.cap, c.chunk);
        CHECK_EQ(speed_download_total(a), c.cap);
        int n = 0;
        bool small = true;
        const std::string prefix = kSpeedDownUrl;
        for (const std::string &x : a) {
            std::uint64_t b = 0;
            if (starts_with(x, prefix) && parse_u64(x.substr(prefix.size()), b)) {
                ++n;
                if (b > kSpeedMaxRequest || b == 0) small = false;
            }
        }
        CHECK_EQ(n, c.urls);
        CHECK(small);
    }
    const std::vector<std::string> up = speed_upload_argv();
    CHECK(std::find(up.begin(), up.end(), std::string("-T")) != up.end() &&
          std::find(up.begin(), up.end(), std::string("Expect:")) != up.end() && up.back() == kSpeedUpUrl);
    // the configuration's bounds
    SpeedConfig big;
    big.max_bytes = 10000000000ull;
    big.chunk_bytes = 500000000;
    big.target_ms = 999999;
    big.ping_target = " off ";
    const SpeedConfig c = clamp_speed_config(big);
    CHECK(c.max_bytes == 500000000 && c.chunk_bytes == kSpeedMaxRequest && c.target_ms == 20000 && c.ping_target.empty());
}

void test_speed_numbers()
{
    CHECK_NEAR(ThroughputMeter::mbps(1000000, 1.0), 8.0, 1e-9);
    CHECK_NEAR(ThroughputMeter::mbps(125000, 0.5), 2.0, 1e-9);
    CHECK_EQ(ThroughputMeter::mbps(5, 0), 0.0);
    // a steady 1 MB/s with the first second at half speed: the result leaves the ramp out
    {
        ThroughputMeter m(500, 1000);
        CHECK(!m.add(0, 0) && !m.started());
        std::uint64_t b = 0;
        int samples = 0;
        for (int i = 1; i <= 80; ++i) {
            b += i <= 10 ? 50000 : 100000;
            if (m.add(static_cast<std::int64_t>(i) * 100000, b)) ++samples;
        }
        CHECK(m.started());
        CHECK_NEAR(m.result_mbps(), 8.0, 1e-6);   /* not the average with the slow start */
        CHECK(m.history().size() == 15 && samples == 15);
        CHECK_NEAR(m.current_mbps(), 8.0, 1e-6);
        CHECK_NEAR(m.history()[0].mbps, 4.0, 1e-6); /* the first half second: the ramp */
        CHECK_NEAR(m.elapsed_s(), 7.9, 1e-9);
        CHECK(!m.add(5 * 100000, b + 1));          /* never backwards */
        CHECK_EQ(m.bytes(), b);
    }
    // shorter than two ramps: the whole phase
    {
        ThroughputMeter m(500, 1000);
        m.add(1000000, 1000);
        m.add(1500000, 501000);
        m.add(2200000, 1201000);
        CHECK_NEAR(m.result_mbps(), ThroughputMeter::mbps(1200000, 1.2), 1e-6);
    }
    // the history keeps the last points only
    {
        ThroughputMeter m(200, 0, 4);
        for (int i = 0; i <= 20; ++i) m.add(static_cast<std::int64_t>(i) * 200000, 1 + static_cast<std::uint64_t>(i) * 1000);
        CHECK(m.history().size() == 4 && m.history().back().t_s > 3.9);
    }
    // the grades
    struct G {
        double ms;
        BloatGrade g;
    } grades[] = {{-5, BloatGrade::A},  {0, BloatGrade::A},    {29.9, BloatGrade::A},  {30, BloatGrade::B},
                  {59.9, BloatGrade::B}, {60, BloatGrade::C},  {199, BloatGrade::C},   {200, BloatGrade::D},
                  {399.9, BloatGrade::D}, {400, BloatGrade::F}, {5000, BloatGrade::F}, {NAN, BloatGrade::Unknown}};
    for (const G &g : grades) CHECK(bufferbloat_grade(g.ms) == g.g);
    CHECK_EQ(std::string(grade_letter(BloatGrade::C)), std::string("C"));
    CHECK_EQ(std::string(grade_letter(BloatGrade::Unknown)), std::string("-"));
    CHECK(std::string(grade_words(BloatGrade::F)).find("bufferbloat") != std::string::npos);
    CHECK(worse_grade(BloatGrade::A, BloatGrade::D) == BloatGrade::D && worse_grade(BloatGrade::F, BloatGrade::B) == BloatGrade::F);
    CHECK(worse_grade(BloatGrade::Unknown, BloatGrade::B) == BloatGrade::B && worse_grade(BloatGrade::C, BloatGrade::Unknown) == BloatGrade::C);
    // median and jitter
    CHECK_EQ(median_of({}), 0.0);
    CHECK_NEAR(median_of({5, 1, 3}), 3, 1e-9);
    CHECK_NEAR(median_of({4, 1, 3, 2}), 2.5, 1e-9);
    CHECK_NEAR(jitter_of({10, 12, 9}), 2.5, 1e-9);
    CHECK_EQ(jitter_of({10}), 0.0);
    const LatencyStats ls = latency_from_values({10, 30, 20}, 1);
    CHECK(ls.ok && ls.sent == 4 && ls.received == 3 && ls.lost == 1 && ls.median_ms == 20 && ls.min_ms == 10 && ls.max_ms == 30);
    CHECK_NEAR(ls.loss_pct(), 25, 1e-9);
    CHECK(!latency_from_values({}, 3).ok);
    // the error words
    CommandResult r;
    r.started = true;
    r.exit_code = 6;
    CHECK(speed_error_words(r, 0, "").find("cannot be found") != std::string::npos);
    r.exit_code = 7;
    CHECK(speed_error_words(r, 0, "").find("No connection") != std::string::npos);
    r.exit_code = 35;
    CHECK(speed_error_words(r, 0, "").find("TLS") != std::string::npos);
    r.exit_code = 60;
    CHECK(speed_error_words(r, 0, "").find("clock") != std::string::npos);
    r.exit_code = 56;
    CHECK(speed_error_words(r, 0, "").find("broke") != std::string::npos);
    r.exit_code = 0;
    CHECK(speed_error_words(r, 429, "").find("429") != std::string::npos);
    CHECK(speed_error_words(r, 403, "").find("403") != std::string::npos);
    CHECK(speed_error_words(r, 503, "").find("503") != std::string::npos);
    r.exit_code = 127;
    CHECK(speed_error_words(r, 0, "").find("not installed") != std::string::npos);
    CommandResult t;
    t.started = true;
    t.timed_out = true;
    CHECK(speed_error_words(t, 0, "").find("too long") != std::string::npos);
    CommandResult sig;
    sig.started = true;
    sig.term_signal = 9;
    CHECK(speed_error_words(sig, 0, "").find("signal 9") != std::string::npos);
}

void test_speed_engine()
{
    const double R = 3000000; /* 3 MB/s: 24 Mbit/s, the deck's measured link */
    // the whole test: latency, download stopped at its time, upload ended at its time and confirmed
    {
        Run t;
        t.runner->scripts = {latency_script(), direction_script(false, R, 10, 0.5),
                             direction_script(true, R / 2, 10, 1.0)};
        t.runner->scripts[2].after_end = {fake::Step{11 * kSec, 0, 0, {upload_line(12000000)}}};
        t.ping->runs = {{10, 11, 12, 10, 11, 12, 10, 11, 12, 10}, {80, 90, 85, 88, 86}, {300, 320, 310, -1, 305}};
        SpeedConfig cfg;
        t.go(cfg);
        const SpeedSnapshot &s = t.last;
        const SpeedResult &r = s.result;
        CHECK(s.state == SpeedSnapshot::State::Done && s.error.empty());
        CHECK(r.latency_ok && r.latency_samples == 3 && r.colo == "CDG");
        CHECK_NEAR(r.latency_ms, (0.047034 - 0.000345) * 1000 - 33, 1e-6); /* the median of the three */
        CHECK_NEAR(r.setup_ms, 189.160, 1e-6);
        CHECK_NEAR(r.jitter_ms, (std::fabs(((0.031559 - 0.000272) * 1000 - 22) - ((0.070425 - 0.000543) * 1000 - 28)) +
                                 std::fabs(((0.047034 - 0.000345) * 1000 - 33) - ((0.031559 - 0.000272) * 1000 - 22))) /
                                    2,
                   1e-6);
        CHECK(r.down.measured && !r.down.capped);
        CHECK_NEAR(r.down.mbps, 24.0, 0.01); /* the slow first second left out */
        CHECK_NEAR(r.down.seconds, 8.0, 0.11);
        CHECK(r.down.history.size() >= 15 && r.down.history.size() <= 17);
        CHECK(r.up.measured && r.up.has_server_bytes && r.up.server_bytes == 12000000);
        CHECK_NEAR(r.up.mbps, 12.0, 0.01);
        CHECK(r.idle.ok && r.idle.median_ms == 11);
        CHECK(r.down.has_increase && r.down.increase_ms == 86 - 11 && r.down.grade == BloatGrade::C);
        CHECK(r.up.has_increase && r.up.increase_ms == 307.5 - 11 && r.up.grade == BloatGrade::D);
        CHECK(r.up.loaded.lost == 1 && r.up.loaded.received == 4);
        CHECK(r.grade == BloatGrade::D);
        // what curl was asked: the cap, the stdin generator, the stop at the time target
        CHECK(t.runner->specs.size() == 3);
        CHECK_EQ(speed_download_total(t.runner->specs[1].argv), static_cast<std::uint64_t>(150000000));
        CHECK(t.runner->specs[1].stdout_is_data && t.runner->stopped[1] && !t.runner->input_ended[1]);
        CHECK(t.runner->specs[2].input == StreamSpec::Input::Zeros && t.runner->specs[2].zero_bytes == 150000000);
        CHECK(t.runner->input_ended[2] && !t.runner->stopped[2]);
        // the pings: idle (count 10), then one beside each direction, all stopped
        CHECK(t.ping->made == 3 && t.ping->stopped == 3);
        CHECK(t.ping->started.size() == 3 && t.ping->started[0].count == 10 && t.ping->started[1].count == 0 &&
              t.ping->started[0].target == "1.1.1.1" && t.ping->started[1].interval_ms == 200);
        // the live snapshots: phases in order, a figure every half second
        int order = 0, down_live = 0;
        bool ordered = true;
        for (const SpeedSnapshot &x : t.seen) {
            const int p = static_cast<int>(x.phase);
            if (x.phase != SpeedSnapshot::Phase::None && p < order) ordered = false;
            if (x.phase != SpeedSnapshot::Phase::None) order = p;
            if (x.phase == SpeedSnapshot::Phase::Download && x.now_mbps > 0) {
                ++down_live;
                if (x.phase_s > 2 && std::fabs(x.now_mbps - 24) > 0.01) ordered = false;
            }
        }
        CHECK(ordered && down_live >= 15);
        CHECK(t.seen.front().phase == SpeedSnapshot::Phase::Latency && t.seen.back().state == SpeedSnapshot::State::Done);
    }
    // the cap reached before the time target: every byte moved, nothing more asked
    {
        Run t;
        const std::string d = fixture_lines('D')[0];
        t.runner->scripts = {latency_script(), direction_script(false, R, 10, 1.0, 9000000, {d}),
                             direction_script(true, R, 10, 1.0, 9000000)};
        t.runner->scripts[2].steps.push_back(fake::Step{6 * kSec, 0, 9000000, {upload_line(9000000)}});
        SpeedConfig cfg;
        cfg.max_bytes = 9000000;
        cfg.ping_target = "";
        t.go(cfg);
        const SpeedResult &r = t.last.result;
        CHECK(t.last.state == SpeedSnapshot::State::Done);
        CHECK(r.down.capped && r.down.measured && r.down.bytes == 9000000 && !t.runner->stopped[1]);
        CHECK_NEAR(r.down.mbps, 24.0, 0.01);
        CHECK(r.up.capped && r.up.measured && r.up.server_bytes == 9000000);
        CHECK(r.up.mbps > 0 && r.up.mbps < 24.0); /* the time until the answer counts */
        CHECK_EQ(speed_download_total(t.runner->specs[1].argv), static_cast<std::uint64_t>(9000000));
        CHECK(t.runner->specs[2].zero_bytes == 9000000);
        CHECK(t.ping->made == 0 && r.ping_note.find("No ping target") != std::string::npos);
        CHECK(r.grade == BloatGrade::Unknown && r.down.grade == BloatGrade::Unknown);
    }
    // an upload the server never confirms: stopped 6 s after the end of input, measured anyway
    {
        Run t;
        t.runner->scripts = {latency_script(), direction_script(false, R, 9), direction_script(true, R, 9)};
        for (int i = 1; i <= 8; ++i) t.runner->scripts[2].after_end.push_back(fake::Step{(10 + i) * kSec, 0, 0, {}});
        SpeedConfig cfg;
        cfg.ping_target = "";
        t.go(cfg);
        CHECK(t.last.state == SpeedSnapshot::State::Done);
        CHECK(t.last.result.up.measured && t.runner->stopped[2] && t.runner->input_ended[2]);
        CHECK(t.last.result.up.note.find("did not confirm") != std::string::npos);
        CHECK_NEAR(t.last.result.up.mbps, 24.0, 0.01);
    }
    // failures, in plain words
    {
        Run t; /* no DNS: curl's own line and its report */
        fake::Script s;
        s.steps.push_back(fake::Step{kSec, 0, 0, {"curl: (6) Could not resolve host: speed.cloudflare.com", fixture_lines('L')[4]}});
        s.exit_code = 6;
        t.runner->scripts = {s};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Failed && t.last.error.find("cannot be found") != std::string::npos);
        CHECK(t.last.detail.find("Could not resolve host") != std::string::npos);
        CHECK(t.runner->specs.size() == 1 && t.ping->stopped == t.ping->made);
    }
    {
        Run t; /* Cloudflare says 429 */
        fake::Script s;
        s.steps.push_back(fake::Step{kSec, 0, 0, {"NTSPEED|L|429|0|1|0.04|0.18|0.18|0.2|0.2|0|0|CDG|||"}});
        t.runner->scripts = {s};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Failed && t.last.error.find("429") != std::string::npos);
    }
    {
        Run t; /* TLS fails: no report at all */
        fake::Script s;
        s.exit_code = 35;
        t.runner->scripts = {s};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Failed && t.last.error.find("TLS") != std::string::npos);
    }
    {
        Run t; /* no curl */
        fake::Script s;
        s.started = false;
        t.runner->scripts = {s};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Failed && t.last.error.find("curl is not installed") != std::string::npos);
    }
    {
        Run t; /* the download is refused (403): no upload after it */
        fake::Script d;
        d.steps.push_back(fake::Step{2 * kSec, 1, 0, {fixture_lines('D')[2]}});
        t.runner->scripts = {latency_script(), d};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Failed && t.last.error.find("403") != std::string::npos);
        CHECK(t.runner->specs.size() == 2 && !t.last.result.down.measured && t.last.result.latency_ok);
    }
    {
        Run t; /* the connection breaks after 3 s: measured over what came, with a note; the upload still runs */
        fake::Script d = direction_script(false, R, 3);
        d.exit_code = 56;
        t.runner->scripts = {latency_script(), d, direction_script(true, R, 9)};
        t.runner->scripts[2].after_end = {fake::Step{12 * kSec, 0, 0, {upload_line(20000000)}}};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Done && t.last.result.down.measured);
        CHECK(t.last.result.down.note.find("Measured over 2.9 s only") != std::string::npos);
        CHECK(t.last.result.down.note.find("broke") != std::string::npos);
        CHECK(t.last.result.up.measured);
    }
    {
        Run t; /* it breaks at once: no measure */
        fake::Script d = direction_script(false, R, 0.4);
        d.exit_code = 56;
        t.runner->scripts = {latency_script(), d};
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Failed && t.last.error.find("broke") != std::string::npos);
    }
    // ping not allowed: the speeds are measured, the grade is not
    {
        Run t;
        t.runner->scripts = {latency_script(), direction_script(false, R, 9), direction_script(true, R, 9)};
        t.runner->scripts[2].after_end = {fake::Step{12 * kSec, 0, 0, {upload_line(20000000)}}};
        t.ping->fail_start = true;
        t.ping->fail_words = "Ping is not allowed for this user";
        t.go(SpeedConfig());
        CHECK(t.last.state == SpeedSnapshot::State::Done && t.last.result.down.measured && t.last.result.up.measured);
        CHECK(t.ping->made == 1 && t.last.result.ping_note == "Ping is not allowed for this user");
        CHECK(t.last.result.grade == BloatGrade::Unknown);
    }
    // every ping lost under load: F, said so
    {
        Run t;
        t.runner->scripts = {latency_script(), direction_script(false, R, 9), direction_script(true, R, 9)};
        t.runner->scripts[2].after_end = {fake::Step{12 * kSec, 0, 0, {upload_line(20000000)}}};
        t.ping->runs = {{10, 10, 10}, {-1, -1, -1, -1}, {20, 20, 20}};
        t.go(SpeedConfig());
        CHECK(t.last.result.down.grade == BloatGrade::F && !t.last.result.down.has_increase);
        CHECK(t.last.result.down.grade_note.find("lost") != std::string::npos);
        CHECK(t.last.result.up.grade == BloatGrade::A && t.last.result.grade == BloatGrade::F);
    }
    // cancel in the middle of the download: curl stopped, no upload, every ping stopped
    {
        Run t;
        t.runner->scripts = {latency_script(), direction_script(false, R, 10)};
        t.ping->runs = {{10, 10, 10}, {20, 20}};
        CancelToken cancel;
        t.last = run_speedtest(SpeedConfig(), *t.runner, fake::scripted_ping(t.ping), cancel, [&](const SpeedSnapshot &s) {
            if (s.phase == SpeedSnapshot::Phase::Download && s.phase_s >= 3) cancel.cancel();
        });
        CHECK(t.last.state == SpeedSnapshot::State::Cancelled && t.last.error.empty());
        CHECK(t.runner->specs.size() == 2 && t.runner->cancelled[1]);
        CHECK(t.ping->made == t.ping->stopped && t.ping->made == 2);
    }
}

/* ---------------------------------------------------------------- the real ping module beside a scripted load */

class SlowEcho : public EchoSocket {
public:
    explicit SlowEcho(int delay_ms) : delay_ms_(delay_ms) {}
    int send(std::uint16_t seq) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        sent_.push_back({seq, now_ms()});
        return 0;
    }
    int receive(int timeout_ms, std::uint16_t &seq) override
    {
        const std::int64_t end = now_ms() + timeout_ms;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (auto it = sent_.begin(); it != sent_.end(); ++it)
                    if (now_ms() >= it->second + delay_ms_) {
                        seq = it->first;
                        sent_.erase(it);
                        return 1;
                    }
            }
            if (now_ms() >= end) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    int delay_ms_;
    std::mutex mu_;
    std::vector<std::pair<std::uint16_t, std::int64_t>> sent_;
};

void test_speed_real_ping()
{
    auto delays = std::make_shared<std::vector<int>>(std::vector<int>{5, 120, 300});
    auto opened = std::make_shared<std::atomic<int>>(0);
    const EchoFactory echo = [delays, opened](const Address &, int, int *err) -> std::unique_ptr<EchoSocket> {
        const int i = opened->fetch_add(1);
        if (err) *err = 0;
        return std::make_unique<SlowEcho>(i < static_cast<int>(delays->size()) ? (*delays)[static_cast<std::size_t>(i)] : 5);
    };
    fake::StreamRunner runner;
    runner.scripts = {latency_script(), direction_script(false, 3000000, 3), direction_script(true, 3000000, 3)};
    runner.scripts[1].real_sleep_us = 50000; /* 2 s of fake time in about 1 s */
    runner.scripts[2].real_sleep_us = 50000;
    runner.scripts[2].after_end = {fake::Step{6 * kSec, 0, 0, {upload_line(6000000)}}};
    SpeedConfig cfg;
    cfg.target_ms = 2000;
    cfg.idle_pings = 3;
    cfg.ping_target = "127.0.0.1"; /* a literal: no DNS */
    const SpeedSnapshot s = run_speedtest(cfg, runner, session_load_ping(echo), CancelToken(), nullptr);
    const SpeedResult &r = s.result;
    CHECK(s.state == SpeedSnapshot::State::Done);
    CHECK(opened->load() == 3);
    CHECK(r.idle.ok && r.idle.received == 3 && r.idle.median_ms < 40);
    CHECK(r.down.loaded.ok && r.down.loaded.received >= 2);
    CHECK(r.up.loaded.ok && r.up.loaded.received >= 2);
    /* about 115 ms and 295 ms: well inside C (60 .. 200) and D (200 .. 400), so a busy build machine does not matter */
    CHECK(r.down.has_increase && r.down.increase_ms > 60 && r.down.increase_ms < 200 && r.down.grade == BloatGrade::C);
    CHECK(r.up.has_increase && r.up.increase_ms >= 200 && r.up.increase_ms < 400 && r.up.grade == BloatGrade::D);
    CHECK(r.ping_address == "127.0.0.1");
}

/* ---------------------------------------------------------------- the session, fake curl programs, the real runner */

void test_speed_session()
{
    const std::string dir = test_tmpdir("fakespeed");
    {
        std::ofstream(dir + "/latency") << fixture("speedtest", "curl-deck.txt");
        std::ofstream f(dir + "/curl");
        f << "#!/bin/sh\n"
             "echo $$ > \"$NT_FAKE_DIR/pid\"\n"
             "case \"$*\" in\n"
             "*'NTSPEED|L|'*) grep '^NTSPEED|L|200' \"$NT_FAKE_DIR/latency\" >&2 ;;\n"
             "*'NTSPEED|D|'*) sleep 30 & echo $! > \"$NT_FAKE_DIR/child\"\n"
             "  while :; do head -c 65536 /dev/zero || exit 0; sleep 0.01; done ;;\n"
             "*'NTSPEED|U|'*) n=$(wc -c)\n"
             "  printf 'NTSPEED|U|200|0|1|0.01|0.02|0.03|0.5|2.1|0|%s||CDG|%s|\\n' \"$n\" \"$n\" >&2 ;;\n"
             "esac\n";
    }
    chmod((dir + "/curl").c_str(), 0755);
    const char *old_path = std::getenv("PATH");
    const std::string saved = old_path ? old_path : "";
    setenv("PATH", (dir + ":" + saved).c_str(), 1);
    setenv("NT_FAKE_DIR", dir.c_str(), 1);
    // a whole run: 2 s per direction, the upload's zero bytes counted by the fake server
    {
        SpeedTestSession s(std::make_shared<DirectStreamRunner>(), nullptr);
        SpeedConfig cfg;
        cfg.target_ms = 2000;
        cfg.ping_target = "";
        CHECK(s.start(cfg));
        CHECK(s.wait(15000));
        const auto snap = s.latest();
        CHECK(snap->state == SpeedSnapshot::State::Done);
        CHECK(snap->result.latency_ok && snap->result.latency_samples == 3);
        CHECK(snap->result.down.measured && snap->result.down.mbps > 0 && snap->result.down.seconds >= 1.9);
        CHECK(snap->result.up.measured && snap->result.up.has_server_bytes);
        CHECK(snap->result.up.server_bytes > 0 && snap->result.up.server_bytes == snap->result.up.bytes);
    }
    // Stop in the middle of the download: curl's whole group ends quickly
    {
        SpeedTestSession s(std::make_shared<DirectStreamRunner>(), nullptr);
        SpeedConfig cfg;
        cfg.target_ms = 20000;
        cfg.ping_target = "";
        CHECK(s.start(cfg));
        bool downloading = false;
        for (int i = 0; i < 200 && !downloading; ++i) {
            const auto snap = s.latest();
            downloading = snap->phase == SpeedSnapshot::Phase::Download && snap->phase_bytes > 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(downloading);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::int64_t t0 = now_ms();
        s.stop();
        CHECK(s.wait(3000));
        CHECK(now_ms() - t0 < 1500); /* a poll slice, then curl's SIGTERM */
        CHECK(s.latest()->state == SpeedSnapshot::State::Cancelled);
        int child = 0, pid = 0;
        std::ifstream(dir + "/child") >> child;
        std::ifstream(dir + "/pid") >> pid;
        CHECK(child > 1 && pid > 1);
        for (const int p : {child, pid}) {
            bool gone = false;
            for (int i = 0; i < 100 && !gone; ++i) {
                gone = p > 1 && kill(p, 0) != 0 && errno == ESRCH;
                if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            CHECK(gone);
        }
    }
    setenv("PATH", saved.c_str(), 1);
    unsetenv("NT_FAKE_DIR");
    remove_dir(dir);
}

} // namespace

void test_speedtest()
{
    test_speed_parse();
    test_speed_argv();
    test_speed_numbers();
    test_speed_engine();
    test_speed_real_ping();
    test_speed_session();
}
