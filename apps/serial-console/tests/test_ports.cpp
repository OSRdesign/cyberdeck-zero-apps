/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of port discovery and identification on a fake sysfs tree (core/ports, core/fake_sys).
 */
#include "check.hpp"

#include "fake_sys.hpp"
#include "fsutil.hpp"
#include "ports.hpp"

#include <cerrno>

using namespace sc;

namespace {

fake::UsbTty usb(const char *tty, std::uint16_t vid, std::uint16_t pid, const char *maker, const char *product,
                 const char *serial, const char *driver, const char *by_id, int port)
{
    fake::UsbTty t;
    t.tty = tty;
    t.vid = vid;
    t.pid = pid;
    t.manufacturer = maker;
    t.product = product;
    t.serial = serial;
    t.driver = driver;
    t.by_id = by_id;
    t.usb_port = port;
    return t;
}

const PortInfo *find(const std::vector<PortInfo> &ports, const std::string &tty)
{
    for (const PortInfo &p : ports)
        if (p.tty == tty) return &p;
    return nullptr;
}

} // namespace

void test_ports()
{
    const std::string base = test_tmpdir("ports");
    const Roots roots = fake::make_roots(base);

    CHECK(scan_ports(roots, true).empty()); /* nothing plugged */

    const char *cp_id = "usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0";
    CHECK(fake::add_usb_tty(roots, usb("ttyUSB0", 0x10c4, 0xea60, "Silicon Labs", "CP2102 USB to UART Bridge Controller",
                                       "0001", "cp210x", cp_id, 1)));
    CHECK(fake::add_usb_tty(roots, usb("ttyUSB10", 0x0403, 0x6001, "FTDI", "FT232R USB UART", "A5 0#x", "ftdi_sio", "", 2)));
    CHECK(fake::add_usb_tty(roots, usb("ttyUSB2", 0x1a86, 0x7523, "", "", "", "ch341", "", 3)));
    CHECK(fake::add_usb_tty(roots, usb("ttyACM0", 0x2341, 0x0043, "Arduino (www.arduino.cc)", "", "8573", "cdc_acm",
                                       "usb-Arduino__www.arduino.cc__0043_8573-if00", 4)));
    CHECK(fake::add_usb_tty(roots, usb("ttyACM1", 0x1234, 0x5678, "Nobody", "My Gadget", "", "cdc_acm", "", 5)));
    fake::UsbTty gone = usb("ttyUSB3", 0x10c4, 0xea60, "", "", "", "cp210x", "", 6);
    CHECK(fake::add_usb_tty(roots, gone));
    fs::remove_tree(roots.dev + "/ttyUSB3"); /* a class entry without its /dev node: not listed */
    CHECK(fake::add_virtual_tty(roots, "tty1"));
    CHECK(fake::add_onboard_tty(roots, "ttyS0", "serial8250", 0, ""));   /* an empty 8250 slot */
    CHECK(fake::add_onboard_tty(roots, "ttyS1", "dw-apb-uart", 4, ""));
    CHECK(fake::add_onboard_tty(roots, "ttyAMA0", "uart-pl011", 1, ""));
    CHECK(fake::add_onboard_tty(roots, "ttyAMA1", "uart-pl011", 1, ""));
    CHECK(fake::add_onboard_tty(roots, "ttyAMA2", "uart-pl011", 1, ""));
    CHECK(fake::add_alias(roots, "serial0", "ttyAMA0"));
    CHECK(fake::add_alias(roots, "serial1", "ttyAMA1")); /* the Bluetooth UART */

    const std::vector<PortInfo> ports = scan_ports(roots, false);
    CHECK_EQ(ports.size(), static_cast<std::size_t>(5));
    if (ports.size() == 5) {
        CHECK_EQ(ports[0].tty, std::string("ttyACM0"));
        CHECK_EQ(ports[1].tty, std::string("ttyACM1"));
        CHECK_EQ(ports[2].tty, std::string("ttyUSB0"));
        CHECK_EQ(ports[3].tty, std::string("ttyUSB2"));
        CHECK_EQ(ports[4].tty, std::string("ttyUSB10")); /* numbers in order, 2 before 10 */
    }
    if (const PortInfo *p = find(ports, "ttyUSB0")) {
        CHECK(p->usb && !p->onboard);
        CHECK_EQ(p->vid, static_cast<std::uint16_t>(0x10c4));
        CHECK_EQ(p->pid, static_cast<std::uint16_t>(0xea60));
        CHECK_EQ(p->manufacturer, std::string("Silicon Labs"));
        CHECK_EQ(p->product, std::string("CP2102 USB to UART Bridge Controller"));
        CHECK_EQ(p->serial, std::string("0001"));
        CHECK_EQ(p->driver, std::string("cp210x"));
        CHECK_EQ(p->interface, std::string("00"));
        CHECK_EQ(p->by_id, std::string(cp_id));
        CHECK_EQ(p->id, std::string(cp_id));
        CHECK_EQ(p->title, std::string("CP210x USB-serial"));
        CHECK_EQ(p->rec.baud, 115200);
        CHECK_EQ(p->path, roots.dev + "/ttyUSB0");
    } else {
        CHECK(!"ttyUSB0 listed");
    }
    if (const PortInfo *p = find(ports, "ttyACM0")) {
        CHECK_EQ(p->driver, std::string("cdc_acm"));
        CHECK_EQ(p->title, std::string("Arduino Uno"));
        CHECK_EQ(p->rec.baud, 9600);
        CHECK_EQ(p->interface, std::string("00"));
    } else {
        CHECK(!"ttyACM0 listed");
    }
    if (const PortInfo *p = find(ports, "ttyACM1")) { /* unknown: named from its product string */
        CHECK(p->known == nullptr);
        CHECK_EQ(p->title, std::string("My Gadget"));
        CHECK_EQ(p->id, std::string("usb-1234:5678-if00"));
        CHECK_EQ(p->rec.baud, 115200);
    } else {
        CHECK(!"ttyACM1 listed");
    }
    if (const PortInfo *p = find(ports, "ttyUSB10")) {
        CHECK_EQ(p->id, std::string("usb-0403:6001-A5_0_x-if00")); /* the serial number made safe */
        CHECK_EQ(p->title, std::string("FT232R USB-serial"));
        CHECK_EQ(p->driver, std::string("ftdi_sio"));
    } else {
        CHECK(!"ttyUSB10 listed");
    }
    if (const PortInfo *p = find(ports, "ttyUSB2")) CHECK_EQ(p->title, std::string("CH340 USB-serial"));
    CHECK(find(ports, "ttyUSB3") == nullptr);
    CHECK(find(ports, "tty1") == nullptr);
    CHECK(find(ports, "ttyAMA0") == nullptr); /* on-board UARTs only on request */

    const std::vector<PortInfo> all = scan_ports(roots, true);
    CHECK_EQ(all.size(), static_cast<std::size_t>(8));
    if (const PortInfo *p = find(all, "ttyAMA0")) {
        CHECK(p->onboard && p->gpio);
        CHECK_EQ(p->title, std::string("On-board UART (GPIO 14/15)"));
        CHECK_EQ(p->driver, std::string("uart-pl011"));
        CHECK_EQ(p->id, std::string("ttyAMA0"));
    } else {
        CHECK(!"ttyAMA0 listed");
    }
    if (const PortInfo *p = find(all, "ttyAMA2")) CHECK_EQ(p->title, std::string("On-board UART (ttyAMA2)"));
    CHECK(find(all, "ttyS1") != nullptr);
    CHECK(find(all, "ttyS0") == nullptr);   /* type 0 */
    CHECK(find(all, "ttyAMA1") == nullptr); /* serial1: Bluetooth */
    CHECK(!all.empty() && !all.front().onboard && all.back().onboard); /* USB first */

    // a board that comes back on another number is found again by its id; an unknown id is not found
    CHECK_EQ(resolve_port(roots, cp_id), roots.dev + "/ttyUSB0");
    CHECK(fake::remove_tty(roots, "ttyUSB0"));
    CHECK(resolve_port(roots, cp_id).empty());
    CHECK(!fs::exists(roots.dev + "/serial/by-id/" + cp_id));
    CHECK(fake::add_usb_tty(roots, usb("ttyUSB5", 0x10c4, 0xea60, "Silicon Labs", "CP2102 USB to UART Bridge Controller",
                                       "0001", "cp210x", cp_id, 1)));
    CHECK_EQ(resolve_port(roots, cp_id), roots.dev + "/ttyUSB5");
    CHECK(resolve_port(roots, "usb-nothing").empty());
    CHECK_EQ(resolve_port(roots, "ttyAMA0"), roots.dev + "/ttyAMA0");

    const std::vector<PortInfo> again = scan_ports(roots, false);
    CHECK(find(again, "ttyACM0") && find(ports, "ttyACM0") && same_port(*find(again, "ttyACM0"), *find(ports, "ttyACM0")));
    CHECK(find(again, "ttyUSB5") && !same_port(*find(again, "ttyUSB5"), *find(ports, "ttyUSB0")));

    // permission advice
    GroupState none;
    none.user = "deck";
    const Problem fix = open_problem(EACCES, none, "ttyUSB0");
    CHECK(fix.title == "No permission for ttyUSB0");
    CHECK(fix.text.find("sudo usermod -aG dialout $USER") != std::string::npos);
    CHECK(fix.text.find("reboot") != std::string::npos);
    GroupState listed = none;
    listed.listed = true;
    CHECK(open_problem(EACCES, listed, "ttyUSB0").text.find("Reboot") != std::string::npos);
    GroupState member = none;
    member.active = member.listed = true;
    CHECK(group_ok(member));
    CHECK(open_problem(EACCES, member, "ttyUSB0").text.find("ls -l /dev/ttyUSB0") != std::string::npos);
    GroupState root;
    root.root = true;
    CHECK(group_ok(root) && !group_ok(none) && !group_ok(listed));
    CHECK(open_problem(EBUSY, member, "ttyACM0").title == "ttyACM0 is busy");
    CHECK(open_problem(ENOENT, member, "ttyACM0").title == "ttyACM0 is gone");
    CHECK(!open_problem(EINVAL, member, "ttyACM0").text.empty());
    CHECK(group_problem(none).text.find(dialout_fix_command()) != std::string::npos);
    const GroupState mine = dialout_state(); /* the build box: it answers without crashing */
    CHECK(mine.root || !mine.user.empty());

    fs::remove_tree(base);
}
