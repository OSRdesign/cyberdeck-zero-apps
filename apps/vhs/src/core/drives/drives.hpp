/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at origin/main fdad829) for VHS (brief 3.1, 9.3); VHS changes marked VHS.
 * VHS changes: namespace vinyl -> vhs; the mount list comes from mounts.hpp (Vinyl: library/roots.hpp); the exit log line says
 * [vhs]; the names by_vinyl() and DriveInfo::by_vinyl stay as they are in Vinyl so a drive list ported from Vinyl compiles
 * unchanged (they mean "mounted by this app"); a few comments say "the app" where Vinyl's say "Vinyl". Nothing else differs.
 *
 * Vinyl drives: removable drives that carry a file system, mounted on request (owner decision 2026-11-14, B45: "Mount on
 * request in Vinyl", the mounting half of D28). On the HackBerry nothing mounts a plugged USB stick: the desktop's file manager
 * does it and does not run while the launcher owns the screen, and Vinyl only read /proc/self/mounts. So:
 *
 *   list     the removable block devices that carry a file system (sysfs /sys/block/<dev>/removable, the udev database in
 *            /run/udev/data for the type, label and UUID: readable without root), mounted or not (/proc/self/mounts)
 *   mount    through udisks2: `udisksctl mount -b <dev>` (runs as the user; polkit allows a process of the active session, it
 *            refuses one of an ssh login: the reason is shown), off the UI thread, with a timeout
 *   unmount  `udisksctl unmount -b <dev>`: only what Vinyl mounted (the manager's own record); a drive the system mounted is
 *            never unmounted by Vinyl
 * udisks2 is a run-time need, not a Depends: the program is looked for in PATH; without it the pages list the drives and a tap
 * says "Drives need the udisks2 package." (exec, not D-Bus: the same polkit rules, no library to load or version, the
 * messages are the user's own). The backend is an interface: the tests and the scenes use FakeDrives. LVGL-free.
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vhs {

struct DriveInfo {
    std::string dev;       /* "/dev/sda1" */
    std::string label;     /* the file system's label ("" when it has none) */
    std::string uuid, fstype;
    std::string mount_dir; /* where it is mounted ("" when it is not) */
    uint64_t size = 0;     /* bytes */
    bool mounted = false;
    bool by_vinyl = false; /* the app mounted it (the manager fills it in) */
    /* "SANDISK", or the device's name when it has no label ("sda1") */
    std::string name() const;
};

/* "14.9 GB", "512 MB", "3.4 TB". VHS: modes/select.hpp has an overload size_text(const Size &); the two never clash. */
std::string size_text(uint64_t bytes);

struct MountReply {
    bool ok = false;
    std::string mount_dir; /* mount: where it is mounted */
    std::string reason;    /* not ok: why, for "Could not mount it: <reason>." (no capital, no full stop) */
};

class DriveBackend {
public:
    virtual ~DriveBackend() = default;
    /* the program that mounts is there */
    virtual bool available() = 0;
    virtual std::vector<DriveInfo> list() = 0;
    virtual MountReply mount(const std::string &dev) = 0;
    virtual MountReply unmount(const std::string &dev) = 0;
};

/* ---- the real backend's pieces (tested on a fake sysfs and udev tree) */

/* the KEY=value pairs of the "E:" lines of a /run/udev/data/b<maj>:<min> file */
std::vector<std::pair<std::string, std::string>> parse_udev_data(const std::string &text);
/* The removable drives: sys_root ("/sys"), udev_root ("/run/udev/data"), the text of /proc/self/mounts. A device whose
 * `removable` is 1 and that carries a file system (ID_FS_TYPE set, ID_FS_USAGE "filesystem" or absent): each of its partitions,
 * or the device itself when it has none. Sorted by device name. */
std::vector<DriveInfo> list_removable_drives(const std::string &sys_root, const std::string &udev_root, const std::string &mounts_text);
/* udisksctl's answers (exit status, its output and error text) as a MountReply */
MountReply parse_mount_reply(int status, const std::string &out, const std::string &err);
MountReply parse_unmount_reply(int status, const std::string &out, const std::string &err);
/* the reason for a message we do not know: its last sentence shortened (no capital, no full stop, at most 60 characters) */
std::string shorten_reason(const std::string &msg);

/* udisksctl through exec (a 20 s limit, the user's own session); `path_dirs`: where to look for it (PATH when empty) */
std::unique_ptr<DriveBackend> make_udisks_backend();

/* ---- a backend made of data (the unit tests, the scenes) */
class FakeDrives : public DriveBackend {
public:
    bool has_udisks = true;
    std::vector<DriveInfo> devices;
    std::string mount_base = "/media/user";
    /* what mount answers for a device (default: ok, mounted at mount_base/<name>) */
    std::function<MountReply(const std::string &dev)> on_mount;
    std::function<MountReply(const std::string &dev)> on_unmount;
    std::vector<std::string> calls; /* "mount /dev/sda1", "unmount /dev/sda1" */
    bool available() override { return has_udisks; }
    std::vector<DriveInfo> list() override;
    MountReply mount(const std::string &dev) override;
    MountReply unmount(const std::string &dev) override;
};

/* The drives of one run: the list with what the app mounted marked, the mounts and unmounts on a thread of their own (a mount
 * takes seconds and may ask polkit), their answers handed back to the UI thread by take(), and at exit the unmount of what
 * the app mounted (never anything the system mounted). */
class DriveManager {
public:
    struct Result {
        enum class Kind { Mounted, MountFailed, Unmounted, UnmountFailed };
        Kind kind = Kind::Mounted;
        std::string dev, name, mount_dir, reason;
    };
    explicit DriveManager(std::unique_ptr<DriveBackend> backend);
    ~DriveManager();
    DriveManager(const DriveManager &) = delete;
    DriveManager &operator=(const DriveManager &) = delete;

    bool available() const;               /* udisks is there */
    std::vector<DriveInfo> list();        /* by_vinyl set for the drives the app mounted that are still mounted */
    bool by_vinyl(const std::string &dev) const;
    /* a drive that is mounted already is the app's to unmount (the scenes; a mount made by an earlier run is not) */
    void note_mounted(const std::string &dev);
    bool busy() const;                    /* a mount or unmount is running or waiting */
    void mount_async(const std::string &dev, const std::string &name);
    void unmount_async(const std::string &dev, const std::string &name);
    bool take(Result &out);               /* the next answer, if any */
    /* Unmounts what the app mounted, one after the other, and stops the thread (at exit). Returns how many it unmounted. */
    int shutdown();
    void wait_idle();                     /* the tests */

private:
    void run();
    std::unique_ptr<DriveBackend> be_;
    mutable std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    struct Op {
        bool mount;
        std::string dev, name;
    };
    std::deque<Op> ops_;
    std::deque<Result> done_;
    std::set<std::string> ours_; /* devices the app mounted */
    bool quit_ = false, busy_ = false;
    std::thread th_;
};

}  // namespace vhs
