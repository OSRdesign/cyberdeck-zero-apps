/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: NMEA 0183 (core/nmea.cpp): the checksum, RMC / GGA / GSA / GSV fields, a void fix, the southern and
 * western hemispheres, empty fields, talker variants and malformed lines, and the byte splitter. Every sentence is
 * synthetic (an invented position, or the NMEA spec's published examples).
 */
#include "check.hpp"
#include "nmea.hpp"

#include <cstring>

using namespace atlas;

namespace {

std::vector<std::string> split(const std::string &bytes, NmeaSplitter &sp)
{
    std::vector<std::string> out;
    sp.feed(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), out);
    return out;
}

} // namespace

void test_nmea()
{
    /* the checksum: the published examples of the NMEA 0183 documentation */
    CHECK(nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    CHECK(nmea_checksum_ok("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68"));
    CHECK(nmea_checksum_ok("$gpgga,x*00") == false);
    CHECK_EQ(nmea_frame("GPGLL,4916.45,N,12311.12,W,225444,A"), std::string("$GPGLL,4916.45,N,12311.12,W,225444,A*31"));
    CHECK_EQ(static_cast<int>(nmea_checksum("GPGLL,4916.45,N,12311.12,W,225444,A")), 0x31);
    /* lower-case hex is fine; one wrong digit, a missing checksum, a missing '$', trailing text are not */
    {
        bool found = false;
        for (int i = 0; i < 60 && !found; ++i) {
            std::string f = nmea_frame("GPXXX," + std::to_string(i));
            if (f[f.size() - 1] >= 'A' || f[f.size() - 2] >= 'A') { /* a letter in the hex digits: write it in lower case */
                for (std::size_t k = f.size() - 2; k < f.size(); ++k)
                    if (f[k] >= 'A') f[k] = static_cast<char>(f[k] - 'A' + 'a');
                CHECK(nmea_checksum_ok(f));
                found = true;
            }
        }
        CHECK(found);
    }
    CHECK(!nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46"));
    CHECK(!nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"));
    CHECK(!nmea_checksum_ok("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    CHECK(!nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47 "));
    CHECK(!nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4"));
    CHECK(!nmea_checksum_ok("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4G"));
    CHECK(!nmea_checksum_ok(""));
    CHECK(!nmea_checksum_ok("$"));
    CHECK(!nmea_checksum_ok("$*00"));
    CHECK(parse_nmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*46").kind == MsgKind::None);

    /* GGA of the documentation */
    {
        const NmeaMsg m = parse_nmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
        CHECK(m.kind == MsgKind::Gga);
        CHECK(m.valid && m.has_pos);
        CHECK_EQ(std::string(m.talker), std::string("GP"));
        CHECK_NEAR(m.lat, 48.0 + 7.038 / 60.0, 1e-9);
        CHECK_NEAR(m.lon, 11.0 + 31.0 / 60.0, 1e-9);
        CHECK_EQ(m.quality, 1);
        CHECK(m.has_sats && m.sats == 8);
        CHECK(m.has_hdop);
        CHECK_NEAR(m.hdop, 0.9, 1e-9);
        CHECK(m.has_alt);
        CHECK_NEAR(m.alt_m, 545.4, 1e-9);
        CHECK(m.has_time && m.hour == 12 && m.minute == 35);
        CHECK_NEAR(m.second, 19.0, 1e-9);
    }
    /* RMC of the documentation: the western hemisphere, knots to m/s, the date */
    {
        const NmeaMsg m = parse_nmea("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68");
        CHECK(m.kind == MsgKind::Rmc);
        CHECK(m.valid && m.has_pos);
        CHECK_NEAR(m.lat, 49.0 + 16.45 / 60.0, 1e-9);
        CHECK_NEAR(m.lon, -(123.0 + 11.12 / 60.0), 1e-9);
        CHECK(m.has_speed);
        CHECK_NEAR(m.speed_mps, 0.5 * 0.514444, 1e-6);
        CHECK(m.has_course);
        CHECK_NEAR(m.course_deg, 54.7, 1e-9);
        CHECK(m.has_date && m.day == 19 && m.month == 11 && m.year == 2094);
    }
    /* southern and western, synthetic: -33 51.12345, -70 12.5 */
    {
        const NmeaMsg m = parse_nmea(nmea_frame("GNRMC,101530.00,A,3351.12345,S,07012.50000,W,5.00,90.0,150126,,,A,V"));
        CHECK(m.kind == MsgKind::Rmc && m.valid && m.has_pos);
        CHECK_NEAR(m.lat, -(33.0 + 51.12345 / 60.0), 1e-9);
        CHECK_NEAR(m.lon, -(70.0 + 12.5 / 60.0), 1e-9);
        CHECK(m.has_date && m.year == 2026 && m.month == 1 && m.day == 15);
        CHECK_NEAR(m.speed_mps, 5.0 * 0.514444, 1e-6);
        CHECK_NEAR(m.course_deg, 90.0, 1e-9);
        CHECK(std::strcmp(m.talker, "GN") == 0);
        const NmeaMsg g = parse_nmea(nmea_frame("GNGGA,101530.00,3351.12345,S,07012.50000,W,1,09,1.10,41.5,M,47.3,M,,"));
        CHECK(g.valid && g.lat < 0 && g.lon < 0);
        CHECK_NEAR(g.lat, m.lat, 1e-12);
    }
    /* a void RMC (status V): no fix, no position, the date still read */
    {
        const NmeaMsg m = parse_nmea(nmea_frame("GNRMC,101530.00,V,,,,,,,150126,,,N,V"));
        CHECK(m.kind == MsgKind::Rmc);
        CHECK(!m.valid && !m.has_pos && !m.has_speed && !m.has_course);
        CHECK(m.has_date);
        /* status A but the mode indicator says N: not a fix either */
        const NmeaMsg n = parse_nmea(nmea_frame("GNRMC,101530.00,A,4853.37855,N,00216.19971,E,0.1,,150126,,,N,V"));
        CHECK(!n.valid && !n.has_pos);
        /* status A without a usable position */
        const NmeaMsg e = parse_nmea(nmea_frame("GNRMC,101530.00,A,,,,,0.1,,150126,,,A,V"));
        CHECK(!e.valid && !e.has_pos);
    }
    /* RMC of an older receiver (no mode field), empty course at a standstill */
    {
        const NmeaMsg m = parse_nmea(nmea_frame("GPRMC,101530.00,A,4853.37855,N,00216.19971,E,0.00,,150126,,"));
        CHECK(m.valid && m.has_pos && m.has_speed && !m.has_course);
        CHECK_NEAR(m.speed_mps, 0.0, 1e-9);
    }
    /* GGA with quality 0: no fix, but the satellites and the HDOP are still read */
    {
        const NmeaMsg m = parse_nmea(nmea_frame("GNGGA,101530.00,,,,,0,03,99.99,,,,,,"));
        CHECK(m.kind == MsgKind::Gga);
        CHECK(!m.valid && !m.has_pos && !m.has_alt);
        CHECK(m.has_sats && m.sats == 3);
        CHECK(m.has_hdop);
        CHECK_NEAR(m.hdop, 99.99, 1e-9);
        CHECK_EQ(m.quality, 0);
        /* a quality 1 GGA with an empty altitude: 2D-like, valid */
        const NmeaMsg n = parse_nmea(nmea_frame("GNGGA,101530.00,4853.37855,N,00216.19971,E,1,04,4.00,,M,,M,,"));
        CHECK(n.valid && n.has_pos && !n.has_alt);
        /* differential (2) and RTK (4) are fixes too */
        CHECK(parse_nmea(nmea_frame("GPGGA,101530.00,4853.37855,N,00216.19971,E,2,10,0.80,40.0,M,47.3,M,,")).valid);
    }
    /* GSA: the mode and the DOPs; a GN receiver sends one per constellation */
    {
        const NmeaMsg m = parse_nmea(nmea_frame("GNGSA,A,3,02,03,04,05,06,07,08,09,10,,,,3.12,2.40,2.64,1"));
        CHECK(m.kind == MsgKind::Gsa);
        CHECK_EQ(m.mode, 3);
        CHECK(m.has_hdop);
        CHECK_NEAR(m.hdop, 2.40, 1e-9);
        const NmeaMsg two = parse_nmea(nmea_frame("GPGSA,A,2,02,03,04,05,,,,,,,,,5.0,4.1,3.0"));
        CHECK_EQ(two.mode, 2);
        const NmeaMsg none = parse_nmea(nmea_frame("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1"));
        CHECK_EQ(none.mode, 1);
        const NmeaMsg empty = parse_nmea(nmea_frame("GNGSA,A,,,,,,,,,,,,,,,,"));
        CHECK_EQ(empty.mode, 0);
        CHECK(!empty.has_hdop);
    }
    /* GSV: satellites in view */
    {
        const NmeaMsg m = parse_nmea(nmea_frame("GPGSV,3,1,11,02,20,030,28,03,27,070,31,04,34,110,34,05,41,150,37"));
        CHECK(m.kind == MsgKind::Gsv);
        CHECK(m.has_in_view && m.in_view == 11);
        CHECK(!parse_nmea(nmea_frame("GPGSV,1,1,")).has_in_view);
    }
    /* talkers */
    for (const char *t : {"GP", "GN", "GL", "GA", "GB", "BD", "GQ"}) {
        const NmeaMsg m = parse_nmea(nmea_frame(std::string(t) + "GGA,101530.00,4853.37855,N,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,"));
        CHECK(m.kind == MsgKind::Gga);
        CHECK(m.valid);
        CHECK_EQ(std::string(m.talker), std::string(t));
        CHECK_EQ(m.type, std::string("GGA"));
    }
    /* sentences the map does not use are valid sentences all the same (they count for the sniff) */
    {
        const NmeaMsg v = parse_nmea(nmea_frame("GPVTG,054.7,T,034.4,M,005.5,N,010.2,K,A"));
        CHECK(v.kind == MsgKind::Other);
        CHECK_EQ(v.type, std::string("GPVTG"));
        const NmeaMsg u = parse_nmea(nmea_frame("PUBX,00,081350.00,4717.113210,N,00833.915187,E,546.589,G3,2.1,2.0,0.007,77.52,0.007,,0.92,1.19,0.77,9,0,0"));
        CHECK(u.kind == MsgKind::Other);
        CHECK(!u.valid && !u.has_pos);
        CHECK_EQ(std::string(u.talker), std::string(""));
        CHECK(parse_nmea(nmea_frame("GPTXT,01,01,02,u-blox ag - www.u-blox.com")).kind == MsgKind::Other);
    }
    /* out-of-range and malformed fields: rejected, never a position */
    {
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,9100.00000,N,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,")).has_pos);
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,4853.37855,N,18100.00000,E,1,09,1.10,41.5,M,47.3,M,,")).has_pos);
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,4860.00000,N,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,")).has_pos); /* 60 minutes */
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,4853.37855,X,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,")).has_pos);
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,48x3.37855,N,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,")).has_pos);
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,4853.37855,N,00216.19971,N,1,09,1.10,41.5,M,47.3,M,,")).valid);
        CHECK(!parse_nmea(nmea_frame("GNGGA,101530.00,4853.,N,00216.19971,E,1,09,1.10,41.5,M,47.3,M,,")).has_pos);
        CHECK(parse_nmea(nmea_frame("GNGGA")).kind == MsgKind::Gga); /* all fields empty: a sentence, no fix */
        CHECK(!parse_nmea(nmea_frame("GNGGA")).valid);
        CHECK(parse_nmea(nmea_frame("")).kind == MsgKind::None);
        CHECK(parse_nmea(nmea_frame("G")).kind == MsgKind::None);
        CHECK(parse_nmea(nmea_frame("gpgga,1")).kind == MsgKind::None);   /* lower-case address */
        CHECK(parse_nmea(nmea_frame("GP GGA,1")).kind == MsgKind::None);
        CHECK(parse_nmea("$GPGGA,\xB5\x62*00").kind == MsgKind::None);
        const NmeaMsg s = parse_nmea(nmea_frame("GNRMC,101530.00,A,4853.37855,N,00216.19971,E,1e9,400.0,150126,,,A,V"));
        CHECK(s.valid && !s.has_speed && !s.has_course); /* absurd speed and course are dropped */
        const NmeaMsg d = parse_nmea(nmea_frame("GNRMC,101530.00,A,4853.37855,N,00216.19971,E,1.0,10.0,321399,,,A,V"));
        CHECK(!d.has_date);
        const NmeaMsg t = parse_nmea(nmea_frame("GNRMC,256100.00,A,4853.37855,N,00216.19971,E,1.0,10.0,150126,,,A,V"));
        CHECK(!t.has_time);
    }

    /* the splitter */
    {
        NmeaSplitter sp;
        const std::string a = nmea_frame("GNGGA,1,x"), b = nmea_frame("GNRMC,2,y");
        auto l = split(a + "\r\n" + b + "\n", sp);
        CHECK_EQ(l.size(), static_cast<std::size_t>(2));
        CHECK_EQ(l[0], a);
        CHECK_EQ(l[1], b);
        /* split across feeds, byte by byte */
        std::vector<std::string> all;
        for (char c : a + "\r\n" + b + "\r\n") {
            auto part = split(std::string(1, c), sp);
            all.insert(all.end(), part.begin(), part.end());
        }
        CHECK_EQ(all.size(), static_cast<std::size_t>(2));
        CHECK_EQ(all[1], b);
        /* garbage before the '$', and a '$' inside a line restarts it */
        l = split("garbage\x01\xff" + a + "\r\nxx$GNRMC,3,partial$GNGGA,4,z\r\n", sp);
        CHECK_EQ(l.size(), static_cast<std::size_t>(2));
        CHECK_EQ(l[0], a);
        CHECK_EQ(l[1], std::string("$GNGGA,4,z"));
        /* a binary byte (a UBX frame) ends the line unseen */
        l = split("$GNGGA,5\xb5\x62\x01\r\n" + b + "\r\n", sp);
        CHECK_EQ(l.size(), static_cast<std::size_t>(1));
        CHECK_EQ(l[0], b);
        /* too long: dropped */
        l = split("$" + std::string(300, 'A') + "\r\n" + a + "\r\n", sp);
        CHECK_EQ(l.size(), static_cast<std::size_t>(1));
        CHECK_EQ(l[0], a);
        /* an empty line and a lone '$' give nothing; a line without an end waits */
        l = split("\r\n$\r\n$GNGGA,6", sp);
        CHECK(l.empty());
        l = split("\r\n", sp);
        CHECK_EQ(l.size(), static_cast<std::size_t>(1));
        CHECK_EQ(l[0], std::string("$GNGGA,6"));
        sp.reset();
        l = split("$GNGGA,7", sp);
        sp.reset();
        l = split("\r\n", sp);
        CHECK(l.empty());
    }
}
