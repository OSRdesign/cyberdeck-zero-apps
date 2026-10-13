/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: port discovery and identification (see ports.hpp).
 */
#include "ports.hpp"

#include "fsutil.hpp"

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>

namespace sc {
namespace {

bool starts_with(const std::string &s, const char *prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

/* "ttyUSB10" -> ("ttyUSB", 10); no number -> -1 */
void split_name(const std::string &name, std::string &prefix, long &number)
{
    std::size_t i = name.size();
    while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') --i;
    prefix = name.substr(0, i);
    number = i < name.size() ? std::strtol(name.c_str() + i, nullptr, 10) : -1;
}

bool name_less(const std::string &a, const std::string &b)
{
    std::string pa, pb;
    long na = 0, nb = 0;
    split_name(a, pa, na);
    split_name(b, pb, nb);
    if (pa != pb) return pa < pb;
    if (na != nb) return na < nb;
    return a < b;
}

std::uint16_t parse_hex16(const std::string &text)
{
    return static_cast<std::uint16_t>(std::strtoul(text.c_str(), nullptr, 16) & 0xFFFF);
}

/* letters, digits and . _ - only (a serial number may hold anything) */
std::string sanitize(const std::string &text)
{
    std::string out;
    for (const char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out;
}

/* Walks from the tty's device directory up to the USB device (the first directory with idVendor). */
void identify_usb(const std::string &sys_root, const std::string &device_dir, PortInfo &port)
{
    const std::string stop = fs::real_path(sys_root);
    std::string dir = fs::real_path(device_dir);
    for (int depth = 0; depth < 10 && !dir.empty() && dir.size() > stop.size(); ++depth) {
        if (port.interface.empty() && fs::exists(dir + "/bInterfaceNumber"))
            port.interface = fs::read_line(dir + "/bInterfaceNumber");
        if (fs::exists(dir + "/idVendor")) {
            port.vid = parse_hex16(fs::read_line(dir + "/idVendor"));
            port.pid = parse_hex16(fs::read_line(dir + "/idProduct"));
            port.manufacturer = fs::read_line(dir + "/manufacturer");
            port.product = fs::read_line(dir + "/product");
            port.serial = fs::read_line(dir + "/serial");
            return;
        }
        dir = fs::parent(dir);
    }
}

} // namespace

std::string port_id(const PortInfo &port)
{
    if (!port.by_id.empty()) return port.by_id;
    if (port.usb && (port.vid || port.pid)) {
        std::string id = "usb-" + usb_id_text(port.vid, port.pid);
        if (!port.serial.empty()) id += "-" + sanitize(port.serial);
        if (!port.interface.empty()) id += "-if" + sanitize(port.interface);
        return id;
    }
    return port.tty;
}

std::string port_title(const PortInfo &port)
{
    if (port.onboard) return port.gpio ? "On-board UART (GPIO 14/15)" : "On-board UART (" + port.tty + ")";
    if (port.known && port.known->pid != kAnyProduct) return port.known->name;
    if (!port.product.empty()) return port.product;
    if (port.known) return port.known->name;
    if (port.usb && (port.vid || port.pid)) return "USB serial (" + usb_id_text(port.vid, port.pid) + ")";
    return port.tty;
}

std::vector<PortInfo> scan_ports(const Roots &roots, bool include_onboard)
{
    std::vector<PortInfo> ports;
    const std::string cls_dir = roots.sys + "/class/tty";

    // /dev/serial/by-id: link name per tty (the link's own target, not its real path: in the demonstration world the
    // /dev node itself is a link to a pseudo-terminal)
    std::map<std::string, std::string> by_id;
    const std::string by_id_dir = roots.dev + "/serial/by-id";
    for (const std::string &link : fs::list_dir(by_id_dir)) {
        const std::string target = fs::read_link(by_id_dir + "/" + link);
        if (target.empty()) continue;
        const std::string tty = fs::base_name(target);
        if (!by_id.count(tty)) by_id[tty] = link; /* the list is sorted: -if00 before -if01 */
    }
    const std::string serial0 = fs::base_name(fs::read_link(roots.dev + "/serial0"));
    const std::string serial1 = fs::base_name(fs::read_link(roots.dev + "/serial1"));

    for (const std::string &name : fs::list_dir(cls_dir)) {
        const bool usb = starts_with(name, "ttyUSB") || starts_with(name, "ttyACM");
        const bool onboard = starts_with(name, "ttyAMA") || starts_with(name, "ttyS");
        if (!usb && !(onboard && include_onboard)) continue;
        const std::string cls = cls_dir + "/" + name;
        if (!fs::exists(cls + "/device")) continue;   /* no hardware behind it */
        if (onboard) {
            if (name == serial1 && name != serial0) continue; /* the Bluetooth UART */
            if (starts_with(name, "ttyS") && fs::read_line(cls + "/type") == "0") continue; /* an empty 8250 slot */
        }
        const std::string node = roots.dev + "/" + name;
        if (!fs::exists(node)) continue;

        PortInfo port;
        port.tty = name;
        port.path = node;
        port.usb = usb;
        port.onboard = onboard;
        port.gpio = onboard && name == serial0;
        port.driver = fs::base_name(fs::read_link(cls + "/device/driver"));
        if (usb) identify_usb(roots.sys, cls + "/device", port);
        const auto link = by_id.find(name);
        if (link != by_id.end()) port.by_id = link->second;
        if (port.vid || port.pid) port.known = lookup_device(port.vid, port.pid, port.product);
        port.id = port_id(port);
        port.title = port_title(port);
        port.rec = recommend(port.known, port.onboard);
        ports.push_back(std::move(port));
    }
    std::sort(ports.begin(), ports.end(), [](const PortInfo &a, const PortInfo &b) {
        if (a.onboard != b.onboard) return !a.onboard;
        return name_less(a.tty, b.tty);
    });
    return ports;
}

std::string resolve_port(const Roots &roots, const std::string &id)
{
    for (const PortInfo &port : scan_ports(roots, true))
        if (port.id == id) return port.path;
    return std::string();
}

bool same_port(const PortInfo &a, const PortInfo &b)
{
    return a.id == b.id && a.tty == b.tty && a.path == b.path && a.title == b.title && a.gpio == b.gpio &&
           a.rec.baud == b.rec.baud;
}

GroupState dialout_state()
{
    GroupState g;
    g.root = geteuid() == 0;
    if (const struct passwd *pw = getpwuid(getuid())) g.user = pw->pw_name ? pw->pw_name : "";
    const struct group *gr = getgrnam("dialout");
    if (!gr) return g;
    const gid_t dialout = gr->gr_gid;
    for (char **member = gr->gr_mem; member && *member; ++member)
        if (!g.user.empty() && g.user == *member) g.listed = true;
    if (const struct passwd *pw = getpwuid(getuid()))
        if (pw->pw_gid == dialout) g.listed = true;
    if (getegid() == dialout) g.active = true;
    const int n = getgroups(0, nullptr);
    if (n > 0) {
        std::vector<gid_t> groups(static_cast<std::size_t>(n));
        const int got = getgroups(n, groups.data());
        for (int i = 0; i < got; ++i)
            if (groups[static_cast<std::size_t>(i)] == dialout) g.active = true;
    }
    if (g.active) g.listed = true;
    return g;
}

Problem group_problem(const GroupState &groups)
{
    Problem p;
    if (groups.listed) {
        p.title = "Restart to use serial ports";
        p.text = "You were added to the dialout group, the group that may use serial ports, but it is not active "
                 "in this session yet. Reboot the deck (or log out and in again).";
        return p;
    }
    p.title = "No permission for serial ports";
    p.text = std::string("Your user is not in the dialout group, the group that may use serial ports. Fix: in a "
                         "terminal type\n") +
             dialout_fix_command() + "\nthen reboot the deck.";
    return p;
}

Problem open_problem(int err, const GroupState &groups, const std::string &tty)
{
    Problem p;
    switch (err) {
    case EACCES:
    case EPERM:
        if (!group_ok(groups)) {
            p = group_problem(groups);
            p.title = "No permission for " + tty;
            return p;
        }
        p.title = "No permission for " + tty;
        p.text = "You are in the dialout group but " + tty + " still refuses access. Check its owner and mode in a "
                 "terminal: ls -l /dev/" + tty;
        return p;
    case EBUSY:
        p.title = tty + " is busy";
        p.text = "Another program has " + tty + " open (a terminal, a flasher, ModemManager). Close it and try "
                 "again.";
        return p;
    case ENOENT:
    case ENODEV:
    case ENXIO:
        p.title = tty + " is gone";
        p.text = tty + " is not there any more. Plug the adapter or the board in again.";
        return p;
    default:
        p.title = "Cannot open " + tty;
        p.text = std::string(std::strerror(err)) + ".";
        return p;
    }
}

} // namespace sc
