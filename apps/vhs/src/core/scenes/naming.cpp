/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: file names and the shared counter (see naming.hpp).
 */
#include "naming.hpp"

#include "atomic_file.hpp"
#include "fileops.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

namespace vhs {
namespace {

bool all_digits(const std::string &s)
{
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool when_ok(const std::string &w)
{
    if (w == "nodate") return true;
    if (w.size() != 15 || w[8] != '-') return false;
    return all_digits(w.substr(0, 8)) && all_digits(w.substr(9));
}

} // namespace

SceneKind SceneName::kind() const
{
    return (ext == "jpg" || ext == "jpeg") ? SceneKind::Photo : SceneKind::Movie;
}

bool parse_scene_name(const std::string &file, SceneName &out)
{
    const std::size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return false;
    SceneName n;
    n.ext = lower(file.substr(dot + 1));
    const bool movie = n.ext == "mkv" || n.ext == "mp4" || n.ext == "mov";
    const bool photo = n.ext == "jpg" || n.ext == "jpeg";
    if (!movie && !photo) return false;
    std::string stem = file.substr(0, dot);
    /* [_pN] */
    std::size_t us = stem.find_last_of('_');
    if (movie && us != std::string::npos && stem.size() > us + 2 && stem[us + 1] == 'p' && all_digits(stem.substr(us + 2))) {
        n.part = std::atoi(stem.c_str() + us + 2);
        if (n.part < 2) return false;
        stem = stem.substr(0, us);
    }
    /* _<when> */
    us = stem.find_last_of('_');
    if (us == std::string::npos) return false;
    n.when = stem.substr(us + 1);
    if (!when_ok(n.when)) return false;
    stem = stem.substr(0, us);
    /* _<NNN> */
    us = stem.find_last_of('_');
    if (us == std::string::npos) return false;
    const std::string num = stem.substr(us + 1);
    if (num.size() < 3 || num.size() > 9 || !all_digits(num)) return false;
    n.number = std::atoi(num.c_str());
    n.prefix = stem.substr(0, us);
    if (!valid_prefix(n.prefix)) return false;
    if (photo && n.prefix != kPhotoPrefix) return false;
    out = n;
    return true;
}

std::string format_scene_name(const SceneName &n)
{
    char num[16];
    std::snprintf(num, sizeof num, "%03d", n.number);
    std::string s = n.prefix + "_" + num + "_" + n.when;
    if (n.part >= 2) s += "_p" + std::to_string(n.part);
    return s + "." + n.ext;
}

std::string when_text(std::int64_t wall_s, bool synchronized, bool utc)
{
    if (!synchronized) return "nodate";
    const std::time_t t = static_cast<std::time_t>(wall_s);
    struct tm tmv;
    if (!(utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv))) return "nodate";
    char buf[32];
    if (std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tmv) != 15) return "nodate";
    return buf;
}

bool clock_synchronized(const std::string &marker)
{
    struct stat st;
    return stat(marker.c_str(), &st) == 0;
}

int highest_number(const std::vector<std::string> &names)
{
    int best = 0;
    for (const std::string &raw : names) {
        std::string name = raw;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) name = name.substr(0, name.size() - 5) + ".mkv";
        SceneName n;
        if (parse_scene_name(name, n)) best = std::max(best, n.number);
    }
    return best;
}

int next_scene_number(FileOps &ops, const std::string &folder, int &number)
{
    std::string text;
    int counter = 0;
    if (read_small_file(ops, folder + "/.counter", text, 64) == 0) counter = std::max(0, std::atoi(text.c_str()));
    std::vector<std::string> names, journal;
    ops.list_dir(folder, names);
    ops.list_dir(folder + "/.journal", journal);
    names.insert(names.end(), journal.begin(), journal.end());
    const int next = std::max(counter, highest_number(names)) + 1;
    const int r = atomic_write_file(ops, folder + "/.counter", std::to_string(next) + "\n");
    if (r != 0) return r;
    number = next;
    return 0;
}

} // namespace vhs
