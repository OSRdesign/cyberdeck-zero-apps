/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares (report 061, 5.6): a share the system mounted (SMB, NFS, sshfs, an automount point that waits for
 * one) is an ordinary music folder, never a removable drive, and it must never hang the UI.
 *
 *   classify   the file-system type of the mount a path lives on, from /proc/self/mounts (cifs, smb3, nfs, nfs4, ceph, 9p,
 *              fuse.sshfs ... and "autofs": a systemd automount point is the share while it is idle). `fuseblk` and the
 *              local FUSE drivers of a stick (ntfs-3g, exfat) are drives, not shares.
 *   bounded    a stat, a readdir or an open on a hard NFS mount blocks for as long as the server is away. BoundedRunner runs
 *              such a job on a thread of its own and waits at most `timeout_ms`; a job that does not come back is abandoned
 *              (its thread stays blocked in the kernel, no other is started for that key while it is) and the key is not
 *              tried again before a back-off (doubling, to a limit) has passed. The jobs own their data (shared_ptr), so an
 *              abandoned job that finishes later touches nothing of the caller. LVGL-free.
 */
#pragma once

#include "roots.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vinyl {

/* Is this /proc/self/mounts file-system type a network share (or the automount point of one)? */
bool is_share_fs(const std::string &type);
/* The mount of `path` when it is a share's (null: no mount, or a local one). */
const MountEntry *share_mount_of(const std::vector<MountEntry> &mounts, const std::string &path);
/* Does the path live on a share now (the real /proc/self/mounts: reading it never touches the share)? */
bool path_on_share(const std::string &path);

class BoundedRunner {
public:
    struct Config {
        uint32_t timeout_ms = 3000;       /* the longest the caller waits for a job */
        uint32_t backoff_ms = 5000;       /* after a job that did not come back: no new try for this long, doubling each time */
        uint32_t backoff_max_ms = 60000;
        std::function<uint64_t()> now_ms; /* a monotonic clock in ms (empty: steady_clock) */
    };
    enum class Status {
        Done,      /* the job ran to its end within the timeout */
        TimedOut,  /* it did not: abandoned */
        BackedOff  /* not started: an earlier job for the key is still stuck, or its back-off has not passed */
    };

    BoundedRunner() = default;
    explicit BoundedRunner(Config c) : cfg_(std::move(c)) {}
    void configure(Config c);
    void reset(); /* forgets every back-off (the tests) */

    /* Runs `job` on a new thread and waits. The job and what it writes must belong to the job (shared_ptr), not to the caller's stack. */
    Status run(const std::string &key, std::function<void()> job);

    /* the job threads that have not ended (the tests wait for 0 before they end) */
    static int live_threads();

private:
    struct Slot {
        uint64_t until = 0;
        uint32_t backoff = 0;
        std::shared_ptr<std::atomic<bool>> busy;
    };
    uint64_t now() const;

    std::mutex mu_;
    Config cfg_;
    std::map<std::string, Slot> slots_;
};

/* The runner every probe of a share goes through (UI, library worker and scanner share its back-off). */
BoundedRunner &net_runner();

/* probe_root(path) behind the runner: false when the share does not answer in time or is backed off (the back-off is kept per
 * `key`, the path when empty: the scanner probes every folder of a root under the root's key). */
bool probe_share_root(const std::string &path, const std::string &key = std::string());

/* Does a stat and a read of the folder come back in time? (false: no answer, or backed off; true: it answered, with whatever
 * result: a folder that is gone answers too, "not found" is for the caller to find out by opening it.) */
bool share_answers(const std::string &path, const std::string &key = std::string());

/* Is the root of a share there and answering? The probe (bounded), then the mounts again: a path that is still only an
 * automount point after the probe (the mount failed) is not there; the folder under it is empty and says nothing. */
bool share_reachable(const std::string &path, const std::function<std::string()> &mounts_text);

}  // namespace vinyl
