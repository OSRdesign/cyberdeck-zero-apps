/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the GPS settings (gps, gps_port, gps_speed in settings.conf; off and Auto by default; a file of a
 * user who never touched the GPS stays what 0.1.0 wrote), the cycles of the Settings rows and their labels.
 */
#include "check.hpp"
#include "format.hpp"
#include "settings.hpp"

using namespace atlas;

void test_settings_gps()
{
    /* defaults: GPS off, port and speed Auto; the default file has no gps line at all */
    const Settings d = parse_settings("");
    CHECK(!d.gps);
    CHECK(d.gps_port.empty());
    CHECK_EQ(d.gps_baud, 0);
    CHECK_EQ(serialize_settings(d).find("gps"), std::string::npos);

    /* a round trip */
    Settings s;
    s.gps = true;
    s.gps_port = "/dev/ttyACM0";
    s.gps_baud = 38400;
    const std::string text = serialize_settings(s);
    CHECK_EQ(text, std::string("v=1\nunits=km\nscale=auto\ncache=auto\noffline=off\ngps=auto\ngps_port=/dev/ttyACM0\ngps_speed=38400\n"));
    const Settings r = parse_settings(text);
    CHECK(r.gps);
    CHECK_EQ(r.gps_port, std::string("/dev/ttyACM0"));
    CHECK_EQ(r.gps_baud, 38400);
    /* only what differs from the defaults is written */
    Settings only_on;
    only_on.gps = true;
    CHECK_EQ(serialize_settings(only_on), std::string("v=1\nunits=km\nscale=auto\ncache=auto\noffline=off\ngps=auto\n"));
    /* the other keys are untouched by the new ones */
    s.units = Units::Mi;
    s.source = "paris";
    const Settings r2 = parse_settings(serialize_settings(s));
    CHECK(r2.units == Units::Mi);
    CHECK_EQ(r2.source, std::string("paris"));
    CHECK(r2.gps);

    /* tolerant: on, off, Auto, junk */
    CHECK(parse_settings("gps=on").gps);
    CHECK(parse_settings("gps=auto").gps);
    CHECK(!parse_settings("gps=off").gps);
    CHECK(!parse_settings("gps=maybe").gps);
    CHECK(!parse_settings("gps=").gps);
    CHECK_EQ(parse_settings("gps_speed=9600").gps_baud, 9600);
    CHECK_EQ(parse_settings("gps_speed=4800").gps_baud, 4800);
    CHECK_EQ(parse_settings("gps_speed=115200").gps_baud, 115200);
    CHECK_EQ(parse_settings("gps_speed=19200").gps_baud, 0);   /* not offered: Auto */
    CHECK_EQ(parse_settings("gps_speed=-9600").gps_baud, 0);
    CHECK_EQ(parse_settings("gps_speed=fast").gps_baud, 0);
    CHECK_EQ(parse_settings("gps_speed=auto").gps_baud, 0);
    CHECK_EQ(parse_settings("gps_port=/dev/ttyUSB1").gps_port, std::string("/dev/ttyUSB1"));
    CHECK_EQ(parse_settings("gps_port=auto").gps_port, std::string(""));
    /* a device path is a /dev/tty... name and nothing else: no other folder, no "..", no shell characters */
    for (const char *bad : {"/etc/passwd", "/dev/null", "/dev/", "/dev/tty", "/dev/ttyACM0/../../etc/passwd", "../dev/ttyACM0",
                            "/dev/ttyACM0;rm -rf", "/dev/ttyACM 0", "dev/ttyACM0", "/dev/ttyAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                            "/dev/tty$HOME", "ttyACM0", ""}) {
        CHECK(!valid_gps_port(bad));
        CHECK_EQ(parse_settings(std::string("gps_port=") + bad).gps_port, std::string(""));
    }
    for (const char *good : {"/dev/ttyACM0", "/dev/ttyUSB12", "/dev/ttyS0", "/dev/ttyAMA0", "/dev/ttyGPS-1", "/dev/tty.usbserial_1"})
        CHECK(valid_gps_port(good));
    CHECK(!valid_gps_port("/dev/ttyACM0\n"));
    /* a hand-edited file with a bad port is not written back with it */
    Settings bad;
    bad.gps_port = "/etc/passwd";
    bad.gps_baud = 31337;
    CHECK_EQ(serialize_settings(bad).find("gps_"), std::string::npos);

    /* the cycles */
    CHECK(valid_gps_baud(0) && valid_gps_baud(4800) && valid_gps_baud(9600) && valid_gps_baud(38400) && valid_gps_baud(115200));
    CHECK(!valid_gps_baud(19200) && !valid_gps_baud(-1));
    int b = 0;
    const int want[] = {9600, 4800, 38400, 115200, 0};
    for (int w : want) {
        b = next_gps_baud(b);
        CHECK_EQ(b, w);
    }
    CHECK_EQ(next_gps_baud(31337), 0);
    const std::vector<std::string> two = {"/dev/ttyACM0", "/dev/ttyUSB0"};
    CHECK_EQ(next_gps_port("", two), std::string("/dev/ttyACM0"));
    CHECK_EQ(next_gps_port("/dev/ttyACM0", two), std::string("/dev/ttyUSB0"));
    CHECK_EQ(next_gps_port("/dev/ttyUSB0", two), std::string(""));
    CHECK_EQ(next_gps_port("/dev/ttyUSB7", two), std::string("")); /* chosen but absent: back to Auto */
    CHECK_EQ(next_gps_port("", {}), std::string(""));
    CHECK_EQ(next_gps_port("/dev/ttyACM0", {}), std::string(""));

    /* the labels of the rows */
    CHECK_EQ(std::string(gps_label(false)), std::string("Off"));
    CHECK_EQ(std::string(gps_label(true)), std::string("Auto"));
    CHECK_EQ(gps_port_label("", false), std::string("Auto"));
    CHECK_EQ(gps_port_label("/dev/ttyACM0", true), std::string("/dev/ttyACM0"));
    CHECK_EQ(gps_port_label("/dev/ttyUSB1", false), std::string("/dev/ttyUSB1 (absent)"));
    CHECK_EQ(gps_speed_label(0), std::string("Auto"));
    CHECK_EQ(gps_speed_label(9600), std::string("9600"));
}
