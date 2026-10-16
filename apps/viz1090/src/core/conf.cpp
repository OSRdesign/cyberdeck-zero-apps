/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the settings file (see conf.hpp).
 */
#include "conf.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace viz {

namespace {

bool starts_with(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

/* RECENT1= .. RECENT9= (only 1..5 are written; any other number is dropped too, so the list never grows) */
bool is_recent_line(const std::string &line)
{
    return line.size() > 7 && starts_with(line, "RECENT") && line[6] >= '0' && line[6] <= '9' &&
           (line[7] == '=' || (line.size() > 8 && line[7] >= '0' && line[7] <= '9' && line[8] == '='));
}

bool is_owned_line(const std::string &line)
{
    return starts_with(line, "LAT=") || starts_with(line, "LON=") || starts_with(line, "AIRPORT=") ||
           is_recent_line(line);
}

std::string strip_cr(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

bool valid_code(const std::string &code)
{
    if (code.size() > kCodeMax) return false;
    for (const char c : code)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

} // namespace

std::string conf_path()
{
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    const std::string in_home = "/.config/cardputerzero/viz1090.conf"; /* the launcher's folder name (kept from 0.1) */
    if (xdg && xdg[0]) return std::string(xdg) + in_home.substr(std::strlen("/.config")); /* $XDG_CONFIG_HOME/... */
    return std::string(home && home[0] ? home : "/tmp") + in_home;
}

std::string recent_value(const Place &place)
{
    return place.code + "," + degrees_text(place.lat) + "," + degrees_text(place.lon);
}

bool parse_recent(const std::string &value, Place &out)
{
    const std::size_t a = value.find(',');
    if (a == std::string::npos) return false;
    const std::size_t b = value.find(',', a + 1);
    if (b == std::string::npos || value.find(',', b + 1) != std::string::npos) return false;
    Place p;
    p.code = value.substr(0, a);
    if (!valid_code(p.code)) return false;
    if (!parse_degrees(value.substr(a + 1, b - a - 1), kLatLimit, p.lat)) return false;
    if (!parse_degrees(value.substr(b + 1), kLonLimit, p.lon)) return false;
    out = p;
    return true;
}

Conf parse_conf(const std::string &text)
{
    Conf conf;
    std::istringstream in(text);
    std::string line;
    std::vector<std::pair<int, Place>> recent;
    while (std::getline(in, line)) {
        line = strip_cr(line);
        if (starts_with(line, "LAT=")) conf.lat = line.substr(4);
        else if (starts_with(line, "LON=")) conf.lon = line.substr(4);
        else if (starts_with(line, "AIRPORT=")) conf.airport = line.substr(8);
        else if (is_recent_line(line)) {
            const std::size_t eq = line.find('=');
            const int n = std::atoi(line.substr(6, eq - 6).c_str());
            Place p;
            if (n >= 1 && n <= static_cast<int>(kRecentMax) && parse_recent(line.substr(eq + 1), p))
                recent.emplace_back(n, p);
        }
    }
    /* in the order of their numbers (a file edited by hand may have them in any order) */
    for (int n = 1; n <= static_cast<int>(kRecentMax); ++n)
        for (const auto &r : recent)
            if (r.first == n) {
                conf.recent.push_back(r.second);
                break;
            }
    return conf;
}

std::string render_conf(const std::string &old_text, const Place &place, const std::vector<Place> &recent)
{
    std::string out = "LAT=" + degrees_text(place.lat) + "\nLON=" + degrees_text(place.lon) + "\n";
    if (!place.code.empty() && valid_code(place.code)) out += "AIRPORT=" + place.code + "\n";
    std::istringstream in(old_text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string bare = strip_cr(line);
        if (bare.empty() || is_owned_line(bare)) continue;
        out += line + "\n";
    }
    std::size_t n = 0;
    for (const Place &p : recent) {
        if (n >= kRecentMax) break;
        if (!valid_code(p.code)) continue;
        out += "RECENT" + std::to_string(++n) + "=" + recent_value(p) + "\n";
    }
    return out;
}

std::string read_text(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    if (in) ss << in.rdbuf();
    return ss.str();
}

std::string write_text_atomic(const std::string &path, const std::string &text)
{
    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = path.substr(0, slash);
        const std::size_t up = dir.rfind('/');
        if (up != std::string::npos && up > 0) mkdir(dir.substr(0, up).c_str(), 0755); /* ~/.config */
        mkdir(dir.c_str(), 0755);
    }
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(getpid()));
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return "Cannot write " + path + ": " + std::strerror(errno);
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const std::string why = std::strerror(errno);
            close(fd);
            unlink(tmp.c_str());
            return "Cannot write " + path + ": " + why;
        }
        done += static_cast<std::size_t>(n);
    }
    fsync(fd);
    if (close(fd) != 0 || rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        unlink(tmp.c_str());
        return "Cannot write " + path + ": " + why;
    }
    return std::string();
}

std::string save_conf(const std::string &path, const Place &place, const std::vector<Place> &recent)
{
    return write_text_atomic(path, render_conf(read_text(path), place, recent));
}

} // namespace viz
