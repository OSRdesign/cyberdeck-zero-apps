/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: a camera's USB identity (see identity.hpp).
 */
#include "identity.hpp"

#include <climits>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace vhs {
namespace {

std::string read_attr(const std::string &path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string real(const std::string &path)
{
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf)) return "";
    return buf;
}

std::string parent(const std::string &dir)
{
    const std::size_t s = dir.find_last_of('/');
    if (s == std::string::npos || s == 0) return "";
    return dir.substr(0, s);
}

std::string base(const std::string &path)
{
    const std::size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string link_base(const std::string &link)
{
    char buf[PATH_MAX];
    const ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return base(buf);
}

} // namespace

std::string CamIdentity::key() const
{
    if (!valid()) return "";
    if (!serial.empty()) return vid + ":" + pid + ":" + serial;
    return vid + ":" + pid + "@" + devpath;
}

std::string video_name_of(const std::string &node_path)
{
    const std::string r = real(node_path);
    return base(r.empty() ? node_path : r);
}

std::string usb_device_dir(const std::string &sys_root, const std::string &video_name)
{
    if (video_name.empty()) return "";
    std::string dir = real(sys_root + "/sys/class/video4linux/" + video_name + "/device");
    for (int i = 0; i < 4 && !dir.empty(); ++i) { /* the interface (1-1.2:1.0), then the device (1-1.2) */
        if (exists(dir + "/idVendor")) return dir;
        dir = parent(dir);
    }
    return "";
}

std::string host_controller_driver(const std::string &usb_dev_dir)
{
    if (usb_dev_dir.empty()) return "";
    std::string hub = usb_dev_dir;
    for (int i = 0; i < 16; ++i) {
        const std::string up = parent(hub);
        if (up.empty() || !exists(up + "/idVendor")) break;
        hub = up;
    }
    const std::string hc = parent(hub);
    return hc.empty() ? "" : link_base(hc + "/driver");
}

CamIdentity identity_for_node(const std::string &sys_root, const std::string &node_path)
{
    CamIdentity id;
    const std::string dev = usb_device_dir(sys_root, video_name_of(node_path));
    if (dev.empty()) return id;
    id.vid = read_attr(dev + "/idVendor");
    id.pid = read_attr(dev + "/idProduct");
    id.serial = read_attr(dev + "/serial");
    id.manufacturer = read_attr(dev + "/manufacturer");
    id.product = read_attr(dev + "/product");
    id.devpath = base(dev);
    id.hc_driver = host_controller_driver(dev);
    return id;
}

bool bus_needs_replug(const std::string &d)
{
    return d.compare(0, 3, "dwc") == 0; /* dwc_otg, dwc2 */
}

std::string node_driver(const std::string &sys_root, const std::string &video_name)
{
    return link_base(sys_root + "/sys/class/video4linux/" + video_name + "/device/driver");
}

} // namespace vhs
