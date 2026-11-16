/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the copy of a scene or a photo to a USB drive (brief 9.3, D33). One job on a thread of its own, polled by the UI:
 *
 *   target   <mount>/VHS/<file name> (the folder is made); the file appears on the drive only when the copy is complete:
 *            it is written as <mount>/VHS/.<name>.part in 1 MiB chunks, fdatasync'ed every 4 MiB (so the progress is not a
 *            cache filling up and a cancel is quick), then fsync, rename, fsync of the folder
 *   thread   the job's thread runs with I/O class idle (ioprio_set; failure ignored), so it never competes with a recording
 *   cancel   stops after the chunk in hand and deletes the .part: nothing appears on the drive
 *   refuse   (the job's first step; check_copy() is the pure part) not enough free space (the file and 1 MiB margin), a file
 *            of 4 GiB or more to a FAT drive, a file of that name already on the drive (provisional: no "(2)" names), the
 *            source or the drive not there. Refusing is a job that ends at once with failed + a reason, nothing created
 *   failure  a write or read error (ENOSPC mid-copy, a pulled drive) ends the job as failed and removes the .part
 *
 * The file system calls go through FileOps (scenes/fileops.hpp) and the time through Clock, so the tests inject failures and
 * a clock. The drive's mount comes from the drive list (drives.hpp): the copy uses it as found, or as the app mounted it, and
 * never mounts, unmounts or remounts anything itself. Every reason is lower case with no full stop: it is shown after
 * "Not copied: ". Nothing here blocks the caller: start() returns at once, status() is a snapshot. LVGL-free.
 */
#pragma once

#include "scenes/fileops.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace vhs {

/* ---- the pure checks */

struct CopyCheck {
    bool ok = false;
    std::string why; /* not ok: for "Not copied: <why>" */
};

/* Is this file system type FAT? "vfat", "fat", "fat12", "fat16", "fat32", "msdos" in any case; "exfat" is not. */
bool is_fat_type(const std::string &fs_type);

/* May a file of file_bytes go to a drive with drive_free bytes free and the file system fs_type ("" = unknown: only the space is
 * checked)? Refuses, in this order:
 *   a file of 4 GiB or more to a FAT drive     "the drive's FAT file system takes files under 4 GB"
 *   not enough room (file + 1 MiB margin)       "not enough free space on SANDISK" ("on the drive" when label is empty) */
CopyCheck check_copy(std::uint64_t file_bytes, std::uint64_t drive_free, const std::string &fs_type, const std::string &label = "");

/* The whole before-start check on real paths, through ops: the source's size (stat), the drive's free space (statvfs; the
 * mount may be dead after a pull, and statvfs can then block: call it off the UI thread, as CopyJob does), a file of that name
 * already at <mount>/VHS/<name> ("a file of that name is on the drive"), then check_copy. Also:
 *   "the file is not there"   the source cannot be read      "the drive is not there"   the mount cannot be read */
CopyCheck preflight_copy(FileOps &ops, const std::string &src_path, const std::string &mount_dir, const std::string &fs_type,
                         const std::string &label);

/* ---- the job */

struct CopyStatus {
    bool running = false;   /* the job's thread is working (also while a cancel is being carried out) */
    bool done = false;      /* ended: the file is on the drive, complete */
    bool cancelled = false; /* ended by cancel(): the .part is gone, nothing is on the drive */
    bool failed = false;    /* ended by a refusal or an error: `error`, nothing is on the drive */
    std::string error;      /* failed: "not enough free space on SANDISK" ... (lower case, no full stop) */
    std::string target;     /* <mount>/VHS/<name> (set from start() on) */
    std::uint64_t bytes = 0, total = 0; /* written so far, size of the file (total is 0 until the job has looked at the file) */
    double bytes_per_s = 0;             /* smoothed over the last seconds; 0 while unknown (the first half second) */
    std::int64_t eta_s = -1;            /* seconds left at that rate; -1 unknown; 0 once done */
    std::string label;                  /* the drive's name, as given to start() */
};
/* Exactly one of running, done, cancelled, failed is set from start() on; none before the first start(). */

/* 0..1 for the progress bar (done: 1; an unknown total: 0) */
double copy_fraction(const CopyStatus &s);
/* "630 MB of 1.4 GB, about 40 s left" while running (the canvas CopyToUsb wording; bytes as bytes_text() of the disk guard, time
 * as time_left_text(), rounded to 5 s from 10 s on; no time part while the rate is unknown). Ended: done "1.4 GB copied",
 * cancelled "Copy cancelled", failed "Not copied: <error>". Before any start: "". */
std::string copy_progress_text(const CopyStatus &s);
/* "Copying to SANDISK" ("Copying to the drive" when the label is empty) */
std::string copy_title(const std::string &label);

class CopyJob {
public:
    static constexpr std::size_t kChunk = 1u << 20;         /* the write size */
    static constexpr std::uint64_t kSyncEvery = 4ull << 20; /* fdatasync after this many bytes */

    explicit CopyJob(FileOps &ops = posix_file_ops(), Clock &clock = system_clock());
    /* A running copy is cancelled (its .part removed) and the thread joined: a copy never outlives the app. */
    ~CopyJob();
    CopyJob(const CopyJob &) = delete;
    CopyJob &operator=(const CopyJob &) = delete;

    /* Copies src_path to <mount_dir>/VHS/<its file name>. Returns at once: true when the job's thread was started (a refusal is
     * a job that ends as failed: poll status()), false only when a copy is already running. A job that ended is replaced.
     * fs_type: the drive's file system (DriveInfo::fstype), for the FAT limit; "" skips that rule. */
    bool start(const std::string &src_path, const std::string &mount_dir, const std::string &drive_label, const std::string &fs_type = "");
    /* Asks a running job to stop (returns at once); status() turns to cancelled when the .part is gone. A copy that has
     * renamed its file already is done, whatever the timing. No effect when nothing runs. */
    void cancel();
    /* A snapshot, safe from any thread. */
    CopyStatus status() const;
    /* Blocks until the thread has ended (the tests; the destructor). */
    void join();

private:
    enum class End { Done, Cancelled, Failed };
    void work(std::string src, std::string mount, std::string label, std::string fs_type); /* the thread */
    void finish(End how, const std::string &error);
    void set_total(std::uint64_t total);

    FileOps &ops_;
    Clock &clock_;
    mutable std::mutex m_;
    CopyStatus st_; /* guarded by m_; its bytes and eta_s are filled in by status() */
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<bool> cancel_{false};
    std::thread thread_; /* start(), cancel(), join() and the destructor are the owner's thread's; status() is any thread's */
};

} // namespace vhs
