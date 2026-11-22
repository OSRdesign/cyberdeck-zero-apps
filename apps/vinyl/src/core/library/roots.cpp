/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: roots, mounts and UUIDs (see roots.hpp).
 */
#include "roots.hpp"

#include "browse.hpp"
#include "library.hpp"
#include "net_share.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace vinyl {

namespace {

std::string unescape(const std::string &s) { /* /proc/mounts writes a space as \040 */
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
            o += static_cast<char>(std::strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

bool within(const std::string &path, const std::string &dir) {
    if (dir == "/") return !path.empty() && path[0] == '/';
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

std::string real(const std::string &p) {
    char buf[PATH_MAX];
    return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

std::string base_name(const std::string &p) {
    const size_t e = p.find_last_not_of('/');
    if (e == std::string::npos) return p;
    const size_t s = p.rfind('/', e);
    return p.substr(s == std::string::npos ? 0 : s + 1, e - (s == std::string::npos ? 0 : s + 1) + 1);
}

}  // namespace

std::vector<MountEntry> parse_mounts(const std::string &text) {
    std::vector<MountEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        MountEntry m;
        if (!(ls >> m.dev >> m.dir >> m.type)) continue;
        m.dev = unescape(m.dev);
        m.dir = unescape(m.dir);
        out.push_back(m);
    }
    return out;
}

std::string read_mounts() {
    std::ifstream f("/proc/self/mounts");
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const MountEntry *mount_of(const std::vector<MountEntry> &mounts, const std::string &path) {
    const MountEntry *best = nullptr;
    for (const MountEntry &m : mounts) /* a later mount over the same point hides the earlier: >= keeps the last */
        if (within(path, m.dir) && (!best || m.dir.size() >= best->dir.size())) best = &m;
    return best;
}

std::string device_uuid(const std::string &dev, const std::string &by_uuid_dir) {
    if (dev.empty() || dev[0] != '/') return "";
    const std::string target = real(dev);
    if (target.empty()) return "";
    DIR *d = ::opendir(by_uuid_dir.c_str());
    if (!d) return "";
    std::string found;
    while (dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (real(by_uuid_dir + "/" + e->d_name) == target) {
            found = e->d_name;
            break;
        }
    }
    ::closedir(d);
    return found;
}

std::vector<RootSpec> library_roots(const std::string &home, const std::string &user, const std::string &mounts_text,
                                    const std::vector<std::string> &folders) {
    std::vector<RootSpec> out;
    for (const Root &r : list_roots(home, user, mounts_text)) out.push_back({r.path, r.label});
    for (std::string f : folders) {
        while (f.size() > 1 && f.back() == '/') f.pop_back();
        if (f.empty() || f[0] != '/') continue;
        const bool overlaps = std::any_of(out.begin(), out.end(), [&](const RootSpec &s) {
            return within(f, s.path) || within(s.path, f);
        });
        if (!overlaps) out.push_back({f, base_name(f)});
    }
    return out;
}

std::vector<RootRow> load_roots(Db &db) {
    std::vector<RootRow> out;
    Stmt s = db.prepare("select id, path, coalesce(label, ''), coalesce(uuid, ''), coalesce(mount, ''), coalesce(online, 1), "
                        "coalesce(last_scan, 0) from roots order by id");
    while (s.step() == sqlite::kRow) {
        RootRow r;
        r.id = s.i64(0);
        r.path = s.text(1);
        r.label = s.text(2);
        r.uuid = s.text(3);
        r.mount = s.text(4);
        r.online = s.i32(5) != 0;
        r.last_scan = s.i64(6);
        out.push_back(r);
    }
    return out;
}

bool root_readable(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    DIR *d = ::opendir(path.c_str());
    if (!d) return false;
    errno = 0;
    (void)::readdir(d);
    const int e = errno;
    ::closedir(d);
    return !(e == EIO || e == ENODEV || e == ENXIO || e == ESTALE || e == ENOTCONN || e == ETIMEDOUT || e == EHOSTDOWN || e == EHOSTUNREACH ||
             e == ENETUNREACH || e == ECONNRESET);
}

namespace {
RootProbe &probe_slot() {
    static RootProbe p;
    return p;
}
}  // namespace

void set_root_probe(RootProbe probe) { probe_slot() = std::move(probe); }

bool probe_root(const std::string &path) { return probe_slot() ? probe_slot()(path) : root_readable(path); }

bool root_online(const RootRow &r, const std::vector<MountEntry> &mounts, const std::string &by_uuid_dir) {
    if (const MountEntry *s = share_mount_of(mounts, r.path)) {
        /* a network share is an ordinary folder that may not answer for a while: never probed here (a hard NFS mount would hold
         * the caller), never hidden for a dropped connection. It is offline only when its mount is gone altogether. */
        return r.mount.empty() || s->dir == r.mount;
    }
    if (!probe_root(r.path)) return false; /* not there, not a folder, or mounted but unreadable: gone */
    if (r.mount.empty()) return true; /* a folder of the system disk */
    const MountEntry *m = mount_of(mounts, r.path);
    if (!m || m->dir != r.mount) return false; /* the drive left: what is there is the folder it was mounted on */
    if (!r.uuid.empty()) {
        const std::string u = device_uuid(m->dev, by_uuid_dir);
        if (!u.empty() && u != r.uuid) return false; /* another drive at the same place */
    }
    return true;
}

bool set_root_online(Db &db, int64_t id, bool online) {
    Stmt s = db.prepare("update roots set online = ?1 where id = ?2");
    s.bind(1, online ? 1 : 0).bind(2, static_cast<long long>(id));
    return s.run();
}

bool sync_roots(Db &db, const std::vector<RootSpec> &wanted, const std::vector<MountEntry> &mounts,
                const std::string &by_uuid_dir) {
    std::vector<RootRow> rows = load_roots(db);
    bool ok = true, in_tx = false;
    auto begin = [&] {
        if (!in_tx) {
            in_tx = db.exec("BEGIN IMMEDIATE");
            ok = ok && in_tx;
        }
    };
    auto is_wanted = [&](const std::string &p) {
        return std::any_of(wanted.begin(), wanted.end(), [&](const RootSpec &s) { return s.path == p; });
    };
    for (const RootSpec &s : wanted) {
        const MountEntry *m = mount_of(mounts, s.path);
        const std::string mount = m && m->dir != "/" ? m->dir : "";
        const std::string uuid = mount.empty() ? "" : device_uuid(m->dev, by_uuid_dir);
        auto it = std::find_if(rows.begin(), rows.end(), [&](const RootRow &r) { return r.path == s.path; });
        if (it == rows.end() && !uuid.empty()) { /* the same drive back under another mount point */
            it = std::find_if(rows.begin(), rows.end(), [&](const RootRow &r) { return r.uuid == uuid && !is_wanted(r.path); });
            if (it != rows.end()) {
                begin();
                Stmt u = db.prepare("update roots set path = ?1, label = ?2, mount = ?3 where id = ?4");
                u.bind(1, s.path).bind(2, s.label).bind(3, mount).bind(4, static_cast<long long>(it->id));
                ok = ok && u.run();
                it->path = s.path;
                it->label = s.label;
                it->mount = mount;
            }
        }
        if (it == rows.end()) {
            begin();
            Stmt ins = db.prepare("insert into roots(path, label, uuid, online, mount) values(?1, ?2, ?3, 0, ?4)");
            ins.bind(1, s.path).bind(2, s.label).bind(3, uuid).bind(4, mount);
            ok = ok && ins.run();
            RootRow r;
            r.id = db.last_insert_rowid();
            r.path = s.path;
            r.label = s.label;
            r.uuid = uuid;
            r.mount = mount;
            rows.push_back(r);
            continue;
        }
        if (mount.empty() && !it->mount.empty()) continue; /* a drive root whose drive is not mounted now: kept as it was */
        const bool new_uuid = !uuid.empty() && uuid != it->uuid;
        if (new_uuid || mount != it->mount || s.label != it->label) {
            begin();
            Stmt u = db.prepare("update roots set label = ?1, uuid = ?2, mount = ?3 where id = ?4");
            u.bind(1, s.label).bind(2, new_uuid ? uuid : it->uuid).bind(3, mount).bind(4, static_cast<long long>(it->id));
            ok = ok && u.run();
            it->label = s.label;
            if (new_uuid) it->uuid = uuid;
            it->mount = mount;
        }
    }
    for (RootRow &r : rows) {
        const bool on = is_wanted(r.path) && root_online(r, mounts, by_uuid_dir);
        if (on != r.online) {
            begin();
            ok = ok && set_root_online(db, r.id, on);
            r.online = on;
        }
    }
    if (in_tx) {
        if (ok) ok = db.exec("COMMIT");
        if (!ok) db.exec("ROLLBACK");
    }
    return ok;
}

bool remove_root(Db &db, int64_t id) {
    Transaction tx(db);
    LibraryWriter w(db);
    return tx.ok() && w.delete_root(id) && w.prune() && tx.commit();
}

int remove_roots_under(Db &db, const std::string &path) {
    if (path.empty() || path == "/") return 0;
    int n = 0;
    for (const RootRow &r : load_roots(db)) {
        if (!within(r.path, path)) continue;
        if (!remove_root(db, r.id)) return -1;
        ++n;
    }
    return n;
}

}  // namespace vinyl
