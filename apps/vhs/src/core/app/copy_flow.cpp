/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: copying a scene to a USB drive, the steps (see copy_flow.hpp).
 */
#include "copy_flow.hpp"

#include "record/disk_guard.hpp"
#include "scenes/atomic_file.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace vhs {

CopyFlow::CopyFlow(std::function<std::unique_ptr<DriveBackend>()> backend, std::function<void(const Notice &)> notice,
                   FileOps &ops)
    : backend_(std::move(backend)), notice_(std::move(notice)), job_(ops)
{
}

CopyFlow::~CopyFlow()
{
    job_.cancel(); /* a copy never outlives the app: its .part goes */
    job_.join();
    if (drives_) {
        const int n = drives_->shutdown(); /* only what VHS mounted */
        if (n > 0) std::fprintf(stderr, "[vhs] copy: %d drive(s) VHS mounted unmounted at exit\n", n);
    }
}

DriveManager &CopyFlow::drives()
{
    if (!drives_) drives_ = std::make_unique<DriveManager>(backend_());
    return *drives_;
}

void CopyFlow::request(const std::string &path)
{
    if (busy()) return;
    StatInfo st;
    src_ = path;
    src_bytes_ = posix_file_ops().stat(path, st) == 0 ? st.size : 0;
    listed_ = drives().list();
    std::fprintf(stderr, "[vhs] copy: %s (%s), %zu drive(s)\n", base_name(path).c_str(), bytes_text(src_bytes_).c_str(), listed_.size());
    if (listed_.empty()) {
        notice_(Notice{kNoticeNoDrive, false});
        return;
    }
    if (listed_.size() == 1) {
        start_on(listed_.front());
        return;
    }
    view_ = CopyView();
    view_.stage = CopyView::Stage::Choosing;
    for (const DriveInfo &d : listed_) {
        CopyDriveRow r;
        r.label = d.name();
        r.value = d.by_vinyl ? "mounted by VHS" : (d.mounted ? size_text(d.size) + ", mounted" : "not mounted"); /* Vinyl's */
        view_.drives.push_back(r);
    }
}

void CopyFlow::choose(int index)
{
    if (view_.stage != CopyView::Stage::Choosing || index < 0 || index >= static_cast<int>(listed_.size())) return;
    view_ = CopyView();
    start_on(listed_[static_cast<std::size_t>(index)]);
}

void CopyFlow::dismiss()
{
    if (view_.stage == CopyView::Stage::Choosing) view_ = CopyView();
}

void CopyFlow::start_on(const DriveInfo &d)
{
    if (d.mounted && !d.mount_dir.empty()) {
        begin_copy(d.mount_dir, d.name(), d.fstype);
        return;
    }
    if (!drives().available()) {
        notice_(Notice{kNoticeNoUdisks, false});
        view_ = CopyView();
        return;
    }
    pending_dev_ = d.dev;
    pending_label_ = d.name();
    pending_fs_ = d.fstype;
    view_ = CopyView();
    view_.stage = CopyView::Stage::Mounting;
    drives().mount_async(d.dev, d.name());
    notice_(Notice{"Mounting " + d.name() + "...", false}); /* Vinyl */
}

void CopyFlow::begin_copy(const std::string &mount_dir, const std::string &label, const std::string &fstype)
{
    view_ = CopyView();
    view_.stage = CopyView::Stage::Copying;
    view_.title = copy_title(label);
    view_.text = base_name(src_) + ", " + bytes_text(src_bytes_) + ". " + kCopyNote;
    view_.progress = "";
    job_started_ = job_.start(src_, mount_dir, label, fstype);
    std::fprintf(stderr, "[vhs] copy: to %s on %s (%s)%s\n", mount_dir.c_str(), label.c_str(), fstype.empty() ? "?" : fstype.c_str(),
                 job_started_ ? "" : ": a copy is running");
    if (!job_started_) view_ = CopyView();
}

void CopyFlow::cancel()
{
    if (view_.stage == CopyView::Stage::Copying) {
        job_.cancel();
        std::fprintf(stderr, "[vhs] copy: cancel\n");
    } else if (view_.stage == CopyView::Stage::Mounting) {
        pending_dev_.clear();
        view_ = CopyView();
    }
}

void CopyFlow::step()
{
    if (drives_) {
        DriveManager::Result r;
        while (drives_->take(r)) {
            if (r.dev != pending_dev_) continue; /* a mount nobody waits for now (cancelled) */
            pending_dev_.clear();
            if (r.kind == DriveManager::Result::Kind::Mounted) {
                begin_copy(r.mount_dir, pending_label_, pending_fs_);
            } else if (r.kind == DriveManager::Result::Kind::MountFailed) {
                view_ = CopyView();
                notice_(Notice{std::string(kNoticeMountFailed) + r.reason + ".", false});
            }
        }
    }
    if (view_.stage != CopyView::Stage::Copying || !job_started_) return;
    const CopyStatus s = job_.status();
    view_.fraction = copy_fraction(s);
    if (s.running) {
        view_.progress = copy_progress_text(s);
        return;
    }
    job_started_ = false;
    view_ = CopyView();
    if (s.done) {
        std::fprintf(stderr, "[vhs] copy: done, %s\n", s.target.c_str());
        notice_(Notice{std::string(kNoticeCopied) + s.label + ".", false});
    } else if (s.failed) {
        std::fprintf(stderr, "[vhs] copy: not copied: %s\n", s.error.c_str());
        notice_(Notice{std::string(kNoticeNotCopied) + s.error + ".", false});
    } else if (s.cancelled) {
        std::fprintf(stderr, "[vhs] copy: cancelled, nothing on the drive\n");
    }
}

/* ------------------------------------------------------------------------------------------------ throttled */

namespace {

class ThrottledOps : public FileOps {
public:
    ThrottledOps(FileOps &b, std::uint64_t rate) : b_(b), rate_(rate ? rate : 1) {}
    int open(const std::string &p, int f, int m) override { return b_.open(p, f, m); }
    int close(int fd) override { return b_.close(fd); }
    ssize_t write(int fd, const void *buf, size_t n) override
    {
        pace(n);
        return b_.write(fd, buf, n);
    }
    ssize_t pwrite(int fd, const void *buf, size_t n, uint64_t off) override
    {
        pace(n);
        return b_.pwrite(fd, buf, n, off);
    }
    ssize_t pread(int fd, void *buf, size_t n, uint64_t off) override { return b_.pread(fd, buf, n, off); }
    int fdatasync(int fd) override { return b_.fdatasync(fd); }
    int fsync(int fd) override { return b_.fsync(fd); }
    int ftruncate(int fd, uint64_t len) override { return b_.ftruncate(fd, len); }
    int fstat(int fd, StatInfo &st) override { return b_.fstat(fd, st); }
    int stat(const std::string &p, StatInfo &st) override { return b_.stat(p, st); }
    int rename(const std::string &a, const std::string &b) override { return b_.rename(a, b); }
    int unlink(const std::string &p) override { return b_.unlink(p); }
    int mkdir(const std::string &p) override { return b_.mkdir(p); }
    int rmdir(const std::string &p) override { return b_.rmdir(p); }
    int fsync_dir(const std::string &p) override { return b_.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &names) override { return b_.list_dir(p, names); }
    int free_space(const std::string &p, uint64_t &avail) override { return b_.free_space(p, avail); }

private:
    void pace(size_t n)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(n * 1000000ull / rate_)));
    }
    FileOps &b_;
    std::uint64_t rate_;
};

} // namespace

std::unique_ptr<FileOps> throttled_file_ops(FileOps &base, std::uint64_t bytes_per_s)
{
    return std::make_unique<ThrottledOps>(base, bytes_per_s);
}

} // namespace vhs
