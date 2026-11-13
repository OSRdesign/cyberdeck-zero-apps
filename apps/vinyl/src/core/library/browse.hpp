/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the folder browser of the MVP (report 059, decision D4: folder first, no library database in P1).
 *
 *   roots     ~/Music, then every mounted removable or extra drive under /media/<user>, /run/media/<user> and /mnt (from
 *             /proc/mounts, real file systems only). With one root the browser starts inside it; with more, the top is
 *             the list of roots.
 *   folders   one level at a time: ".." (up), the sub-folders A-Z, then the playlists (.m3u, .m3u8) and the tracks A-Z
 *             (natural order: "2 x" before "10 x"); hidden files are left out; at most kMaxEntries.
 *   queue     a folder's tracks in the listed order, or an M3U playlist's entries (relative paths against the
 *             playlist's folder, #EXT lines skipped, missing files dropped).
 * LVGL-free, no thread of its own (the controller lists on a worker).
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vinyl {

constexpr size_t kMaxEntries = 5000;

struct Root {
    std::string label; /* "Music", the drive's folder name */
    std::string path;
};

/* `mounts` is the text of /proc/mounts. */
std::vector<Root> list_roots(const std::string &home, const std::string &user, const std::string &mounts);
/* The roots of this system (the environment's HOME / USER and /proc/mounts). */
std::vector<Root> system_roots();

struct Entry {
    enum class Kind { Up, Root, Folder, Playlist, Track };
    Kind kind = Kind::Track;
    std::string name; /* shown: the file or folder name, ".." */
    std::string path;
};

/* Lists `path` (sorted as above, ".." first unless `top`). Returns 0 or -errno. */
int list_folder(const std::string &path, bool top, std::vector<Entry> &out, size_t cap = kMaxEntries);
/* The entries of the root list. */
std::vector<Entry> root_entries(const std::vector<Root> &roots);

/* Natural, case-insensitive order of two names. */
bool natural_less(const std::string &a, const std::string &b);

/* The tracks of a folder listing (paths), in order. */
std::vector<std::string> folder_tracks(const std::vector<Entry> &entries);

/* The entries of an M3U / M3U8 playlist: absolute paths; `exists` filters (null: keep all). */
std::vector<std::string> parse_m3u(const std::string &text, const std::string &base_dir,
                                   bool (*exists)(const std::string &) = nullptr);
bool file_exists(const std::string &path);

/* The root a path is under (longest match), or null. */
const Root *root_of(const std::vector<Root> &roots, const std::string &path);
/* The breadcrumb the browser's heading shows, upper case: "MUSIC / PHOENIX"; the root's label alone at its top; at
 * most the root and the last two folders ("MUSIC / ... / A / B"). */
std::string breadcrumb(const std::vector<Root> &roots, const std::string &path);
/* The parent folder, or "" when `path` is a root (or not under one). */
std::string parent_folder(const std::vector<Root> &roots, const std::string &path);

}  // namespace vinyl
