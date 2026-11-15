/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: photos on disk (see photos.hpp).
 */
#include "photos.hpp"

#include "atomic_file.hpp"
#include "fileops.hpp"
#include "naming.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vhs {

std::string expand_home(const std::string &folder)
{
    if (folder == "~" || folder.compare(0, 2, "~/") == 0) {
        const char *home = std::getenv("HOME");
        return std::string(home && home[0] ? home : ".") + folder.substr(1);
    }
    return folder;
}

int ensure_folder(FileOps &ops, const std::string &folder)
{
    if (path_exists(ops, folder)) return 0;
    const int r = make_dirs(ops, folder);
    if (r != 0) return r;
    if (::chmod(folder.c_str(), 0700) != 0) return -errno;
    return 0;
}

PhotoSave save_photo(FileOps &ops, const std::string &folder, const std::uint8_t *frame, std::size_t n, ExifFields f,
                     std::int64_t wall_s, bool synchronized, bool utc)
{
    PhotoSave s;
    f.datetime = synchronized ? exif_datetime(wall_s, utc) : std::string();
    std::vector<std::uint8_t> file;
    if (!make_photo(frame, n, f, file, &s.error, &s.facts)) return s;
    int r = ensure_folder(ops, folder);
    if (r != 0) {
        s.error = std::string("the folder: ") + std::strerror(-r);
        return s;
    }
    int number = 0;
    r = next_scene_number(ops, folder, number);
    if (r != 0) {
        s.error = std::string("the counter: ") + std::strerror(-r);
        return s;
    }
    SceneName name;
    name.prefix = kPhotoPrefix;
    name.number = number;
    name.when = when_text(wall_s, synchronized, utc);
    name.ext = "jpg";
    s.name = format_scene_name(name);
    s.path = folder + "/" + s.name;
    r = atomic_write_file(ops, s.path, std::string(reinterpret_cast<const char *>(file.data()), file.size()));
    if (r != 0) {
        s.error = std::strerror(-r);
        return s;
    }
    s.bytes = file.size();
    s.ok = true;
    return s;
}

std::vector<PhotoEntry> list_photos(FileOps &ops, const std::string &folder)
{
    std::vector<PhotoEntry> out;
    std::vector<std::string> names;
    if (ops.list_dir(folder, names) != 0) return out;
    for (const std::string &name : names) {
        if (name.empty() || name[0] == '.') continue;
        const std::size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) continue;
        std::string ext = name.substr(dot + 1);
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != "jpg" && ext != "jpeg") continue;
        PhotoEntry e;
        e.name = name;
        e.path = folder + "/" + name;
        StatInfo st;
        if (ops.stat(e.path, st) != 0) continue;
        e.size = st.size;
        e.mtime = st.mtime;
        SceneName sn;
        if (parse_scene_name(name, sn)) e.number = sn.number;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const PhotoEntry &a, const PhotoEntry &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        if (a.number != b.number) return a.number > b.number;
        return a.name > b.name;
    });
    return out;
}

std::string photo_meta(int w, int h, std::uint64_t bytes, std::int64_t mtime, bool utc)
{
    char size[32];
    if (bytes >= 1024ull * 1024ull) std::snprintf(size, sizeof size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(size, sizeof size, "%llu KB", static_cast<unsigned long long>((bytes + 512) / 1024));
    std::string s = std::to_string(w) + "x" + std::to_string(h) + ", " + size;
    const std::time_t t = static_cast<std::time_t>(mtime);
    struct tm tmv;
    char when[48];
    if ((utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv)) && std::strftime(when, sizeof when, "%b %e %Y %H:%M", &tmv) > 0) {
        std::string w2 = when;
        /* %e pads the day with a space: "Nov  4" -> "Nov 4" */
        const std::size_t dbl = w2.find("  ");
        if (dbl != std::string::npos) w2.erase(dbl, 1);
        s += ", " + w2;
    }
    return s;
}

} // namespace vhs
