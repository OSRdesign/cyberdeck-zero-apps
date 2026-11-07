/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: GSV and GSA (core/nmea.cpp) and the satellite table they feed (core/gps_sky.cpp): the sentences'
 * fields (up to four satellites, empty SNR = not tracked, the NMEA 4.1 signal id, the system id), groups of several
 * messages (complete when the number equals the total, a lost message drops the group), aging (10 s), the used flags
 * from the GSA reports per talker, the DOPs, the constellation of a talker or a PRN (SBAS ranges under GP), the link
 * facts (rate, bad checksums, last valid sentence, port, speed, device, how it was chosen). Every sentence is invented.
 */
#include "check.hpp"
#include "fake_gps_port.hpp"
#include "gps_link.hpp"
#include "gps_sky.hpp"
#include "nmea.hpp"

using namespace atlas;

namespace {

NmeaMsg msg(const std::string &body) { return parse_nmea(nmea_frame(body)); }

/* a GSA sentence: twelve PRN fields, then "pdop,hdop,vdop" ("" = three empty fields), then the system id when sys > 0 */
std::string gsa(const char *talker, int mode, const std::vector<int> &used, const std::string &dops, int sys)
{
    std::string s = std::string(talker) + "GSA,A," + std::to_string(mode);
    for (std::size_t i = 0; i < 12; ++i) s += "," + (i < used.size() ? std::to_string(used[i]) : std::string());
    s += "," + (dops.empty() ? std::string(",,") : dops);
    if (sys > 0) s += "," + std::to_string(sys);
    return s;
}

const SkySat *find(const std::vector<SkySat> &v, Constellation c, int prn)
{
    for (const SkySat &s : v)
        if (s.sys == c && s.prn == prn) return &s;
    return nullptr;
}

int count(const std::vector<SkySat> &v, Constellation c)
{
    int n = 0;
    for (const SkySat &s : v) n += s.sys == c;
    return n;
}

} // namespace

void test_gps_sky()
{
    /* ---- GSV fields */
    {
        const NmeaMsg m = msg("GPGSV,3,1,11,02,62,037,41,05,48,078,38,07,35,119,,13,22,160,29");
        CHECK(m.kind == MsgKind::Gsv);
        CHECK_EQ(std::string(m.talker), std::string("GP"));
        CHECK(m.has_in_view);
        CHECK_EQ(m.in_view, 11);
        CHECK_EQ(m.gsv_total, 3);
        CHECK_EQ(m.gsv_num, 1);
        CHECK_EQ(m.gsv_signal, 0);
        CHECK_EQ(m.gsv_sats.size(), static_cast<std::size_t>(4));
        CHECK_EQ(m.gsv_sats[0].prn, 2);
        CHECK_EQ(m.gsv_sats[0].elev, 62);
        CHECK_EQ(m.gsv_sats[0].azim, 37);
        CHECK_EQ(m.gsv_sats[0].snr, 41);
        CHECK_EQ(m.gsv_sats[2].prn, 7);
        CHECK_EQ(m.gsv_sats[2].snr, -1);          /* empty SNR: in view, not tracked */
        CHECK_EQ(m.gsv_sats[3].azim, 160);
    }
    /* the last message of a group: fewer blocks, an empty block skipped, azimuth 360 is 0, the signal id is read */
    {
        const NmeaMsg m = msg("GAGSV,3,3,09,11,05,360,22,,,,,,,,,1");
        CHECK_EQ(m.gsv_num, 3);
        CHECK_EQ(m.gsv_sats.size(), static_cast<std::size_t>(1));
        CHECK_EQ(m.gsv_sats[0].azim, 0);
        CHECK_EQ(m.gsv_signal, 1);
        const NmeaMsg none = msg("GLGSV,1,1,00");
        CHECK(none.has_in_view);
        CHECK_EQ(none.in_view, 0);
        CHECK(none.gsv_sats.empty());
        CHECK_EQ(none.gsv_total, 1);
        /* an unusable header: no group numbers */
        CHECK_EQ(msg("GPGSV,0,1,05,02,10,010,20").gsv_total, 0);
        CHECK_EQ(msg("GPGSV,2,3,05,02,10,010,20").gsv_total, 0);
        CHECK_EQ(msg("GPGSV,x,1,05,02,10,010,20").gsv_total, 0);
        /* a satellite with nothing but a PRN */
        const NmeaMsg bare = msg("GPGSV,1,1,01,09,,,");
        CHECK_EQ(bare.gsv_sats.size(), static_cast<std::size_t>(1));
        CHECK_EQ(bare.gsv_sats[0].elev, -1);
        CHECK_EQ(bare.gsv_sats[0].azim, -1);
        CHECK_EQ(bare.gsv_sats[0].snr, -1);
        /* the checksum rule is the old one */
        CHECK(parse_nmea("$GPGSV,1,1,01,09,10,010,20*00").kind == MsgKind::None);
    }
    /* ---- GSA fields */
    {
        const NmeaMsg m = msg(gsa("GN", 3, {2, 5, 7, 13, 15, 20, 29, 31, 33}, "1.35,0.90,1.08", 1));
        CHECK(m.kind == MsgKind::Gsa);
        CHECK_EQ(m.mode, 3);
        CHECK(!m.gsa_manual);
        CHECK_EQ(m.system_id, 1);
        CHECK_EQ(m.used_prns.size(), static_cast<std::size_t>(9));
        CHECK_EQ(m.used_prns[0], 2);
        CHECK_EQ(m.used_prns[8], 33);
        CHECK(m.has_pdop && m.has_hdop && m.has_vdop);
        CHECK_NEAR(m.pdop, 1.35, 1e-9);
        CHECK_NEAR(m.hdop, 0.90, 1e-9);
        CHECK_NEAR(m.vdop, 1.08, 1e-9);
        /* NMEA 2.x: no system id, manual mode, a gap between the PRNs, no VDOP */
        const NmeaMsg old = msg("GPGSA,M,2,04,,09" + std::string(10, ',') + "2.1,1.7,");
        CHECK(old.gsa_manual);
        CHECK_EQ(old.mode, 2);
        CHECK_EQ(old.system_id, 0);
        CHECK_EQ(old.used_prns.size(), static_cast<std::size_t>(2));
        CHECK(old.has_pdop && old.has_hdop && !old.has_vdop);
        /* no fix: no PRNs, the receiver's placeholder DOPs are read as sent */
        const NmeaMsg nofix = msg(gsa("GN", 1, {}, "99.99,99.99,99.99", 1));
        CHECK_EQ(nofix.mode, 1);
        CHECK(nofix.used_prns.empty());
        CHECK_NEAR(nofix.hdop, 99.99, 1e-9);
    }

    /* ---- constellations */
    {
        CHECK(constellation_of("GP", 5) == Constellation::Gps);
        CHECK(constellation_of("GP", 32) == Constellation::Gps);
        CHECK(constellation_of("GP", 33) == Constellation::Sbas);
        CHECK(constellation_of("GP", 64) == Constellation::Sbas);
        CHECK(constellation_of("GP", 65) == Constellation::Gps);
        CHECK(constellation_of("GP", 119) == Constellation::Gps);
        CHECK(constellation_of("GP", 120) == Constellation::Sbas);
        CHECK(constellation_of("GP", 158) == Constellation::Sbas);
        CHECK(constellation_of("GP", 159) == Constellation::Gps);
        CHECK(constellation_of("GL", 66) == Constellation::Glonass);
        CHECK(constellation_of("GL", 120) == Constellation::Glonass); /* the SBAS ranges are a GP rule */
        CHECK(constellation_of("GA", 11) == Constellation::Galileo);
        CHECK(constellation_of("GB", 6) == Constellation::Beidou);
        CHECK(constellation_of("BD", 6) == Constellation::Beidou);
        CHECK(constellation_of("GQ", 1) == Constellation::Qzss);
        CHECK(constellation_of("GI", 1) == Constellation::Unknown);
        CHECK(constellation_of("PU", 1) == Constellation::Unknown);
        /* GN: by the system id of a GSA, else by PRN range */
        CHECK(constellation_of("GN", 5, 1) == Constellation::Gps);
        CHECK(constellation_of("GN", 33, 1) == Constellation::Sbas);
        CHECK(constellation_of("GN", 70, 2) == Constellation::Glonass);
        CHECK(constellation_of("GN", 4, 3) == Constellation::Galileo);
        CHECK(constellation_of("GN", 9, 4) == Constellation::Beidou);
        CHECK(constellation_of("GN", 2, 5) == Constellation::Qzss);
        CHECK(constellation_of("GN", 5, 0) == Constellation::Gps);
        CHECK(constellation_of("GN", 70, 0) == Constellation::Glonass);
        CHECK(constellation_of("GN", 135, 0) == Constellation::Sbas);
        CHECK(constellation_of("GN", 195, 0) == Constellation::Qzss);
        CHECK(constellation_of("GN", 305, 0) == Constellation::Galileo);
        CHECK(constellation_of("GN", 405, 0) == Constellation::Beidou);
        CHECK(constellation_of("GN", 999, 0) == Constellation::Unknown);
        CHECK_EQ(constellation_letter(Constellation::Gps), 'G');
        CHECK_EQ(constellation_letter(Constellation::Glonass), 'R');
        CHECK_EQ(constellation_letter(Constellation::Galileo), 'E');
        CHECK_EQ(constellation_letter(Constellation::Beidou), 'C');
        CHECK_EQ(constellation_letter(Constellation::Qzss), 'J');
        CHECK_EQ(constellation_letter(Constellation::Sbas), 'S');
        CHECK_EQ(std::string(constellation_name(Constellation::Beidou)), std::string("BeiDou"));
    }

    /* ---- a group of three messages: nothing shows until the last one, then eleven satellites (SBAS under GP) */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,3,1,11,02,62,037,41,05,48,078,38,07,35,119,36,13,22,160,29"), 1000);
        CHECK(t.satellites(1000).empty());
        t.on_msg(msg("GPGSV,3,2,11,15,15,201,24,20,71,242,44,24,10,283,,29,40,324,33"), 1010);
        CHECK(t.satellites(1010).empty());
        t.on_msg(msg("GPGSV,3,3,11,31,55,005,40,33,30,150,27,135,25,190,25"), 1020);
        const std::vector<SkySat> v = t.satellites(1020);
        CHECK_EQ(v.size(), static_cast<std::size_t>(11));
        CHECK_EQ(count(v, Constellation::Gps), 9);
        CHECK_EQ(count(v, Constellation::Sbas), 2);
        const SkySat *s24 = find(v, Constellation::Gps, 24);
        CHECK(s24 != nullptr);
        if (s24) {
            CHECK_EQ(s24->snr, -1);
            CHECK_EQ(s24->elev, 10);
            CHECK_EQ(s24->azim, 283);
            CHECK_EQ(static_cast<int>(s24->seen_ms), 1020);
        }
        CHECK(find(v, Constellation::Sbas, 33) != nullptr);
        CHECK(find(v, Constellation::Sbas, 135) != nullptr);
        CHECK(find(v, Constellation::Gps, 33) == nullptr);
        /* ordered by constellation then PRN: GPS first, SBAS last */
        CHECK_EQ(v.front().prn, 2);
        CHECK(v.back().sys == Constellation::Sbas);
        CHECK_EQ(v.back().prn, 135);
        for (const SkySat &s : v) CHECK(!s.used);   /* no GSA yet */
    }
    /* a lost message drops the group whole; a restarted group replaces a half one; a group is never half applied */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,3,1,09,02,62,037,41"), 1000);
        t.on_msg(msg("GPGSV,3,3,09,31,55,005,40"), 1010); /* message 2 never came */
        CHECK(t.satellites(1010).empty());
        t.on_msg(msg("GPGSV,2,1,05,02,62,037,41"), 2000);
        t.on_msg(msg("GPGSV,2,1,05,07,35,119,36"), 2010); /* restarted: the first half is forgotten */
        t.on_msg(msg("GPGSV,2,2,05,13,22,160,29"), 2020);
        const std::vector<SkySat> v = t.satellites(2020);
        CHECK_EQ(v.size(), static_cast<std::size_t>(2));
        CHECK(find(v, Constellation::Gps, 7) != nullptr);
        CHECK(find(v, Constellation::Gps, 13) != nullptr);
        CHECK(find(v, Constellation::Gps, 2) == nullptr);
        /* a group with a message in the wrong order */
        t.on_msg(msg("GPGSV,3,1,09,20,62,037,41"), 3000);
        t.on_msg(msg("GPGSV,3,1,09,21,62,037,41"), 3010); /* a restart again */
        t.on_msg(msg("GPGSV,3,3,09,22,62,037,41"), 3020); /* 2 missing */
        CHECK(find(t.satellites(3020), Constellation::Gps, 20) == nullptr);
        CHECK(find(t.satellites(3020), Constellation::Gps, 22) == nullptr);
    }
    /* two talkers interleave: each has its own group (GPS, GLONASS), the used list does not mix them up */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,2,1,05,02,62,037,41,05,48,078,38,07,35,119,36,13,22,160,29"), 1000);
        t.on_msg(msg("GLGSV,1,1,02,66,58,020,39,67,44,071,35"), 1005);
        t.on_msg(msg("GPGSV,2,2,05,15,15,201,24"), 1010);
        t.on_msg(msg(gsa("GN", 3, {2, 5, 7}, "1.5,0.9,1.2", 1)), 1015);
        t.on_msg(msg(gsa("GN", 3, {66}, "1.5,0.9,1.2", 2)), 1016);
        const std::vector<SkySat> v = t.satellites(1020);
        CHECK_EQ(v.size(), static_cast<std::size_t>(7));
        CHECK_EQ(count(v, Constellation::Gps), 5);
        CHECK_EQ(count(v, Constellation::Glonass), 2);
        int used = 0;
        for (const SkySat &s : v) used += s.used;
        CHECK_EQ(used, 4);
        CHECK(find(v, Constellation::Gps, 2)->used);
        CHECK(!find(v, Constellation::Gps, 13)->used);
        CHECK(find(v, Constellation::Glonass, 66)->used);
        CHECK(!find(v, Constellation::Glonass, 67)->used);
    }
    /* used flags: SBAS named in the GPS system's GSA, PRN 66 in a GP GSA is no GLONASS satellite, a stale GSA and a no-fix GSA
     * mark nothing; GLONASS numbered 1-32 in a GSA matches 65-96 in a GSV */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,1,1,03,02,62,037,41,33,30,150,27,05,40,100,30"), 1000);
        t.on_msg(msg("GLGSV,1,1,02,66,58,020,39,67,44,071,35"), 1000);
        t.on_msg(msg(gsa("GN", 3, {2, 33}, "1.5,0.9,1.2", 1)), 1000);
        t.on_msg(msg(gsa("GN", 3, {2}, "1.5,0.9,1.2", 2)), 1000);   /* GLONASS slot 2: no sat 2 in GLONASS's GSV (66) */
        const std::vector<SkySat> v = t.satellites(1500);
        CHECK(find(v, Constellation::Gps, 2)->used);
        CHECK(find(v, Constellation::Sbas, 33)->used);
        CHECK(!find(v, Constellation::Gps, 5)->used);
        CHECK(find(v, Constellation::Glonass, 66)->used);   /* slot 2 = 66 */
        CHECK(!find(v, Constellation::Glonass, 67)->used);
        /* the GSA expires after 3 s, the satellites after 10 s */
        const std::vector<SkySat> later = t.satellites(4100);
        CHECK_EQ(later.size(), static_cast<std::size_t>(5));
        for (const SkySat &s : later) CHECK(!s.used);
        t.on_msg(msg(gsa("GN", 1, {}, "99.99,99.99,99.99", 1)), 4200);   /* no fix: lists nothing */
        for (const SkySat &s : t.satellites(4300)) CHECK(!s.used);
    }
    /* aging: a satellite not reported for 10 s leaves the table, one that is reported again stays */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,1,1,02,02,62,037,41,05,48,078,38"), 1000);
        CHECK_EQ(t.satellites(10999).size(), static_cast<std::size_t>(2));
        CHECK_EQ(t.satellites(11000).size(), static_cast<std::size_t>(2)); /* exactly 10 s: still there */
        CHECK(t.satellites(11001).empty());
        t.on_msg(msg("GPGSV,1,1,01,02,60,040,30"), 8000);  /* 02 again, 05 not */
        const std::vector<SkySat> v = t.satellites(12000);
        CHECK_EQ(v.size(), static_cast<std::size_t>(1));
        CHECK_EQ(v[0].prn, 2);
        CHECK_EQ(v[0].elev, 60);          /* the newer values */
        CHECK_EQ(v[0].snr, 30);
        CHECK_EQ(static_cast<int>(v[0].seen_ms), 8000);
        CHECK(t.satellites(18001).empty());
        t.reset();
        CHECK(t.satellites(8000).empty());
    }
    /* a satellite that stops being tracked: the SNR field goes empty, the satellite stays in view */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,1,1,01,02,62,037,41"), 1000);
        t.on_msg(msg("GPGSV,1,1,01,02,62,037,"), 2000);
        CHECK_EQ(find(t.satellites(2000), Constellation::Gps, 2)->snr, -1);
    }
    /* a second signal (NMEA 4.1 signal id) of one satellite heard within 2 s keeps the stronger SNR */
    {
        SkyTracker t;
        t.on_msg(msg("GPGSV,1,1,01,02,62,037,41,1"), 1000);
        t.on_msg(msg("GPGSV,1,1,01,02,62,037,25,7"), 1300);
        CHECK_EQ(find(t.satellites(1300), Constellation::Gps, 2)->snr, 41);
        t.on_msg(msg("GPGSV,1,1,01,02,62,037,30,1"), 5000); /* the same signal later: the new value counts */
        CHECK_EQ(find(t.satellites(5000), Constellation::Gps, 2)->snr, 30);
    }

    /* ---- GSA reports and DOPs per talker */
    {
        SkyTracker t;
        CHECK(t.reports(0).empty());
        CHECK_NEAR(t.dop(0).hdop, -1, 1e-9);
        t.on_msg(msg(gsa("GN", 3, {2, 5, 7, 13, 15, 20, 29, 31, 33}, "1.35,0.90,1.08", 1)), 1000);
        t.on_msg(msg(gsa("GN", 3, {66, 67, 74, 82}, "1.40,0.95,1.10", 2)), 1010);
        t.on_msg(msg(gsa("GP", 3, {2, 3}, "2.0,1.5,1.2", 0)), 1020);
        const std::vector<GsaReport> r = t.reports(1500);
        CHECK_EQ(r.size(), static_cast<std::size_t>(3));
        int gn1 = 0, gn2 = 0, gp = 0;
        for (const GsaReport &g : r) {
            if (std::string(g.talker) == "GN" && g.system_id == 1) {
                ++gn1;
                CHECK(g.sys == Constellation::Gps);
                CHECK_EQ(g.used.size(), static_cast<std::size_t>(9));
                CHECK_EQ(g.mode, 3);
                CHECK_NEAR(g.pdop, 1.35, 1e-9);
            } else if (std::string(g.talker) == "GN" && g.system_id == 2) {
                ++gn2;
                CHECK(g.sys == Constellation::Glonass);
                CHECK_EQ(g.used.size(), static_cast<std::size_t>(4));
                CHECK_NEAR(g.hdop, 0.95, 1e-9);
            } else if (std::string(g.talker) == "GP") {
                ++gp;
                CHECK_EQ(g.system_id, 0);
                CHECK(g.sys == Constellation::Gps);
                CHECK_NEAR(g.vdop, 1.2, 1e-9);
            }
        }
        CHECK(gn1 == 1 && gn2 == 1 && gp == 1);
        /* the DOPs of the newest report that has them */
        const Dop d = t.dop(1500);
        CHECK_NEAR(d.pdop, 2.0, 1e-9);
        CHECK_NEAR(d.hdop, 1.5, 1e-9);
        CHECK_NEAR(d.vdop, 1.2, 1e-9);
        /* reports are fresh for 3 s */
        CHECK_EQ(t.reports(4010).size(), static_cast<std::size_t>(2));
        CHECK(t.reports(4100).empty());
        CHECK_NEAR(t.dop(4100).hdop, -1, 1e-9);
        /* a report without DOPs does not blank them */
        t.on_msg(msg(gsa("GN", 3, {2}, "", 1)), 5000);
        t.on_msg(msg(gsa("GN", 3, {66}, "3.0,2.5,2.0", 2)), 5010);
        t.on_msg(msg(gsa("GN", 3, {2}, "", 1)), 5020);
        CHECK_NEAR(t.dop(5100).hdop, 2.5, 1e-9);
    }
    /* the fix tracker carries the sky: GpsStatus holds the table, the reports and the DOPs */
    {
        FixTracker t;
        t.on_msg(msg(gsa("GN", 3, {2, 5}, "1.35,0.90,1.08", 1)), 1000);
        t.on_msg(msg("GPGSV,1,1,02,02,62,037,41,05,48,078,38"), 1000);
        t.on_msg(msg("GNGGA,101530.00,4853.37855,N,00216.19971,E,1,09,0.90,41.5,M,47.3,M,,"), 1000);
        const GpsStatus s = make_status(GpsLinkState::Locked, t, 1100);
        CHECK(s.kind == GpsStatus::Kind::Fix3D);
        CHECK_EQ(s.sats.size(), static_cast<std::size_t>(2));
        CHECK(s.sats[0].used && s.sats[1].used);
        CHECK_EQ(s.gsa.size(), static_cast<std::size_t>(1));
        CHECK_NEAR(s.dop.hdop, 0.9, 1e-9);
        t.reset();
        CHECK(make_status(GpsLinkState::Locked, t, 1100).sats.empty());
        /* an unconnected link has no sky */
        CHECK(make_status(GpsLinkState::NoDevice, t, 1100).sats.empty());
    }

    /* ---- the link's facts (on a fake port) */
    {
        FakeGpsPorts fake;
        FakeDevice &d = fake.add("ttyACM0", 38400);
        d.info.product = "u-blox 7 - GPS/GNSS Receiver";
        GpsLink link(fake);
        GpsConfig on;
        on.on = true;
        link.configure(on);
        CHECK(link.facts(0).port.empty());
        CHECK(!receiver_alive(link.facts(0), 0));
        const std::string good = nmea_frame("GNRMC,101530.00,V,,,,,,,150126,,,N,V") + "\r\n";
        long t = 0;
        for (; t <= 12000; t += 100) {
            fake.now = t;
            if (t % 1000 == 0 && t >= 6000) {
                fake.push(0, good + good + good);
                if (t == 8000) fake.push(0, "$GNGGA,broken*00\r\n$GNGGA,also,broken*12\r\n");
            }
            if (t % 1000 == 0 && t < 6000) fake.push(0, good + good); /* at 38400: heard at the third speed */
            link.tick(t);
            if (t == 200) CHECK(link.state() == GpsLinkState::Searching);
        }
        const GpsStatus s = link.status(12000);
        CHECK(s.link.port == "/dev/ttyACM0");
        CHECK_EQ(s.link.baud, 38400);
        CHECK(s.link.baud_auto);
        CHECK(s.link.choice == PortChoice::AutoKnown);
        CHECK_EQ(s.link.device, std::string("u-blox 7 - GPS/GNSS Receiver"));
        CHECK_EQ(s.link.vid, static_cast<std::uint16_t>(0x1546));
        CHECK_EQ(s.link.bad_checksums, 2);
        CHECK(s.link.last_valid_ms >= 11000);
        CHECK(s.receiver_alive);
        CHECK(s.link.sentences_per_s > 0.5 && s.link.sentences_per_s < 6);
        /* silent for more than 3 s: no longer "connected", though the port is open and the link still locked */
        const GpsStatus quiet = link.status(12000 + 3500);
        CHECK(link.state() == GpsLinkState::Locked);
        CHECK(!quiet.link.port.empty());
        CHECK(!quiet.receiver_alive);
        CHECK_EQ(quiet.link.sentences_per_s, 0.0);
        /* after 10 s of silence the link lets go: no port, no facts */
        for (; t <= 24000; t += 100) {
            fake.now = t;
            link.tick(t);
        }
        CHECK(link.facts(24000).port.empty());
        CHECK(!link.status(24000).receiver_alive);
    }
    /* a chosen port and a fixed speed are said so; the rate of a steady 5 sentences a second */
    {
        FakeGpsPorts fake;
        fake.add("ttyUSB0", 9600, 0x067b, "", 0x2303);
        GpsLink link(fake);
        GpsConfig c;
        c.on = true;
        c.port = "/dev/ttyUSB0";
        c.baud = 9600;
        link.configure(c);
        const std::string s1 = nmea_frame("GNRMC,101530.00,V,,,,,,,150126,,,N,V") + "\r\n";
        for (long t = 0; t <= 6000; t += 100) {
            fake.now = t;
            if (t % 200 == 0) fake.push(0, s1);    /* 5 a second */
            link.tick(t);
        }
        const GpsStatus s = link.status(6000);
        CHECK(s.link.choice == PortChoice::ByUser);
        CHECK(!s.link.baud_auto);
        CHECK_EQ(s.link.baud, 9600);
        CHECK(s.link.device.empty());                    /* the fake gives no product string */
        CHECK_NEAR(s.link.sentences_per_s, 5.0, 0.8);
        CHECK_EQ(s.link.bad_checksums, 0);
        CHECK(receiver_alive(s.link, 6000));
        CHECK(receiver_alive(s.link, 6000 + 2900));
        CHECK(!receiver_alive(s.link, 6000 + 3200));
        /* the judgment itself: sentences, not the port */
        GpsLinkFacts f;
        f.port = "/dev/x";
        CHECK(!receiver_alive(f, 5000));       /* open, nothing valid yet */
        f.last_valid_ms = 4000;
        CHECK(receiver_alive(f, 5000));
        CHECK(receiver_alive(f, 7000));        /* exactly 3 s */
        CHECK(!receiver_alive(f, 7001));
        f.port.clear();
        CHECK(!receiver_alive(f, 5000));       /* no port, whatever the time */
    }
}
