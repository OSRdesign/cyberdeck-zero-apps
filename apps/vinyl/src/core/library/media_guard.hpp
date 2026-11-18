/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl media guard (owner ruling 2026-11-14, B49/B50): "if media was removed vinyl should get back to default music folder and on
 * reopen go to the library directly". The drives a queue's files live on, and which of them are gone (pulled, a stale mount that
 * cannot be read, ejected, unmounted at exit): the queue drops their entries at once. Pure but for the real `present` (the mounts
 * and the readability probe of core/library/roots); the tests and the scenes pass their own. LVGL-free.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace vinyl {

/* Where drives are mounted: a path under `prefix` belongs to the drive whose directory is `depth` components below it
 * (/media/<user>/<label>: 2, /run/media/<user>/<label>: 2, /mnt/<label>: 1). */
struct DriveBase {
    std::string prefix; /* with the trailing slash */
    int depth = 1;
};
std::vector<DriveBase> default_drive_bases();

/* the drive directory a path lives on ("" when it is under no drive base) */
std::string drive_dir_of(const std::string &path, const std::vector<DriveBase> &bases);

/* The drive directories of `paths` for which `present(dir)` is false (each once, in order of first use). A directory that is not a
 * mount but holds music all the same (a folder of /mnt on the system disk) is not a drive that went away: when `exists` is given
 * and the first file of that directory still exists, the directory is not gone. */
std::vector<std::string> gone_drive_dirs(const std::vector<std::string> &paths, const std::function<bool(const std::string &dir)> &present,
                                         const std::vector<DriveBase> &bases,
                                         const std::function<bool(const std::string &path)> &exists = nullptr);

/* The real test: the directory lies on a mount that is listed in /proc/self/mounts (not "/": the drive's own mount) and that can
 * be read (probe_root: a stale mount is not). */
bool drive_present_real(const std::string &dir);
/* The same with the mounts text and the readability probe given (the tests). A path on a network share (cifs, nfs, sshfs, an
 * automount point: net_share.hpp) is always "present": it is an ordinary folder, the drive rule does not apply, and nothing
 * is probed (a hard NFS mount would hang the caller). */
bool drive_present_with(const std::string &dir, const std::string &mounts_text, const std::function<bool(const std::string &dir)> &probe);

}  // namespace vinyl
