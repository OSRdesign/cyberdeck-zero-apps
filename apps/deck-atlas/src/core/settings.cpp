/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: settings and files (see settings.hpp).
 */
#include "settings.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace atlas {

namespace {

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool parse_double(const std::string &s, double &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parse_int(const std::string &s, int &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0' || v < -1000000 || v > 1000000) return false;
    out = static_cast<int>(v);
    return true;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

std::string scale_name(float s)
{
    if (s <= 0.0f) return "auto";
    if (s < 1.25f) return "1x";
    if (s < 1.75f) return "1.5x";
    return "2x";
}

} // namespace

float snap_scale(float s)
{
    if (!(s > 0.0f)) return 0.0f;
    if (s < 1.25f) return 1.0f;
    if (s < 1.75f) return 1.5f;
    return 2.0f;
}

bool valid_cache_mb(int mb) { return mb == 0 || mb == 128 || mb == 256 || mb == 512 || mb == 1024; }

bool valid_id(const std::string &id)
{
    if (id.empty() || id.size() > 63) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string serialize_settings(const Settings &s)
{
    std::string out = "v=1\n";
    out += std::string("units=") + (s.units == Units::Mi ? "mi" : "km") + "\n";
    out += "scale=" + scale_name(s.scale) + "\n";
    out += "cache=" + (s.cache_mb == 0 ? std::string("auto") : std::to_string(s.cache_mb)) + "\n";
    out += std::string("offline=") + (s.offline ? "on" : "off") + "\n";
    if (!s.source.empty() && valid_id(s.source)) out += "source=" + s.source + "\n";
    if (s.view.set) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "view=%.6f,%.6f,%d\n", s.view.lat, s.view.lon, s.view.zoom);
        out += buf;
    }
    if (!s.terms.empty()) {
        out += "terms=";
        for (std::size_t i = 0; i < s.terms.size(); ++i) out += (i ? "," : "") + s.terms[i];
        out += "\n";
    }
    return out;
}

Settings parse_settings(const std::string &text)
{
    Settings s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "units") {
            s.units = value == "mi" ? Units::Mi : Units::Km;
        } else if (key == "scale") {
            if (value == "1x" || value == "1") s.scale = 1.0f;
            else if (value == "1.5x" || value == "1.5") s.scale = 1.5f;
            else if (value == "2x" || value == "2") s.scale = 2.0f;
            else s.scale = 0.0f;
        } else if (key == "cache") {
            int mb = 0;
            s.cache_mb = (value != "auto" && parse_int(value, mb) && valid_cache_mb(mb)) ? mb : 0;
        } else if (key == "offline") {
            s.offline = value == "on";
        } else if (key == "source") {
            s.source = valid_id(value) ? value : std::string();
        } else if (key == "view") {
            const std::vector<std::string> p = split(value, ',');
            double lat = 0, lon = 0;
            int z = 0;
            if (p.size() == 3 && parse_double(p[0], lat) && parse_double(p[1], lon) && parse_int(p[2], z) &&
                lat >= -85.06 && lat <= 85.06 && lon >= -180.0 && lon <= 180.0 && z >= 0 && z <= 24) {
                s.view.set = true;
                s.view.lat = lat;
                s.view.lon = lon;
                s.view.zoom = z;
            }
        } else if (key == "terms") {
            s.terms.clear();
            for (const std::string &id : split(value, ','))
                if (valid_id(id)) accept_terms(s, id);
        }
    }
    return s;
}

Units next_units(Units u) { return u == Units::Km ? Units::Mi : Units::Km; }

float next_scale(float s)
{
    const float v = snap_scale(s);
    if (v == 0.0f) return 1.0f;
    if (v == 1.0f) return 1.5f;
    if (v == 1.5f) return 2.0f;
    return 0.0f;
}

int next_cache_mb(int mb)
{
    switch (mb) {
    case 0: return 128;
    case 128: return 256;
    case 256: return 512;
    case 512: return 1024;
    default: return 0;
    }
}

bool terms_accepted(const Settings &s, const std::string &id)
{
    return std::binary_search(s.terms.begin(), s.terms.end(), id);
}

void accept_terms(Settings &s, const std::string &id)
{
    if (!valid_id(id) || terms_accepted(s, id)) return;
    s.terms.insert(std::upper_bound(s.terms.begin(), s.terms.end(), id), id);
}

std::string config_dir()
{
    if (const char *d = std::getenv("DECKATLAS_CONFIG_DIR"); d && *d) return d;
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/deck-atlas";
    if (const char *h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config/deck-atlas";
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

} // namespace atlas
