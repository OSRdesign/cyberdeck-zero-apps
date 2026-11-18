/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: roots, folder listing, playlists (see browse.hpp).
 */
#include "browse.hpp"

#include "net_share.hpp"
#include "roots.hpp"
#include "../playlist/m3u.hpp"
#include "../decode/track_decoder.hpp"
#include "../tags/tag_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace vinyl {

namespace {

std::string unescape_mount(const std::string &s) { /* /proc/mounts writes a space as \040 */
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            o += static_cast<char>(std::strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

bool real_fs(const std::string &t) {
    static const char *const ok[] = {"vfat", "exfat", "ntfs", "ntfs3", "fuseblk", "ext2", "ext3", "ext4", "btrfs",
                                     "xfs", "f2fs", "iso9660", "udf", "hfsplus", "nfs", "nfs4", "cifs", "smb3", "autofs"};
    for (const char *x : ok)
        if (t == x) return true;
    return false;
}

bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

std::string base_of(const std::string &p) {
    const size_t s = p.find_last_not_of('/');
    if (s == std::string::npos) return p;
    const size_t a = p.rfind('/', s);
    return p.substr(a == std::string::npos ? 0 : a + 1, s - (a == std::string::npos ? 0 : a + 1) + 1);
}

std::string upper(std::string s) {
    for (char &c : s)
        if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<Root> list_roots(const std::string &home, const std::string &user, const std::string &mounts) {
    std::vector<Root> roots;
    if (!home.empty()) roots.push_back({"Music", home + "/Music"});
    std::vector<std::string> prefixes;
    if (!user.empty()) {
        prefixes.push_back("/media/" + user + "/");
        prefixes.push_back("/run/media/" + user + "/");
    }
    prefixes.push_back("/mnt/");
    prefixes.push_back("/media/");
    std::istringstream in(mounts);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string dev, mp, type;
        if (!(ls >> dev >> mp >> type)) continue;
        mp = unescape_mount(mp);
        if (!real_fs(type)) continue;
        bool under = false;
        for (const std::string &p : prefixes)
            if (starts_with(mp, p) && mp.size() > p.size()) under = true;
        if (!under) continue;
        if (std::any_of(roots.begin(), roots.end(), [&](const Root &r) { return r.path == mp; })) continue;
        Root nr{base_of(mp), mp};
        nr.network = is_share_fs(type);
        roots.push_back(nr);
    }
    return roots;
}

std::vector<Root> system_roots() {
    const char *home = std::getenv("HOME");
    const char *user = std::getenv("USER");
    std::string mounts;
    std::ifstream f("/proc/mounts");
    if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        mounts = ss.str();
    }
    std::vector<Root> all = list_roots(home ? home : "", user ? user : "", mounts), roots;
    for (const Root &r : all) { /* a drive that is mounted but unreadable (pulled: a stale mount) is gone: no row */
        const bool drive = r.path.compare(0, 7, "/media/") == 0 || r.path.compare(0, 11, "/run/media/") == 0 || r.path.compare(0, 5, "/mnt/") == 0;
        /* a network share is no drive: never probed here (this runs on the UI thread; a hard NFS mount would hold it) */
        if (!drive || r.network || probe_root(r.path)) roots.push_back(r);
    }
    return roots;
}

bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ie = i, je = j;
            while (ie < a.size() && a[ie] == '0') ++ie;
            while (je < b.size() && b[je] == '0') ++je;
            size_t ia = ie, jb = je;
            while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) ++ia;
            while (jb < b.size() && std::isdigit(static_cast<unsigned char>(b[jb]))) ++jb;
            const size_t la = ia - ie, lb = jb - je;
            if (la != lb) return la < lb;
            const int c = a.compare(ie, la, b, je, lb);
            if (c != 0) return c < 0;
            i = ia;
            j = jb;
            continue;
        }
        const int la = ca < 0x80 ? std::tolower(ca) : ca, lb = cb < 0x80 ? std::tolower(cb) : cb;
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b;
}

int list_folder(const std::string &path, bool top, std::vector<Entry> &out, size_t cap) {
    out.clear();
    DIR *d = ::opendir(path.c_str());
    if (!d) return -errno;
    std::vector<Entry> folders, playlists, tracks;
    while (dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        const std::string full = path + "/" + name;
        bool dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            struct stat st;
            if (::stat(full.c_str(), &st) != 0) continue;
            dir = S_ISDIR(st.st_mode);
        }
        if (dir) folders.push_back({Entry::Kind::Folder, clean_utf8(name), full, ""});
        else if (is_playlist_name(name)) playlists.push_back({Entry::Kind::Playlist, clean_utf8(name), full, ""});
        else if (is_audio_name(name)) tracks.push_back({Entry::Kind::Track, clean_utf8(name), full, ""});
        if (folders.size() + playlists.size() + tracks.size() >= cap) break;
    }
    ::closedir(d);
    auto by_name = [](const Entry &x, const Entry &y) { return natural_less(x.name, y.name); };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(playlists.begin(), playlists.end(), by_name);
    std::sort(tracks.begin(), tracks.end(), by_name);
    if (!top) out.push_back({Entry::Kind::Up, "..", "", ""});
    out.insert(out.end(), folders.begin(), folders.end());
    out.insert(out.end(), playlists.begin(), playlists.end());
    out.insert(out.end(), tracks.begin(), tracks.end());
    return 0;
}

int list_folder_bounded(const std::string &path, bool top, std::vector<Entry> &out, size_t cap) {
    struct Result {
        int rc = 0;
        std::vector<Entry> entries;
    };
    auto res = std::make_shared<Result>();
    const BoundedRunner::Status st = net_runner().run("ls:" + path, [res, path, top, cap] { res->rc = list_folder(path, top, res->entries, cap); });
    out.clear();
    if (st != BoundedRunner::Status::Done) return -ETIMEDOUT;
    out = std::move(res->entries);
    return res->rc;
}

std::vector<Entry> root_entries(const std::vector<Root> &roots) {
    std::vector<Entry> out;
    for (const Root &r : roots) out.push_back({Entry::Kind::Root, r.label, r.path, ""});
    return out;
}

std::vector<std::string> folder_tracks(const std::vector<Entry> &entries) {
    std::vector<std::string> out;
    for (const Entry &e : entries)
        if (e.kind == Entry::Kind::Track) out.push_back(e.path);
    return out;
}

bool file_exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> parse_m3u(const std::string &text, const std::string &base_dir, bool (*exists)(const std::string &)) {
    /* P2c: the reading is core/playlist/m3u (Windows paths, Latin-1, file:// URLs); this keeps P1's contract: the files that are
     * there (all of them when `exists` is null), as absolute paths */
    m3u::ParseOptions opt;
    opt.base_dir = base_dir;
    opt.max_entries = kMaxEntries;
    if (!exists) opt.exists = [](const std::string &) { return true; };
    else opt.exists = [exists](const std::string &p) { return exists(p); };
    std::vector<std::string> out;
    for (const m3u::Entry &e : m3u::parse(text, opt))
        if (e.resolved && (!exists || exists(e.path))) out.push_back(e.path);
    return out;
}

const Root *root_of(const std::vector<Root> &roots, const std::string &path) {
    const Root *best = nullptr;
    for (const Root &r : roots) {
        if (path == r.path || (starts_with(path, r.path) && path.size() > r.path.size() && path[r.path.size()] == '/')) {
            if (!best || r.path.size() > best->path.size()) best = &r;
        }
    }
    return best;
}

std::string breadcrumb(const std::vector<Root> &roots, const std::string &path) {
    const Root *r = root_of(roots, path);
    if (!r) return upper(base_of(path));
    std::vector<std::string> parts;
    std::string rest = path.substr(r->path.size());
    std::istringstream in(rest);
    std::string part;
    while (std::getline(in, part, '/'))
        if (!part.empty()) parts.push_back(part);
    std::string out = upper(r->label);
    if (parts.size() > 2) {
        out += " / ...";
        parts.erase(parts.begin(), parts.end() - 2);
    }
    for (const std::string &p : parts) out += " / " + upper(clean_utf8(p));
    return out;
}

std::string parent_folder(const std::vector<Root> &roots, const std::string &path) {
    const Root *r = root_of(roots, path);
    if (!r || path == r->path) return "";
    const size_t s = path.rfind('/');
    if (s == std::string::npos || s < r->path.size()) return r->path;
    return path.substr(0, s);
}

}  // namespace vinyl
