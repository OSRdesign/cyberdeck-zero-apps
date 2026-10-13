/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the fake /sys and /dev tree (see fake_sys.hpp).
 */
#include "fake_sys.hpp"

#include "fsutil.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace sc {
namespace fake {
namespace {

bool put(const std::string &path, const std::string &text)
{
    return fs::make_dirs(fs::parent(path)) && fs::write_file(path, text + "\n");
}

bool make_node(const Roots &roots, const std::string &tty, const std::string &target, unsigned mode)
{
    const std::string node = roots.dev + "/" + tty;
    if (!target.empty()) return fs::make_link(target, node);
    unlink(node.c_str());
    if (!fs::write_file(node, "")) return false;
    return chmod(node.c_str(), static_cast<mode_t>(mode)) == 0;
}

std::string hex4(std::uint16_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04x", v);
    return buf;
}

} // namespace

Roots make_roots(const std::string &base)
{
    Roots roots;
    roots.sys = base + "/sys";
    roots.dev = base + "/dev";
    fs::make_dirs(roots.sys + "/class/tty");
    fs::make_dirs(roots.dev + "/serial/by-id");
    return roots;
}

bool add_usb_tty(const Roots &roots, const UsbTty &port)
{
    const std::string usb = roots.sys + "/devices/usb1/1-" + std::to_string(port.usb_port);
    const std::string iface = usb + "/1-" + std::to_string(port.usb_port) + ":1." +
                              std::to_string(std::atoi(port.interface.c_str()));
    bool ok = put(usb + "/idVendor", hex4(port.vid)) && put(usb + "/idProduct", hex4(port.pid));
    if (!port.manufacturer.empty()) ok = ok && put(usb + "/manufacturer", port.manufacturer);
    if (!port.product.empty()) ok = ok && put(usb + "/product", port.product);
    if (!port.serial.empty()) ok = ok && put(usb + "/serial", port.serial);
    ok = ok && put(iface + "/bInterfaceNumber", port.interface);
    const bool acm = port.tty.compare(0, 6, "ttyACM") == 0;
    std::string device = acm ? iface : iface + "/" + port.tty;
    ok = ok && fs::make_dirs(device);
    const std::string driver_dir =
        roots.sys + (acm ? "/bus/usb/drivers/" : "/bus/usb-serial/drivers/") + (port.driver.empty() ? "x" : port.driver);
    ok = ok && fs::make_dirs(driver_dir) && fs::make_link(driver_dir, device + "/driver");
    const std::string cls = roots.sys + "/class/tty/" + port.tty;
    ok = ok && fs::make_dirs(cls) && fs::make_link(device, cls + "/device");
    ok = ok && make_node(roots, port.tty, port.node_target, port.node_mode);
    if (!port.by_id.empty()) {
        fs::make_dirs(roots.dev + "/serial/by-id");
        ok = ok && fs::make_link("../../" + port.tty, roots.dev + "/serial/by-id/" + port.by_id);
    }
    return ok;
}

bool add_onboard_tty(const Roots &roots, const std::string &tty, const std::string &driver, int type,
                     const std::string &node_target)
{
    const std::string device = roots.sys + "/devices/platform/" + tty + "-uart";
    const std::string driver_dir = roots.sys + "/bus/platform/drivers/" + driver;
    const std::string cls = roots.sys + "/class/tty/" + tty;
    return fs::make_dirs(device) && fs::make_dirs(driver_dir) && fs::make_link(driver_dir, device + "/driver") &&
           fs::make_dirs(cls) && fs::make_link(device, cls + "/device") && put(cls + "/type", std::to_string(type)) &&
           make_node(roots, tty, node_target, 0666);
}

bool add_virtual_tty(const Roots &roots, const std::string &tty)
{
    const std::string cls = roots.sys + "/class/tty/" + tty;
    return fs::make_dirs(cls) && make_node(roots, tty, "", 0666);
}

bool add_alias(const Roots &roots, const std::string &alias, const std::string &tty)
{
    return fs::make_link(tty, roots.dev + "/" + alias);
}

bool remove_tty(const Roots &roots, const std::string &tty)
{
    const std::string by_id = roots.dev + "/serial/by-id";
    for (const std::string &link : fs::list_dir(by_id))
        if (fs::base_name(fs::read_link(by_id + "/" + link)) == tty) unlink((by_id + "/" + link).c_str());
    unlink((roots.dev + "/" + tty).c_str());
    return fs::remove_tree(roots.sys + "/class/tty/" + tty);
}

} // namespace fake
} // namespace sc
