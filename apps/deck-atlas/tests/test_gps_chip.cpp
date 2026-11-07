/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the GPS chip's state (core/gps_chip.cpp): the five states (no receiver, receiver and no fix, 2D or
 * weak 3D, good 3D, stale), the thresholds (HDOP 5.0 and 5 satellites used), the receiver-connected judgment (valid
 * sentences within 3 s, driven end to end through the link on a fake port), nothing when GPS is off.
 */
#include "check.hpp"
#include "fake_gps_port.hpp"
#include "gps_chip.hpp"
#include "gps_link.hpp"

using namespace atlas;

namespace {

GpsStatus fix_status(int kind, double hdop, int sats, bool stale = false)
{
    GpsStatus s;
    s.kind = kind == 3 ? GpsStatus::Kind::Fix3D : GpsStatus::Kind::Fix2D;
    s.fix.kind = kind;
    s.fix.hdop = hdop;
    s.fix.sats_used = sats;
    s.stale = stale;
    s.receiver_alive = !stale;
    return s;
}

GpsStatus plain(GpsStatus::Kind k, bool alive = false)
{
    GpsStatus s;
    s.kind = k;
    s.receiver_alive = alive;
    return s;
}

std::string frame_line(const std::string &body) { return nmea_frame(body) + "\r\n"; }

} // namespace

void test_gps_chip()
{
    /* (0) GPS off: nothing is drawn */
    {
        const ChipState c = chip_state(plain(GpsStatus::Kind::Off));
        CHECK(c.level == ChipLevel::Hidden);
        CHECK_EQ(c.bars, 0);
        CHECK(!c.crossed && !c.stale);
        CHECK_EQ(chip_description(c), std::string("GPS off"));
        /* even with an "alive" receiver in the status: off is off */
        CHECK(chip_state(plain(GpsStatus::Kind::Off, true)).level == ChipLevel::Hidden);
    }
    /* (1) no receiver, no access, only unknown ports: the dish crossed out, no bar lit */
    for (GpsStatus::Kind k : {GpsStatus::Kind::NoDevice, GpsStatus::Kind::NoReceiver, GpsStatus::Kind::NoAccess}) {
        const ChipState c = chip_state(plain(k));
        CHECK(c.level == ChipLevel::NoReceiver);
        CHECK_EQ(c.bars, 0);
        CHECK(c.crossed);
        CHECK(!c.stale);
        CHECK_EQ(chip_description(c), std::string("No GPS receiver"));
        CHECK(chip_state(plain(k, true)).level == ChipLevel::NoReceiver); /* whatever "alive" says */
    }
    /* (2) a connected receiver that sends valid sentences and has no fix: one bar */
    {
        const ChipState c = chip_state(plain(GpsStatus::Kind::Searching, true));
        CHECK(c.level == ChipLevel::Searching);
        CHECK_EQ(c.bars, 1);
        CHECK(!c.crossed && !c.stale);
        CHECK_EQ(chip_description(c), std::string("Receiver connected, no fix yet"));
        /* a port that is open but says nothing valid is no receiver: grey, crossed */
        const ChipState quiet = chip_state(plain(GpsStatus::Kind::Searching, false));
        CHECK(quiet.level == ChipLevel::NoReceiver);
        CHECK_EQ(quiet.bars, 0);
        CHECK(quiet.crossed);
    }
    /* (3) a 2D fix: two bars; a 3D fix with weak geometry too */
    {
        const ChipState d2 = chip_state(fix_status(2, 1.0, 9)); /* 2D is weak whatever the numbers say */
        CHECK(d2.level == ChipLevel::Weak);
        CHECK_EQ(d2.bars, 2);
        CHECK(!d2.crossed && !d2.stale);
        CHECK_EQ(chip_description(d2), std::string("Fix with weak geometry"));
        CHECK(chip_state(fix_status(3, 5.01, 9)).level == ChipLevel::Weak);   /* HDOP above 5 */
        CHECK(chip_state(fix_status(3, 12.0, 9)).level == ChipLevel::Weak);
        CHECK(chip_state(fix_status(3, 1.0, 4)).level == ChipLevel::Weak);    /* fewer than 5 satellites used */
        CHECK(chip_state(fix_status(3, 1.0, 0)).level == ChipLevel::Weak);
        CHECK(chip_state(fix_status(3, -1, 9)).level == ChipLevel::Weak);     /* HDOP unknown: not good */
        CHECK(chip_state(fix_status(3, 1.0, -1)).level == ChipLevel::Weak);   /* satellites unknown: not good */
        CHECK_EQ(chip_state(fix_status(3, 5.01, 9)).bars, 2);
    }
    /* (4) a good 3D fix: HDOP <= 5 and >= 5 satellites used, three bars; the thresholds are inclusive */
    {
        const ChipState g = chip_state(fix_status(3, 2.4, 9));
        CHECK(g.level == ChipLevel::Good);
        CHECK_EQ(g.bars, 3);
        CHECK(!g.crossed && !g.stale);
        CHECK_EQ(chip_description(g), std::string("Good 3D fix"));
        CHECK(chip_state(fix_status(3, 5.0, 5)).level == ChipLevel::Good);
        CHECK(chip_state(fix_status(3, 0.0, 5)).level == ChipLevel::Good);
        CHECK(chip_state(fix_status(3, 4.99, 5)).level == ChipLevel::Good);
        CHECK(chip_state(fix_status(3, 5.0, 4)).level == ChipLevel::Weak);
        CHECK(fix_geometry_good(fix_status(3, 5.0, 5).fix));
        CHECK(!fix_geometry_good(fix_status(2, 1.0, 9).fix));
    }
    /* (5) stale: the previous level's bars, dimmed, with the "!" mark */
    {
        const ChipState g = chip_state(fix_status(3, 2.4, 9, true));
        CHECK(g.level == ChipLevel::Good);
        CHECK_EQ(g.bars, 3);
        CHECK(g.stale);
        CHECK_EQ(chip_description(g), std::string("Good 3D fix, signal lost"));
        const ChipState w = chip_state(fix_status(2, 4.0, 4, true));
        CHECK(w.level == ChipLevel::Weak);
        CHECK_EQ(w.bars, 2);
        CHECK(w.stale);
        /* the state differs from the fresh one: a redraw happens */
        CHECK(g != chip_state(fix_status(3, 2.4, 9)));
        CHECK(g == chip_state(fix_status(3, 2.4, 9, true)));
    }

    /* ---- end to end on a fake receiver: cold start -> sentences, no fix -> fix -> silence (stale) -> lost */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        GpsConfig c;
        c.on = true;
        link.configure(c);
        CHECK(chip_state(link.status(0)).level == ChipLevel::NoReceiver);     /* nothing opened yet */
        auto sentence = [&](bool fix, int sats, double hdop, long t) {
            char hhmmss[16];
            std::snprintf(hhmmss, sizeof hhmmss, "1015%02d.00", static_cast<int>((t / 1000) % 60));
            const std::string pos = "4853.37855,N,00216.19971,E";
            if (!fix)
                fake.push(0, frame_line(std::string("GNRMC,") + hhmmss + ",V,,,,,,,150126,,,N,V") +
                                 frame_line(std::string("GNGGA,") + hhmmss + ",,,,,0,00,99.99,,,,,,"));
            else {
                char q[32];
                std::snprintf(q, sizeof q, "%02d,%.2f", sats, hdop);
                fake.push(0, frame_line(std::string("GNRMC,") + hhmmss + ",A," + pos + ",0.10,,150126,,,A,V") +
                                 frame_line(std::string("GNGGA,") + hhmmss + "," + pos + ",1," + q + ",41.5,M,47.3,M,,") +
                                 frame_line("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,1.10,2.64,1"));
            }
        };
        ChipLevel at_3s = ChipLevel::Hidden, at_10s = ChipLevel::Hidden, at_stale = ChipLevel::Hidden;
        bool stale_flag = false;
        for (long t = 0; t <= 40000; t += 100) {
            fake.now = t;
            if (t % 1000 == 0 && t <= 4000) sentence(false, 0, 99.99, t);                 /* a receiver, cold: no fix */
            if (t % 1000 == 0 && t >= 5000 && t <= 15000) sentence(true, 9, 1.1, t);      /* a good 3D fix */
            link.tick(t);
            if (t == 3000) at_3s = chip_state(link.status(t)).level;
            if (t == 10000) at_10s = chip_state(link.status(t)).level;
            if (t == 21000) { /* the fix is 6 s old: stale, the bars dimmed (the receiver went quiet) */
                const ChipState s = chip_state(link.status(t));
                at_stale = s.level;
                stale_flag = s.stale;
            }
        }
        CHECK(at_3s == ChipLevel::Searching);      /* valid sentences, no fix: one red bar */
        CHECK(at_10s == ChipLevel::Good);          /* nine satellites, HDOP 1.1: three green bars */
        CHECK(at_stale == ChipLevel::Good);
        CHECK(stale_flag);
        /* 10 s of silence: the link gives the port up; the chip is the crossed dish again */
        const ChipState end = chip_state(link.status(40000));
        CHECK(end.level == ChipLevel::NoReceiver);
        CHECK(end.crossed);
    }
    /* the 3 s rule on a receiver that falls silent before it had a fix: connected for 3 s, then grey */
    {
        FakeGpsPorts fake;
        fake.add("ttyACM0", 9600);
        GpsLink link(fake);
        GpsConfig c;
        c.on = true;
        link.configure(c);
        const std::string v = frame_line("GNRMC,101530.00,V,,,,,,,150126,,,N,V");
        for (long t = 0; t <= 2000; t += 100) {
            fake.now = t;
            if (t % 500 == 0) fake.push(0, v);
            link.tick(t);
        }
        CHECK(chip_state(link.status(2000)).level == ChipLevel::Searching);
        CHECK(chip_state(link.status(2000 + 2900)).level == ChipLevel::Searching);
        CHECK(chip_state(link.status(2000 + 3100)).level == ChipLevel::NoReceiver);
    }
}
