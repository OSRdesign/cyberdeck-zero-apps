/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl media guard (see media_guard.hpp).
 */
#include "media_guard.hpp"

#include "net_share.hpp"
#include "roots.hpp"

#include <algorithm>

namespace vinyl {

std::vector<DriveBase> default_drive_bases() { return {{"/media/", 2}, {"/run/media/", 2}, {"/mnt/", 1}}; }

std::string drive_dir_of(const std::string &path, const std::vector<DriveBase> &bases) {
    for (const DriveBase &b : bases) {
        if (path.compare(0, b.prefix.size(), b.prefix) != 0) continue;
        std::string dir = b.prefix;
        size_t at = b.prefix.size();
        for (int i = 0; i < b.depth; ++i) {
            const size_t slash = path.find('/', at);
            const std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
            if (part.empty()) return "";
            dir += (i ? "/" : "") + part;
            if (slash == std::string::npos) {
                if (i + 1 < b.depth) return "";
                return dir;
            }
            at = slash + 1;
        }
        return dir;
    }
    return "";
}

std::vector<std::string> gone_drive_dirs(const std::vector<std::string> &paths, const std::function<bool(const std::string &dir)> &present,
                                         const std::vector<DriveBase> &bases, const std::function<bool(const std::string &path)> &exists) {
    std::vector<std::string> seen, gone;
    for (const std::string &p : paths) {
        const std::string d = drive_dir_of(p, bases);
        if (d.empty() || std::find(seen.begin(), seen.end(), d) != seen.end()) continue;
        seen.push_back(d);
        if (!present(d) && !(exists && exists(p))) gone.push_back(d);
    }
    return gone;
}

bool drive_present_with(const std::string &dir, const std::string &mounts_text, const std::function<bool(const std::string &)> &probe) {
    const std::vector<MountEntry> mounts = parse_mounts(mounts_text);
    const MountEntry *m = mount_of(mounts, dir);
    if (m && is_share_fs(m->type)) return true; /* a network share is not a drive: never "unplugged", never probed here (a hard NFS mount would hang) */
    if (!m || m->dir == "/") return false; /* the mount left: what is there is the folder it was mounted on */
    return probe(m->dir);
}

bool drive_present_real(const std::string &dir) { return drive_present_with(dir, read_mounts(), [](const std::string &d) { return probe_root(d); }); }

}  // namespace vinyl
