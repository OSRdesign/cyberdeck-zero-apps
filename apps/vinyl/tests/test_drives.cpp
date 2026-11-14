/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the drives that are mounted on request (src/core/drives, owner decision 2026-11-14 "Mount on request in
 * Vinyl"): the list of removable drives from a fake sysfs and udev tree, udisks's answers read as replies (mounted, already
 * mounted, not authorized, no file system, busy, unknown), and the manager with a fake backend: a mount that works, one that is
 * denied, an unmount at exit of what Vinyl mounted and nothing else, Eject, a drive the system mounted left alone, a drive
 * pulled, no udisks.
 */
#include "check.hpp"

#include "drives/drives.hpp"

#include <sys/stat.h>

using namespace vinyl;

namespace {

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

}  // namespace

void test_drives() {
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

    /* the manager on a fake backend */
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
    /* a mount that works: answered off this thread, Vinyl's to undo */
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
    /* Eject of Vinyl's: unmounted, no longer ours */
    m.unmount_async("/dev/sda1", "SANDISK");
    m.wait_idle();
    CHECK(m.take(res));
    CHECK(res.kind == DriveManager::Result::Kind::Unmounted);
    CHECK_EQ(res.name, std::string("SANDISK"));
    CHECK(!m.by_vinyl("/dev/sda1"));
    CHECK(!m.busy());
    /* at exit: what Vinyl mounted is unmounted, what the system mounted is not; a drive pulled meanwhile is not asked for */
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
