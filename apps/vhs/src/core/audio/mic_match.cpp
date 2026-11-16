/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: which ALSA card is the camera's microphone (see mic_match.hpp).
 */
#include "mic_match.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <climits>
#include <cstdlib>
#include <fstream>

namespace vhs {
namespace {

std::string real(const std::string &path)
{
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf)) return "";
    return buf;
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string parent(const std::string &dir)
{
    const std::size_t s = dir.find_last_of('/');
    if (s == std::string::npos || s == 0) return "";
    return dir.substr(0, s);
}

std::string read_line(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    if (!f || !std::getline(f, s)) return "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

/* The USB device directory (holding idVendor) at or above dir. */
std::string usb_device_above(std::string dir)
{
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        if (exists(dir + "/idVendor")) return dir;
        dir = parent(dir);
    }
    return "";
}

} // namespace

MicCard camera_mic(const std::string &sys_root, const std::string &camera_usb_dir)
{
    MicCard m;
    const std::string cam = real(camera_usb_dir);
    if (cam.empty()) return m;
    const std::string cls = sys_root + "/sys/class/sound";
    DIR *d = opendir(cls.c_str());
    if (!d) return m;
    int best = INT_MAX;
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() < 5 || name.compare(0, 4, "card") != 0) continue;
        char *end = nullptr;
        const long n = std::strtol(name.c_str() + 4, &end, 10);
        if (!end || *end != '\0' || n < 0) continue;
        const std::string dev = usb_device_above(real(cls + "/" + name + "/device"));
        if (dev.empty() || dev != cam) continue;
        if (n >= best) continue; /* two cards on one camera: the lower number (stable order on one boot) */
        std::string id = read_line(sys_root + "/proc/asound/" + name + "/id");
        if (id.empty()) id = read_line(cls + "/" + name + "/id");
        if (id.empty()) continue;
        best = static_cast<int>(n);
        m.found = true;
        m.usb_dir = dev;
        m.card = CardInfo();
        m.card.index = static_cast<int>(n);
        m.card.id = id;
        m.card.capture = true;
    }
    closedir(d);
    return m;
}

MicCard camera_mic_by_port(AlsaApi &api, const std::string &devpath)
{
    MicCard m;
    if (devpath.empty()) return m;
    for (const CardInfo &c : api.cards()) {
        if (!c.capture || c.usb_port != devpath) continue;
        m.found = true;
        m.card = c;
        return m;
    }
    return m;
}

} // namespace vhs
