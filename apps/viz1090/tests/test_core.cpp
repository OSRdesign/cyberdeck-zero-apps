/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the viz1090 intro's core/ (no LVGL, no screen): places and typed values, the settings file (old keys
 * and unknown lines kept, atomic write), the SDR id table and the sysfs scan, NMEA sentences recorded from a receiver
 * with and without a fix, the GPS watcher on fixture ports, the airport list of the world map file (a small synthetic
 * file, and the real one when it is there), the page texts and rules, the screen probe of run_viz1090.sh, and the
 * package files. Built and run by ../build.sh host (../../_common/build_app.sh, make -C _common/headless).
 */
#include "airports.hpp"
#include "conf.hpp"
#include "gps.hpp"
#include "model.hpp"
#include "nmea.hpp"
#include "place.hpp"
#include "sdr.hpp"
#include "viz_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        const std::string va_ = (a), vb_ = (b);                                                                        \
        if (va_ == vb_) ++g_pass;                                                                                      \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: [%s] vs [%s]\n", __FILE__, __LINE__, #a, #b,          \
                         va_.c_str(), vb_.c_str());                                                                    \
        }                                                                                                              \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static const std::string kApp = CDK_APP_DIR;
static std::string fixture(const std::string &rel) { return kApp + "/tests/fixtures/" + rel; }

static std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const std::string &path, const std::string &text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string tmpdir(const char *tag)
{
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/viz1090-test-%s-XXXXXX", tag);
    const char *d = mkdtemp(tmpl);
    return d ? d : "/tmp";
}

static std::vector<std::string> dir_names(const std::string &dir)
{
    std::vector<std::string> names;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
    }
    return names;
}

static bool contains(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

/* ---------------------------------------------------------------- places and typed values */

static void test_place()
{
    double v = 0;
    CHECK(viz::parse_degrees("48.8566", 90, v) && std::fabs(v - 48.8566) < 1e-9);
    CHECK(viz::parse_degrees(" -2.5 ", 90, v) && v == -2.5);
    CHECK(viz::parse_degrees("+12", 90, v) && v == 12);
    CHECK(viz::parse_degrees("90", 90, v) && v == 90);
    CHECK(viz::parse_degrees("-180.0", 180, v) && v == -180);
    CHECK(!viz::parse_degrees("90.0001", 90, v));
    CHECK(!viz::parse_degrees("", 90, v));
    CHECK(!viz::parse_degrees("-", 90, v));
    CHECK(!viz::parse_degrees(".", 90, v));
    CHECK(!viz::parse_degrees("1.2.3", 90, v));
    CHECK(!viz::parse_degrees("nan", 90, v));
    CHECK(!viz::parse_degrees("inf", 90, v));
    CHECK(!viz::parse_degrees("1e1", 90, v));
    CHECK(!viz::parse_degrees("0x10", 90, v));
    CHECK(!viz::parse_degrees("12abc", 90, v));
    CHECK(!viz::parse_degrees("1 2", 90, v));
    CHECK(viz::parse_degrees(".5", 90, v) && v == 0.5);

    CHECK_EQ(viz::degrees_text(48.8566), "48.85660");
    CHECK_EQ(viz::degrees_text(-73.7793), "-73.77930");
    CHECK_EQ(viz::short_degrees(49.00970), "49.0097");
    CHECK_EQ(viz::short_degrees(-0.00001), "0.0000");
    CHECK_EQ(viz::coords_text(49.0097, 2.5478), "49.0097, 2.5478");
    CHECK_EQ(viz::coords_text(48.8566, -2.3522, 2), "48.86, -2.35");
    CHECK_EQ(viz::coords_text(48.8566, 2.3522, 3), "48.857, 2.352");
    CHECK_EQ(viz::place_text(viz::Place{"CDG", 49.0097, 2.5478}), "CDG  49.0097, 2.5478");
    CHECK_EQ(viz::place_text(viz::Place{"", 48.8566, 2.3522}), "48.8566, 2.3522");

    const viz::Place cdg{"CDG", 49.0097, 2.5478}, cdg2{"CDG", 49.0, 2.5}, paris{"", 48.8566, 2.3522},
        paris2{"", 48.85661, 2.35221}, jfk{"JFK", 40.6394, -73.7793};
    CHECK(viz::same_place(cdg, cdg2));
    CHECK(viz::same_place(paris, paris2));
    CHECK(!viz::same_place(cdg, paris));
    CHECK(!viz::same_place(cdg, viz::Place{"", 49.0097, 2.5478})); /* the airport and its bare coordinates differ */
    CHECK(!viz::same_place(cdg, jfk));

    std::vector<viz::Place> recent;
    recent = viz::push_recent(recent, cdg);
    recent = viz::push_recent(recent, paris);
    recent = viz::push_recent(recent, jfk);
    CHECK(recent.size() == 3 && recent[0].code == "JFK" && recent[2].code == "CDG");
    recent = viz::push_recent(recent, cdg2); /* used again: first, the older copy goes */
    CHECK(recent.size() == 3 && recent[0].code == "CDG" && recent[0].lat == 49.0 && recent[1].code == "JFK");
    for (int i = 0; i < 10; ++i) recent = viz::push_recent(recent, viz::Place{"", 10.0 + i, 20.0});
    CHECK(recent.size() == viz::kRecentMax);
    CHECK(recent[0].lat == 19.0 && recent[4].lat == 15.0);

    CHECK_EQ(viz::filter_code("cdg"), "CDG");
    CHECK_EQ(viz::filter_code("c-d 9g"), "CDG");
    CHECK_EQ(viz::filter_code("lfpgx"), "LFPG");
    CHECK_EQ(viz::filter_code(""), "");
    CHECK_EQ(viz::filter_degrees("-48.85a6"), "-48.856");
    CHECK_EQ(viz::filter_degrees("4-8"), "48");
    CHECK_EQ(viz::filter_degrees("1.2.3"), "1.23");
    CHECK_EQ(viz::filter_degrees("2,35"), "2.35"); /* a decimal comma */
    CHECK_EQ(viz::filter_degrees("1234567890123456"), "123456789012");

    CHECK(viz::latitude_error("48.8566").empty());
    CHECK(viz::latitude_error("-90").empty());
    CHECK(!viz::latitude_error("91").empty());
    CHECK(!viz::latitude_error("").empty());
    CHECK(!viz::latitude_error("-").empty());
    CHECK(viz::longitude_error("-179.9").empty());
    CHECK(!viz::longitude_error("180.5").empty());
    CHECK(contains(viz::latitude_error("95"), "-90 to 90"));
    CHECK(contains(viz::longitude_error("200"), "-180 to 180"));
}

/* ---------------------------------------------------------------- the settings file */

static void test_conf()
{
    const viz::Conf c = viz::parse_conf(read_file(fixture("conf/cdg.conf")));
    CHECK_EQ(c.lat, "49.00970");
    CHECK_EQ(c.lon, "2.54780");
    CHECK_EQ(c.airport, "CDG");
    CHECK(c.recent.size() == 3);
    CHECK(c.recent.size() == 3 && c.recent[0].code == "CDG" && c.recent[1].code.empty() && c.recent[2].code == "JFK");
    CHECK(c.recent.size() == 3 && std::fabs(c.recent[2].lon + 73.7793) < 1e-9);

    /* the 0.1 file: no AIRPORT, no RECENT, METRIC kept */
    const viz::Conf old = viz::parse_conf(read_file(fixture("conf/old-0.1.conf")));
    CHECK_EQ(old.lat, "48.85660");
    CHECK(old.airport.empty() && old.recent.empty());

    /* CRLF, out-of-order and broken RECENT lines */
    const viz::Conf messy = viz::parse_conf("LAT=1.5\r\nRECENT2=,2,3\r\nRECENT1=ORY,48.7,2.3\r\nRECENT3=bad\r\n"
                                            "RECENT4=TOOLONG,1,2\r\nRECENT5=,95,0\r\nRECENT6=,1,1\r\n");
    CHECK_EQ(messy.lat, "1.5");
    CHECK(messy.recent.size() == 2 && messy.recent[0].code == "ORY" && messy.recent[1].lat == 2);

    viz::Place p;
    CHECK(viz::parse_recent("CDG,49.00970,2.54780", p) && p.code == "CDG" && p.lon == 2.5478);
    CHECK(viz::parse_recent(",-33.9,151.2", p) && p.code.empty() && p.lat == -33.9);
    CHECK(!viz::parse_recent("CDG,49.0", p));
    CHECK(!viz::parse_recent("CDG,49,2,3", p));
    CHECK(!viz::parse_recent("cd g,49,2", p));
    CHECK(!viz::parse_recent("CDG,91,2", p));
    CHECK_EQ(viz::recent_value(viz::Place{"JFK", 40.6394, -73.7793}), "JFK,40.63940,-73.77930");

    /* render: the position first, the user's lines kept in their order, the old owned lines replaced, RECENT last */
    const std::string old_text = "UISCALE=2\nLAT=1\nLON=2\n\n# a comment\nAIRPORT=XXX\nMETRIC=1\nRECENT1=,1,2\n"
                                 "RECENT9=,1,2\nRADIUS=300\n";
    const std::vector<viz::Place> recent{{"CDG", 49.0097, 2.5478}, {"", 48.8566, 2.3522}};
    const std::string out = viz::render_conf(old_text, viz::Place{"CDG", 49.0097, 2.5478}, recent);
    CHECK_EQ(out, "LAT=49.00970\nLON=2.54780\nAIRPORT=CDG\nUISCALE=2\n# a comment\nMETRIC=1\nRADIUS=300\n"
                  "RECENT1=CDG,49.00970,2.54780\nRECENT2=,48.85660,2.35220\n");
    /* a position without an airport: no AIRPORT line (the old one goes) */
    const std::string bare = viz::render_conf(old_text, viz::Place{"", -33.9, 151.2}, {});
    CHECK(!contains(bare, "AIRPORT="));
    CHECK(contains(bare, "LAT=-33.90000\nLON=151.20000\n"));
    CHECK(!contains(bare, "RECENT"));
    /* every line the intro writes is a shell assignment without spaces or quotes (run_viz1090.sh sources the file) */
    {
        std::istringstream in(out);
        std::string line;
        bool safe = true;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#' || line.rfind("UISCALE", 0) == 0 || line.rfind("METRIC", 0) == 0 ||
                line.rfind("RADIUS", 0) == 0)
                continue;
            safe = safe && line.find_first_of(" \t'\"$;`\\|&<>()") == std::string::npos && line.find('=') != std::string::npos;
        }
        CHECK(safe);
    }
    /* parse(render(x)) gives x back */
    const viz::Conf again = viz::parse_conf(out);
    CHECK(again.lat == "49.00970" && again.airport == "CDG" && again.recent.size() == 2);

    /* save: the folder made, an atomic write (no temporary file left), the user's lines kept */
    const std::string dir = tmpdir("conf");
    const std::string path = dir + "/sub/viz1090.conf"; /* a folder that does not exist yet */
    CHECK(viz::save_conf(path, viz::Place{"JFK", 40.6394, -73.7793}, {{"JFK", 40.6394, -73.7793}}).empty());
    CHECK_EQ(read_file(path), "LAT=40.63940\nLON=-73.77930\nAIRPORT=JFK\nRECENT1=JFK,40.63940,-73.77930\n");
    write_file(path, read_file(path) + "UISCALE=3\n");
    CHECK(viz::save_conf(path, viz::Place{"", 1, 2}, {}).empty());
    CHECK_EQ(read_file(path), "LAT=1.00000\nLON=2.00000\nUISCALE=3\n");
    CHECK(dir_names(dir + "/sub").size() == 1);
    /* a folder that cannot be written: the reason, not a crash */
    const std::string err = viz::save_conf("/proc/viz1090-test/viz1090.conf", viz::Place{"", 1, 2}, {});
    CHECK(contains(err, "Cannot write /proc/viz1090-test/viz1090.conf"));

    /* where the file is: XDG_CONFIG_HOME, else HOME/.config (as 0.1) */
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_xdg = xdg ? xdg : "";
    setenv("XDG_CONFIG_HOME", "/x/.config", 1);
    CHECK_EQ(viz::conf_path(), "/x/.config/cardputerzero/viz1090.conf");
    unsetenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    CHECK_EQ(viz::conf_path(), std::string(home ? home : "/tmp") + "/.config/cardputerzero/viz1090.conf");
    if (!saved_xdg.empty()) setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
}

/* ---------------------------------------------------------------- SDR */

static void test_sdr()
{
    std::size_t n = 0;
    const viz::UsbId *ids = viz::sdr_ids(&n);
    CHECK(ids && n == 42);
    CHECK(viz::is_sdr_id("0bda", "2838"));
    CHECK(viz::is_sdr_id("0BDA", "2832"));
    CHECK(viz::is_sdr_id("1d19", "1104"));
    CHECK(viz::is_sdr_id("1f4d", "d803"));
    CHECK(!viz::is_sdr_id("0bda", "8179")); /* a Realtek Wi-Fi adapter */
    CHECK(!viz::is_sdr_id("1d6b", "0002")); /* a root hub */
    CHECK(!viz::is_sdr_id("", ""));
    for (std::size_t i = 0; i < n; ++i) CHECK(std::strlen(ids[i].vid) == 4 && std::strlen(ids[i].pid) == 4);

    CHECK_EQ(viz::find_sdr(fixture("usb-sdr")), "RTL2838UHIDIR");
    CHECK_EQ(viz::find_sdr(fixture("usb-none")), "");
    CHECK_EQ(viz::find_sdr("/nonexistent/usb"), "");
    /* a maker other than Realtek goes in front; no product: vid:pid */
    const std::string dir = tmpdir("usb");
    mkdir((dir + "/2-1").c_str(), 0755);
    write_file(dir + "/2-1/idVendor", "1D19\n");
    write_file(dir + "/2-1/idProduct", "1101\n");
    CHECK_EQ(viz::find_sdr(dir), "1d19:1101");
    write_file(dir + "/2-1/manufacturer", "Dexatek\n");
    write_file(dir + "/2-1/product", "DK DVB-T Dongle\n");
    CHECK_EQ(viz::find_sdr(dir), "Dexatek DK DVB-T Dongle");

    const char *saved = std::getenv("VIZ_USB_DIR");
    unsetenv("VIZ_USB_DIR");
    CHECK_EQ(viz::usb_devices_dir(), "/sys/bus/usb/devices");
    setenv("VIZ_USB_DIR", "/x", 1);
    CHECK_EQ(viz::usb_devices_dir(), "/x");
    if (saved) setenv("VIZ_USB_DIR", saved, 1);
    else unsetenv("VIZ_USB_DIR");
}

/* ---------------------------------------------------------------- NMEA */

static viz::NmeaResult feed_file(const std::string &path)
{
    viz::NmeaResult r;
    std::istringstream in(read_file(path));
    std::string line;
    while (std::getline(in, line)) viz::nmea_feed_line(line, r);
    return r;
}

static void test_nmea()
{
    CHECK_NEAR(viz::nmea_degrees("4851.396", "N"), 48.8566, 1e-9);
    CHECK_NEAR(viz::nmea_degrees("00221.132", "E"), 2.3522, 1e-9);
    CHECK_NEAR(viz::nmea_degrees("2248.600", "S"), -22.81, 1e-9);
    CHECK_NEAR(viz::nmea_degrees("04315.036", "W"), -43.2506, 1e-9);
    CHECK(viz::nmea_degrees("12", "N") == 0);

    CHECK(viz::nmea_checksum_ok("$GPRMC,,V,,,,,,,,,,N*53"));
    CHECK(viz::nmea_checksum_ok("$GPRMC,,V,,,,,,,,,,N*53\r"));
    CHECK(!viz::nmea_checksum_ok("$GPRMC,,V,,,,,,,,,,N*54"));
    CHECK(!viz::nmea_checksum_ok("$GPRMC,,V,,,,,,,,,,N*5"));
    CHECK(!viz::nmea_checksum_ok("$GPRMC,,V,,,,,,,,,,N*ZZ"));
    CHECK(viz::nmea_checksum_ok("$GPRMC,,V,,,,,,,,,,N")); /* no checksum: accepted */

    const viz::NmeaResult nofix = feed_file(fixture("nmea/no-fix.nmea"));
    CHECK(nofix.seen && !nofix.fix);

    const viz::NmeaResult paris = feed_file(fixture("nmea/fix-paris.nmea"));
    CHECK(paris.seen && paris.fix);
    CHECK_NEAR(paris.lat, 48.8566, 1e-6);
    CHECK_NEAR(paris.lon, 2.3522, 1e-6);
    CHECK(paris.sats == 8);

    const viz::NmeaResult sw = feed_file(fixture("nmea/south-west.nmea"));
    CHECK(sw.seen && sw.fix);
    CHECK_NEAR(sw.lat, -22.81, 1e-6);
    CHECK_NEAR(sw.lon, -43.2506, 1e-6);

    const viz::NmeaResult bad = feed_file(fixture("nmea/bad-checksum.nmea"));
    CHECK(!bad.seen && !bad.fix);

    viz::NmeaResult r;
    CHECK(!viz::nmea_feed_line("$PUBX,00,,,,*34", r) && !r.seen);   /* proprietary: not a receiver's standard sentence */
    CHECK(!viz::nmea_feed_line("GPGGA,1,2", r) && !r.seen);         /* no '$' */
    CHECK(viz::nmea_feed_line("$GPGGA,101530.00,9851.39600,N,00221.13200,E,1,08,1.02,35.2,M,46.6,M,,", r));
    CHECK(r.seen && !r.fix);                                          /* 98 degrees north: not a position */
    CHECK(viz::nmea_feed_line("$GPGGA,101530.00,,N,,E,1,08,1.02,35.2,M,46.6,M,,", r) && !r.fix); /* empty fields */

    /* bytes in odd chunks, a sentence cut between two reads */
    const std::string all = read_file(fixture("nmea/fix-paris.nmea"));
    viz::NmeaResult chunked;
    std::string pending;
    for (std::size_t i = 0; i < all.size(); i += 7)
        viz::nmea_feed_bytes(pending, all.data() + i, std::min<std::size_t>(7, all.size() - i), chunked);
    CHECK(chunked.fix && std::fabs(chunked.lat - 48.8566) < 1e-6 && pending.empty());
    /* a port that never sends an end of line: the pending text stays bounded */
    std::string junk(1000, 'x');
    viz::NmeaResult none;
    pending.clear();
    viz::nmea_feed_bytes(pending, junk.data(), junk.size(), none);
    CHECK(pending.size() <= 256 && !none.seen);
}

/* ---------------------------------------------------------------- GPS watcher */

static viz::GpsSnapshot watch(const std::string &dir)
{
    viz::GpsWatcher w(dir);
    w.start();
    viz::GpsSnapshot s;
    for (int i = 0; i < 100; ++i) { /* up to 5 s: the fixtures answer at once */
        s = w.snapshot();
        if (s.seq > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto t0 = std::chrono::steady_clock::now();
    w.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000); /* stop() ends the thread quickly (it is resting: 100 ms slices) */
    return s;
}

static void test_gps()
{
    const std::vector<std::string> ports = viz::serial_candidates(fixture("dev-gps-none"));
    CHECK(ports.size() == 1 && ports[0] == "ttyACM0"); /* ttyAMA0 is not a USB serial port */
    CHECK(viz::serial_candidates("/nonexistent/dev").empty());
    const std::string dir = tmpdir("dev");
    for (const char *name : {"ttyUSB1", "ttyACM1", "ttyS0", "ttyACM0", "ttyUSB0"}) write_file(dir + "/" + name, "");
    const std::vector<std::string> sorted = viz::serial_candidates(dir);
    CHECK(sorted.size() == 4 && sorted[0] == "ttyACM0" && sorted[1] == "ttyACM1" && sorted[3] == "ttyUSB1");

    std::atomic<bool> stop{false};
    const viz::NmeaResult r = viz::listen_nmea(fixture("dev-gps-fix/ttyACM0"), 9600, 2500, stop);
    CHECK(r.seen && r.fix && std::fabs(r.lon - 2.3522) < 1e-6);
    CHECK(!viz::listen_nmea("/nonexistent/ttyACM0", 9600, 100, stop).seen);

    const viz::GpsSnapshot fix = watch(fixture("dev-gps-fix"));
    CHECK(fix.state == viz::GpsState::Fix && fix.port == "ttyACM0");
    CHECK_NEAR(fix.lat, 48.8566, 1e-6);
    CHECK_NEAR(fix.lon, 2.3522, 1e-6);
    const viz::GpsSnapshot wait = watch(fixture("dev-gps-wait"));
    CHECK(wait.state == viz::GpsState::Waiting && wait.port == "ttyACM0");
    const viz::GpsSnapshot none = watch(fixture("dev-gps-none"));
    CHECK(none.state == viz::GpsState::None && none.port.empty() && none.seq == 1);
    const viz::GpsSnapshot nodev = watch("/nonexistent/dev");
    CHECK(nodev.state == viz::GpsState::None && nodev.seq == 1 && nodev.denied.empty());

    /* a port the user may not read (not in the dialout group): said, not taken for "none" */
    if (geteuid() != 0) {
        const std::string dd = tmpdir("denied");
        write_file(dd + "/ttyACM0", read_file(fixture("dev-gps-fix/ttyACM0")));
        chmod((dd + "/ttyACM0").c_str(), 0);
        bool denied = false;
        CHECK(!viz::listen_nmea(dd + "/ttyACM0", 9600, 100, stop, &denied).seen && denied);
        const viz::GpsSnapshot s = watch(dd);
        CHECK(s.state == viz::GpsState::None && s.denied == "ttyACM0");
        chmod((dd + "/ttyACM0").c_str(), 0644);
    }
}

/* ---------------------------------------------------------------- the airport list */

static std::string u32(std::uint32_t v)
{
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    return s;
}

static std::string airport_record(double lat, double lon, const char *code)
{
    std::string r = u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(lat * 10000)))) +
                     u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(lon * 10000))));
    std::string c(8, '\0');
    std::memcpy(&c[0], code, std::min<std::size_t>(8, std::strlen(code)));
    return r + c;
}

static void test_airports()
{
    /* VIZWORLD, 2 chunks (24 bytes each), 3 points (8 bytes each), 4 airports, then runways and places (ignored) */
    std::string file = "VIZWORLD" + u32(2) + u32(3) + u32(4) + u32(1) + u32(0);
    file += std::string(2 * 24, 'c') + std::string(3 * 8, 'p');
    file += airport_record(49.0097, 2.5478, "CDG");
    file += airport_record(40.6394, -73.7793, "JFK");
    file += airport_record(-33.9461, 151.1772, "SYD");
    file += airport_record(10, 10, ""); /* no code: skipped */
    file += std::string(16, 'r');
    const std::string dir = tmpdir("world");
    write_file(dir + "/world-map.bin", file);

    viz::AirportList list;
    CHECK(list.load(dir + "/world-map.bin"));
    CHECK(list.loaded() && list.size() == 3);
    const viz::Airport *cdg = list.find("CDG");
    CHECK(cdg && std::fabs(cdg->lat - 49.0097) < 1e-9 && std::fabs(cdg->lon - 2.5478) < 1e-9);
    const viz::Airport *syd = list.find("SYD");
    CHECK(syd && syd->lat < -33.9 && syd->lon > 151);
    CHECK(list.find("JFK") && list.find("JFK")->lon < -73);
    CHECK(!list.find("cdg")); /* upper case only: the field makes it so */
    CHECK(!list.find("XXX") && !list.find(""));

    write_file(dir + "/bad.bin", "NOTWORLD" + file.substr(8));
    CHECK(!list.load(dir + "/bad.bin") && !list.loaded() && list.size() == 0);
    write_file(dir + "/short.bin", file.substr(0, 28 + 48 + 24 + 20)); /* cut inside the airport records */
    CHECK(!list.load(dir + "/short.bin"));
    CHECK(!list.load(dir + "/missing.bin"));
    write_file(dir + "/huge.bin", "VIZWORLD" + u32(0) + u32(0) + u32(0xFFFFFFF0u) + u32(0) + u32(0));
    CHECK(!list.load(dir + "/huge.bin")); /* an absurd count is refused before any read */

    /* the real world map of the package, when the checkout has it */
    const std::string real = kApp + "/root/opt/viz1090/world-map.bin";
    if (access(real.c_str(), R_OK) == 0) {
        viz::AirportList world;
        CHECK(world.load(real) && world.size() > 4000);
        const viz::Airport *a = world.find("CDG");
        CHECK(a && std::fabs(a->lat - 49.009) < 0.01 && std::fabs(a->lon - 2.5541) < 0.01);
        CHECK(world.find("JFK") && world.find("SYD") && world.find("NRT"));
    }

    const char *saved = std::getenv("VIZ_WORLD");
    unsetenv("VIZ_WORLD");
    CHECK_EQ(viz::world_map_path(), "/opt/viz1090/world-map.bin");
    if (saved) setenv("VIZ_WORLD", saved, 1);
}

/* ---------------------------------------------------------------- pages: texts and rules */

static bool has_word(const viz::RowText &r)
{
    /* every status value has letters (a word), not only a symbol or a colour */
    for (const std::string &v : r.values) {
        bool letter = false;
        for (const char c : v) letter = letter || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!letter) return false;
    }
    return !r.values.empty();
}

static void test_model()
{
    viz::Nav nav;
    CHECK(nav.current() == viz::View::Root && nav.depth() == 1);
    CHECK(nav.title_path() == std::vector<std::string>{"viz1090"});
    nav.open(viz::View::Position);
    nav.open(viz::View::Latitude);
    CHECK((nav.title_path() == std::vector<std::string>{"viz1090", "Position", "Latitude"}));
    CHECK(nav.back() && nav.current() == viz::View::Position);
    CHECK(nav.back() && nav.current() == viz::View::Root);
    CHECK(!nav.back()); /* Esc at the root: the app quits */

    viz::Setup s;
    viz::Place p;
    CHECK(!s.position(p));
    s.lat = "48.8566";
    CHECK(!s.position(p));
    s.lon = "2.3522";
    CHECK(s.position(p) && p.code.empty() && p.lon == 2.3522);
    s.set_place(viz::Place{"CDG", 49.0097, 2.5478});
    CHECK(s.code == "CDG" && s.lat == "49.00970" && s.position(p) && p.code == "CDG");
    s.set_lat(50);
    CHECK(s.code.empty() && s.lat == "50.00000" && s.lon == "2.54780"); /* a typed value: the airport goes */
    s.lat = "95";
    CHECK(!s.position(p));

    /* SDR: a word and a shape */
    const viz::RowText found = viz::sdr_row("RTL2838UHIDIR");
    CHECK(found.label == "SDR receiver" && found.tone == viz::Tone::Ok);
    CHECK(found.values.size() == 1 && found.values[0] == std::string(viz::kCheck) + " Found");
    const viz::RowText missing = viz::sdr_row("");
    CHECK(missing.tone == viz::Tone::Bad && missing.values[0] == std::string(viz::kCross) + " Not found: plug it in");
    CHECK(has_word(found) && has_word(missing));

    viz::GpsSnapshot g;
    const viz::RowText none = viz::gps_row(g);
    CHECK(none.label == "GPS receiver" && none.values[0] == "None detected" &&
          none.values.back() == "None"); /* a portrait screen's shorter word */
    CHECK(none.tone == viz::Tone::Muted); /* an absent optional receiver: grey, the words say it */
    g.denied = "ttyACM0";
    const viz::RowText denied = viz::gps_row(g);
    CHECK(denied.values[0] == std::string(viz::kCross) + " No access to /dev/ttyACM0" && denied.tone == viz::Tone::Bad);
    CHECK_EQ(viz::gps_band(g).text, "No access to /dev/ttyACM0: join the dialout group");
    CHECK(viz::gps_band(g).tone == viz::Tone::Bad && has_word(denied));
    g.denied.clear();
    g.state = viz::GpsState::Waiting;
    g.port = "ttyACM0";
    const viz::RowText waiting = viz::gps_row(g);
    CHECK(waiting.values[0] == "Found on /dev/ttyACM0, waiting for a fix" && waiting.tone == viz::Tone::Warn);
    CHECK(waiting.values.size() > 1 && waiting.values.back().size() < waiting.values[0].size()); /* shorter ones */
    g.state = viz::GpsState::Fix;
    g.lat = 48.8566;
    g.lon = 2.3522;
    const viz::RowText fix = viz::gps_row(g);
    CHECK(fix.values[0] == std::string(viz::kCheck) + " Fix: 48.8566, 2.3522" && fix.tone == viz::Tone::Ok);
    CHECK(fix.values.size() == 4 && fix.values[2] == std::string(viz::kCheck) + " Fix: 48.86, 2.35");
    CHECK(fix.values.back() == std::string(viz::kCheck) + " Fix"); /* a narrow row: the word and the shape stay */
    CHECK(has_word(none) && has_word(waiting) && has_word(fix));

    viz::Setup cdg;
    cdg.set_place(viz::Place{"CDG", 49.0097, 2.5478});
    CHECK(viz::position_row(cdg).values[0] == "CDG  49.0097, 2.5478");
    viz::Setup bare;
    bare.lat = "48.8566";
    bare.lon = "2.3522";
    CHECK(viz::position_row(bare).values[0] == "48.8566, 2.3522");
    CHECK(viz::position_row(viz::Setup()).values[0] == "Not set" && viz::position_row(viz::Setup()).tone == viz::Tone::Warn);
    CHECK(viz::latitude_row(cdg).values[0] == "49.0097" && viz::longitude_row(cdg).values[0] == "2.5478");
    CHECK(viz::latitude_row(viz::Setup()).values[0] == "Not set");
    /* values that are set: grey; Not set stays amber (a call to act) */
    CHECK(viz::position_row(cdg).tone == viz::Tone::Muted && viz::position_row(bare).tone == viz::Tone::Muted);
    CHECK(viz::latitude_row(cdg).tone == viz::Tone::Muted && viz::longitude_row(cdg).tone == viz::Tone::Muted);
    CHECK(viz::latitude_row(viz::Setup()).tone == viz::Tone::Warn && viz::longitude_row(viz::Setup()).tone == viz::Tone::Warn);
    CHECK(viz::recent_row(viz::Place{"JFK", 40.6394, -73.7793}).tone == viz::Tone::Muted);
    CHECK(viz::recent_row(viz::Place{"JFK", 40.6394, -73.7793}).label == "JFK");
    CHECK(viz::recent_row(viz::Place{"JFK", 40.6394, -73.7793}).values[0] == "40.6394, -73.7793");
    CHECK(viz::recent_row(viz::Place{"", 1, 2}).label == "1.0000, 2.0000");

    /* the status line: not installed > no position > no dongle > where Start goes */
    CHECK(viz::root_band(false, "x", cdg).tone == viz::Tone::Bad);
    CHECK(contains(viz::root_band(true, "", viz::Setup()).text, "Position"));
    CHECK(viz::root_band(true, "", cdg).tone == viz::Tone::Warn && contains(viz::root_band(true, "", cdg).text, "SDR"));
    CHECK_EQ(viz::root_band(true, "RTL", cdg).text, "Start opens the map around CDG");
    CHECK_EQ(viz::root_band(true, "RTL", bare).text, "Start opens the map around 48.8566, 2.3522");

    CHECK(viz::check_start(true, cdg) == viz::StartCheck::Ok);
    CHECK(viz::check_start(true, viz::Setup()) == viz::StartCheck::NoPosition);
    CHECK(viz::check_start(false, cdg) == viz::StartCheck::NotInstalled);
    CHECK(viz::refusal_band(viz::StartCheck::NoPosition).tone == viz::Tone::Warn);
    CHECK(contains(viz::refusal_band(viz::StartCheck::NoPosition).text, "No position"));
    CHECK(viz::refusal_band(viz::StartCheck::Ok).text.empty());

    CHECK(contains(viz::gps_band(g).text, "48.8566, 2.3522") && viz::gps_band(g).tone == viz::Tone::Ok);
    g.state = viz::GpsState::Waiting;
    CHECK_EQ(viz::gps_band(g).text, "GPS on /dev/ttyACM0: no fix yet, it needs open sky");
    CHECK(contains(viz::sdr_band("").text, "plug"));
    CHECK_EQ(viz::place_set_band(viz::Place{"CDG", 49.0097, 2.5478}).text, "Position set: CDG  49.0097, 2.5478");

    viz::AirportList empty;
    CHECK(contains(viz::code_hint("CDG", empty).text, "No airport list"));
    const std::string dir = tmpdir("hint");
    write_file(dir + "/w.bin", "VIZWORLD" + u32(0) + u32(0) + u32(1) + u32(0) + u32(0) + airport_record(49.0097, 2.5478, "CDG"));
    viz::AirportList one;
    CHECK(one.load(dir + "/w.bin"));
    CHECK(contains(viz::code_hint("", one).text, "e.g. CDG"));
    CHECK(contains(viz::code_hint("CD", one).text, "e.g. CDG"));
    CHECK_EQ(viz::code_hint("CDG", one).text, "found: CDG");
    CHECK(viz::code_hint("CDG", one).tone == viz::Tone::Ok);
    CHECK_EQ(viz::code_hint("LFPG", one).text, "not in the list");
    CHECK(viz::code_hint("LFPG", one).tone == viz::Tone::Bad);
    CHECK(contains(viz::degrees_hint(true).text, "-90 to 90"));
    CHECK(contains(viz::degrees_hint(false).text, "-180 to 180"));
}

/* ---------------------------------------------------------------- vizsetup --screen (viz_screen.h) */

static void test_screen()
{
    viz_screen_t s{};
    s.h = 480;
    s.ppmm_x100 = 1130;
    CHECK(viz_screen_uiscale(&s) == 2); /* the deck */
    s.ppmm_x100 = 660;
    CHECK(viz_screen_uiscale(&s) == 1); /* a 7" 1024x600 panel */
    s.h = 720;
    s.ppmm_x100 = 1500;
    CHECK(viz_screen_uiscale(&s) == 3);
    s.h = 320;
    CHECK(viz_screen_uiscale(&s) == 1); /* never more than one step per 240 rows */
    /* the cap is by the SHORT side: landscape sizes as before, portrait canvases (PDA mode) at their panels' densities */
    for (const int dens : {660, 920, 1130, 1500}) {
        s.w = 800;
        s.h = 480;
        s.ppmm_x100 = dens;
        const int landscape = viz_screen_uiscale(&s);
        s.w = 480; /* the same panel turned upright: the same step */
        s.h = 800;
        CHECK(viz_screen_uiscale(&s) == landscape);
    }
    s.w = 640;
    s.h = 480;
    s.ppmm_x100 = 1130;
    CHECK(viz_screen_uiscale(&s) == 2); /* the deck, with its width known */
    s.w = 1280;
    s.h = 720;
    s.ppmm_x100 = 1160;
    CHECK(viz_screen_uiscale(&s) == 2);
    s.w = 1024;
    s.h = 600;
    s.ppmm_x100 = 1500;
    CHECK(viz_screen_uiscale(&s) == 2); /* 600 rows: two steps at most (landscape, unchanged) */
    s.w = 480;
    s.h = 800;
    s.ppmm_x100 = 850;
    CHECK(viz_screen_uiscale(&s) == 2); /* the 4.3" DSI panel, upright */
    s.h = 640;
    s.ppmm_x100 = 900;
    CHECK(viz_screen_uiscale(&s) == 2); /* a 3.5" panel */
    s.w = 800;
    s.h = 1280;
    s.ppmm_x100 = 860;
    CHECK(viz_screen_uiscale(&s) == 2); /* the FZ-M1 tablet: not 3 by its 1280 rows */
    s.w = 480;
    s.h = 800;
    s.ppmm_x100 = 1500;
    CHECK(viz_screen_uiscale(&s) == 2); /* a dense narrow screen: capped by its 480 short side */
    s.w = 320;
    s.h = 480;
    CHECK(viz_screen_uiscale(&s) == 1);
    /* the 7.0 px/mm threshold: the 5" 800x480 panel (7.4 px/mm) gets 2, the 7" 1024x600 (6.6) stays 1 */
    s.w = 480;
    s.h = 800;
    s.ppmm_x100 = 740;
    CHECK(viz_screen_uiscale(&s) == 2); /* the 5" panel, upright */
    s.w = 800;
    s.h = 480;
    CHECK(viz_screen_uiscale(&s) == 2); /* the 5" panel, landscape */
    s.ppmm_x100 = 699;
    CHECK(viz_screen_uiscale(&s) == 1);
    s.ppmm_x100 = 700;
    CHECK(viz_screen_uiscale(&s) == 2);
    s.w = 1024;
    s.h = 600;
    s.ppmm_x100 = 660;
    CHECK(viz_screen_uiscale(&s) == 1); /* the 7" panel */
    int w = 0, h = 0;
    CHECK(viz_parse_wxh("800x480", &w, &h) == 0 && w == 800 && h == 480);
    CHECK(viz_parse_wxh("800x", &w, &h) != 0 && viz_parse_wxh("10x10", &w, &h) != 0);

    setenv("VIZ_SCREEN", "1024x600", 1);
    setenv("APPLAUNCH_SCREEN_PPMM", "660", 1);
    viz_screen_detect(&s);
    CHECK(s.w == 1024 && s.h == 600 && std::strcmp(s.source, "override") == 0 && s.ppmm_x100 == 660);
    unsetenv("VIZ_SCREEN");
    setenv("APPLAUNCH_CONTRACT", "1", 1);
    setenv("APPLAUNCH_CANVAS", "720x720", 1);
    viz_screen_detect(&s);
    CHECK(s.w == 720 && s.h == 720 && std::strcmp(s.source, "contract") == 0);
    unsetenv("APPLAUNCH_CONTRACT");
    unsetenv("APPLAUNCH_CANVAS");
    unsetenv("APPLAUNCH_SCREEN_PPMM");
}

/* ---------------------------------------------------------------- the package */

static void test_package()
{
    const std::string desktop = read_file(kApp + "/root/usr/share/APPLaunch/applications/viz1090.desktop");
    CHECK(contains(desktop, "\nX-Fullscreen=true"));
    CHECK(contains(desktop, "\nX-MinSize=" + std::to_string(viz::kMinW) + "x" + std::to_string(viz::kMinH)));
    CHECK(contains(desktop, "\nExec=/opt/viz1090/run_viz1090.sh"));
    CHECK(contains(desktop, "\nIcon=share/images/viz1090.png"));
    CHECK(!read_file(kApp + "/root/usr/share/APPLaunch/share/images/viz1090.png").empty());
    CHECK(!read_file(kApp + "/root/usr/share/doc/viz1090/copyright").empty());

    /* the script: the one of build/ is the one packaged; the intro runs without the SDL bridge, with --screen first */
    const std::string script = read_file(kApp + "/root/opt/viz1090/run_viz1090.sh");
    CHECK(!script.empty() && script == read_file(kApp + "/build/run_viz1090.sh"));
    CHECK(contains(script, "/opt/viz1090/vizsetup --screen"));
    CHECK(contains(script, "\n/opt/viz1090/vizsetup || exit 0"));
    CHECK(!contains(script, "LD_PRELOAD=$BRIDGE /opt/viz1090/vizsetup"));
    CHECK(contains(script, "LD_PRELOAD=$BRIDGE /opt/viz1090/viz1090"));

    const std::string manifest = read_file(kApp + "/app.json");
    CHECK(contains(manifest, "\"share_code\": \"viz1090\""));
    CHECK(contains(manifest, "\"version\": \"0.3.1\""));
    /* what the native intro links (cp0_lvgl: libinput, libxkbcommon, FreeType) and what viz1090 and readsb link */
    for (const char *dep : {"libfreetype6", "libinput10", "libxkbcommon0", "libsdl2-2.0-0", "libsdl2-ttf-2.0-0",
                            "libsdl2-gfx-1.0-0", "librtlsdr0", "python3"})
        CHECK(contains(manifest, dep));
    CHECK(!contains(manifest, "libc6"));
    /* the old SDL intro is gone */
    CHECK(access((kApp + "/build/vizsetup.cpp").c_str(), F_OK) != 0);
}

int main()
{
    test_place();
    test_conf();
    test_sdr();
    test_nmea();
    test_gps();
    test_airports();
    test_model();
    test_screen();
    test_package();
    std::printf("test-viz1090: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
