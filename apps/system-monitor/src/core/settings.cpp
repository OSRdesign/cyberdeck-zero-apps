/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the settings file (see settings.hpp).
 */
#include "settings.hpp"

#include "text.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace sm {
namespace {

bool make_dirs(const std::string &path)
{
    if (path.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

} // namespace

std::string Settings::serialize() const
{
    return "v=1\nperiod=" + std::to_string(period_s) + "\n";
}

Settings Settings::parse(const std::string &text)
{
    Settings s;
    for (const std::string &raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        std::uint64_t v = 0;
        if (key == "period" && parse_u64(value, v) && (v == 1 || v == 2 || v == 5)) s.period_s = static_cast<int>(v);
    }
    return s;
}

std::string settings_path()
{
    const char *over = std::getenv("SYSTEM_MONITOR_CONFIG");
    if (over && over[0]) return over;
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string base;
    if (xdg && xdg[0] == '/') base = xdg;
    else if (home && home[0]) base = std::string(home) + "/.config";
    else return std::string();
    return base + "/cyberdeckzero/system-monitor.conf";
}

Settings load_settings(const std::string &path)
{
    if (path.empty()) return Settings();
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return Settings();
    std::string text;
    char buf[512];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 65536) text.append(buf, n);
    std::fclose(f);
    return Settings::parse(text);
}

bool save_settings(const std::string &path, const Settings &settings)
{
    if (path.empty()) return false;
    const std::string dir = parent_of(path);
    if (!dir.empty()) make_dirs(dir);
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const std::string text = settings.serialize();
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    const bool synced = fsync(fd) == 0;
    close(fd);
    if (!ok || !synced || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace sm
