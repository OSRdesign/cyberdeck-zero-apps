/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: settings and files (see settings.hpp).
 */
#include "settings.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace mmw {

std::string serialize_settings(const Settings &s)
{
    std::string out;
    out += std::string("units=") + unit_mode_name(s.units) + "\n";
    out += std::string("orientation=") + orientation_name(s.orientation) + "\n";
    out += "range=" + (s.range_m == 0 ? std::string("auto") : std::to_string(s.range_m)) + "\n";
    out += std::string("trails=") + (s.trails ? "on" : "off") + "\n";
    out += std::string("hide_held=") + (s.hide_held ? "on" : "off") + "\n";
    if (s.sensor != kSensorAuto) out += "sensor=" + s.sensor + "\n"; /* only when not Auto: old files stay as they were */
    if (s.flip_angle) out += "flip_angle=on\n"; /* likewise only when on */
    return out;
}

Settings parse_settings(const std::string &text)
{
    Settings s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "units") s.units = unit_mode_from_name(value);
        else if (key == "orientation") {
            if (value == "mirror") s.orientation = Orientation::Mirror;
            else if (value == "rotate180") s.orientation = Orientation::Rotate180;
            else s.orientation = Orientation::Normal;
        } else if (key == "range") {
            const int r = std::atoi(value.c_str());
            s.range_m = (r == 2 || r == 4 || r == 6) ? r : 0;
        } else if (key == "trails") {
            s.trails = value != "off";
        } else if (key == "hide_held") {
            s.hide_held = value == "on";
        } else if (key == "flip_angle") {
            s.flip_angle = value == "on";
        } else if (key == "sensor") {
            s.sensor = valid_sensor_type(value.c_str()) ? value : std::string(kSensorAuto); /* unknown: Auto */
        }
    }
    return s;
}

const char *units_label(UnitMode u) { return u == UnitMode::MmCms ? "Millimetres" : "Metres"; }

const char *orientation_label(Orientation o)
{
    switch (o) {
    case Orientation::Mirror: return "Mirror";
    case Orientation::Rotate180: return "Rotate 180";
    default: return "Normal";
    }
}

const char *orientation_name(Orientation o)
{
    switch (o) {
    case Orientation::Mirror: return "mirror";
    case Orientation::Rotate180: return "rotate180";
    default: return "normal";
    }
}

std::string range_label(int range_m) { return range_m == 0 ? std::string("Auto") : std::to_string(range_m) + " m"; }

UnitMode next_units(UnitMode u) { return u == UnitMode::MetresKmh ? UnitMode::MmCms : UnitMode::MetresKmh; }

Orientation next_orientation(Orientation o)
{
    switch (o) {
    case Orientation::Normal: return Orientation::Mirror;
    case Orientation::Mirror: return Orientation::Rotate180;
    default: return Orientation::Normal;
    }
}

int next_range_setting(int range_m)
{
    switch (range_m) {
    case 0: return 2;
    case 2: return 4;
    case 4: return 6;
    default: return 0;
    }
}

std::string config_dir()
{
    if (const char *d = std::getenv("MMWAVE_CONFIG_DIR"); d && *d) return d;
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/mmwave-radar";
    if (const char *h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config/mmwave-radar";
    return std::string();
}

bool make_dirs(const std::string &path)
{
    if (path.empty()) return false;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        const std::string cur = path.substr(0, next);
        if (!cur.empty() && ::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
        pos = next + 1;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_small_file(const std::string &path, std::string &out, std::size_t max_bytes)
{
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            out.clear();
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > max_bytes) {
            ::close(fd);
            out.clear();
            return false;
        }
    }
    ::close(fd);
    return true;
}

bool write_file_atomic(const std::string &path, const std::string &data)
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    const bool synced = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!synced || !closed || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    const std::size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

} // namespace mmw
