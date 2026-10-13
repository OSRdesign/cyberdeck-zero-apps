/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the Serial Console's core/ (no LVGL, no screen) and of its package files (.desktop, app.json).
 * Built and run by ../../_common/build_app.sh host (make -C _common/headless): this file has main() and the page
 * logic, the other .cpp files of tests/ the groups of check.hpp.
 */
#include "check.hpp"

#include "console_core.hpp"
#include "fsutil.hpp"
#include "keymap.hpp"

#include <linux/input-event-codes.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

int g_pass = 0, g_fail = 0;

using namespace sc;

std::string test_tmpdir(const char *tag)
{
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/sc-test-" + tag + "-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string("/tmp");
}

static std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void test_nav()
{
    Nav nav;
    CHECK(nav.current() == View::Ports);
    nav.open(View::Port);
    nav.open(View::Choice);
    CHECK_EQ(nav.depth(), 3);
    CHECK(nav.back() == Back::Popped && nav.current() == View::Port);
    nav.open(View::Terminal);
    CHECK(nav.back() == Back::Popped && nav.current() == View::Port);
    CHECK(nav.back() == Back::Popped && nav.current() == View::Ports);
    CHECK(nav.back() == Back::Quit); /* a short Esc at the ports page ends the app */
    CHECK_EQ(nav.depth(), 1);
}

static void test_texts()
{
    PortInfo p;
    p.tty = "ttyUSB0";
    p.usb = true;
    p.vid = 0x10c4;
    p.pid = 0xea60;
    p.manufacturer = "Silicon Labs";
    p.product = "CP2102 USB to UART Bridge Controller";
    p.serial = "0001";
    p.driver = "cp210x";
    p.known = lookup_device(p.vid, p.pid, p.product);
    p.title = port_title(p);
    p.rec = recommend(p.known, false);
    PortSettings s;
    CHECK_EQ(port_caption(p, s), std::string("ttyUSB0 - 115200 recommended"));
    s.baud = 57600;
    CHECK_EQ(port_caption(p, s), std::string("ttyUSB0 - 57600 (115200 recommended)"));
    s.baud = 115200;
    CHECK_EQ(port_caption(p, s), std::string("ttyUSB0 - 115200 recommended"));
    CHECK_EQ(speed_value(115200, 115200), std::string("115200 (recommended)"));
    CHECK_EQ(speed_value(9600, 115200), std::string("9600"));

    const auto items = port_info_items(p, true);
    std::string keys;
    for (const auto &kv : items) keys += kv.first + "=" + kv.second + ";";
    CHECK_EQ(keys, std::string("Device=CP210x USB-serial;Maker=Silicon Labs;Product=CP2102 USB to UART Bridge Controller;"
                               "USB ID=10c4:ea60;Driver=cp210x;Serial no=0001;Typically=ESP32, Meshtastic;"
                               "Recommended=115200 8N1;Port=/dev/ttyUSB0;"));
    // the USB details when sysfs gives them: the exact chip, the speed and the interfaces
    PortInfo q = p;
    q.bcd = 0x0100;
    q.chip = chip_variant(q.vid, q.pid, q.bcd, 1, q.product);
    q.speed = "12";
    q.interfaces = 1;
    std::string qkeys;
    for (const auto &kv : port_info_items(q, true)) qkeys += kv.first + "=" + kv.second + ";";
    CHECK(qkeys.find("Chip=CP2102 (rev 1.00);") != std::string::npos);
    CHECK(qkeys.find("USB=12 Mbit/s, 1 interface;") != std::string::npos);
    CHECK(bare_adapter(p));
    CHECK(port_row_text(p) == std::make_pair(std::string("CP210x USB-serial"), std::string("ttyUSB0")));
    CHECK(port_info_items(p, false).back().first == "Access");

    PortInfo u;
    u.tty = "ttyAMA0";
    u.onboard = u.gpio = true;
    u.driver = "uart-pl011";
    u.title = port_title(u);
    CHECK_EQ(u.title, std::string("On-board UART (GPIO 14/15)"));
    std::string ukeys;
    for (const auto &kv : port_info_items(u, true)) ukeys += kv.first + ";";
    CHECK_EQ(ukeys, std::string("Device;Driver;Typically;Recommended;Port;")); /* no USB rows */
    CHECK(port_row_text(u) == std::make_pair(std::string("On-board UART"), std::string("GPIO 14/15")));
    u.gpio = false;
    u.tty = "ttyAMA2";
    CHECK(port_row_text(u) == std::make_pair(std::string("On-board UART"), std::string("ttyAMA2")));

    PortInfo unknown;
    unknown.tty = "ttyACM3";
    unknown.usb = true;
    unknown.vid = 0x1234;
    unknown.pid = 0x5678;
    CHECK_EQ(port_title(unknown), std::string("USB serial (1234:5678)"));
    unknown.product = "Widget";
    CHECK_EQ(port_title(unknown), std::string("Widget"));
    PortInfo bare;
    bare.tty = "ttyUSB7";
    CHECK_EQ(port_title(bare), std::string("ttyUSB7"));

    CHECK_EQ(terminal_title("ttyUSB0", 115200, "8N1"), std::string("ttyUSB0 115200 8N1"));
    SessionStatus st;
    st.state = LinkState::Open;
    st.rx = 1234;
    st.tx = 34;
    CHECK_EQ(pill_text(st), std::string("Connected"));
    CHECK_EQ(status_text(st, false), std::string("RX 1.2 kB  TX 34 B"));
    st.log_on = true;
    st.log_path = "/home/deck/serial-logs/ttyUSB0-20261013-101500.log";
    CHECK_EQ(status_text(st, true), std::string("RX 1.2 kB  TX 34 B  Paused  Log ttyUSB0-20261013-101500.log"));
    st.state = LinkState::Lost;
    CHECK_EQ(pill_text(st), std::string("Port lost: waiting"));
    st.log_on = false;
    st.log_error = true;
    st.ui_dropped = 2000;
    CHECK(status_text(st, false).find("Log failed") != std::string::npos);
    CHECK(status_text(st, false).find("Skipped 2.0 kB") != std::string::npos);
}

static void test_keys()
{
    const auto key = [](std::uint32_t code, std::uint32_t mods, std::uint32_t keysym, const char *utf8,
                        Ending e = Ending::CR, Backspace b = Backspace::Del) {
        return translate_key(code, mods, keysym, utf8, e, b);
    };
    CHECK(key(KEY_ESC, 0, 0, "").act == KeyAct::Leave);
    CHECK_EQ(key(KEY_ENTER, 0, 0, "").bytes, std::string("\r"));
    CHECK_EQ(key(KEY_ENTER, 0, 0, "", Ending::LF).bytes, std::string("\n"));
    CHECK_EQ(key(KEY_KPENTER, 0, 0, "", Ending::CRLF).bytes, std::string("\r\n"));
    CHECK_EQ(key(KEY_BACKSPACE, 0, 0, "").bytes, std::string("\x7f"));
    CHECK_EQ(key(KEY_BACKSPACE, 0, 0, "", Ending::CR, Backspace::Bs).bytes, std::string("\x08"));
    CHECK_EQ(key(KEY_TAB, 0, 0, "\t").bytes, std::string("\t"));
    CHECK_EQ(key(KEY_TAB, kModShift, 0, "").bytes, std::string("\x1b[Z"));
    CHECK_EQ(key(KEY_UP, 0, 0, "").bytes, std::string("\x1b[A"));
    CHECK_EQ(key(KEY_DOWN, 0, 0, "").bytes, std::string("\x1b[B"));
    CHECK_EQ(key(KEY_RIGHT, 0, 0, "").bytes, std::string("\x1b[C"));
    CHECK_EQ(key(KEY_LEFT, 0, 0, "").bytes, std::string("\x1b[D"));
    CHECK_EQ(key(KEY_HOME, 0, 0, "").bytes, std::string("\x1b[H"));
    CHECK_EQ(key(KEY_END, 0, 0, "").bytes, std::string("\x1b[F"));
    CHECK_EQ(key(KEY_DELETE, 0, 0, "").bytes, std::string("\x1b[3~"));
    CHECK_EQ(key(KEY_PAGEUP, 0, 0, "").bytes, std::string("\x1b[5~"));
    CHECK(key(KEY_PAGEUP, kModShift, 0, "").act == KeyAct::PageUp);
    CHECK(key(KEY_PAGEDOWN, kModShift, 0, "").act == KeyAct::PageDown);
    CHECK_EQ(key(KEY_F1, 0, 0, "").bytes, std::string("\x1bOP"));
    CHECK_EQ(key(KEY_F5, 0, 0, "").bytes, std::string("\x1b[15~"));
    CHECK_EQ(key(KEY_F12, 0, 0, "").bytes, std::string("\x1b[24~"));
    // Ctrl: from the keysym (layout-aware), the text, or the key code
    CHECK_EQ(key(KEY_C, kModCtrl, 'c', "c").bytes, std::string("\x03"));
    CHECK_EQ(key(KEY_C, kModCtrl, 0, "c").bytes, std::string("\x03"));
    CHECK_EQ(key(KEY_C, kModCtrl, 0, "").bytes, std::string("\x03"));
    CHECK_EQ(key(KEY_D, kModCtrl | kModShift, 'D', "D").bytes, std::string("\x04"));
    CHECK_EQ(key(KEY_Q, kModCtrl, 'a', "a").bytes, std::string("\x01")); /* AZERTY: the A key is where Q is */
    CHECK_EQ(key(KEY_Z, kModCtrl, 'z', "\x1a").bytes, std::string("\x1a"));
    CHECK_EQ(key(KEY_LEFTBRACE, kModCtrl, '[', "[").bytes, std::string("\x1b"));
    CHECK_EQ(key(KEY_SPACE, kModCtrl, ' ', " ").bytes, std::string(1, '\0'));
    CHECK_EQ(key(0, kModCtrl, 0, "\x03").bytes, std::string("\x03"));
    CHECK(key(KEY_1, kModCtrl, '1', "1").act == KeyAct::None);
    // the app's own Alt keys; other Alt keys are Meta (ESC + key)
    CHECK(key(KEY_P, kModAlt, 'p', "p").act == KeyAct::Pause);
    CHECK(key(KEY_X, kModAlt, 'x', "x").act == KeyAct::Clear);
    CHECK(key(KEY_L, kModAlt, 'l', "l").act == KeyAct::Log);
    CHECK(key(KEY_R, kModAlt, 'r', "r").act == KeyAct::Reset);
    CHECK(key(KEY_E, kModAlt, 'e', "e").act == KeyAct::SendEsc);
    CHECK_EQ(key(KEY_B, kModAlt, 'b', "b").bytes, std::string("\x1b" "b"));
    // text
    CHECK_EQ(key(KEY_A, 0, 'a', "a").bytes, std::string("a"));
    CHECK_EQ(key(KEY_A, kModShift, 'A', "A").bytes, std::string("A"));
    CHECK_EQ(key(0, 0, 0, "\xc3\xa9").bytes, std::string("\xc3\xa9"));
    CHECK(key(KEY_LEFTSHIFT, kModShift, 0, "").act == KeyAct::None);
    CHECK(key(KEY_LEFTCTRL, kModCtrl, 0, "").act == KeyAct::None);
    CHECK(key(KEY_VOLUMEUP, 0, 0, "").act == KeyAct::None);
}

/* The package: a native fullscreen app now (no terminal, no picocom), X-MinSize equal to the app's minimum. */
static void test_package()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_file(app + "/root/usr/share/APPLaunch/applications/serial-console.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nName=Serial Console\n") != std::string::npos);
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    const std::string min = "\nX-MinSize=" + std::to_string(kMinW) + "x" + std::to_string(kMinH);
    CHECK(desktop.find(min) != std::string::npos);
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/serial-console\n") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/serial-console.png") != std::string::npos);
    CHECK(desktop.find("Terminal=true") == std::string::npos);
    CHECK(!read_file(app + "/root/usr/share/APPLaunch/share/images/serial-console.png").empty());
    const std::string copyright = read_file(app + "/root/usr/share/doc/serial-console/copyright");
    CHECK(!copyright.empty() && copyright.find("picocom") == std::string::npos);
    const std::string manifest = read_file(app + "/app.json");
    CHECK(manifest.find("\"share_code\": \"serial-console\"") != std::string::npos);
    CHECK(manifest.find("\"package\": \"serial-console\"") != std::string::npos);
    CHECK(manifest.find("\"version\": \"0.3.0\"") != std::string::npos);
    CHECK(manifest.find("\"draft\": true") != std::string::npos);
    // the fixtures of the classifier say they are representative samples
    for (const char *f : {"esp32-idf-boot.txt", "gnss-ublox-m8.txt", "micropython.txt", "marlin-m115.txt"})
        CHECK(read_file(app + "/tests/fixtures/" + f).find("## Representative sample, NOT a capture") == 0);
    CHECK(manifest.find("\"title\": \"Serial Console\"") != std::string::npos);
    CHECK(manifest.find("\"depends\": \"libfreetype6, libinput10, libxkbcommon0\"") != std::string::npos);
    CHECK(manifest.find("picocom") == std::string::npos);
    // the binary, once build.sh cross wrote it, is an ELF file (the 0.1.0 shell script is gone)
    const std::string bin = read_file(app + "/root/usr/share/APPLaunch/bin/serial-console");
    CHECK(bin.empty() || bin.compare(0, 4, "\x7f" "ELF") == 0);
    CHECK(!fs::exists(app + "/tests/run_tests.sh"));
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    test_nav();
    test_texts();
    test_keys();
    test_devices();
    test_ports();
    test_term();
    test_baud();
    test_settings();
    test_serial_pty();
    test_identify();
    test_bootproto();
    test_probe_pty();
    test_package();
    std::printf("test-serial-console: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
