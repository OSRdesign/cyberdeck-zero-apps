/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the RTL-SDR dongle check (see sdr.hpp). The id table is the one of the 0.1 intro screen.
 */
#include "sdr.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <dirent.h>

namespace viz {

namespace {

const UsbId kSdrIds[] = {
    {"0bda", "2832"}, {"0bda", "2838"}, {"0413", "6680"}, {"0413", "6f0f"}, {"0458", "707f"}, {"0ccd", "00a9"},
    {"0ccd", "00b3"}, {"0ccd", "00b4"}, {"0ccd", "00b5"}, {"0ccd", "00b7"}, {"0ccd", "00b8"}, {"0ccd", "00b9"},
    {"0ccd", "00c0"}, {"0ccd", "00c6"}, {"0ccd", "00d3"}, {"0ccd", "00d7"}, {"0ccd", "00e0"}, {"1554", "5020"},
    {"15f4", "0131"}, {"15f4", "0133"}, {"185b", "0620"}, {"185b", "0650"}, {"185b", "0680"}, {"1b80", "d393"},
    {"1b80", "d394"}, {"1b80", "d395"}, {"1b80", "d397"}, {"1b80", "d398"}, {"1b80", "d39d"}, {"1b80", "d3a4"},
    {"1b80", "d3a8"}, {"1b80", "d3af"}, {"1b80", "d3b0"}, {"1d19", "1101"}, {"1d19", "1102"}, {"1d19", "1103"},
    {"1d19", "1104"}, {"1f4d", "a803"}, {"1f4d", "b803"}, {"1f4d", "c803"}, {"1f4d", "d286"}, {"1f4d", "d803"},
};

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string read_line(const std::string &path)
{
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')) line.pop_back();
    return line;
}

} // namespace

const UsbId *sdr_ids(std::size_t *count)
{
    if (count) *count = sizeof(kSdrIds) / sizeof(kSdrIds[0]);
    return kSdrIds;
}

bool is_sdr_id(const std::string &vid, const std::string &pid)
{
    const std::string v = lower(vid), p = lower(pid);
    for (const UsbId &id : kSdrIds)
        if (v == id.vid && p == id.pid) return true;
    return false;
}

std::string usb_devices_dir()
{
    const char *env = std::getenv("VIZ_USB_DIR");
    return env && env[0] ? std::string(env) : std::string("/sys/bus/usb/devices");
}

std::string find_sdr(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return "";
    std::vector<std::string> names;
    while (dirent *entry = readdir(d))
        if (entry->d_name[0] != '.') names.push_back(entry->d_name);
    closedir(d);
    std::sort(names.begin(), names.end()); /* the same dongle first every time when there are two */
    for (const std::string &name : names) {
        const std::string base = dir + "/" + name;
        const std::string vid = read_line(base + "/idVendor");
        if (vid.empty()) continue;
        const std::string pid = read_line(base + "/idProduct");
        if (!is_sdr_id(vid, pid)) continue;
        const std::string product = read_line(base + "/product");
        const std::string maker = read_line(base + "/manufacturer");
        std::string found = !maker.empty() && maker != "Realtek" && !product.empty() ? maker + " " + product : product;
        if (found.empty()) found = lower(vid) + ":" + lower(pid);
        return found;
    }
    return "";
}

} // namespace viz
