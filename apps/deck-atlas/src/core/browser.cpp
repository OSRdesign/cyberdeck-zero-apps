/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the import page's file browser (see browser.hpp).
 */
#include "browser.hpp"

#include "format.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace atlas {

bool PosixLister::list(const std::string &path, std::vector<DirEntry> &out, std::string *why)
{
    out.clear();
    DIR *d = ::opendir(path.c_str());
    if (!d) {
        if (why) *why = std::strerror(errno);
        return false;
    }
    while (struct dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (std::strcmp(n, ".") == 0 || std::strcmp(n, "..") == 0) continue;
        DirEntry de;
        de.name = n;
        struct stat st;
        if (::stat(path_join(path, n).c_str(), &st) != 0) continue; /* a dangling link, a vanished file */
        de.dir = S_ISDIR(st.st_mode);
        if (!de.dir && !S_ISREG(st.st_mode)) continue;
        de.size = de.dir ? 0 : static_cast<std::uint64_t>(st.st_size);
        out.push_back(std::move(de));
        if (out.size() > 20000) break; /* a pathological folder: enough to decide */
    }
    ::closedir(d);
    return true;
}

std::string path_join(const std::string &dir, const std::string &name)
{
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string path_parent(const std::string &path)
{
    if (path.empty() || path == "/") return "/";
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const std::size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

std::string path_name(const std::string &path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const std::size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string path_display(const std::string &path, const std::string &home)
{
    if (!home.empty() && home != "/") {
        if (path == home) return "~";
        if (path.size() > home.size() && path.compare(0, home.size(), home) == 0 && path[home.size()] == '/')
            return "~" + path.substr(home.size());
    }
    return path;
}

namespace {
std::string lower(const std::string &s)
{
    std::string o = s;
    for (char &c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool name_less(const DirEntry &a, const DirEntry &b)
{
    const std::string la = lower(a.name), lb = lower(b.name);
    return la != lb ? la < lb : a.name < b.name;
}
} // namespace

bool is_archive_name(const std::string &name)
{
    const std::string l = lower(name);
    return (ends_with(l, ".mbtiles") && l.size() > 8) || (ends_with(l, ".pmtiles") && l.size() > 8);
}

bool is_number(const std::string &s)
{
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool is_tile_file_name(const std::string &name)
{
    std::size_t i = 0;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') ++i;
    if (i == 0 || i > 9) return false;
    const std::string ext = lower(name.substr(i));
    static const char *const kExt[] = {"", ".png", ".jpg", ".jpeg", ".webp", ".pbf", ".mvt", ".png.tile", ".jpg.tile",
                                       ".jpeg.tile"};
    for (const char *e : kExt)
        if (ext == e) return true;
    return false;
}

bool looks_like_tile_folder(Lister &lister, const std::string &path)
{
    std::vector<DirEntry> zs;
    if (!lister.list(path, zs, nullptr)) return false;
    std::sort(zs.begin(), zs.end(), name_less);
    int tried_z = 0;
    for (const DirEntry &z : zs) {
        if (!z.dir || !is_number(z.name) || std::atoi(z.name.c_str()) > 30) continue;
        if (++tried_z > 3) break;
        std::vector<DirEntry> xs;
        if (!lister.list(path_join(path, z.name), xs, nullptr)) continue;
        int tried_x = 0;
        for (const DirEntry &x : xs) {
            if (!x.dir || !is_number(x.name)) continue;
            if (++tried_x > 3) break;
            std::vector<DirEntry> ys;
            if (!lister.list(path_join(path_join(path, z.name), x.name), ys, nullptr)) continue;
            for (const DirEntry &y : ys)
                if (!y.dir && is_tile_file_name(y.name)) return true;
        }
    }
    return false;
}

std::vector<BrowserRow> browser_roots(Lister &lister, const std::string &home, const std::string &user)
{
    std::vector<BrowserRow> rows;
    if (!home.empty()) rows.push_back({RowKind::Root, "Home folder", "~", home});
    auto add_mounts = [&](const std::string &base, bool per_user_level) {
        std::vector<DirEntry> es;
        if (!lister.list(base, es, nullptr)) return;
        std::sort(es.begin(), es.end(), name_less);
        for (const DirEntry &e : es) {
            if (!e.dir || e.name.empty() || e.name[0] == '.') continue;
            const std::string p = path_join(base, e.name);
            if (per_user_level && e.name == user) continue; /* /media/<user> is listed one level down */
            rows.push_back({RowKind::Root, e.name, "USB stick or card", p});
        }
    };
    if (!user.empty()) add_mounts(path_join("/media", user), false);
    add_mounts("/media", true);
    if (!user.empty()) add_mounts(path_join("/run/media", user), false);
    add_mounts("/mnt", false);
    /* the same folder twice (a link): keep the first */
    std::vector<BrowserRow> out;
    for (const BrowserRow &r : rows) {
        bool dup = false;
        for (const BrowserRow &o : out) dup = dup || o.path == r.path;
        if (!dup) out.push_back(r);
    }
    return out;
}

std::vector<BrowserRow> browser_rows(Lister &lister, const std::string &path, bool at_top, std::string *error)
{
    std::vector<BrowserRow> rows;
    if (error) error->clear();
    std::vector<DirEntry> es;
    std::string why;
    if (!lister.list(path, es, &why)) {
        if (error) *error = "Cannot open this folder: " + why;
        rows.push_back({RowKind::Up, at_top ? "Back to the start" : "Up one level", "", at_top ? "" : path_parent(path)});
        return rows;
    }
    if (looks_like_tile_folder(lister, path)) {
        int zooms = 0, zmin = 99, zmax = -1;
        for (const DirEntry &e : es) {
            if (!e.dir || !is_number(e.name)) continue;
            const int z = std::atoi(e.name.c_str());
            if (z > 30) continue;
            ++zooms;
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
        }
        rows.push_back({RowKind::ImportFolder, "Import this tile folder", zooms ? zoom_label(zmin, zmax) : "", path});
        rows.push_back({RowKind::Up, at_top ? "Back to the start" : "Up one level", "", at_top ? "" : path_parent(path)});
        return rows;
    }
    rows.push_back({RowKind::Up, at_top ? "Back to the start" : "Up one level", "", at_top ? "" : path_parent(path)});
    std::sort(es.begin(), es.end(), name_less);
    int shown = 0, more = 0, archives = 0;
    for (const DirEntry &e : es) {
        if (e.name.empty() || e.name[0] == '.' || !e.dir) continue;
        if (shown >= kMaxEntries) {
            ++more;
            continue;
        }
        rows.push_back({RowKind::Folder, e.name, "", path_join(path, e.name)});
        ++shown;
    }
    for (const DirEntry &e : es) {
        if (e.name.empty() || e.name[0] == '.' || e.dir || !is_archive_name(e.name)) continue;
        if (shown >= kMaxEntries) {
            ++more;
            continue;
        }
        rows.push_back({RowKind::Archive, e.name, bytes_label(e.size), path_join(path, e.name)});
        ++shown;
        ++archives;
    }
    if (more) rows.push_back({RowKind::Note, count_label(static_cast<std::uint64_t>(more)) + " more not shown", "", ""});
    if (shown == 0) rows.push_back({RowKind::Note, "No folders or map files here", "", ""});
    (void)archives;
    return rows;
}

std::uint64_t folder_bytes(const std::string &path, std::uint64_t *files, const std::function<bool()> &cancel)
{
    std::uint64_t total = 0, n = 0;
    PosixLister l;
    std::vector<DirEntry> zs, xs, ys;
    if (l.list(path, zs, nullptr)) {
        for (const DirEntry &z : zs) {
            if (!z.dir || !is_number(z.name)) continue;
            const std::string zp = path_join(path, z.name);
            if (!l.list(zp, xs, nullptr)) continue;
            for (const DirEntry &x : xs) {
                if (cancel && cancel()) break;
                if (!x.dir || !is_number(x.name)) continue;
                if (!l.list(path_join(zp, x.name), ys, nullptr)) continue;
                for (const DirEntry &y : ys) {
                    if (y.dir || !is_tile_file_name(y.name)) continue;
                    total += y.size;
                    ++n;
                }
            }
        }
    }
    if (files) *files = n;
    return total;
}

std::uint64_t free_bytes(const std::string &path)
{
    struct statvfs v;
    if (::statvfs(path.c_str(), &v) != 0) return 0;
    return static_cast<std::uint64_t>(v.f_bavail) * static_cast<std::uint64_t>(v.f_frsize ? v.f_frsize : v.f_bsize);
}

std::uint64_t file_size(const std::string &path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return static_cast<std::uint64_t>(st.st_size);
}

} // namespace atlas
