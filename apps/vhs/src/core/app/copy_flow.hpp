/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: copying a scene to a USB drive, the steps the Scenes page shows (brief 9.3, D33; canvas CopyToUsb-480x320), over
 * Vinyl's DriveManager (drives/drives.hpp) and the copy job (drives/copy.hpp):
 *
 *   request  no drive: the notice "No USB drive found." (provisional); one drive: that one at once; several: Choosing (the
 *            drive list, Vinyl's rows: the name, "not mounted" / "<size>, mounted" / "mounted by VHS"; provisional, 12.4
 *            item 5)
 *   a drive  mounted (by the system or by VHS): the copy starts; not mounted: udisks2 mounts it (Vinyl: "Mounting
 *            SANDISK...", then the copy; "Could not mount it: <reason>."; without udisks2 "Drives need the udisks2
 *            package.")
 *   Copying  the dialog's title, its line ("<name>, 1.4 GB. The file appears on the drive only when the copy is
 *            complete."), the bar and "630 MB of 1.4 GB, about 40 s left"; cancel() deletes the .part
 *   the end  done: "Copied to SANDISK." (provisional); failed or refused: "Not copied: <why>."; cancelled: nothing
 * At exit (the destructor) a running copy is cancelled first, then the drives VHS mounted are unmounted (never one the
 * system mounted). Polled from the UI thread (step); the mounts and the copy run on threads of their own. LVGL-free.
 */
#pragma once

#include "app/notice_queue.hpp"
#include "drives/copy.hpp"
#include "drives/drives.hpp"
#include "scenes/fileops.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vhs {

constexpr const char *kNoticeNoDrive = "No USB drive found.";                    /* provisional */
constexpr const char *kNoticeNoUdisks = "Drives need the udisks2 package.";     /* Vinyl */
constexpr const char *kNoticeCopied = "Copied to ";                             /* + the drive's name + "." (provisional) */
constexpr const char *kNoticeNotCopied = "Not copied: ";                        /* + the reason + "." */
constexpr const char *kNoticeMountFailed = "Could not mount it: ";              /* Vinyl: + the reason + "." */
constexpr const char *kCopyNote = "The file appears on the drive only when the copy is complete."; /* canvas */

struct CopyDriveRow {
    std::string label, value;
};

struct CopyView {
    enum class Stage { Idle, Choosing, Mounting, Copying };
    Stage stage = Stage::Idle;
    std::vector<CopyDriveRow> drives; /* Choosing */
    std::string title;                /* Copying: "Copying to SANDISK" */
    std::string text;                 /* Copying: "<name>, 1.4 GB. The file appears ..." */
    std::string progress;             /* Copying: "630 MB of 1.4 GB, about 40 s left" */
    double fraction = 0;              /* Copying: 0..1 */
};

class CopyFlow {
public:
    /* backend: made at the first request (the real udisks one, or the demo drives); notice: the toast line; ops: the
     * copy's file system calls (the scenes slow it down with throttled_file_ops). */
    CopyFlow(std::function<std::unique_ptr<DriveBackend>()> backend, std::function<void(const Notice &)> notice,
             FileOps &ops = posix_file_ops());
    ~CopyFlow();
    CopyFlow(const CopyFlow &) = delete;
    CopyFlow &operator=(const CopyFlow &) = delete;

    /* The scene or photo at path to a drive (ignored while a copy or a mount is under way). */
    void request(const std::string &path);
    void choose(int index); /* Choosing: that drive */
    void dismiss();         /* Choosing: no copy */
    void cancel();          /* Copying: stops it, the .part is deleted; Mounting: no copy after the mount */
    void step();            /* the mounts' answers and the job's state: the view, the notices at the end */
    const CopyView &view() const { return view_; }
    bool busy() const { return view_.stage != CopyView::Stage::Idle; }

private:
    void start_on(const DriveInfo &d);
    void begin_copy(const std::string &mount_dir, const std::string &label, const std::string &fstype);
    DriveManager &drives();

    std::function<std::unique_ptr<DriveBackend>()> backend_;
    std::function<void(const Notice &)> notice_;
    std::unique_ptr<DriveManager> drives_;
    CopyJob job_;
    std::vector<DriveInfo> listed_;
    std::string src_, pending_dev_, pending_label_, pending_fs_;
    std::uint64_t src_bytes_ = 0;
    bool job_started_ = false;
    CopyView view_;
};

/* A FileOps that writes at most bytes_per_s (sleeps in write / pwrite) and passes everything else to base: the scenes'
 * copy dialog takes a few seconds on the build box (VHS_FAKE_COPY_KBPS). */
std::unique_ptr<FileOps> throttled_file_ops(FileOps &base, std::uint64_t bytes_per_s);

} // namespace vhs
