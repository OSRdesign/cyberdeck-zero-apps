/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the fix-state logic (core/gps_fix.cpp): the tracker folding sentences into a fix (2D / 3D, void,
 * satellites, speed and course, UTC), the stale rule (5 s), "searching", the status chip's text and the accuracy
 * estimate. Every position is invented.
 */
#include "check.hpp"
#include "gps_fix.hpp"

using namespace atlas;

namespace {

NmeaMsg msg(const std::string &body) { return parse_nmea(nmea_frame(body)); }

const char *kPos = "4853.37855,N,00216.19971,E";

/* One second of a 3D fix at time t_ms (the tracker's clock). */
void epoch3d(FixTracker &t, std::int64_t at, int sats = 9, double hdop = 1.1, const char *speed = "0.10", const char *course = "")
{
    t.on_msg(msg(std::string("GNRMC,101530.00,A,") + kPos + "," + speed + "," + course + ",150126,,,A,V"), at);
    t.on_msg(msg(std::string("GNGGA,101530.00,") + kPos + ",1," + (sats < 10 ? "0" : "") + std::to_string(sats) + "," +
                 std::to_string(hdop).substr(0, 4) + ",41.5,M,47.3,M,,"),
             at);
    t.on_msg(msg("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,1.10,2.64,1"), at);
    t.on_msg(msg("GPGSV,1,1,07,02,20,030,28"), at);
    t.on_msg(msg("GLGSV,1,1,05,70,20,030,28"), at);
}

} // namespace

void test_gps_fix()
{
    /* nothing yet: no fix, searching once the link is up */
    {
        FixTracker t;
        CHECK_EQ(t.fix().kind, 0);
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 1000);
        CHECK(s.kind == GpsStatus::Kind::Searching);
        CHECK(!s.has_fix());
        CHECK_EQ(chip_text(s, false), std::string("Searching"));
    }
    /* a 3D fix: position, altitude, nine satellites, hdop, the sky's 12 in view, time and date */
    {
        FixTracker t;
        epoch3d(t, 5000);
        const Fix &f = t.fix();
        CHECK_EQ(f.kind, 3);
        CHECK_NEAR(f.lat, 48.0 + 53.37855 / 60.0, 1e-9);
        CHECK_NEAR(f.lon, 2.0 + 16.19971 / 60.0, 1e-9);
        CHECK(f.has_alt);
        CHECK_NEAR(f.alt_m, 41.5, 1e-9);
        CHECK_EQ(f.sats_used, 9);
        CHECK_EQ(f.sats_view, 12);
        CHECK_NEAR(f.hdop, 1.1, 1e-9);
        CHECK_EQ(f.utc, std::string("2026-01-15T10:15:30Z"));
        CHECK_EQ(static_cast<long>(f.fix_ms), 5000L);
        CHECK_NEAR(f.speed_mps, 0.10 * 0.514444, 1e-6);
        CHECK(f.course_deg < 0); /* unknown at a standstill */
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 5200);
        CHECK(s.kind == GpsStatus::Kind::Fix3D);
        CHECK(!s.stale);
        CHECK_EQ(chip_text(s, false), std::string("3D fix 9 sats"));
    }
    /* 2D: the receiver says so (GSA 2, no altitude) */
    {
        FixTracker t;
        t.on_msg(msg(std::string("GNRMC,101530.00,A,") + kPos + ",0.0,,150126,,,A,V"), 100);
        t.on_msg(msg(std::string("GNGGA,101530.00,") + kPos + ",1,04,4.00,,M,,M,,"), 100);
        t.on_msg(msg("GNGSA,A,2,02,03,04,05,,,,,,,,,5.00,4.00,3.00,1"), 100);
        CHECK_EQ(t.fix().kind, 2);
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 300);
        CHECK(s.kind == GpsStatus::Kind::Fix2D);
        CHECK_EQ(chip_text(s, false), std::string("2D fix"));
        /* without any GSA: an altitude means 3D, none means 2D */
        FixTracker u;
        u.on_msg(msg(std::string("GNGGA,101530.00,") + kPos + ",1,04,4.00,,M,,M,,"), 100);
        CHECK_EQ(u.fix().kind, 2);
        u.on_msg(msg(std::string("GNGGA,101531.00,") + kPos + ",1,06,2.00,40.0,M,,M,,"), 1100);
        CHECK_EQ(u.fix().kind, 3);
        /* a GSA counts for 3 s only: afterwards the altitude decides */
        FixTracker w;
        w.on_msg(msg("GNGSA,A,2,02,03,04,05,,,,,,,,,5.00,4.00,3.00,1"), 0);
        w.on_msg(msg(std::string("GNGGA,101530.00,") + kPos + ",1,04,4.00,,M,,M,,"), 100);
        CHECK_EQ(w.fix().kind, 2);
        w.on_msg(msg(std::string("GNGGA,101535.00,") + kPos + ",1,06,2.00,40.0,M,,M,,"), 5000);
        CHECK_EQ(w.fix().kind, 3);
        /* two constellations: the better mode wins (GPS 3D, GLONASS empty 1) */
        FixTracker x;
        x.on_msg(msg("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,1.10,2.64,1"), 0);
        x.on_msg(msg("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,2"), 0);
        x.on_msg(msg(std::string("GNRMC,101530.00,A,") + kPos + ",0.0,,150126,,,A,V"), 0);
        CHECK_EQ(x.fix().kind, 3);
    }
    /* a void fix: searching, and the old position is no longer a fix */
    {
        FixTracker t;
        epoch3d(t, 1000);
        CHECK_EQ(t.fix().kind, 3);
        /* an invalid RMC in the same second as a valid GGA is not a loss */
        t.on_msg(msg("GNRMC,101531.00,V,,,,,,,150126,,,N,V"), 1100);
        CHECK_EQ(t.fix().kind, 3);
        /* the receiver loses the fix: RMC V and GGA 0, 3 s later */
        t.on_msg(msg("GNRMC,101534.00,V,,,,,,,150126,,,N,V"), 4000);
        CHECK_EQ(t.fix().kind, 0);
        t.on_msg(msg("GNGGA,101534.00,,,,,0,02,99.99,,,,,,"), 4000);
        CHECK_EQ(t.fix().kind, 0);
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 4100);
        CHECK(s.kind == GpsStatus::Kind::Searching);
        CHECK_EQ(chip_text(s, false), std::string("Searching"));
        CHECK(t.fix().speed_mps < 0);
        /* the fix comes back */
        epoch3d(t, 9000);
        CHECK_EQ(t.fix().kind, 3);
    }
    /* stale: no valid fix sentence for more than 5 s */
    {
        FixTracker t;
        epoch3d(t, 10000);
        GpsStatus s = make_status(GpsLinkState::Locked, t, 14900);
        CHECK(!s.stale);
        CHECK_EQ(s.age_ms, 4900L);
        s = make_status(GpsLinkState::Locked, t, 15000);
        CHECK(!s.stale); /* exactly 5 s is not yet stale */
        s = make_status(GpsLinkState::Locked, t, 15001);
        CHECK(s.stale);
        CHECK(s.kind == GpsStatus::Kind::Fix3D); /* the dot stays, hollow */
        CHECK_EQ(chip_text(s, false), std::string("Last fix 5 s ago"));
        s = make_status(GpsLinkState::Locked, t, 10000 + 42000);
        CHECK_EQ(chip_text(s, false), std::string("Last fix 42 s ago"));
        s = make_status(GpsLinkState::Locked, t, 10000 + 3 * 60000 + 5000);
        CHECK_EQ(chip_text(s, false), std::string("Last fix 3 min ago"));
        /* a new fix makes it fresh again */
        epoch3d(t, 60000);
        s = make_status(GpsLinkState::Locked, t, 60500);
        CHECK(!s.stale);
        /* a valid sentence of any kind that is not a fix does not refresh it: GSV and invalid GGA alone */
        FixTracker u;
        epoch3d(u, 0);
        u.on_msg(msg("GPGSV,1,1,07,02,20,030,28"), 4000);
        u.on_msg(msg("GNGGA,101534.00,,,,,0,02,99.99,,,,,,"), 8000);
        CHECK(make_status(GpsLinkState::Locked, u, 9000).kind == GpsStatus::Kind::Searching);
    }
    /* the link states without a fix */
    {
        FixTracker t;
        epoch3d(t, 0);
        CHECK(make_status(GpsLinkState::Off, t, 0).kind == GpsStatus::Kind::Off);
        CHECK_EQ(chip_text(make_status(GpsLinkState::Off, t, 0), false), std::string(""));
        const GpsStatus nd = make_status(GpsLinkState::NoDevice, t, 100);
        CHECK(nd.kind == GpsStatus::Kind::NoDevice);
        CHECK(!nd.has_fix()); /* a lost receiver: no dot, whatever the tracker remembers */
        CHECK_EQ(chip_text(nd, false), std::string("No GPS device"));
        CHECK_EQ(chip_text(make_status(GpsLinkState::NoAccess, t, 100), false), std::string("No access to GPS port"));
        const GpsStatus nr = make_status(GpsLinkState::NoReceiver, t, 100);
        CHECK(nr.kind == GpsStatus::Kind::NoReceiver);
        CHECK(!nr.has_fix());
        CHECK_EQ(chip_text(nr, false), std::string("No GPS receiver found"));
        CHECK_EQ(chip_text(make_status(GpsLinkState::Searching, t, 100), false), std::string("Searching"));
    }
    /* the chip: a moving fix shows its speed; 1.0 m/s and below does not; miles */
    {
        FixTracker t;
        epoch3d(t, 0, 9, 1.1, "10.10", "70.0"); /* 10.1 kn = 5.196 m/s = 18.7 km/h */
        CHECK_NEAR(t.fix().speed_mps, 5.196, 0.01);
        CHECK_NEAR(t.fix().course_deg, 70.0, 1e-9);
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 100);
        CHECK_EQ(chip_text(s, false), std::string("3D fix 9 sats, 19 km/h"));
        CHECK_EQ(chip_text(s, true), std::string("3D fix 9 sats, 12 mph"));
        FixTracker slow;
        epoch3d(slow, 0, 9, 1.1, "1.90", "70.0"); /* 1.9 kn = 0.98 m/s */
        CHECK_EQ(chip_text(make_status(GpsLinkState::Locked, slow, 100), false), std::string("3D fix 9 sats"));
        FixTracker one;
        epoch3d(one, 0, 1);
        CHECK_EQ(chip_text(make_status(GpsLinkState::Locked, one, 100), false), std::string("3D fix 1 sat"));
    }
    /* the accuracy estimate: HDOP x 5 m, 3..100 m, 10 m unknown */
    CHECK_NEAR(accuracy_from_hdop(1.1), 5.5, 1e-9);
    CHECK_NEAR(accuracy_from_hdop(0.5), 3.0, 1e-9);
    CHECK_NEAR(accuracy_from_hdop(40.0), 100.0, 1e-9);
    CHECK_NEAR(accuracy_from_hdop(-1.0), 10.0, 1e-9);
    CHECK_NEAR(accuracy_from_hdop(0.0), 10.0, 1e-9);
    /* reset forgets everything */
    {
        FixTracker t;
        epoch3d(t, 0);
        t.reset();
        CHECK_EQ(t.fix().kind, 0);
        CHECK_EQ(t.fix().sats_used, -1);
        CHECK(t.fix().utc.empty());
    }
}
