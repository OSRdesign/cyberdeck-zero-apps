/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the roots, the folders the library indexes (report 060 3a, "Heat and drives that go away"; P2b).
 *
 *   wanted    ~/Music, the drives mounted under /media/<user>, /run/media/<user>, /mnt and /media (P1's list_roots, from
 *             /proc/self/mounts; Vinyl never mounts one: D28's mounting is P2e), and the folders the settings add
 *             (Settings::library_folders). A folder inside another root, or holding one, is left out (no double index).
 *   rows      roots(id, path, label, uuid, online, last_scan, mount). A drive root keeps the mount point it was found at
 *             and its file system's UUID (/dev/disk/by-uuid, no root rights needed).
 *   online    the folder exists and, for a drive root, its mount point is still in /proc/self/mounts with the same
 *             UUID. A root that is not online (a pulled stick, a removed setting) keeps its rows, hidden from every view;
 *             nothing is deleted. A stick mounted again under another name (STICK1) is matched by its UUID and its path
 *             updated, so its rows come back.
 * Written only when something changed (SD wear). LVGL-free; the database calls run on the scanner's thread.
 */
#pragma once

#include "../db/sqlite_lib.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {

struct MountEntry {
    std::string dev, dir, type;
};

/* The lines of /proc/self/mounts (octal escapes such as \040 undone). */
std::vector<MountEntry> parse_mounts(const std::string &text);
/* The text of /proc/self/mounts ("" when unreadable). */
std::string read_mounts();
/* The mount a path lives on: the longest mount point that is the path or one of its parents (null: none). */
const MountEntry *mount_of(const std::vector<MountEntry> &mounts, const std::string &path);
/* The file-system UUID of a device ("/dev/sda1") from the links in by_uuid_dir, "" when none points at it. */
std::string device_uuid(const std::string &dev, const std::string &by_uuid_dir = "/dev/disk/by-uuid");

struct RootSpec {
    std::string path;
    std::string label;
};

/* The roots the library indexes (see the top). */
std::vector<RootSpec> library_roots(const std::string &home, const std::string &user, const std::string &mounts_text,
                                    const std::vector<std::string> &folders);

struct RootRow {
    int64_t id = 0;
    std::string path, label, uuid, mount;
    bool online = false;
    int64_t last_scan = 0;
};

std::vector<RootRow> load_roots(Db &db);

/* Is the root there now (see the top)? */
bool root_online(const RootRow &r, const std::vector<MountEntry> &mounts, const std::string &by_uuid_dir);

/* Brings the roots table in line with `wanted` (new roots made, a drive back under another mount point matched by its
 * UUID, uuid / mount / label kept up to date, the online flags set). Writes only what changed. false on a database
 * error. */
bool sync_roots(Db &db, const std::vector<RootSpec> &wanted, const std::vector<MountEntry> &mounts,
                const std::string &by_uuid_dir);

/* Marks one root offline (a drive pulled during a scan). */
bool set_root_online(Db &db, int64_t id, bool online);

/* The user removed a folder from the library: its rows go (tracks, FTS rows, play counts, folders, the root). */
bool remove_root(Db &db, int64_t id);

}  // namespace vinyl
