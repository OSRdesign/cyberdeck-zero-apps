/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the drive backend the app opens, and the demo drives of the headless scenes and the tests (brief 15.3: "FakeDrives").
 * Vinyl builds its demo drives inside its UI (VINYL_DEMO_DRIVES in vinyl_app.cpp); VHS keeps them here, LVGL-free, so the
 * controller, the harness and the tests choose the backend the same way:
 *
 *   auto drives = std::make_unique<DriveManager>(make_drive_backend(demo_root));
 *
 * VHS_FAKE_DRIVES unset or empty: the real backend (make_udisks_backend()). Set: a FakeDrives whose mount points are real
 * folders under <root>/media (made when a drive is "mounted"), chosen by the value:
 *
 *   (any other value, e.g. 1, unmounted)   SANDISK (vfat, 31 GB) and Generic Flash Disk (vfat, 15.5 GB), both plugged in and not
 *                                          mounted; a mount makes <root>/media/<name> and says ok
 *   one        SANDISK only
 *   mounted    SANDISK already mounted (by the "system": the app never unmounts it) and the generic one not mounted
 *   exfat      one exFAT drive (no 4 GiB limit), not mounted
 *   denied     as the default, but every mount is refused: "this session is not allowed to mount drives"
 *   nofs       every mount fails: "the disk has no readable filesystem"
 *   nodisks    no udisks2: the drive is listed, a mount says "udisks2 is missing" (the page shows "Drives need the udisks2 package.")
 *   none       udisks2 there, no drive plugged in
 */
#pragma once

#include "drives.hpp"

#include <memory>
#include <string>

namespace vhs {

/* The demo drives for `mode` (see above; "" is the default case) with mount points under root/media; root is made. When fake_out
 * is given it gets the FakeDrives inside the returned backend (the scenes "pull" a drive by editing its `devices`: only while the
 * DriveManager is idle). */
std::unique_ptr<DriveBackend> make_demo_drives(const std::string &mode, const std::string &root, FakeDrives **fake_out = nullptr);

/* The app's backend: udisks, or the demo drives when VHS_FAKE_DRIVES is set (root: where their folders go; "" = a folder
 * vhs-demo-drives under $TMPDIR or /tmp). */
std::unique_ptr<DriveBackend> make_drive_backend(const std::string &demo_root = "", FakeDrives **fake_out = nullptr);

} // namespace vhs
