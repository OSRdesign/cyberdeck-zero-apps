/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the line settings and the settings file, including the migration of a 0.1.0 file (core/settings).
 */
#include "check.hpp"

#include "fsutil.hpp"
#include "settings.hpp"

#include <cstdlib>

using namespace sc;

void test_settings()
{
    Frame f;
    CHECK(parse_frame("7E1", f) && f.data == 7 && f.parity == 'E' && f.stop == 1);
    CHECK(parse_frame("8N2", f) && f.text() == "8N2");
    CHECK(parse_frame("8O1", f) && f.parity == 'O');
    CHECK(!parse_frame("9N1", f) && !parse_frame("8X1", f) && !parse_frame("8N3", f) && !parse_frame("8N", f));
    CHECK(frame_list().size() == 6 && frame_list()[0] == "8N1");
    for (const std::string &name : frame_list()) CHECK(parse_frame(name, f));

    const std::vector<int> &speeds = speed_list();
    CHECK(speeds.front() == 1200 && speeds.back() == 921600);
    bool sorted = true, has74880 = false;
    for (std::size_t i = 1; i < speeds.size(); ++i) sorted = sorted && speeds[i - 1] < speeds[i];
    for (const int s : speeds) has74880 = has74880 || s == 74880;
    CHECK(sorted && has74880);
    CHECK(valid_baud(74880) && valid_baud(115200) && !valid_baud(0) && !valid_baud(99999999));

    CHECK_EQ(ending_bytes(Ending::CR), std::string("\r"));
    CHECK_EQ(ending_bytes(Ending::LF), std::string("\n"));
    CHECK_EQ(ending_bytes(Ending::CRLF), std::string("\r\n"));
    CHECK_EQ(std::string(ending_name(Ending::CRLF)), std::string("CR LF"));
    CHECK(backspace_byte(Backspace::Del) == '\x7f' && backspace_byte(Backspace::Bs) == '\x08');

    PortSettings defaults;
    CHECK(defaults.baud == 0 && defaults.frame == "8N1" && !defaults.echo && defaults.ending == Ending::CR &&
          defaults.backspace == Backspace::Del && !defaults.log && !defaults.detect_enter);
    CHECK_EQ(effective_baud(defaults, 9600), 9600);
    PortSettings fixed;
    fixed.baud = 57600;
    CHECK_EQ(effective_baud(fixed, 9600), 57600);

    // round trip
    Config c;
    c.show_onboard = true;
    c.last_port = "usb-Arduino__www.arduino.cc__0043_8573-if00";
    PortSettings a;
    a.baud = 74880;
    a.frame = "7E1";
    a.echo = true;
    a.ending = Ending::CRLF;
    a.backspace = Backspace::Bs;
    a.log = true;
    a.detect_enter = true;
    c.set(c.last_port, a);
    c.set("ttyAMA0", PortSettings());
    const Config back = Config::parse(c.serialize());
    CHECK(back.show_onboard);
    CHECK_EQ(back.last_port, c.last_port);
    CHECK(back.ports.size() == 2);
    CHECK(back.settings(c.last_port) == a);
    CHECK(back.settings("ttyAMA0") == PortSettings());
    CHECK(back.settings("never-seen") == PortSettings());
    CHECK(c.serialize().find("v=2\n") != std::string::npos);

    // a 0.1.0 file: its one set of settings becomes its last port's
    bool migrated = false;
    const Config old = Config::parse("port=usb-X-if00\nbaud=74880\nframe=7E1\necho=on\nending=LF\nlog=on\nall=1\n",
                                     &migrated);
    CHECK(migrated);
    CHECK(old.show_onboard);
    CHECK_EQ(old.last_port, std::string("usb-X-if00"));
    const PortSettings o = old.settings("usb-X-if00");
    CHECK(o.baud == 74880 && o.frame == "7E1" && o.echo && o.ending == Ending::LF && o.log);
    const Config old_default = Config::parse("port=/dev/ttyUSB0\nbaud=115200\nframe=8N1\necho=off\nending=CR\n"
                                             "log=off\nall=0\n", &migrated);
    CHECK(migrated);
    CHECK_EQ(old_default.last_port, std::string("ttyUSB0"));
    CHECK_EQ(old_default.settings("ttyUSB0").baud, 0); /* 0.1.0's default: the recommended speed now */
    const Config only_all = Config::parse("all=1\n", &migrated);
    CHECK(only_all.show_onboard && !migrated && only_all.ports.empty());

    // bad values are ignored, unknown keys and sections too
    const Config bad = Config::parse("v=2\n[port a]\nbaud=abc\nframe=9Z9\necho=maybe\nending=XX\nbaud=99999999\n"
                                     "backspace=??\n$(touch /tmp/x)=1\n[weird]\nbaud=1200\n[port ]\nbaud=1200\n");
    CHECK(bad.settings("a") == PortSettings());
    CHECK(bad.ports.size() == 1);
    CHECK(Config::parse("garbage without equals\n\x01\x02\n").ports.empty());

    // the paths
    const char *old_home = std::getenv("HOME");
    const std::string saved_home = old_home ? old_home : "";
    const char *old_xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_xdg = old_xdg ? old_xdg : "";
    setenv("HOME", "/home/deck", 1);
    unsetenv("XDG_CONFIG_HOME");
    CHECK(config_path().compare(0, 18, "/home/deck/.config") == 0);
    CHECK(config_path().size() > 20 && fs::base_name(config_path()) == "serial-console.conf");
    CHECK_EQ(fs::base_name(fs::parent(fs::parent(config_path()))), std::string(".config"));
    setenv("XDG_CONFIG_HOME", "/xdg", 1);
    CHECK(config_path().compare(0, 5, "/xdg/") == 0);
    CHECK_EQ(log_dir(), std::string("/home/deck/serial-logs"));
    if (saved_home.empty()) unsetenv("HOME");
    else setenv("HOME", saved_home.c_str(), 1);
    if (saved_xdg.empty()) unsetenv("XDG_CONFIG_HOME");
    else setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);

    const char *old_tz = std::getenv("TZ");
    const std::string saved_tz = old_tz ? old_tz : "";
    setenv("TZ", "UTC", 1);
    tzset();
    CHECK_EQ(log_file_name("ttyUSB0", 1767225600), std::string("ttyUSB0-20260101-000000.log"));
    if (saved_tz.empty()) unsetenv("TZ");
    else setenv("TZ", saved_tz.c_str(), 1);
    tzset();

    // save (the folder is created) and load
    const std::string dir = test_tmpdir("settings");
    const std::string path = dir + "/a/b/serial-console.conf";
    CHECK(save_config(path, c));
    CHECK(fs::read_all(path).compare(0, 17, "# Serial Console ") == 0);
    bool m = true;
    const Config loaded = load_config(path, &m);
    CHECK(!m && loaded.settings(c.last_port) == a && loaded.show_onboard);
    CHECK(load_config(dir + "/missing.conf").ports.empty());
    fs::write_file(dir + "/v1.conf", "port=ttyACM0\nbaud=9600\n");
    CHECK(load_config(dir + "/v1.conf", &m).settings("ttyACM0").baud == 9600 && m);
    fs::remove_tree(dir);
}
