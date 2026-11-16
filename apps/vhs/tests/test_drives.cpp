/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests, group "drives" (src/core/drives, brief 9.3, 15.1 "Drives and copy").
 *
 * The drives are Vinyl 0.3.0's tests ported (tests/test_drives.cpp there): the list of removable drives from a fake sysfs and
 * udev tree, udisks's answers read as replies, the manager on FakeDrives (a mount that works, one that is denied, an unmount at
 * exit of what the app mounted and nothing else, Eject, a drive the system mounted left alone, a drive pulled, no udisks), and
 * VHS's own demo drives (VHS_FAKE_DRIVES).
 *
 * The copy job (copy.hpp): a synthetic file copied to a temporary "mount" (equal byte for byte, no .part left, the VHS folder
 * made), cancel in the middle (a gate in FileOps holds the copy after the first chunk) removes the .part, the refusals (FAT and
 * 4 GiB, free space, a file of that name, source or drive missing) with no real 4 GiB file, ENOSPC and a read error injected in
 * the middle (failed, .part removed), the rate and the time left on a fake clock, a copy abandoned by the destructor, and the
 * progress texts.
 */
#include "check.hpp"

#include "app/copy_flow.hpp"
#include "drives/copy.hpp"
#include "drives/demo_drives.hpp"
#include "drives/drives.hpp"
#include "drives/mounts.hpp"
#include "scenes/fileops.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <thread>

using namespace vhs;

namespace {

constexpr std::uint64_t MiB = 1ull << 20;
constexpr std::uint64_t GiB = 1ull << 30;

void mk(const std::string &p) { ::mkdir(p.c_str(), 0755); }

void put(const std::string &p, const std::string &text) { write_file(p, text); }

/* a device in the fake sysfs: /sys/block/<disk>[/<part>] with its dev, size, removable, and its udev data file */
void add_part(const std::string &sys, const std::string &udev, const std::string &disk, const std::string &part, const std::string &majmin,
              uint64_t sectors, const std::string &udev_text) {
    const std::string d = part.empty() ? sys + "/block/" + disk : sys + "/block/" + disk + "/" + part;
    mk(d);
    put(d + "/dev", majmin + "\n");
    put(d + "/size", std::to_string(sectors) + "\n");
    if (!part.empty()) put(d + "/partition", "1\n");
    put(udev + "/b" + majmin, udev_text);
}

bool is_dir(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::vector<std::string> names_in(const std::string &dir) {
    std::vector<std::string> v;
    posix_file_ops().list_dir(dir, v);
    std::sort(v.begin(), v.end());
    return v;
}

std::string slurp_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* synthetic bytes that do not repeat on the chunk size */
std::string pattern_bytes(std::size_t n, std::uint32_t seed) {
    std::string s(n, '\0');
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = static_cast<char>(x >> 24);
    }
    return s;
}

/* ------------------------------------------------------------------------------------------------ the list and the manager */

void test_drive_list_and_replies() {
    CHECK_EQ(size_text(512000000), std::string("512 MB"));
    CHECK_EQ(size_text(14900000000ull), std::string("15 GB"));
    CHECK_EQ(size_text(3400000000ull), std::string("3.4 GB"));
    CHECK_EQ(size_text(2000000000000ull), std::string("2.0 TB"));
    DriveInfo di;
    di.dev = "/dev/sda1";
    CHECK_EQ(di.name(), std::string("sda1"));
    di.label = "SANDISK";
    CHECK_EQ(di.name(), std::string("SANDISK"));

    /* the list: removable disks with a file system, from a fake /sys and /run/udev/data */
    const std::string dir = make_temp_dir("drives");
    const std::string sys = dir + "/sys", udev = dir + "/udev";
    mk(sys);
    mk(sys + "/block");
    mk(udev);
    /* sda: removable, two partitions: sda1 vfat SANDISK (mounted), sda2 with no file system */
    mk(sys + "/block/sda");
    put(sys + "/block/sda/removable", "1\n");
    add_part(sys, udev, "sda", "sda1", "8:1", 60000000, "E:ID_FS_TYPE=vfat\nE:ID_FS_LABEL=SANDISK\nE:ID_FS_UUID=ABCD-1234\nE:ID_FS_USAGE=filesystem\n");
    add_part(sys, udev, "sda", "sda2", "8:2", 1000, "E:ID_PART_ENTRY_TYPE=0x83\n");
    /* sdb: not removable (the system disk) */
    mk(sys + "/block/sdb");
    put(sys + "/block/sdb/removable", "0\n");
    add_part(sys, udev, "sdb", "sdb1", "8:17", 1000, "E:ID_FS_TYPE=ext4\nE:ID_FS_USAGE=filesystem\n");
    /* sdc: removable, no partitions, the file system on the disk (ext4, no label) */
    add_part(sys, udev, "sdc", "", "8:32", 2000000, "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=1111\nE:ID_FS_USAGE=filesystem\n");
    put(sys + "/block/sdc/removable", "1\n");
    /* sdd: removable, a RAID member: not a file system */
    add_part(sys, udev, "sdd", "", "8:48", 2000, "E:ID_FS_TYPE=linux_raid_member\nE:ID_FS_USAGE=raid\n");
    put(sys + "/block/sdd/removable", "1\n");
    const std::string mounts = "/dev/sda1 /media/user/SANDISK vfat rw 0 0\n/dev/sdb1 / ext4 rw 0 0\n";
    std::vector<DriveInfo> l = list_removable_drives(sys, udev, mounts);
    CHECK_EQ(l.size(), static_cast<size_t>(2));
    CHECK_EQ(l[0].dev, std::string("/dev/sda1"));
    CHECK_EQ(l[0].label, std::string("SANDISK"));
    CHECK_EQ(l[0].uuid, std::string("ABCD-1234"));
    CHECK_EQ(l[0].fstype, std::string("vfat"));
    CHECK(l[0].mounted);
    CHECK_EQ(l[0].mount_dir, std::string("/media/user/SANDISK"));
    CHECK_EQ(l[0].size, static_cast<uint64_t>(60000000) * 512);
    CHECK_EQ(l[1].dev, std::string("/dev/sdc"));
    CHECK(!l[1].mounted);
    CHECK_EQ(l[1].name(), std::string("sdc"));
    CHECK(list_removable_drives(dir + "/nothing", udev, "").empty());
    auto kv = parse_udev_data("N:sda1\nE:ID_FS_TYPE=vfat\nS:disk/by-uuid/X\nE:ID_FS_LABEL=A B\n");
    CHECK_EQ(kv.size(), static_cast<size_t>(2));
    CHECK_EQ(kv[1].second, std::string("A B"));
    rm_tree(dir);

    /* /proc/self/mounts: a space is written as \040 */
    const std::vector<MountEntry> me = parse_mounts("/dev/sda1 /media/user/MY\\040STICK vfat rw 0 0\nbad\n/dev/sdb1 / ext4 rw 0 0\n");
    CHECK_EQ(me.size(), static_cast<size_t>(2));
    CHECK_EQ(me[0].dir, std::string("/media/user/MY STICK"));
    CHECK_EQ(me[0].type, std::string("vfat"));

    /* udisksctl's answers */
    MountReply r = parse_mount_reply(0, "Mounted /dev/sda1 at /media/user/SANDISK.\n", "");
    CHECK(r.ok);
    CHECK_EQ(r.mount_dir, std::string("/media/user/SANDISK"));
    r = parse_mount_reply(1, "", "Error mounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.AlreadyMounted: Device /dev/sda1 is already mounted at `/media/user/SANDISK'.\n");
    CHECK(r.ok);
    CHECK_EQ(r.mount_dir, std::string("/media/user/SANDISK"));
    r = parse_mount_reply(1, "", "Error mounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain: Not authorized to perform operation\n");
    CHECK(!r.ok);
    CHECK_EQ(r.reason, std::string("this session is not allowed to mount drives"));
    r = parse_mount_reply(1, "", "Error mounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.Failed: Error mounting /dev/sda1 at /media/u/X: wrong fs type, bad option, bad superblock on /dev/sda1\n");
    CHECK_EQ(r.reason, std::string("the disk has no readable filesystem"));
    r = parse_mount_reply(1, "", "Error mounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.DeviceBusy: Device is busy\n");
    CHECK_EQ(r.reason, std::string("it is in use"));
    r = parse_mount_reply(1, "", "Error mounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.Failed: The disk is on fire\n");
    CHECK(!r.ok);
    CHECK_EQ(r.reason, std::string("the disk is on fire")); /* unknown: its last sentence shortened */
    r = parse_mount_reply(127, "", "");
    CHECK_EQ(r.reason, std::string("udisks2 is missing"));
    CHECK_EQ(shorten_reason("Error: Something went wrong in a very long sentence that goes on and on past the sixty characters limit"),
             std::string("something went wrong in a very long sentence that goes on an"));
    CHECK_EQ(shorten_reason(""), std::string("unknown reason"));
    r = parse_unmount_reply(0, "Unmounted /dev/sda1.\n", "");
    CHECK(r.ok);
    r = parse_unmount_reply(1, "", "Error unmounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.NotMounted: Device is not mounted\n");
    CHECK(r.ok); /* already gone: nothing to undo */
    r = parse_unmount_reply(1, "", "Error unmounting /dev/sda1: GDBus.Error:org.freedesktop.UDisks2.Error.DeviceBusy: target is busy\n");
    CHECK(!r.ok);
    CHECK_EQ(r.reason, std::string("it is in use"));
}

void test_drive_manager() {
    auto fake = std::make_unique<FakeDrives>();
    FakeDrives *fb = fake.get();
    fb->mount_base = "/media/user";
    DriveInfo sys_drive, gen, sd;
    sys_drive.dev = "/dev/sdb1";
    sys_drive.label = "SYSTEM";
    sys_drive.mounted = true; /* mounted by the system */
    sys_drive.mount_dir = "/media/user/SYSTEM";
    gen.dev = "/dev/sdc1";
    gen.label = "Generic Flash Disk";
    sd.dev = "/dev/sda1";
    sd.label = "SANDISK";
    fb->devices = {sd, gen, sys_drive};
    DriveManager m(std::move(fake));
    CHECK(m.available());
    std::vector<DriveInfo> dl = m.list();
    CHECK_EQ(dl.size(), static_cast<size_t>(3));
    CHECK(!dl[0].mounted && !dl[2].by_vinyl);
    /* a mount that works: answered off this thread, the app's to undo */
    m.mount_async("/dev/sda1", "SANDISK");
    m.wait_idle();
    DriveManager::Result res;
    CHECK(m.take(res));
    CHECK(res.kind == DriveManager::Result::Kind::Mounted);
    CHECK_EQ(res.mount_dir, std::string("/media/user/SANDISK"));
    CHECK(!m.take(res));
    CHECK(m.by_vinyl("/dev/sda1"));
    dl = m.list();
    CHECK(dl[0].mounted && dl[0].by_vinyl);
    CHECK(dl[2].mounted && !dl[2].by_vinyl); /* the system's stays the system's */
    /* denied: an answer with the reason, nothing recorded */
    fb->on_mount = [](const std::string &) { return MountReply{false, "", "this session is not allowed to mount drives"}; };
    m.mount_async("/dev/sdc1", "Generic Flash Disk");
    m.wait_idle();
    CHECK(m.take(res));
    CHECK(res.kind == DriveManager::Result::Kind::MountFailed);
    CHECK_EQ(res.reason, std::string("this session is not allowed to mount drives"));
    CHECK(!m.by_vinyl("/dev/sdc1"));
    /* Eject of a drive the system mounted: refused, the backend never called */
    const size_t calls = fb->calls.size();
    m.unmount_async("/dev/sdb1", "SYSTEM");
    m.wait_idle();
    CHECK(!m.take(res));
    CHECK_EQ(fb->calls.size(), calls);
    /* Eject of the app's: unmounted, no longer ours */
    m.unmount_async("/dev/sda1", "SANDISK");
    m.wait_idle();
    CHECK(m.take(res));
    CHECK(res.kind == DriveManager::Result::Kind::Unmounted);
    CHECK_EQ(res.name, std::string("SANDISK"));
    CHECK(!m.by_vinyl("/dev/sda1"));
    CHECK(!m.busy());
    /* at exit: what the app mounted is unmounted, what the system mounted is not; a drive pulled meanwhile is not asked for */
    fb->on_mount = nullptr;
    m.mount_async("/dev/sda1", "SANDISK");
    m.mount_async("/dev/sdc1", "Generic Flash Disk");
    m.wait_idle();
    while (m.take(res)) {}
    CHECK(m.by_vinyl("/dev/sda1") && m.by_vinyl("/dev/sdc1"));
    fb->devices.erase(fb->devices.begin() + 1); /* sdc1 was pulled */
    (void)m.list();                              /* the list forgets it */
    CHECK(!m.by_vinyl("/dev/sdc1"));
    fb->calls.clear();
    CHECK_EQ(m.shutdown(), 1);
    CHECK_EQ(fb->calls.size(), static_cast<size_t>(1));
    CHECK_EQ(fb->calls[0], std::string("unmount /dev/sda1"));
    CHECK(sys_drive.mounted);
    CHECK_EQ(m.shutdown(), 0);

    /* a drive that is mounted already can be made the app's (note_mounted) and is then unmounted at exit */
    auto fake3 = std::make_unique<FakeDrives>();
    FakeDrives *f3 = fake3.get();
    DriveInfo pre = sys_drive;
    pre.dev = "/dev/sdd1";
    f3->devices = {pre};
    DriveManager m3(std::move(fake3));
    m3.note_mounted("/dev/sdd1");
    CHECK(m3.list()[0].by_vinyl);
    CHECK_EQ(m3.shutdown(), 1);

    /* no udisks: the drives are listed, a mount says why (the pages say "Drives need the udisks2 package.") */
    auto fake2 = std::make_unique<FakeDrives>();
    fake2->has_udisks = false;
    fake2->devices = {gen};
    DriveManager m2(std::move(fake2));
    CHECK(!m2.available());
    CHECK_EQ(m2.list().size(), static_cast<size_t>(1));
    m2.mount_async("/dev/sdc1", "Generic Flash Disk");
    m2.wait_idle();
    CHECK(m2.take(res));
    CHECK(res.kind == DriveManager::Result::Kind::MountFailed);
    CHECK_EQ(res.reason, std::string("udisks2 is missing"));
    CHECK_EQ(m2.shutdown(), 0);
}

/* ------------------------------------------------------------------------------------------------ the copy: pure checks */

void test_check_copy() {
    CHECK(check_copy(100 * MiB, 8 * GiB, "vfat", "SANDISK").ok);
    CHECK(check_copy(0, 2 * MiB, "vfat").ok);
    /* free space: the file and a 1 MiB margin must fit */
    CHECK(check_copy(10 * MiB, 11 * MiB, "vfat", "SANDISK").ok);
    const CopyCheck tight = check_copy(10 * MiB, 11 * MiB - 1, "vfat", "SANDISK");
    CHECK(!tight.ok);
    CHECK_EQ(tight.why, std::string("not enough free space on SANDISK"));
    CHECK_EQ(check_copy(10 * MiB, 10 * MiB, "ext4", "SANDISK").why, std::string("not enough free space on SANDISK"));
    CHECK_EQ(check_copy(1, 0, "ext4").why, std::string("not enough free space on the drive")); /* no label */
    CHECK(!check_copy(1, MiB - 1, "ext4").ok);                                                 /* less than the margin */
    CHECK(!check_copy(~0ull, ~0ull, "ext4").ok);                                               /* no overflow */
    /* the FAT limit: 4 GiB or more to a FAT drive */
    const std::string fat_why = "the drive's FAT file system takes files under 4 GB";
    for (const char *fs : {"vfat", "VFAT", "fat", "msdos", "fat32"}) {
        const CopyCheck c = check_copy(4 * GiB, 100 * GiB, fs, "SANDISK");
        CHECK(!c.ok);
        CHECK_EQ(c.why, fat_why);
        CHECK(is_fat_type(fs));
    }
    CHECK(check_copy(4 * GiB - 1, 100 * GiB, "vfat").ok);
    CHECK(check_copy(4 * GiB, 100 * GiB, "exfat").ok);
    CHECK(check_copy(4 * GiB, 100 * GiB, "ext4").ok);
    CHECK(check_copy(5 * GiB, 100 * GiB, "").ok); /* file system unknown: only the space is checked */
    CHECK(!is_fat_type("exfat") && !is_fat_type("") && !is_fat_type("ntfs"));
    /* both: the FAT rule first, it cannot be fixed by deleting something */
    CHECK_EQ(check_copy(5 * GiB, GiB, "vfat", "SANDISK").why, fat_why);
    CHECK_EQ(check_copy(5 * GiB, GiB, "exfat", "SANDISK").why, std::string("not enough free space on SANDISK"));
}

/* ------------------------------------------------------------------------------------------------ the copy: the job */

class StepClock final : public Clock {
public:
    std::atomic<std::uint64_t> now{1000};
    uint64_t mono_ms() override { return now.load(); }
    int64_t wall_s() override { return 1700000000; }
    void sleep_ms(uint32_t) override {}
};

/* The real calls with failures and a gate for the copy: every switch is set before start() or from the test's thread. */
class CopyOps final : public FileOps {
public:
    FileOps &base = posix_file_ops();
    StepClock *step_clock = nullptr;             /* each .part write takes step_ms of it */
    std::uint64_t step_ms = 0;
    std::atomic<int> part_fd{-1};
    std::atomic<std::int64_t> part_bytes{0};     /* written to the .part so far */
    std::atomic<std::int64_t> write_budget{-1};  /* bytes the .part may still take; the call that crosses it is short, then -write_errno */
    std::atomic<int> write_errno{ENOSPC};
    std::atomic<std::int64_t> read_fail_at{-1};  /* a pread at or past this offset gives -EIO */
    std::atomic<std::int64_t> gate_at{-1};       /* a .part write blocks once this many bytes are in, until release */
    std::atomic<bool> blocked{false};
    std::atomic<bool> release{false};
    std::atomic<std::int64_t> free_override{-1}; /* free_space() reports this (the drive's folder must exist) */
    std::string size_path;                       /* stat of this path reports size_override (no real 4 GiB file) */
    std::int64_t size_override = -1;
    std::atomic<int> fdatasyncs{0};

    int open(const std::string &p, int f, int m) override {
        const int fd = base.open(p, f, m);
        if (fd >= 0 && p.size() > 5 && p.compare(p.size() - 5, 5, ".part") == 0) part_fd = fd;
        return fd;
    }
    int close(int fd) override { return base.close(fd); }
    ssize_t write(int fd, const void *b, size_t n) override {
        if (fd != part_fd.load()) return base.write(fd, b, n);
        const std::int64_t gate = gate_at.load();
        if (gate >= 0 && part_bytes.load() >= gate) {
            blocked = true;
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::int64_t budget = write_budget.load();
        if (budget >= 0) {
            if (budget == 0) return -write_errno.load();
            if (static_cast<std::int64_t>(n) > budget) n = static_cast<size_t>(budget);
        }
        const ssize_t w = base.write(fd, b, n);
        if (w > 0) {
            part_bytes += w;
            if (budget >= 0) write_budget -= w;
            if (step_clock) step_clock->now += step_ms;
        }
        return w;
    }
    ssize_t pwrite(int fd, const void *b, size_t n, uint64_t off) override { return base.pwrite(fd, b, n, off); }
    ssize_t pread(int fd, void *b, size_t n, uint64_t off) override {
        const std::int64_t at = read_fail_at.load();
        if (at >= 0 && static_cast<std::int64_t>(off) >= at) return -EIO;
        return base.pread(fd, b, n, off);
    }
    int fdatasync(int fd) override {
        ++fdatasyncs;
        return base.fdatasync(fd);
    }
    int fsync(int fd) override { return base.fsync(fd); }
    int ftruncate(int fd, uint64_t len) override { return base.ftruncate(fd, len); }
    int fstat(int fd, StatInfo &st) override { return base.fstat(fd, st); }
    int stat(const std::string &p, StatInfo &st) override {
        const int r = base.stat(p, st);
        if (r == 0 && size_override >= 0 && p == size_path) st.size = static_cast<uint64_t>(size_override);
        return r;
    }
    int rename(const std::string &a, const std::string &b) override { return base.rename(a, b); }
    int unlink(const std::string &p) override { return base.unlink(p); }
    int mkdir(const std::string &p) override { return base.mkdir(p); }
    int rmdir(const std::string &p) override { return base.rmdir(p); }
    int fsync_dir(const std::string &p) override { return base.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return base.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &avail) override {
        const int r = base.free_space(p, avail);
        const std::int64_t f = free_override.load();
        if (r == 0 && f >= 0) avail = static_cast<uint64_t>(f);
        return r;
    }
};

/* a source in a "home" folder and a drive's mount point in a temporary tree */
struct Rig {
    std::string dir, home, mount, src, name;
    explicit Rig(std::size_t bytes, std::uint32_t seed = 7) {
        dir = make_temp_dir("drives-copy");
        home = dir + "/home";
        mount = dir + "/media/SANDISK";
        make_dirs(home);
        make_dirs(mount);
        name = "scene_014_20261114-185200.mkv";
        src = home + "/" + name;
        write_file(src, pattern_bytes(bytes, seed));
    }
    ~Rig() { rm_tree(dir); }
    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;
    std::string folder() const { return mount + "/VHS"; }
    std::string part() const { return folder() + "/." + name + ".part"; }
    std::string target() const { return folder() + "/" + name; }
};

/* Polls until the job has ended, checking every snapshot on the way: exactly one state flag, the bytes only grow and never pass
 * the total. */
bool wait_ended(CopyJob &j) {
    bool consistent = true;
    std::uint64_t last = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < end) {
        const CopyStatus s = j.status();
        const int flags = (s.running ? 1 : 0) + (s.done ? 1 : 0) + (s.cancelled ? 1 : 0) + (s.failed ? 1 : 0);
        consistent = consistent && flags == 1 && s.bytes >= last && (s.total == 0 || s.bytes <= s.total) && (s.failed || s.error.empty());
        last = s.bytes;
        if (!s.running) {
            CHECK(consistent);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false); /* the job did not end */
    return false;
}

bool no_job_yet(const CopyStatus &s) {
    return !s.running && !s.done && !s.cancelled && !s.failed && s.error.empty() && s.bytes == 0 && s.total == 0 && s.eta_s == -1;
}

void test_copy_ok() {
    Rig rig(3 * MiB + 12345);
    CopyJob job;
    CHECK(no_job_yet(job.status()));
    CHECK_EQ(copy_progress_text(job.status()), std::string(""));
    CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
    CHECK(wait_ended(job));
    CopyStatus s = job.status();
    CHECK(s.done && !s.running && !s.cancelled && !s.failed);
    CHECK_EQ(s.error, std::string(""));
    CHECK_EQ(s.target, rig.target());
    CHECK_EQ(s.label, std::string("SANDISK"));
    CHECK_EQ(s.total, 3 * MiB + 12345);
    CHECK_EQ(s.bytes, s.total);
    CHECK_EQ(s.eta_s, static_cast<std::int64_t>(0));
    CHECK_NEAR(copy_fraction(s), 1.0, 1e-9);
    CHECK_EQ(copy_progress_text(s), std::string("3 MB copied"));
    /* the VHS folder was made, the file is there and nothing else (no .part) */
    CHECK(is_dir(rig.folder()));
    CHECK(names_in(rig.folder()) == std::vector<std::string>{rig.name});
    CHECK(slurp_file(rig.target()) == slurp_file(rig.src));
    CHECK_EQ(slurp_file(rig.target()).size(), static_cast<std::size_t>(3 * MiB + 12345));
    CHECK(exists(rig.src)); /* a copy, not a move */

    /* the same file again: refused, nothing touched; the same job object takes the next one */
    CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
    CHECK(wait_ended(job));
    s = job.status();
    CHECK(s.failed && !s.done && !s.running && !s.cancelled);
    CHECK_EQ(s.error, std::string("a file of that name is on the drive"));
    CHECK(names_in(rig.folder()) == std::vector<std::string>{rig.name});
    CHECK(slurp_file(rig.target()) == slurp_file(rig.src));
    CHECK_EQ(copy_progress_text(s), std::string("Not copied: a file of that name is on the drive"));

    /* another file, an exact multiple of the chunk, and an empty one */
    for (const std::size_t n : {static_cast<std::size_t>(2 * MiB), static_cast<std::size_t>(0)}) {
        const std::string name = n ? "photo_013_20261114-184710.jpg" : "scene_015_nodate.mkv";
        const std::string src = rig.home + "/" + name;
        write_file(src, pattern_bytes(n, 11));
        CHECK(job.start(src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK(job.status().done);
        CHECK(slurp_file(rig.folder() + "/" + name) == slurp_file(src));
    }
    CHECK_EQ(names_in(rig.folder()).size(), static_cast<size_t>(3));
    for (const std::string &n : names_in(rig.folder())) CHECK(n.compare(0, 1, ".") != 0); /* no .part anywhere */

    /* a mount path with a trailing slash */
    Rig rig2(100000);
    CHECK(job.start(rig2.src, rig2.mount + "/", "SANDISK", "vfat"));
    CHECK(wait_ended(job));
    CHECK(job.status().done);
    CHECK(slurp_file(rig2.target()) == slurp_file(rig2.src));
}

void test_copy_cancel() {
    Rig rig(4 * MiB + 1);
    CopyOps ops;
    ops.gate_at = static_cast<std::int64_t>(MiB); /* the second chunk waits */
    CopyJob job(ops);
    CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
    CHECK(wait_for(10000, [&] { return ops.blocked.load(); }));
    /* in the middle: the .part is on the drive, the file is not */
    CHECK(exists(rig.part()));
    CHECK(!exists(rig.target()));
    CopyStatus mid = job.status();
    CHECK(mid.running && !mid.done && !mid.cancelled && !mid.failed);
    CHECK_EQ(mid.bytes, MiB);
    CHECK_EQ(mid.total, 4 * MiB + 1);
    CHECK(copy_fraction(mid) > 0.2 && copy_fraction(mid) < 0.3);
    CHECK(!job.start(rig.src, rig.mount, "SANDISK", "vfat")); /* one copy at a time */
    CHECK(job.status().running);
    job.cancel();
    ops.release = true;
    CHECK(wait_ended(job));
    CopyStatus s = job.status();
    CHECK(s.cancelled && !s.done && !s.running && !s.failed);
    CHECK_EQ(s.error, std::string(""));
    CHECK(!exists(rig.part()));
    CHECK(!exists(rig.target()));
    CHECK(!exists(rig.folder())); /* G7: nothing on the drive, not even the VHS folder the copy made */
    CHECK_EQ(copy_progress_text(s), std::string("Copy cancelled"));
    CHECK_EQ(s.eta_s, static_cast<std::int64_t>(-1));
    /* after a cancel the next copy runs and completes */
    ops.gate_at = -1;
    CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
    CHECK(wait_ended(job));
    CHECK(job.status().done);
    CHECK(slurp_file(rig.target()) == slurp_file(rig.src));
    CHECK(names_in(rig.folder()) == std::vector<std::string>{rig.name});
    CHECK_EQ(ops.fdatasyncs.load(), 1); /* the cancelled copy never reached 4 MiB; the full one synced once, at 4 MiB */

    /* G7: a VHS folder that was on the drive before (with a file in it) stays after a cancel, its file too */
    {
        Rig rig3(4 * MiB + 1);
        make_dirs(rig3.folder());
        write_file(rig3.folder() + "/older.mkv", "old");
        CopyOps o3;
        o3.gate_at = static_cast<std::int64_t>(MiB);
        CopyJob j3(o3);
        CHECK(j3.start(rig3.src, rig3.mount, "SANDISK", "vfat"));
        CHECK(wait_for(10000, [&] { return o3.blocked.load(); }));
        j3.cancel();
        o3.release = true;
        CHECK(wait_ended(j3));
        CHECK(j3.status().cancelled);
        CHECK(names_in(rig3.folder()) == std::vector<std::string>{"older.mkv"});
        /* an empty VHS folder that was there before stays too */
        Rig rig4(4 * MiB + 1);
        make_dirs(rig4.folder());
        CopyOps o4;
        o4.gate_at = static_cast<std::int64_t>(MiB);
        CopyJob j4(o4);
        CHECK(j4.start(rig4.src, rig4.mount, "SANDISK", "vfat"));
        CHECK(wait_for(10000, [&] { return o4.blocked.load(); }));
        j4.cancel();
        o4.release = true;
        CHECK(wait_ended(j4));
        CHECK(is_dir(rig4.folder()) && names_in(rig4.folder()).empty());
    }

    /* a cancel with nothing running has no effect on the next start */
    Rig rig2(2 * MiB);
    CopyJob j2;
    j2.cancel();
    CHECK(j2.start(rig2.src, rig2.mount, "SANDISK", "vfat"));
    CHECK(wait_ended(j2));
    CHECK(j2.status().done);
}

void test_copy_destructor_abandons() {
    Rig rig(4 * MiB);
    CopyOps ops;
    ops.gate_at = static_cast<std::int64_t>(MiB);
    std::thread releaser;
    {
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_for(10000, [&] { return ops.blocked.load(); }));
        CHECK(exists(rig.part()));
        /* the destructor cancels and joins; the held write is let go a moment later */
        releaser = std::thread([&ops] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ops.release = true;
        });
    }
    releaser.join();
    CHECK(!exists(rig.part()));
    CHECK(!exists(rig.target()));
}

void test_copy_refusals() {
    const std::string fat_why = "the drive's FAT file system takes files under 4 GB";
    /* a file of that name is on the drive already */
    {
        Rig rig(3 * MiB);
        make_dirs(rig.folder());
        put(rig.target(), "old");
        CopyJob job;
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        const CopyStatus s = job.status();
        CHECK(s.failed && !s.running);
        CHECK_EQ(s.error, std::string("a file of that name is on the drive"));
        CHECK_EQ(slurp_file(rig.target()), std::string("old"));
        CHECK(names_in(rig.folder()) == std::vector<std::string>{rig.name}); /* no .part */
        const CopyCheck pf = preflight_copy(posix_file_ops(), rig.src, rig.mount, "vfat", "SANDISK");
        CHECK(!pf.ok);
        CHECK_EQ(pf.why, std::string("a file of that name is on the drive"));
    }
    /* not enough free space: refused before anything is made on the drive */
    {
        Rig rig(3 * MiB);
        CopyOps ops;
        ops.free_override = static_cast<std::int64_t>(2 * MiB);
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "ext4"));
        CHECK(wait_ended(job));
        const CopyStatus s = job.status();
        CHECK(s.failed);
        CHECK_EQ(s.error, std::string("not enough free space on SANDISK"));
        CHECK(!exists(rig.folder()));
        CopyJob j2(ops);
        CHECK(j2.start(rig.src, rig.mount, "", "ext4"));
        CHECK(wait_ended(j2));
        CHECK_EQ(j2.status().error, std::string("not enough free space on the drive"));
        /* just enough: file and margin */
        ops.free_override = static_cast<std::int64_t>(4 * MiB);
        CopyJob j3(ops);
        CHECK(j3.start(rig.src, rig.mount, "SANDISK", "ext4"));
        CHECK(wait_ended(j3));
        CHECK(j3.status().done);
    }
    /* a file of 4 GiB to a vfat drive: no real 4 GiB file, the source reports its size */
    {
        Rig rig(1000);
        CopyOps ops;
        ops.size_path = rig.src;
        ops.size_override = static_cast<std::int64_t>(4 * GiB);
        ops.free_override = static_cast<std::int64_t>(100 * GiB);
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        const CopyStatus s = job.status();
        CHECK(s.failed);
        CHECK_EQ(s.error, fat_why);
        CHECK(!exists(rig.folder()));
        CHECK_EQ(copy_progress_text(s), "Not copied: " + fat_why);
        /* one byte less is allowed on vfat: the job starts, and the real (small) file then ends early */
        ops.size_override = static_cast<std::int64_t>(4 * GiB - 1);
        CopyJob j2(ops);
        CHECK(j2.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(j2));
        CHECK_EQ(j2.status().error, std::string("the file changed while it was copied"));
        CHECK(names_in(rig.folder()).empty()); /* the .part is removed */
        /* exFAT takes 4 GiB (the preflight passes: the same early end) */
        ops.size_override = static_cast<std::int64_t>(4 * GiB);
        CopyJob j3(ops);
        CHECK(j3.start(rig.src, rig.mount, "BIGSTICK", "exfat"));
        CHECK(wait_ended(j3));
        CHECK_EQ(j3.status().error, std::string("the file changed while it was copied"));
        CHECK(names_in(rig.folder()).empty());
        /* without the file system type the job cannot apply the FAT rule (the caller's check_copy does) */
        CopyJob j4(ops);
        CHECK(j4.start(rig.src, rig.mount, "SANDISK"));
        CHECK(wait_ended(j4));
        CHECK_EQ(j4.status().error, std::string("the file changed while it was copied"));
    }
    /* the source or the drive is not there */
    {
        Rig rig(1000);
        CopyJob job;
        CHECK(job.start(rig.home + "/missing.mkv", rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK_EQ(job.status().error, std::string("the file is not there"));
        CHECK(job.start(rig.src, rig.dir + "/nodrive", "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK_EQ(job.status().error, std::string("the drive is not there"));
        CHECK(job.start(rig.src, "", "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK_EQ(job.status().error, std::string("the drive is not mounted"));
        CHECK(job.start("", rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK_EQ(job.status().error, std::string("the file is not there"));
        CHECK(!exists(rig.folder()));
    }
}

void test_copy_failures() {
    /* ENOSPC in the middle: the second chunk and 100 bytes of the third go in, then the drive says full */
    {
        Rig rig(3 * MiB + 12345);
        CopyOps ops;
        ops.write_budget = static_cast<std::int64_t>(2 * MiB + 100);
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        const CopyStatus s = job.status();
        CHECK(s.failed && !s.done && !s.cancelled && !s.running);
        CHECK_EQ(s.error, std::string("not enough free space on SANDISK"));
        CHECK_EQ(s.bytes, 2 * MiB); /* the whole chunks that went in */
        CHECK_EQ(copy_progress_text(s), std::string("Not copied: not enough free space on SANDISK"));
        CHECK(!exists(rig.part())); /* the .part is removed: it freed the drive */
        CHECK(!exists(rig.target()));
        CHECK(names_in(rig.folder()).empty());
    }
    /* the same as short writes: the first call takes part of a chunk, write_all asks again */
    {
        Rig rig(2 * MiB + 5);
        CopyOps ops;
        ops.write_budget = static_cast<std::int64_t>(MiB + 7);
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "", "ext4"));
        CHECK(wait_ended(job));
        CHECK_EQ(job.status().error, std::string("not enough free space on the drive"));
        CHECK(names_in(rig.folder()).empty());
    }
    /* other write errors: each has its own words */
    struct {
        int err;
        const char *why;
    } errs[] = {{EIO, "the drive stopped answering"}, {EROFS, "the drive is read-only"}, {EACCES, "no permission to write to the drive"},
                {EFBIG, "the drive's FAT file system takes files under 4 GB"}, {EINVAL, "could not write to the drive (error 22)"}};
    for (const auto &e : errs) {
        Rig rig(2 * MiB);
        CopyOps ops;
        ops.write_budget = static_cast<std::int64_t>(MiB);
        ops.write_errno = e.err;
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK(job.status().failed);
        CHECK_EQ(job.status().error, std::string(e.why));
        CHECK(names_in(rig.folder()).empty());
    }
    /* a read error in the middle of the source */
    {
        Rig rig(3 * MiB);
        CopyOps ops;
        ops.read_fail_at = static_cast<std::int64_t>(2 * MiB);
        CopyJob job(ops);
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        const CopyStatus s = job.status();
        CHECK(s.failed);
        CHECK_EQ(s.error, std::string("could not read the file (error 5)"));
        CHECK_EQ(s.bytes, 2 * MiB);
        CHECK(names_in(rig.folder()).empty());
        CHECK(!exists(rig.target()));
    }
    /* the drive's folder cannot be made (a plain file called VHS is in the way) */
    {
        Rig rig(1000);
        put(rig.folder(), "not a folder");
        CopyJob job;
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK(job.status().failed);
        CHECK_EQ(job.status().error, std::string("the drive has a file called VHS in the way"));
        CHECK_EQ(slurp_file(rig.folder()), std::string("not a folder"));
    }
    /* a stale .part of an earlier crash is overwritten, and gone after the copy */
    {
        Rig rig(1500000);
        make_dirs(rig.folder());
        put(rig.part(), "stale bytes of an interrupted copy that are longer than nothing");
        CopyJob job;
        CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
        CHECK(wait_ended(job));
        CHECK(job.status().done);
        CHECK(names_in(rig.folder()) == std::vector<std::string>{rig.name});
        CHECK(slurp_file(rig.target()) == slurp_file(rig.src));
    }
}

void test_copy_rate_and_syncs() {
    /* 1 MiB per 100 ms of a fake clock: 10.49 MB/s; 25 MiB; held at 10 MiB to read the status */
    Rig rig(25 * MiB);
    StepClock clk;
    CopyOps ops;
    ops.step_clock = &clk;
    ops.step_ms = 100;
    ops.gate_at = static_cast<std::int64_t>(10 * MiB);
    CopyJob job(ops, clk);
    CHECK(job.start(rig.src, rig.mount, "SANDISK", "vfat"));
    CHECK(wait_for(20000, [&] { return ops.blocked.load(); }));
    CopyStatus s = job.status();
    CHECK(s.running);
    CHECK_EQ(s.bytes, 10 * MiB);
    CHECK_EQ(s.total, 25 * MiB);
    CHECK_NEAR(s.bytes_per_s, 10485760.0, 1.0);
    CHECK_EQ(s.eta_s, static_cast<std::int64_t>(2)); /* 15 MiB left at 10 MiB per second: 1.5 s, rounded up */
    CHECK_EQ(copy_progress_text(s), std::string("10 MB of 26 MB, about 2 s left"));
    CHECK_NEAR(copy_fraction(s), 0.4, 1e-9);
    ops.release = true;
    CHECK(wait_ended(job));
    s = job.status();
    CHECK(s.done);
    CHECK_NEAR(s.bytes_per_s, 10485760.0, 1.0);
    CHECK_EQ(s.eta_s, static_cast<std::int64_t>(0));
    CHECK(slurp_file(rig.target()) == slurp_file(rig.src));
    /* the data is flushed as it goes (every 4 MiB), not only at the end: 25 MiB gives 6 */
    CHECK_EQ(ops.fdatasyncs.load(), 6);

    /* a slow start: no rate until the first half second has passed, so no time left either */
    Rig rig2(3 * MiB);
    StepClock clk2;
    CopyOps ops2;
    ops2.step_clock = &clk2;
    ops2.step_ms = 100;
    ops2.gate_at = static_cast<std::int64_t>(2 * MiB);
    CopyJob j2(ops2, clk2);
    CHECK(j2.start(rig2.src, rig2.mount, "SANDISK", "vfat"));
    CHECK(wait_for(20000, [&] { return ops2.blocked.load(); }));
    s = j2.status();
    CHECK_EQ(s.bytes_per_s, 0.0);
    CHECK_EQ(s.eta_s, static_cast<std::int64_t>(-1));
    CHECK_EQ(copy_progress_text(s), std::string("2 MB of 3 MB"));
    ops2.release = true;
    CHECK(wait_ended(j2));
    CHECK(j2.status().done);
}

/* ------------------------------------------------------------------------------------------------ the texts */

void test_copy_texts() {
    CopyStatus s;
    s.running = true;
    s.label = "SANDISK";
    s.bytes = 630000000;
    s.total = 1400000000;
    s.eta_s = 40;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 40 s left")); /* the canvas CopyToUsb line */
    s.eta_s = 41;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 40 s left")); /* no flicker: nearest 5 s from 10 s on */
    s.eta_s = 43;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 45 s left"));
    s.eta_s = 9;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 9 s left"));
    s.eta_s = 1;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 1 s left"));
    s.eta_s = 0;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, almost done"));
    s.eta_s = 58;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 1 min left"));
    s.eta_s = 200;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 3 min left"));
    s.eta_s = 12000;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB, about 3 h 20 min left"));
    s.eta_s = -1;
    CHECK_EQ(copy_progress_text(s), std::string("630 MB of 1.4 GB"));
    s.bytes = 0;
    s.total = 98000;
    CHECK_EQ(copy_progress_text(s), std::string("0 KB of 98 KB"));
    s.total = 0;
    CHECK_EQ(copy_progress_text(s), std::string("Preparing"));
    CHECK_NEAR(copy_fraction(s), 0.0, 1e-12);
    s.total = 1400000000;
    s.bytes = 630000000;
    CHECK_NEAR(copy_fraction(s), 0.45, 1e-9);
    s.bytes = 2000000000;
    CHECK_NEAR(copy_fraction(s), 1.0, 1e-12);

    CopyStatus d;
    d.done = true;
    d.total = 1400000000;
    CHECK_EQ(copy_progress_text(d), std::string("1.4 GB copied"));
    CHECK_NEAR(copy_fraction(d), 1.0, 1e-12);
    CopyStatus c;
    c.cancelled = true;
    CHECK_EQ(copy_progress_text(c), std::string("Copy cancelled"));
    CopyStatus f;
    f.failed = true;
    f.error = "the drive is read-only";
    CHECK_EQ(copy_progress_text(f), std::string("Not copied: the drive is read-only"));
    CHECK_EQ(copy_progress_text(CopyStatus()), std::string(""));

    CHECK_EQ(copy_title("SANDISK"), std::string("Copying to SANDISK"));
    CHECK_EQ(copy_title("Generic Flash Disk"), std::string("Copying to Generic Flash Disk"));
    CHECK_EQ(copy_title(""), std::string("Copying to the drive"));
}

/* ------------------------------------------------------------------------------------------------ the demo drives */

void test_demo_drives() {
    const std::string dir = make_temp_dir("drives-demo");
    /* the default case: two drives, unmounted; a mount makes its folder */
    {
        FakeDrives *fb = nullptr;
        DriveManager m(make_demo_drives("1", dir, &fb));
        CHECK(fb != nullptr);
        CHECK(m.available());
        std::vector<DriveInfo> l = m.list();
        CHECK_EQ(l.size(), static_cast<size_t>(2));
        CHECK_EQ(l[0].name(), std::string("SANDISK"));
        CHECK_EQ(l[0].fstype, std::string("vfat"));
        CHECK(!l[0].mounted && !l[1].mounted);
        m.mount_async("/dev/sda1", "SANDISK");
        m.wait_idle();
        DriveManager::Result res;
        CHECK(m.take(res));
        CHECK(res.kind == DriveManager::Result::Kind::Mounted);
        CHECK_EQ(res.mount_dir, dir + "/media/SANDISK");
        CHECK(is_dir(res.mount_dir));
        l = m.list();
        CHECK(l[0].mounted && l[0].by_vinyl);
        CHECK_EQ(l[0].mount_dir, res.mount_dir);
        /* the whole flow the controller runs: the drive's mount, then the copy */
        const std::string src = dir + "/scene_001_nodate.mkv";
        put(src, pattern_bytes(300000, 3));
        CopyJob job;
        CHECK(job.start(src, res.mount_dir, "SANDISK", l[0].fstype));
        CHECK(wait_ended(job));
        CHECK(job.status().done);
        CHECK(slurp_file(res.mount_dir + "/VHS/scene_001_nodate.mkv") == slurp_file(src));
        CHECK_EQ(m.shutdown(), 1); /* the app unmounts what it mounted at exit */
    }
    {
        DriveManager m(make_demo_drives("mounted", dir));
        std::vector<DriveInfo> l = m.list();
        CHECK_EQ(l.size(), static_cast<size_t>(2));
        CHECK(l[0].mounted && !l[0].by_vinyl);
        CHECK(is_dir(l[0].mount_dir));
        CHECK_EQ(m.shutdown(), 0); /* the system's mount is left alone */
    }
    {
        DriveManager m(make_demo_drives("one", dir));
        CHECK_EQ(m.list().size(), static_cast<size_t>(1));
        CHECK_EQ(m.shutdown(), 0);
    }
    {
        DriveManager m(make_demo_drives("exfat", dir));
        std::vector<DriveInfo> l = m.list();
        CHECK_EQ(l.size(), static_cast<size_t>(1));
        CHECK_EQ(l[0].fstype, std::string("exfat"));
        CHECK_EQ(m.shutdown(), 0);
    }
    {
        DriveManager m(make_demo_drives("none", dir));
        CHECK(m.available());
        CHECK(m.list().empty());
        CHECK_EQ(m.shutdown(), 0);
    }
    const struct {
        const char *mode, *reason;
    } bad[] = {{"denied", "this session is not allowed to mount drives"}, {"nofs", "the disk has no readable filesystem"}, {"nodisks", "udisks2 is missing"}};
    for (const auto &b : bad) {
        DriveManager m(make_demo_drives(b.mode, dir));
        CHECK_EQ(m.available(), std::string(b.mode) != "nodisks");
        CHECK_EQ(m.list().size(), static_cast<size_t>(2));
        m.mount_async("/dev/sda1", "SANDISK");
        m.wait_idle();
        DriveManager::Result res;
        CHECK(m.take(res));
        CHECK(res.kind == DriveManager::Result::Kind::MountFailed);
        CHECK_EQ(res.reason, std::string(b.reason));
        CHECK_EQ(m.shutdown(), 0);
    }
    /* the choice from the environment */
    const char *old = std::getenv("VHS_FAKE_DRIVES");
    const std::string saved = old ? old : "";
    ::unsetenv("VHS_FAKE_DRIVES");
    FakeDrives *fb = reinterpret_cast<FakeDrives *>(1);
    std::unique_ptr<DriveBackend> real = make_drive_backend(dir, &fb);
    CHECK(real != nullptr);
    CHECK(fb == nullptr);
    CHECK(dynamic_cast<FakeDrives *>(real.get()) == nullptr);
    ::setenv("VHS_FAKE_DRIVES", "one", 1);
    std::unique_ptr<DriveBackend> demo = make_drive_backend(dir, &fb);
    CHECK(fb != nullptr);
    CHECK(demo.get() == fb);
    CHECK_EQ(demo->list().size(), static_cast<size_t>(1));
    if (old) ::setenv("VHS_FAKE_DRIVES", saved.c_str(), 1);
    else ::unsetenv("VHS_FAKE_DRIVES");
    rm_tree(dir);
}

/* the Scenes page's steps (app/copy_flow.hpp) over the demo drives: the notices, the drive list, the mount, the copy,
 * Cancel, the exit's unmount */
void test_copy_flow() {
    const std::string dir = make_temp_dir("copy-flow");
    const std::string src = dir + "/scene_002_nodate.mkv";
    put(src, pattern_bytes(3 * MiB, 7));
    std::vector<std::string> notes;
    auto note = [&](const Notice &n) { notes.push_back(n.text); };
    auto run = [](CopyFlow &f) {
        for (int i = 0; i < 1000 && f.busy() && f.view().stage != CopyView::Stage::Choosing; ++i) {
            f.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    /* no drive: the notice, nothing else */
    {
        CopyFlow f([&] { return make_demo_drives("none", dir + "/a"); }, note);
        f.request(src);
        CHECK(!f.busy());
        CHECK_EQ(notes.size(), static_cast<size_t>(1));
        CHECK_EQ(notes.back(), std::string(kNoticeNoDrive));
    }
    /* one drive, not mounted: VHS mounts it (Vinyl's notice), the copy runs, "Copied to SANDISK.", unmounted at exit */
    notes.clear();
    {
        CopyFlow f([&] { return make_demo_drives("one", dir + "/b"); }, note);
        f.request(src);
        CHECK(f.view().stage == CopyView::Stage::Mounting);
        CHECK_EQ(notes.back(), std::string("Mounting SANDISK..."));
        run(f);
        CHECK(!f.busy());
        CHECK_EQ(notes.back(), std::string("Copied to SANDISK."));
        CHECK(slurp_file(dir + "/b/media/SANDISK/VHS/scene_002_nodate.mkv") == slurp_file(src));
    }
    /* two drives: the list (Vinyl's rows), a choice of the mounted one copies at once */
    notes.clear();
    {
        CopyFlow f([&] { return make_demo_drives("mounted", dir + "/c"); }, note);
        f.request(src);
        CHECK(f.view().stage == CopyView::Stage::Choosing);
        CHECK_EQ(f.view().drives.size(), static_cast<size_t>(2));
        CHECK_EQ(f.view().drives[0].label, std::string("SANDISK"));
        CHECK(f.view().drives[0].value.find(", mounted") != std::string::npos);
        CHECK_EQ(f.view().drives[1].value, std::string("not mounted"));
        f.dismiss();
        CHECK(!f.busy());
        CHECK(notes.empty());
        f.request(src);
        f.choose(0);
        CHECK(f.view().stage == CopyView::Stage::Copying);
        CHECK_EQ(f.view().title, std::string("Copying to SANDISK"));
        CHECK_EQ(f.view().text, std::string("scene_002_nodate.mkv, 3 MB. ") + kCopyNote);
        run(f);
        CHECK_EQ(notes.back(), std::string("Copied to SANDISK."));
        /* the same file again: refused before it starts */
        f.request(src);
        f.choose(0);
        run(f);
        CHECK_EQ(notes.back(), std::string("Not copied: a file of that name is on the drive."));
    }
    /* no udisks2 and the drive not mounted: Vinyl's notice; a refused mount: its reason */
    notes.clear();
    {
        CopyFlow f([&] { return make_demo_drives("nodisks", dir + "/d"); }, note);
        f.request(src);
        f.choose(0);
        CHECK(!f.busy());
        CHECK_EQ(notes.back(), std::string(kNoticeNoUdisks));
    }
    {
        CopyFlow f([&] { return make_demo_drives("denied", dir + "/e"); }, note);
        f.request(src);
        f.choose(0);
        run(f);
        CHECK(!f.busy());
        CHECK_EQ(notes.back(), std::string("Could not mount it: this session is not allowed to mount drives."));
    }
    /* Cancel during a slow copy: no file, no .part, no notice; the progress text while it runs */
    notes.clear();
    {
        std::unique_ptr<FileOps> slow = throttled_file_ops(posix_file_ops(), 2 * MiB);
        CopyFlow f([&] { return make_demo_drives("mounted", dir + "/f"); }, note, *slow);
        f.request(src);
        f.choose(0);
        for (int i = 0; i < 400 && f.view().progress.find(" of ") == std::string::npos; ++i) {
            f.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(f.view().progress.find(" of 3 MB") != std::string::npos);
        f.cancel();
        run(f);
        CHECK(!f.busy());
        CHECK(notes.empty());
        CHECK(!exists(dir + "/f/media/SANDISK/VHS/scene_002_nodate.mkv"));
        CHECK(!exists(dir + "/f/media/SANDISK/VHS/.scene_002_nodate.mkv.part"));
    }
    rm_tree(dir);
}

}  // namespace

void test_drives() {
    test_drive_list_and_replies();
    test_drive_manager();
    test_check_copy();
    test_copy_texts();
    test_copy_ok();
    test_copy_cancel();
    test_copy_destructor_abandons();
    test_copy_refusals();
    test_copy_failures();
    test_copy_rate_and_syncs();
    test_demo_drives();
    test_copy_flow();
}
