/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the demo drives (see demo_drives.hpp).
 */
#include "demo_drives.hpp"

#include "scenes/atomic_file.hpp" /* make_dirs */
#include "scenes/fileops.hpp"

#include <cstdlib>

namespace vhs {

std::unique_ptr<DriveBackend> make_demo_drives(const std::string &mode, const std::string &root, FakeDrives **fake_out)
{
    auto fake = std::make_unique<FakeDrives>();
    const std::string base = root + "/media";
    make_dirs(posix_file_ops(), base);
    fake->mount_base = base;

    DriveInfo sandisk, generic, exfat;
    sandisk.dev = "/dev/sda1";
    sandisk.label = "SANDISK";
    sandisk.fstype = "vfat";
    sandisk.uuid = "ABCD-1234";
    sandisk.size = 31000000000ull;
    generic.dev = "/dev/sdb1";
    generic.label = "Generic Flash Disk";
    generic.fstype = "vfat";
    generic.uuid = "1234-ABCD";
    generic.size = 15500000000ull;
    exfat.dev = "/dev/sdc1";
    exfat.label = "BIGSTICK";
    exfat.fstype = "exfat";
    exfat.uuid = "5A5A-0001";
    exfat.size = 128000000000ull;

    if (mode == "none") {
        /* udisks2 is there and nothing is plugged in */
    } else if (mode == "one") {
        fake->devices = {sandisk};
    } else if (mode == "exfat") {
        fake->devices = {exfat};
    } else if (mode == "mounted") {
        sandisk.mounted = true;
        sandisk.mount_dir = base + "/SANDISK";
        make_dirs(posix_file_ops(), sandisk.mount_dir);
        fake->devices = {sandisk, generic};
    } else {
        fake->devices = {sandisk, generic};
    }
    if (mode == "nodisks") fake->has_udisks = false;
    if (mode == "denied" || mode == "nofs") {
        const std::string why = mode == "denied" ? "this session is not allowed to mount drives" : "the disk has no readable filesystem";
        fake->on_mount = [why](const std::string &) { return MountReply{false, "", why}; };
    } else {
        FakeDrives *raw = fake.get();
        fake->on_mount = [raw, base](const std::string &dev) {
            MountReply r;
            for (const DriveInfo &d : raw->devices)
                if (d.dev == dev) {
                    r.ok = true;
                    r.mount_dir = base + "/" + d.name();
                }
            if (!r.ok) r.reason = "no such device";
            else make_dirs(posix_file_ops(), r.mount_dir);
            return r;
        };
    }
    if (fake_out) *fake_out = fake.get();
    return fake;
}

std::unique_ptr<DriveBackend> make_drive_backend(const std::string &demo_root, FakeDrives **fake_out)
{
    if (fake_out) *fake_out = nullptr;
    const char *mode = std::getenv("VHS_FAKE_DRIVES");
    if (!mode || !mode[0]) return make_udisks_backend();
    std::string root = demo_root;
    if (root.empty()) {
        const char *tmp = std::getenv("TMPDIR");
        root = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/vhs-demo-drives";
    }
    return make_demo_drives(mode, root, fake_out);
}

} // namespace vhs
