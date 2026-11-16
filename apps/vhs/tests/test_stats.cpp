/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the stats math (mean, p95, cores), the /proc and /sys parsers, the system probe on a fake root, the
 * command runner (vcgencmd's path), the log line and the overlay.
 */
#include "check.hpp"

#include "live/clock.hpp"
#include "live/stats.hpp"

#include <unistd.h>

using namespace vhs;

namespace {

void math()
{
    Summary e = summarize({});
    CHECK_EQ(e.n, 0);
    CHECK_EQ(e.mean, 0.0);
    std::vector<double> v;
    for (int i = 100; i >= 1; --i) v.push_back(i); /* 1..100, unsorted */
    Summary s = summarize(v);
    CHECK_EQ(s.n, 100);
    CHECK_NEAR(s.mean, 50.5, 1e-9);
    CHECK_EQ(s.p95, 95.0);
    CHECK_EQ(s.max, 100.0);
    s = summarize({7.0});
    CHECK_EQ(s.p95, 7.0);
    CHECK_EQ(s.max, 7.0);
    s = summarize({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 1000});
    CHECK_EQ(s.p95, 19.0); /* rank ceil(0.95 x 20) = 19 */
    CHECK_EQ(s.max, 1000.0);

    SampleSet set;
    set.add(1.5);
    set.add(2.5);
    std::vector<double> got = set.take();
    CHECK_EQ(got.size(), static_cast<std::size_t>(2));
    CHECK(set.take().empty());

    CHECK_NEAR(cores_between(1000, 1100, 0, 1000000, 100), 1.0, 1e-9);   /* 100 ticks at 100 Hz in 1 s */
    CHECK_NEAR(cores_between(0, 50, 0, 2000000, 100), 0.25, 1e-9);
    CHECK_NEAR(cores_between(0, 300, 0, 1000000, 100), 3.0, 1e-9);       /* three cores busy */
    CHECK_EQ(cores_between(0, 10, 5, 5, 100), -1.0);
    CHECK_EQ(cores_between(10, 5, 0, 1000000, 100), -1.0);
}

void parsers()
{
    std::uint64_t t = 0;
    /* the command name may hold spaces and parentheses: fields are counted after the last ')' */
    const std::string stat =
        "4242 (vhs (spike) x) S 1 4242 4242 0 -1 4194560 1234 0 0 0 517 83 0 0 20 0 6 0 12345 123456789 5000 "
        "18446744073709551615 1 1 0 0 0 0 0 4096 0 0 0 0 17 2 0 0 0 0 0";
    CHECK(parse_stat_ticks(stat, t));
    CHECK_EQ(t, 600ull);
    CHECK(!parse_stat_ticks("garbage", t));
    CHECK(!parse_stat_ticks("1 (x) S 1 2", t));
    long kb = 0;
    CHECK(parse_vmrss_kb("Name:\tvhs\nVmPeak:\t  99999 kB\nVmRSS:\t   21504 kB\nRssAnon:\t 1 kB\n", kb));
    CHECK_EQ(kb, 21504l);
    CHECK(!parse_vmrss_kb("Name: x\n", kb));
    double c = 0;
    CHECK(parse_millideg("48312\n", c));
    CHECK_NEAR(c, 48.312, 1e-9);
    CHECK(!parse_millideg("", c));
    std::uint32_t b = 1;
    CHECK(parse_throttled("throttled=0x50005\n", b));
    CHECK_EQ(b, 0x50005u);
    CHECK(parse_throttled("0x0", b));
    CHECK_EQ(b, 0u);
    CHECK(parse_throttled("80008\n", b));
    CHECK_EQ(b, 0x80008u);
    CHECK(!parse_throttled("volts=1.2V", b));
    CHECK(!parse_throttled("throttled=", b));
    CHECK(!parse_throttled("0x5 junk", b));
    CHECK_EQ(throttled_text(0), std::string("ok"));
    CHECK_EQ(throttled_text(0x50005), std::string("under-voltage now, throttled now, under-voltage since boot, throttled since boot"));
    CHECK_EQ(throttled_text(0x80000), std::string("temp-limit since boot"));
}

void probe_fake_root()
{
    const std::string root = make_temp_dir("sys");
    make_dirs(root + "/proc/self/task/77");
    write_file(root + "/proc/self/stat", "1 (vhs) S 1 1 1 0 -1 0 0 0 0 0 250 50 0 0 20 0 4 0\n");
    write_file(root + "/proc/self/task/77/stat", "77 (vhs) R 1 1 1 0 -1 0 0 0 0 0 40 2 0 0 20 0 4 0\n");
    write_file(root + "/proc/self/status", "VmRSS:\t  10240 kB\n");
    make_dirs(root + "/sys/class/thermal/thermal_zone0");
    make_dirs(root + "/sys/class/thermal/thermal_zone1");
    write_file(root + "/sys/class/thermal/thermal_zone0/type", "gpu-thermal\n");
    write_file(root + "/sys/class/thermal/thermal_zone0/temp", "40000\n");
    write_file(root + "/sys/class/thermal/thermal_zone1/type", "cpu-thermal\n");
    write_file(root + "/sys/class/thermal/thermal_zone1/temp", "51234\n");
    make_dirs(root + "/sys/devices/platform/soc/soc:firmware");
    write_file(root + "/sys/devices/platform/soc/soc:firmware/get_throttled", "50000\n");
    SystemProbe p(root);
    std::uint64_t t = 0;
    CHECK(p.process_ticks(t));
    CHECK_EQ(t, 300ull);
    CHECK(p.thread_ticks(77, t));
    CHECK_EQ(t, 42ull);
    CHECK(!p.thread_ticks(78, t));
    CHECK(!p.thread_ticks(0, t));
    CHECK_NEAR(p.rss_mb(), 10.0, 1e-9);
    double c = 0;
    CHECK(p.temp_c(c));
    CHECK_NEAR(c, 51.234, 1e-9); /* the cpu zone, not zone 0 */
    std::uint32_t bits = 0;
    CHECK(p.throttled(bits, 0));
    CHECK_EQ(bits, 0x50000u);
    CHECK(p.throttle_source().find("get_throttled") != std::string::npos);
    rm_tree(root);

    /* an empty root: nothing known, nothing crashes */
    const std::string empty = make_temp_dir("nosys");
    SystemProbe q(empty);
    CHECK(!q.process_ticks(t));
    CHECK_EQ(q.rss_mb(), -1.0);
    CHECK(!q.temp_c(c));
    CHECK(!q.throttled(bits, 0));
    CHECK(q.throttle_source().empty());
    rm_tree(empty);

    /* the real system of the build box: the process itself is readable */
    SystemProbe real;
    CHECK(real.process_ticks(t));
    CHECK(real.thread_ticks(static_cast<int>(getpid()), t));
    CHECK(real.rss_mb() > 0);
    CHECK(real.hz() > 0);
}

void commands()
{
    std::string out;
    CHECK(run_capture_output({"/bin/echo", "throttled=0x0"}, 2000, out));
    std::uint32_t bits = 9;
    CHECK(parse_throttled(out, bits));
    CHECK_EQ(bits, 0u);
    CHECK(!run_capture_output({"/bin/false"}, 2000, out));
    CHECK(!run_capture_output({"/nonexistent/vcgencmd", "get_throttled"}, 2000, out));
    const std::int64_t t0 = mono_us();
    CHECK(!run_capture_output({"/bin/sleep", "5"}, 200, out)); /* timed out, killed */
    CHECK(mono_us() - t0 < 3000000);
    CHECK(!run_capture_output({}, 100, out));
}

void lines()
{
    Interval iv;
    iv.index = 3;
    iv.state = "streaming";
    iv.mode = "MJPG 1280x720 30 fps";
    iv.scale = "auto 1/2 640x360";
    iv.fmt = "RGB565";
    iv.box = "352x198";
    iv.cap_fps = 30.0;
    iv.dec_fps = 29.5;
    iv.shown_fps = 29.0;
    iv.drop_dec = 1;
    iv.drop_ui = 2;
    iv.gaps = 3;
    iv.dec_ms = summarize({10, 12, 14});
    iv.conv_ms = summarize({1, 1, 2});
    iv.cores = 0.52;
    iv.cap_cores = 0.02;
    iv.dec_cores = 0.4;
    iv.ui_cores = 0.09;
    iv.rss_mb = 21.0;
    iv.have_temp = true;
    iv.temp_c = 48.3;
    iv.have_thr = true;
    iv.thr = 0x50005;
    const std::string l = format_log(iv);
    CHECK(l.compare(0, 11, "[vhs] stats") == 0);
    CHECK(l.find("mode=\"MJPG 1280x720 30 fps\"") != std::string::npos);
    CHECK(l.find("cap=30.0 dec=29.5 shown=29.0") != std::string::npos);
    CHECK(l.find("drop=dec:1,ui:2,cam:3") != std::string::npos);
    CHECK(l.find("decode_ms=12.0/14.0") != std::string::npos);
    CHECK(l.find("convert_ms=1.3/2.0") != std::string::npos);
    CHECK(l.find("ui_ms=- ") != std::string::npos);
    CHECK(l.find("cpu=0.52 threads=cap:0.02,dec:0.40,ui:0.09") != std::string::npos);
    CHECK(l.find("rss_mb=21.0 temp_c=48.3 throttled=0x50005") != std::string::npos);
    const std::vector<std::string> o = format_overlay(iv);
    CHECK(o.size() >= 7);
    if (o.size() >= 7) {
        CHECK_EQ(o[0], std::string("MJPG 1280x720 30 fps"));
        CHECK(o[2].find("cap 30.0") != std::string::npos);
        CHECK(o.back().find("thr 0x50005") != std::string::npos);
    }
    Interval none;
    none.state = "no-camera";
    CHECK(format_log(none).find("state=no-camera") != std::string::npos);
    CHECK_EQ(format_overlay(none)[0], std::string("no-camera"));
}

} // namespace

void test_stats()
{
    math();
    parsers();
    probe_fake_root();
    commands();
    lines();
}
