/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the settings file, the screen geometry (range, orientation, arrow, labels, scales) and the
 * scripted ports of the scenes (they must be found by the real link, and never look like the radar when they are not).
 */
#include "check.hpp"

#include "demo_sys.hpp"
#include "link.hpp"
#include "settings.hpp"
#include "view.hpp"

#include <cmath>
#include <cstdlib>
#include <unistd.h>

using namespace mmw;

namespace {

void test_settings()
{
    Settings d;
    CHECK(d.units == UnitMode::MetresKmh);
    CHECK(d.orientation == Orientation::Normal);
    CHECK_EQ(d.range_m, 0);
    CHECK(d.trails);
    CHECK_EQ(serialize_settings(d), "units=metres\norientation=normal\nrange=auto\ntrails=on\n");
    Settings s;
    s.units = UnitMode::MmCms;
    s.orientation = Orientation::Rotate180;
    s.range_m = 6;
    s.trails = false;
    const Settings r = parse_settings(serialize_settings(s));
    CHECK(r.units == UnitMode::MmCms);
    CHECK(r.orientation == Orientation::Rotate180);
    CHECK_EQ(r.range_m, 6);
    CHECK(!r.trails);
    /* tolerant: CRLF, junk, bad values keep the defaults */
    const Settings j = parse_settings("junk\r\nrange=5\r\norientation=sideways\r\nunits=raw\r\ntrails=maybe\r\n");
    CHECK_EQ(j.range_m, 0);
    CHECK(j.orientation == Orientation::Normal);
    CHECK(j.units == UnitMode::MmCms);
    CHECK(j.trails);
    CHECK(parse_settings("orientation=mirror").orientation == Orientation::Mirror);
    /* labels and cycles */
    CHECK_EQ(std::string(units_label(UnitMode::MetresKmh)), "Metres");
    CHECK_EQ(std::string(units_label(UnitMode::MmCms)), "Millimetres");
    CHECK_EQ(std::string(orientation_label(Orientation::Rotate180)), "Rotate 180");
    CHECK_EQ(range_label(0), "Auto");
    CHECK_EQ(range_label(4), "4 m");
    CHECK(next_units(UnitMode::MmCms) == UnitMode::MetresKmh);
    CHECK(next_orientation(Orientation::Normal) == Orientation::Mirror);
    CHECK(next_orientation(Orientation::Mirror) == Orientation::Rotate180);
    CHECK(next_orientation(Orientation::Rotate180) == Orientation::Normal);
    CHECK_EQ(next_range_setting(0), 2);
    CHECK_EQ(next_range_setting(2), 4);
    CHECK_EQ(next_range_setting(4), 6);
    CHECK_EQ(next_range_setting(6), 0);
}

void test_files()
{
    char tmpl[] = "/tmp/mmw-files-XXXXXX";
    const char *base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) return;
    const std::string dir = std::string(base) + "/a/b";
    CHECK(make_dirs(dir));
    CHECK(write_file_atomic(dir + "/settings.conf", "units=raw\n"));
    std::string text;
    CHECK(read_small_file(dir + "/settings.conf", text));
    CHECK_EQ(text, "units=raw\n");
    CHECK(!read_small_file(dir + "/missing", text));
    CHECK(write_file_atomic(dir + "/settings.conf", std::string(100, 'x')));
    CHECK(!read_small_file(dir + "/settings.conf", text, 50)); /* larger than the cap */
    unlink((dir + "/settings.conf").c_str());
    rmdir(dir.c_str());
    rmdir((std::string(base) + "/a").c_str());
    rmdir(base);
    /* the folder */
    setenv("MMWAVE_CONFIG_DIR", "/x/y", 1);
    CHECK_EQ(config_dir(), "/x/y");
    unsetenv("MMWAVE_CONFIG_DIR");
    setenv("XDG_CONFIG_HOME", "/cfg", 1);
    CHECK_EQ(config_dir(), "/cfg/mmwave-radar");
    unsetenv("XDG_CONFIG_HOME");
}

void test_range()
{
    /* grows at once */
    CHECK_EQ(auto_range(2, 1800, true), 4);  /* 1.8 + 0.5 > 2 */
    CHECK_EQ(auto_range(2, 1400, true), 2);
    CHECK_EQ(auto_range(4, 5600, true), 6);
    CHECK_EQ(auto_range(6, 7900, true), 6);  /* beyond 6 m: stays at the largest */
    /* shrinks only well inside the smaller step, one step at a time */
    CHECK_EQ(auto_range(6, 3300, true), 6);  /* 3.8 > 4 - 0.3 */
    CHECK_EQ(auto_range(6, 3100, true), 4);  /* 3.6 <= 3.7 */
    CHECK_EQ(auto_range(6, 500, true), 4);   /* one step */
    CHECK_EQ(auto_range(4, 1300, true), 4);  /* 1.8 > 1.7 */
    CHECK_EQ(auto_range(4, 1200, true), 2);
    /* no target: keeps */
    CHECK_EQ(auto_range(6, 0, false), 6);
    /* hysteresis: around the 4 m step no flapping */
    int r = 4;
    for (int d : {3400, 3600, 3400, 3550, 3450}) r = auto_range(r, d, true);
    CHECK_EQ(r, 6);
    r = auto_range(r, 3300, true);
    CHECK_EQ(r, 6);
    /* the keys */
    CHECK_EQ(step_range(0, 4, 1), 6);
    CHECK_EQ(step_range(0, 4, -1), 2);
    CHECK_EQ(step_range(0, 6, 1), 6);
    CHECK_EQ(step_range(0, 2, -1), 2);
    CHECK_EQ(step_range(2, 2, 1), 4);
    CHECK_EQ(step_range(6, 6, 1), 0);   /* past the largest: Auto */
    CHECK_EQ(step_range(2, 2, -1), 0);  /* under the smallest: Auto */
    CHECK_EQ(step_range(4, 4, -1), 2);
}

void test_geometry()
{
    /* 480 x 236: the wedge's height limits (4 m) */
    PlotGeom g = plot_geom(0, 0, 480, 236, 4, Orientation::Normal, 12, 10);
    CHECK_EQ(g.sx, 240);
    CHECK_EQ(g.sy, 225);
    CHECK(std::fabs(g.px_per_mm * 4000.0 - (236 - 12 - 10)) < 0.5);
    int px, py;
    plot_point(g, 0, 4000, px, py);
    CHECK_EQ(px, 240);
    CHECK_EQ(py, 225 - 214);
    CHECK(plot_contains(g, px, py));
    /* the wedge's corners at full range stay inside */
    const double a = kFovDeg * M_PI / 180.0;
    plot_point(g, static_cast<int>(4000 * std::sin(a)), static_cast<int>(4000 * std::cos(a)), px, py);
    CHECK(plot_contains(g, px, py));
    plot_point(g, -static_cast<int>(4000 * std::sin(a)), static_cast<int>(4000 * std::cos(a)), px, py);
    CHECK(plot_contains(g, px, py));
    /* +x is right in Normal, left in Mirror */
    plot_point(g, 1000, 1000, px, py);
    CHECK(px > g.sx);
    g = plot_geom(0, 0, 480, 236, 4, Orientation::Mirror, 12, 10);
    plot_point(g, 1000, 1000, px, py);
    CHECK(px < g.sx);
    CHECK(py < g.sy);
    /* Rotate180: the sensor at the top, the front down, x mirrored */
    g = plot_geom(0, 0, 480, 236, 4, Orientation::Rotate180, 12, 10);
    CHECK_EQ(g.sy, 10);
    plot_point(g, 1000, 1000, px, py);
    CHECK(px < g.sx);
    CHECK(py > g.sy);
    /* 320 x 380 (narrow portrait): the width limits */
    g = plot_geom(0, 0, 320, 380, 6, Orientation::Normal, 12, 10);
    const double half = 6000 * std::sin(a) * g.px_per_mm;
    CHECK(half <= (320 - 24) / 2.0 + 0.5);
    CHECK(6000 * g.px_per_mm <= 380 - 22 + 0.5);
    /* the arrow */
    CHECK_EQ(arrow_length(0, 12), 0);
    CHECK_EQ(arrow_length(-4, 12), 0);
    CHECK_EQ(arrow_length(5, 12), 19);       /* 12 * (1.5 + 2.5 * 5/150) */
    CHECK_EQ(arrow_length(-150, 12), 48);
    CHECK_EQ(arrow_length(900, 12), 48);     /* capped */
    CHECK(arrow_length(80, 12) > arrow_length(40, 12));
}

void test_labels()
{
    const PlotGeom g = plot_geom(0, 0, 480, 236, 4, Orientation::Normal, 12, 10);
    /* right of the dot */
    Box b = place_label(g, 100, 100, 6, 120, 18, nullptr, 0);
    CHECK(b.x > 106);
    CHECK_EQ(b.y, 91);
    /* at the right edge: on the left */
    b = place_label(g, 470, 100, 6, 120, 18, nullptr, 0);
    CHECK(b.x + b.w <= 470 - 6);
    /* at the top edge: kept inside */
    b = place_label(g, 100, 2, 6, 120, 18, nullptr, 0);
    CHECK_EQ(b.y, 0);
    /* an overlap moves it below the label already placed */
    const Box first = place_label(g, 100, 100, 6, 120, 18, nullptr, 0);
    const Box second = place_label(g, 104, 104, 6, 120, 18, &first, 1);
    CHECK(!second.overlaps(first));
    CHECK(second.y >= first.y + first.h);
    /* near the bottom: above instead */
    const Box low = place_label(g, 100, 228, 6, 120, 18, nullptr, 0);
    const Box low2 = place_label(g, 100, 228, 6, 120, 18, &low, 1);
    CHECK(!low2.overlaps(low));
    CHECK(low2.y + low2.h <= low.y);
    CHECK(low2.y >= 0);
    /* nice scales */
    CHECK_EQ(nice_ceiling(0), 1.0);
    CHECK_EQ(nice_ceiling(3.2), 5.0);
    CHECK_EQ(nice_ceiling(4000), 5000.0);
    CHECK_EQ(nice_ceiling(51), 100.0);
    CHECK_EQ(nice_ceiling(2), 2.0);
    CHECK_EQ(nice_ceiling(0.18), 0.2);
}

void test_demo()
{
    /* the encoder is the decoder's inverse */
    Frame f = demo_frame(3.0, 3);
    std::uint8_t raw[30];
    encode_frame(f, raw);
    Frame back;
    CHECK(decode_frame(raw, back));
    for (int i = 0; i < 3; ++i) {
        CHECK(back.target[i].present);
        CHECK_EQ(back.target[i].x_mm, f.target[i].x_mm);
        CHECK_EQ(back.target[i].y_mm, f.target[i].y_mm);
        CHECK_EQ(back.target[i].speed_cm_s, f.target[i].speed_cm_s);
        CHECK_EQ(back.target[i].resolution_mm, 360);
    }
    CHECK(plausible(back));
    Frame one = demo_frame(1.0, 1);
    encode_frame(one, raw);
    CHECK(decode_frame(raw, back));
    CHECK(back.target[0].present);
    CHECK(!back.target[1].present);
    CHECK(!back.target[2].present);
    /* negative values in sign-magnitude */
    Frame n;
    n.target[0].present = true;
    n.target[0].x_mm = -1234;
    n.target[0].y_mm = 2000;
    n.target[0].speed_cm_s = -51;
    encode_frame(n, raw);
    CHECK(decode_frame(raw, back));
    CHECK_EQ(back.target[0].x_mm, -1234);
    CHECK_EQ(back.target[0].speed_cm_s, -51);

    /* the scenarios through the real link, on a test clock */
    struct Case {
        const char *name;
        LinkState want;
        int present;
    } cases[] = {{"three", LinkState::Found, 3}, {"one", LinkState::Found, 1}, {"busy", LinkState::Busy, 0},
                 {"none", LinkState::Searching, 0}, {"denied", LinkState::Error, 0}, {"empty", LinkState::Searching, 0}};
    for (const Case &c : cases) {
        std::int64_t now = 1000;
        DemoSys sys(c.name, [&now] { return now; });
        Link link(sys);
        link.start(now);
        for (int k = 0; k < 150; ++k) { /* 4.5 s */
            now += 30;
            link.poll(now);
        }
        CHECK_EQ(std::string(link_state_name(link.state())), std::string(link_state_name(c.want)));
        if (c.want == LinkState::Found) {
            CHECK_EQ(link.last_frame().present_count(), c.present);
            CHECK(link.stats(now).fps > 15.0);
            CHECK_EQ(link.stats().bad_frames, 0u);
        }
        if (std::string(c.name) == "none") {
            CHECK_EQ(link.reports().size(), static_cast<std::size_t>(2));
            if (link.reports().size() == 2) {
                CHECK(link.reports()[0].text.find("GPS") != std::string::npos);
                CHECK(link.reports()[1].verdict == Verdict::Silent);
            }
        }
    }
    /* lost: found, then the stream stops and the link says so */
    {
        std::int64_t now = 1000;
        DemoSys sys("lost", [&now] { return now; });
        Link link(sys);
        link.start(now);
        bool found = false;
        for (int k = 0; k < 250; ++k) {
            now += 30;
            link.poll(now);
            found = found || link.state() == LinkState::Found;
        }
        CHECK(found);
        CHECK(link.state() == LinkState::Lost);
    }
    /* the recorded walk, from the fixture */
    {
        setenv("MMWAVE_DEMO_FILE", (test_data_dir() + "ld2452-9600-walk-15s.bin").c_str(), 1);
        std::int64_t now = 1000;
        DemoSys sys("walk", [&now] { return now; });
        Link link(sys);
        link.start(now);
        for (int k = 0; k < 100; ++k) {
            now += 30;
            link.poll(now);
        }
        unsetenv("MMWAVE_DEMO_FILE");
        CHECK(link.state() == LinkState::Found);
        CHECK(link.tracker().slot(0).active);
        CHECK_EQ(link.stats().bad_frames, 0u);
    }
}

} // namespace

void test_view()
{
    test_settings();
    test_files();
    test_range();
    test_geometry();
    test_labels();
    test_demo();
}
