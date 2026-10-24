/*
 * SPDX-License-Identifier: MIT
 */
#include "trash.hpp"

#include "fsutil.hpp"
#include "names.hpp"

#include <cerrno>
#include <cstdio>
#include <unistd.h>

namespace mdnotes {

std::string free_trash_path(const std::string &trash_dir, const std::string &file_name)
{
    const std::string first = path_join(trash_dir, file_name);
    if (!stat_path(first, false).exists) return first;
    const bool md = is_note_file(file_name);
    const std::string stem = md ? file_name.substr(0, file_name.size() - 3) : file_name;
    const std::string ext = md ? file_name.substr(file_name.size() - 3) : std::string();
    for (int i = 2; i < 10000; ++i) {
        const std::string p = path_join(trash_dir, stem + " " + std::to_string(i) + ext);
        if (!stat_path(p, false).exists) return p;
    }
    return std::string();
}

TrashResult move_to_trash(const std::string &vault, const std::string &rel)
{
    TrashResult r;
    const std::string from = path_join(vault, rel);
    const FileStat fs = stat_path(from, false);
    if (!fs.exists || !fs.is_reg) {
        r.err = fs.exists ? EISDIR : ENOENT;
        return r;
    }
    const std::string trash = path_join(vault, kTrashFolder);
    int err = 0;
    if (!make_dirs(trash, &err)) {
        r.err = err;
        return r;
    }
    const std::string to = free_trash_path(trash, base_name(rel));
    if (to.empty()) {
        r.err = EEXIST;
        return r;
    }
    if (std::rename(from.c_str(), to.c_str()) == 0) {
        fsync_dir(trash);
        fsync_dir(dir_name(from));
        r.ok = true;
        r.trashed = to;
        return r;
    }
    if (errno != EXDEV) {
        r.err = errno;
        return r;
    }
    std::string data;
    if (read_file(from, data, static_cast<std::size_t>(fs.size) + 1, &err) != ReadStatus::Ok) {
        r.err = err ? err : EIO;
        return r;
    }
    if (!create_file_exclusive(to, data, &err)) {
        r.err = err;
        return r;
    }
    if (::unlink(from.c_str()) != 0) {
        r.err = errno;
        ::unlink(to.c_str());
        return r;
    }
    fsync_dir(dir_name(from));
    r.ok = true;
    r.trashed = to;
    return r;
}

} // namespace mdnotes
