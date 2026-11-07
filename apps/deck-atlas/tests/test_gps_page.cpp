/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the GPS status page's model (core/gps_page.cpp): the texts (hemisphere letters, 6 decimals, dashes,
 * speed units, compass words, accuracy, the card contents per state), the sky plot's geometry (N up, ring radii, label
 * room), the signal bars' layout (heights, row wrapping at every canvas width, label fit), the constellation colours, how
 * the page is opened and left (chip, S key, Settings row, Esc) and the live-update gate.
 */
#include "check.hpp"
#include "gps_page.hpp"

#include <cmath>
#include <set>

using namespace atlas;

namespace {

SkySat sat(Constellation c, int prn, int el, int az, int snr, bool used)
{
    SkySat s;
    s.sys = c;
    s.prn = prn;
    s.elev = el;
    s.azim = az;
    s.snr = snr;
    s.used = used;
    return s;
}

GpsStatus fix3d()
{
    GpsStatus s;
    s.kind = GpsStatus::Kind::Fix3D;
    s.receiver_alive = true;
    s.fix.kind = 3;
    s.fix.lat = -33.856789;
    s.fix.lon = 151.2153;
    s.fix.has_alt = true;
    s.fix.alt_m = 41.54;
    s.fix.speed_mps = 3.4;
    s.fix.course_deg = 50;
    s.fix.hdop = 0.9;
    s.fix.sats_used = 13;
    s.fix.utc = "2026-01-15T10:15:30Z";
    s.fix.fix_ms = 9000;
    s.sats = {sat(Constellation::Gps, 2, 62, 37, 41, true), sat(Constellation::Gps, 5, 48, 78, 38, true),
              sat(Constellation::Glonass, 66, 58, 20, 39, true), sat(Constellation::Sbas, 135, 25, 190, -1, false)};
    s.dop.hdop = 0.9;
    s.dop.vdop = 1.1;
    s.dop.pdop = 1.4;
    s.link.port = "/dev/ttyACM0";
    s.link.device = "u-blox 7";
    s.link.vid = 0x1546;
    s.link.pid = 0x01a7;
    s.link.baud = 9600;
    s.link.choice = PortChoice::AutoKnown;
    s.link.sentences_per_s = 9.0;
    s.link.last_valid_ms = 9500;
    s.link.bad_checksums = 2;
    return s;
}

} // namespace

void test_gps_page()
{
    const std::string dash = kUnknownText;

    /* ---- the texts */
    CHECK_EQ(lat_text(48.889643), "48.889643 N");
    CHECK_EQ(lat_text(-33.856789), "33.856789 S");
    CHECK_EQ(lon_text(2.269995), "2.269995 E");
    CHECK_EQ(lon_text(-70.5), "70.500000 W");
    CHECK_EQ(lat_text(0), "0.000000 N");
    CHECK_EQ(lon_text(0), "0.000000 E");
    CHECK_EQ(lat_text(std::nan("")), dash);
    CHECK_EQ(lat_text(1.0000004), "1.000000 N");   /* six decimals, rounded */
    Fix f;
    CHECK_EQ(altitude_text(f), dash);
    f.has_alt = true;
    f.alt_m = 41.54;
    CHECK_EQ(altitude_text(f), "41.5 m");
    f.alt_m = -3.04;
    CHECK_EQ(altitude_text(f), "-3.0 m");
    f.hdop = 0.9;
    CHECK_EQ(accuracy_text(f), "+-5 m");      /* 0.9 x 5 = 4.5, at least 3 m */
    f.hdop = 0.3;
    CHECK_EQ(accuracy_text(f), "+-3 m");
    f.hdop = 1e9;
    CHECK_EQ(accuracy_text(f), "+-100 m");
    CHECK_EQ(speed_text(-1, false), dash);
    CHECK_EQ(speed_text(0, false), "0.0 km/h (0.0 m/s)");
    CHECK_EQ(speed_text(3.4, false), "12 km/h (3.4 m/s)");
    CHECK_EQ(speed_text(3.4, true), "7.6 mph (3.4 m/s)");
    CHECK_EQ(speed_text(1.0, false), "3.6 km/h (1.0 m/s)");
    CHECK_EQ(speed_text(30.0, true), "67 mph (30.0 m/s)");
    CHECK_EQ(std::string(compass_word(0)), "N");
    CHECK_EQ(std::string(compass_word(22.4)), "N");
    CHECK_EQ(std::string(compass_word(22.5)), "NE");
    CHECK_EQ(std::string(compass_word(50)), "NE");
    CHECK_EQ(std::string(compass_word(90)), "E");
    CHECK_EQ(std::string(compass_word(135)), "SE");
    CHECK_EQ(std::string(compass_word(180)), "S");
    CHECK_EQ(std::string(compass_word(225)), "SW");
    CHECK_EQ(std::string(compass_word(270)), "W");
    CHECK_EQ(std::string(compass_word(315)), "NW");
    CHECK_EQ(std::string(compass_word(337.5)), "N");
    CHECK_EQ(std::string(compass_word(359.9)), "N");
    CHECK_EQ(std::string(compass_word(-10)), "N");
    CHECK_EQ(course_text(50), "50° NE");
    CHECK_EQ(course_text(70), "70° E");
    CHECK_EQ(course_text(359.6), "0\xC2\xB0 N");
    CHECK_EQ(course_text(-1), dash);
    CHECK_EQ(utc_date_text("2026-01-15T10:15:30Z"), "2026-01-15");
    CHECK_EQ(utc_time_text("2026-01-15T10:15:30Z"), "10:15:30");
    CHECK_EQ(utc_time_text("2026-01-15"), dash);
    CHECK_EQ(utc_date_text(""), dash);
    CHECK_EQ(dop_text(0.94), "0.9");
    CHECK_EQ(dop_text(-1), dash);
    CHECK_EQ(dop_text(99.99), dash);   /* a receiver without a fix sends 99.99 */
    CHECK_EQ(std::string(fix_mode_text(1)), "none");
    CHECK_EQ(std::string(fix_mode_text(2)), "2D");
    CHECK_EQ(std::string(fix_mode_text(3)), "3D");
    CHECK_EQ(std::string(fix_mode_text(0)), dash);
    CHECK_EQ(age_text(0, 5000), "no data yet");
    CHECK_EQ(age_text(1000, 3400), "2 s ago");
    CHECK_EQ(age_text(1000, 1000), "0 s ago");
    CHECK_EQ(age_text(1000, 200000), "3 min ago");
    CHECK_EQ(age_text(5000, 1000), "0 s ago");   /* a clock that stepped back never shows a negative age */

    /* ---- the cards, state by state */
    {
        const GpsPageText t = gps_page_text(fix3d(), false, 10000);
        CHECK(!t.off);
        CHECK_EQ(t.headline, "3D fix");
        CHECK_EQ(t.detail, "Good 3D fix");
        CHECK(t.chip.level == ChipLevel::Good);
        CHECK_EQ(t.device, "u-blox 7");
        CHECK_EQ(t.port, "ttyACM0");
        CHECK_EQ(t.speed, "9600 baud");
        CHECK_EQ(t.rate, "9.0 per s");
        CHECK_EQ(t.last_data, "0 s ago");
        CHECK_EQ(t.bad_checksums, "2");
        CHECK_EQ(t.chosen, "Auto (known receiver)");
        CHECK_EQ(t.lat, "33.856789 S");
        CHECK_EQ(t.lon, "151.215300 E");
        CHECK_EQ(t.altitude, "41.5 m");
        CHECK_EQ(t.accuracy, "+-5 m");
        CHECK_EQ(t.speed_over_ground, "12 km/h (3.4 m/s)");
        CHECK_EQ(t.course, "50\xC2\xB0 NE");
        CHECK_EQ(t.utc_date, "2026-01-15");
        CHECK_EQ(t.utc_time, "10:15:30");
        CHECK_EQ(t.hdop, "0.9");
        CHECK_EQ(t.vdop, "1.1");
        CHECK_EQ(t.pdop, "1.4");
        CHECK_EQ(t.mode, "3D");
        CHECK_EQ(t.used, "13");
        CHECK_EQ(t.in_view, "4");
        CHECK_EQ(t.in_fix, "3");
        CHECK_EQ(t.per_system, "G 2  R 1  S 1");
        CHECK_EQ(gps_row_value(fix3d()), "3D fix");
        const GpsPageText mi = gps_page_text(fix3d(), true, 10000);
        CHECK_EQ(mi.speed_over_ground, "7.6 mph (3.4 m/s)");
        CHECK(mi != t);
        CHECK(gps_page_text(fix3d(), false, 10000) == t);
        /* a baud found by listening, a port chosen by hand, a port without a product string */
        GpsStatus a = fix3d();
        a.link.baud_auto = true;
        a.link.choice = PortChoice::ByUser;
        a.link.device.clear();
        CHECK_EQ(gps_page_text(a, false, 10000).speed, "9600 baud (auto)");
        CHECK_EQ(gps_page_text(a, false, 10000).chosen, "Chosen by hand");
        CHECK_EQ(gps_page_text(a, false, 10000).device, "USB 1546:01a7");
        a.link.vid = 0;
        CHECK_EQ(gps_page_text(a, false, 10000).device, "serial port");
    }
    {
        /* a 2D fix: no altitude, no course at a standstill */
        GpsStatus s = fix3d();
        s.kind = GpsStatus::Kind::Fix2D;
        s.fix.kind = 2;
        s.fix.has_alt = false;
        s.fix.course_deg = -1;
        s.fix.speed_mps = 0.1;
        s.fix.hdop = 4.0;
        const GpsPageText t = gps_page_text(s, false, 10000);
        CHECK_EQ(t.headline, "2D fix");
        CHECK_EQ(t.altitude, dash);
        CHECK_EQ(t.course, dash);
        CHECK_EQ(t.mode, "2D");
        CHECK_EQ(t.accuracy, "+-20 m");
        CHECK_EQ(t.detail, "Fix with weak geometry");
    }
    {
        /* stale: the last values stay, the headline keeps the mode, the detail says how long ago */
        GpsStatus s = fix3d();
        s.stale = true;
        s.age_ms = 12000;
        s.receiver_alive = false;
        const GpsPageText t = gps_page_text(s, false, 21000);
        CHECK_EQ(t.headline, "3D fix");
        CHECK_EQ(t.detail, "Good 3D fix, signal lost, last fix 12 s ago");
        CHECK_EQ(t.lat, "33.856789 S");
        CHECK(t.chip.stale);
        CHECK_EQ(gps_row_value(s), "Signal lost");
    }
    {
        /* connected, no fix, three satellites in view */
        GpsStatus s;
        s.kind = GpsStatus::Kind::Searching;
        s.receiver_alive = true;
        s.link.port = "/dev/ttyACM0";
        s.link.baud = 9600;
        s.link.last_valid_ms = 9000;
        s.sats = {sat(Constellation::Gps, 2, 30, 40, 20, false), sat(Constellation::Gps, 5, 50, 80, 15, false),
                  sat(Constellation::Glonass, 66, 20, 120, -1, false)};
        const GpsPageText t = gps_page_text(s, false, 10000);
        CHECK_EQ(t.headline, "Searching for satellites");
        CHECK_EQ(t.detail, "Receiver connected, no fix yet");
        CHECK_EQ(t.lat, dash);
        CHECK_EQ(t.lon, dash);
        CHECK_EQ(t.speed_over_ground, dash);
        CHECK_EQ(t.utc_date, dash);
        CHECK_EQ(t.utc_time, dash);
        CHECK_EQ(t.accuracy, dash);
        CHECK_EQ(t.hdop, dash);
        CHECK_EQ(t.mode, "none");
        CHECK_EQ(t.used, "0");
        CHECK_EQ(t.in_view, "3");
        CHECK_EQ(t.in_fix, "0");
        CHECK_EQ(t.per_system, "G 2  R 1");
        CHECK_EQ(t.last_data, "1 s ago");
        CHECK_EQ(gps_row_value(s), "Searching");
        /* the receiver reports 99.99 before a fix */
        s.dop.hdop = 99.99;
        CHECK_EQ(gps_page_text(s, false, 10000).hdop, dash);
        /* a port that is open and silent */
        s.receiver_alive = false;
        s.sats.clear();
        const GpsPageText q = gps_page_text(s, false, 10000);
        CHECK_EQ(q.headline, "Receiver connected, no data");
        CHECK_EQ(q.in_view, dash);
        CHECK_EQ(gps_row_value(s), "No data");
    }
    {
        GpsStatus s;
        s.kind = GpsStatus::Kind::NoDevice;
        GpsPageText t = gps_page_text(s, false, 1000);
        CHECK_EQ(t.headline, "No GPS receiver found");
        CHECK_EQ(t.port, dash);
        CHECK_EQ(t.device, dash);
        CHECK_EQ(t.rate, dash);
        CHECK_EQ(t.in_view, dash);
        CHECK(t.chip.crossed);
        CHECK_EQ(gps_row_value(s), "No receiver");
        s.kind = GpsStatus::Kind::NoReceiver;
        CHECK_EQ(gps_page_text(s, false, 1000).headline, "No GPS receiver found");
        CHECK(gps_page_text(s, false, 1000).detail.find("Pick its port") != std::string::npos);
        s.kind = GpsStatus::Kind::NoAccess;
        CHECK_EQ(gps_page_text(s, false, 1000).headline, "No access to the port (dialout group?)");
    }
    {
        /* GPS off: only the sentence, nothing else is filled */
        const GpsPageText t = gps_page_text(GpsStatus(), false, 1000);
        CHECK(t.off);
        CHECK_EQ(t.headline, "GPS is off. Switch it on in Settings > GPS receiver.");
        CHECK(t.chip.level == ChipLevel::Hidden);
        CHECK(t.lat.empty() && t.device.empty() && t.in_view.empty());
        CHECK_EQ(gps_row_value(GpsStatus()), "Off");
    }
    CHECK_EQ(per_system_text({}), "");

    /* ---- constellation colours: six distinct, none the chip's colours */
    {
        std::set<std::uint32_t> seen;
        for (Constellation c : {Constellation::Gps, Constellation::Glonass, Constellation::Galileo, Constellation::Beidou,
                                Constellation::Qzss, Constellation::Sbas})
            seen.insert(constellation_rgb(c));
        CHECK_EQ(seen.size(), 6u);
        for (std::uint32_t chip : {0xFF4D4Du, 0xFF9A1Fu, 0x3FD166u}) CHECK(!seen.count(chip));
        CHECK(!seen.count(constellation_rgb(Constellation::Unknown)));
    }

    /* ---- the sky plot's geometry */
    {
        const SkyGeom g = sky_geom(300, 280, 20);
        CHECK_NEAR(g.cx, 150, 1e-9);
        CHECK_NEAR(g.cy, 140, 1e-9);
        CHECK_NEAR(g.r, 120, 1e-9);   /* the smaller side bounds the circle */
        CHECK_NEAR(sky_ring_radius(g, 0), 120, 1e-9);
        CHECK_NEAR(sky_ring_radius(g, 30), 80, 1e-9);
        CHECK_NEAR(sky_ring_radius(g, 60), 40, 1e-9);
        CHECK_NEAR(sky_ring_radius(g, 90), 0, 1e-9);
        CHECK_NEAR(sky_ring_radius(g, 120), 0, 1e-9);   /* clamped */
        SkyPoint p = sky_point(g, 0, 0);                /* horizon, north: the top */
        CHECK_NEAR(p.x, 150, 1e-9);
        CHECK_NEAR(p.y, 20, 1e-9);
        p = sky_point(g, 0, 90);                        /* east: the right */
        CHECK_NEAR(p.x, 270, 1e-9);
        CHECK_NEAR(p.y, 140, 1e-9);
        p = sky_point(g, 0, 180);                       /* south: the bottom */
        CHECK_NEAR(p.x, 150, 1e-9);
        CHECK_NEAR(p.y, 260, 1e-9);
        p = sky_point(g, 0, 270);                       /* west: the left */
        CHECK_NEAR(p.x, 30, 1e-9);
        p = sky_point(g, 90, 123);                      /* zenith: the centre whatever the azimuth */
        CHECK_NEAR(p.x, 150, 1e-9);
        CHECK_NEAR(p.y, 140, 1e-9);
        p = sky_point(g, 60, 90);                       /* 60 degrees up, east: a third of the radius out */
        CHECK_NEAR(p.x, 190, 1e-9);
        CHECK_NEAR(p.y, 140, 1e-9);
        p = sky_point(g, 45, 225);                      /* south-west: left and below the centre */
        CHECK(p.x < 150 && p.y > 140);
        CHECK_NEAR(150 - p.x, p.y - 140, 1e-9);
        CHECK(sky_geom(10, 10, 20).r >= 1.0);           /* a box too small never gives a negative radius */
        CHECK(sky_placeable(sat(Constellation::Gps, 1, 0, 0, 10, false)));
        CHECK(sky_placeable(sat(Constellation::Gps, 1, 90, 360, 10, false)));
        CHECK(!sky_placeable(sat(Constellation::Gps, 1, -1, 10, 10, false)));
        CHECK(!sky_placeable(sat(Constellation::Gps, 1, 10, -1, 10, false)));
    }
    {
        /* PRN labels: one beside a lone dot, none for two dots on top of each other (the second), none past the box edge */
        const SkyGeom g = sky_geom(200, 200, 20);
        std::vector<SkySat> v = {sat(Constellation::Gps, 2, 45, 270, 30, true), sat(Constellation::Gps, 3, 45, 90, 30, true)};
        std::vector<bool> fit = sky_labels_fit(v, g, 8, 24, 14, 200, 200);
        CHECK(fit.size() == 2 && fit[0] && fit[1]);
        v.push_back(sat(Constellation::Gps, 4, 45, 270, 30, true));   /* the same place as the first */
        fit = sky_labels_fit(v, g, 8, 24, 14, 200, 200);
        CHECK(fit[0] && !fit[2]);
        v = {sat(Constellation::Gps, 9, 0, 90, 30, true)};            /* at the right edge: no room beside the dot */
        fit = sky_labels_fit(v, g, 8, 24, 14, 200, 200);
        CHECK(!fit[0]);
        v = {sat(Constellation::Gps, 9, -1, -1, 30, true)};           /* unplaceable */
        CHECK(!sky_labels_fit(v, g, 8, 24, 14, 200, 200)[0]);
        /* a dot just right of another one covers that one's label */
        v = {sat(Constellation::Gps, 1, 45, 270, 30, true), sat(Constellation::Gps, 2, 67, 270, 30, true)};
        fit = sky_labels_fit(v, g, 8, 24, 14, 200, 200);
        CHECK(!fit[0]);
    }

    /* ---- the bars */
    CHECK_EQ(bar_height(-1, 100), 0);
    CHECK_EQ(bar_height(0, 100), 0);
    CHECK_EQ(bar_height(25, 100), 50);
    CHECK_EQ(bar_height(50, 100), 100);
    CHECK_EQ(bar_height(70, 100), 100);   /* the scale ends at 50 */
    CHECK_EQ(bar_height(1, 20), 1);       /* a tracked satellite always shows */
    CHECK_EQ(bar_height(10, 0), 0);
    CHECK_EQ(bar_layout(0, 300, 20, 24).rows, 0);
    for (int avail : {270, 300, 460, 616}) {
        for (int n : {1, 3, 9, 16, 17, 20, 24, 32, 40}) {
            const BarLayout L = bar_layout(n, avail, 20, 24);
            CHECK_EQ(static_cast<int>(L.cells.size()), n);
            CHECK(L.rows >= 1 && L.per_row >= 1 && L.rows * L.per_row >= n);
            CHECK(L.cell_w >= 26 && L.cell_w <= 72);   /* the PRN label (24 px) fits, a lone bar is not enormous */
            bool inside = true, ordered = true;
            for (std::size_t i = 0; i < L.cells.size(); ++i) {
                const BarCell &c = L.cells[i];
                inside = inside && c.x >= L.axis_w && c.x + c.w <= avail && c.bar_x >= c.x && c.bar_x + c.bar_w <= c.x + c.w &&
                         c.bar_w >= 2 && c.w >= 24;
                if (i) ordered = ordered && (c.row > L.cells[i - 1].row || (c.row == L.cells[i - 1].row && c.x > L.cells[i - 1].x));
            }
            CHECK(inside);
            CHECK(ordered);
        }
    }
    {
        /* 20 satellites on a 320 px wide canvas (about 270 px of plot after the margins): three rows of seven; 9 fit one row;
         * on 640 px (about 560 px) even 20 are one row */
        const BarLayout L = bar_layout(20, 270, 20, 24);
        CHECK_EQ(L.rows, 3);
        CHECK_EQ(L.per_row, 7);
        CHECK_EQ(L.cells[7].row, 1);
        CHECK_EQ(L.cells[7].col, 0);
        CHECK_EQ(bar_layout(9, 270, 20, 24).rows, 1);
        CHECK_EQ(bar_layout(16, 270, 20, 24).rows, 2);
        CHECK_EQ(bar_layout(20, 560, 20, 24).rows, 1);
        /* 40 satellites wrap into several equal rows, the last one may be shorter */
        const BarLayout W = bar_layout(40, 270, 20, 24);
        CHECK(W.rows >= 4);
        CHECK(W.per_row * W.rows >= 40 && W.per_row * (W.rows - 1) < 40);
        /* a plot narrower than one cell still gives one column and does not divide by zero */
        const BarLayout T = bar_layout(5, 10, 20, 24);
        CHECK_EQ(T.per_row, 1);
        CHECK_EQ(T.rows, 5);
    }

    /* ---- opening and leaving */
    {
        StatusNav nav;
        CHECK(!nav.is_open());
        CHECK(nav.leave() == StatusOrigin::None);
        CHECK(!nav.open_from(StatusOrigin::Map, false));        /* no chip and no S key with GPS off */
        CHECK(!nav.is_open());
        CHECK(nav.open_from(StatusOrigin::Map, true));          /* the chip's tap, the S key */
        CHECK(nav.is_open());
        CHECK(!nav.open_from(StatusOrigin::Settings, true));    /* a second request while open is ignored */
        CHECK(nav.leave() == StatusOrigin::Map);                /* Esc: back to the map */
        CHECK(!nav.is_open());
        CHECK(nav.open_from(StatusOrigin::Settings, false));    /* the Settings row works with GPS off */
        CHECK(nav.leave() == StatusOrigin::Settings);           /* Esc: back to Settings */
        CHECK(nav.leave() == StatusOrigin::None);
        CHECK(!nav.open_from(StatusOrigin::None, true));
    }

    /* ---- the live gate */
    {
        GpsPageLive live;
        GpsStatus s = fix3d();
        GpsPageText t = gps_page_text(s, false, 10000);
        GpsPageChange c = live.update(t, s.sats);
        CHECK(c.text && c.sky && c.bars);                       /* the first look draws everything */
        c = live.update(gps_page_text(s, false, 10000), s.sats);
        CHECK(!c.any());                                        /* nothing new: nothing drawn */
        c = live.update(gps_page_text(s, false, 11000), s.sats);
        CHECK(c.text && !c.sky && !c.bars);                     /* "last data" moved: the texts only */
        s.sats[0].snr = 30;
        c = live.update(gps_page_text(s, false, 11000), s.sats);
        CHECK(!c.text && !c.sky && c.bars);                     /* a signal changed: the bars only */
        s.sats[1].elev += 1;
        c = live.update(gps_page_text(s, false, 11000), s.sats);
        CHECK(!c.text && c.sky && !c.bars);                     /* a satellite moved: the sky only */
        s.sats[2].used = false;
        c = live.update(gps_page_text(s, false, 11000), s.sats);
        CHECK(c.sky && c.bars);                                 /* used in the fix: both */
        s.sats.pop_back();
        c = live.update(gps_page_text(s, false, 11000), s.sats);
        CHECK(c.sky && c.bars);                                 /* a satellite left */
        live.reset();
        CHECK(live.update(gps_page_text(s, false, 11000), s.sats).any());
    }
}
