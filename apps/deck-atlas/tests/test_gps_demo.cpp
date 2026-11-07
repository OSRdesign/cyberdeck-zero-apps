/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the demonstration GPS port (core/gps_demo.cpp) and the synthetic feeds the scenes play
 * (tests/tools/gen_nmea.py writes tests/gps-feeds at build time): every sentence of every feed has a valid
 * checksum, the positions lie in the fixture tiles' area, and a feed played through the link gives the fix its name says.
 */
#include "check.hpp"
#include "gps_demo.hpp"
#include "gps_link.hpp"

#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace atlas;

namespace {

/* the fixture's zoom-15 block: x 16590..16592, y 11268..11270 (lat 48.8792 .. 48.8936, lon 2.2644 .. 2.2864 about) */
bool in_fixture_area(double lat, double lon) { return lat > 48.8792 && lat < 48.8939 && lon > 2.2640 && lon < 2.2866; }

} // namespace

void test_gps_demo()
{
    /* no feed: no device */
    {
        DemoGpsPorts none("");
        CHECK(none.list_ports().empty());
        DemoGpsPorts missing("/nonexistent/feed.nmea");
        CHECK(missing.list_ports().empty());
        PortErr err;
        CHECK_EQ(missing.open("/dev/ttyACM0", err), -1);
    }
    /* a small feed with a pause: the first line at once, the second after the wait */
    {
        const std::string dir = test_temp_dir();
        const std::string path = dir + "/f.nmea";
        {
            std::ofstream o(path);
            o << "# a comment\n" << nmea_frame("GNGGA,101530.00,,,,,0,00,99.99,,,,,,") << "\n#wait 200\n"
              << nmea_frame("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1") << "\n";
        }
        DemoGpsPorts demo(path);
        const std::vector<PortInfo> l = demo.list_ports();
        CHECK_EQ(l.size(), static_cast<std::size_t>(1));
        CHECK_EQ(l[0].path, std::string("/dev/ttyACM0"));
        PortErr err;
        const int h = demo.open("/dev/ttyACM0", err);
        CHECK(h >= 0);
        std::uint8_t buf[256];
        std::string got;
        long n;
        while ((n = demo.read(h, buf, sizeof buf)) > 0) got.append(reinterpret_cast<char *>(buf), static_cast<std::size_t>(n));
        CHECK_EQ(got, nmea_frame("GNGGA,101530.00,,,,,0,00,99.99,,,,,,") + "\r\n"); /* only the first yet */
        ::usleep(300000);
        std::string later;
        while ((n = demo.read(h, buf, sizeof buf)) > 0) later.append(reinterpret_cast<char *>(buf), static_cast<std::size_t>(n));
        CHECK_EQ(later, nmea_frame("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1") + "\r\n");
        demo.close(h);
        CHECK(demo.read(h, buf, sizeof buf) < 0);
        test_remove_tree(dir);
    }
    /* the generated feeds (present after build.sh made them): valid checksums, in the fixture's area, the fix they name */
    const std::string feeds = test_app_dir() + "/tests/gps-feeds";
    struct Want {
        const char *name;
        GpsStatus::Kind kind;
        int sats;
        bool lines_only_valid;
    };
    const Want wants[] = {{"searching", GpsStatus::Kind::Searching, -1, true}, {"fix2d", GpsStatus::Kind::Fix2D, -1, true},
                          {"fix3d", GpsStatus::Kind::Fix3D, 9, true},          {"moving", GpsStatus::Kind::Fix3D, 9, true},
                          {"stale", GpsStatus::Kind::Fix3D, 9, true},          {"follow", GpsStatus::Kind::Fix3D, 8, true}};
    for (const Want &w : wants) {
        const std::string file = feeds + "/" + w.name + ".nmea";
        std::ifstream in(file);
        if (!in) { /* build.sh generates them; a bare unit run without them skips this part */
            std::fprintf(stderr, "test_gps_demo: %s missing (run tests/tools/gen_nmea.py), skipped\n", file.c_str());
            continue;
        }
        std::string line;
        int sentences = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            CHECK(nmea_checksum_ok(line));
            const NmeaMsg m = parse_nmea(line);
            CHECK(m.kind != MsgKind::None);
            if (m.has_pos) CHECK(in_fixture_area(m.lat, m.lon));
            ++sentences;
        }
        CHECK(sentences >= 4);
        /* through the link, with the demonstration port: all the feed's first second arrives at once */
        DemoGpsPorts demo(file);
        GpsLink link(demo);
        GpsConfig c;
        c.on = true;
        link.configure(c);
        for (long t = 0; t < 1500; t += 100) link.tick(t);
        CHECK(link.state() == GpsLinkState::Locked);
        const GpsStatus s = link.status(1500);
        CHECK(s.kind == w.kind);
        if (w.sats >= 0) CHECK_EQ(s.fix.sats_used, w.sats);
    }
    /* the noisy feed: garbage and a bad sentence first, the fix after */
    {
        const std::string file = feeds + "/noise.nmea";
        if (std::ifstream(file)) {
            DemoGpsPorts demo(file);
            GpsLink link(demo);
            GpsConfig c;
            c.on = true;
            link.configure(c);
            for (long t = 0; t < 1000; t += 100) link.tick(t);
            CHECK(link.state() == GpsLinkState::Locked);
            CHECK(link.status(1000).kind == GpsStatus::Kind::Fix3D);
        }
    }
}
