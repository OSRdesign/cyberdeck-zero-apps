/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the copy to a USB drive (see copy.hpp).
 */
#include "copy.hpp"

#include "record/disk_guard.hpp" /* bytes_text, time_left_text */
#include "scenes/atomic_file.hpp" /* base_name, path_exists */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace vhs {

namespace {

constexpr std::uint64_t kMargin = 1ull << 20;   /* room kept free on the drive besides the file */
constexpr std::uint64_t kFatLimit = 1ull << 32; /* a FAT file system takes files under 4 GiB */
constexpr std::uint64_t kWindowMs = 500;        /* the rate is measured over windows of at least this */

const char *const kFatWhy = "the drive's FAT file system takes files under 4 GB";

std::string lower_ascii(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip_slash(std::string p)
{
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string folder_of(const std::string &mount)
{
    return strip_slash(mount) + "/VHS";
}

std::string no_room_why(const std::string &label)
{
    return label.empty() ? "not enough free space on the drive" : "not enough free space on " + label;
}

/* Why a call on the drive's side failed (errno e), for "Not copied: <why>". */
std::string write_why(int e, const std::string &label, bool fat)
{
    switch (e) {
    case ENOSPC:
    case EDQUOT: return no_room_why(label);
    case EFBIG: return fat ? kFatWhy : "the file is too big for the drive";
    case EROFS: return "the drive is read-only";
    case EACCES:
    case EPERM: return "no permission to write to the drive";
    case ENAMETOOLONG: return "the file name is too long for the drive";
    case ENOTDIR: return "the drive has a file called VHS in the way"; /* mkdir said it exists: it is not a folder */
    case ENOENT:
    case EIO:
    case ENODEV:
    case ENXIO:
    case ESTALE: return "the drive stopped answering";
    default: return "could not write to the drive (error " + std::to_string(e) + ")";
    }
}

/* Why a call on the source's side failed. */
std::string read_why(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR: return "the file is not there";
    case EACCES:
    case EPERM: return "no permission to read the file";
    default: return "could not read the file (error " + std::to_string(e) + ")";
    }
}

/* The copy thread reads and writes at the idle I/O class (Linux ioprio_set: class 3 << 13, the calling thread); a kernel or an
 * I/O scheduler that has no use for it just ignores it. */
void set_io_idle()
{
#ifdef SYS_ioprio_set
    constexpr int kWhoProcess = 1; /* IOPRIO_WHO_PROCESS, who 0: the calling thread */
    constexpr int kClassShift = 13;
    constexpr int kClassIdle = 3;
    (void)::syscall(SYS_ioprio_set, kWhoProcess, 0, kClassIdle << kClassShift);
#endif
}

/* The size is returned in `size` once the source has been looked at. */
CopyCheck preflight(FileOps &ops, const std::string &src, const std::string &mount, const std::string &fs_type, const std::string &label,
                    std::uint64_t &size)
{
    CopyCheck r;
    const std::string name = base_name(src);
    StatInfo st;
    if (name.empty() || ops.stat(src, st) < 0) {
        r.why = "the file is not there";
        return r;
    }
    size = st.size;
    if (mount.empty()) {
        r.why = "the drive is not mounted";
        return r;
    }
    std::uint64_t avail = 0;
    if (ops.free_space(strip_slash(mount), avail) < 0) {
        r.why = "the drive is not there";
        return r;
    }
    if (path_exists(ops, folder_of(mount) + "/" + name)) {
        r.why = "a file of that name is on the drive";
        return r;
    }
    return check_copy(size, avail, fs_type, label);
}

} // namespace

/* ------------------------------------------------------------------------------------------------ the pure checks */

bool is_fat_type(const std::string &fs_type)
{
    const std::string t = lower_ascii(fs_type);
    return t == "vfat" || t == "fat" || t == "fat12" || t == "fat16" || t == "fat32" || t == "msdos";
}

CopyCheck check_copy(std::uint64_t file_bytes, std::uint64_t drive_free, const std::string &fs_type, const std::string &label)
{
    CopyCheck r;
    if (file_bytes >= kFatLimit && is_fat_type(fs_type)) {
        r.why = kFatWhy;
        return r;
    }
    if (drive_free < kMargin || file_bytes > drive_free - kMargin) {
        r.why = no_room_why(label);
        return r;
    }
    r.ok = true;
    return r;
}

CopyCheck preflight_copy(FileOps &ops, const std::string &src_path, const std::string &mount_dir, const std::string &fs_type,
                         const std::string &label)
{
    std::uint64_t size = 0;
    return preflight(ops, src_path, mount_dir, fs_type, label, size);
}

/* ------------------------------------------------------------------------------------------------ the texts */

double copy_fraction(const CopyStatus &s)
{
    if (s.done) return 1.0;
    if (s.total == 0) return 0.0;
    return std::min(1.0, static_cast<double>(s.bytes) / static_cast<double>(s.total));
}

std::string copy_progress_text(const CopyStatus &s)
{
    if (s.failed) return "Not copied: " + s.error;
    if (s.cancelled) return "Copy cancelled";
    if (s.done) return bytes_text(s.total) + " copied";
    if (!s.running) return "";
    if (s.total == 0) return "Preparing";
    std::string t = bytes_text(s.bytes) + " of " + bytes_text(s.total);
    if (s.eta_s >= 0) {
        if (s.eta_s < 1) {
            t += ", almost done";
        } else {
            std::int64_t eta = s.eta_s;
            if (eta >= 10) eta = (eta + 2) / 5 * 5; /* "about 40 s", not 41, 42, 41 ... */
            t += ", about " + time_left_text(eta);
        }
    }
    return t;
}

std::string copy_title(const std::string &label)
{
    return label.empty() ? "Copying to the drive" : "Copying to " + label;
}

/* ------------------------------------------------------------------------------------------------ the job */

CopyJob::CopyJob(FileOps &ops, Clock &clock) : ops_(ops), clock_(clock) {}

CopyJob::~CopyJob()
{
    cancel_.store(true);
    join();
}

bool CopyJob::start(const std::string &src_path, const std::string &mount_dir, const std::string &drive_label, const std::string &fs_type)
{
    {
        std::lock_guard<std::mutex> lock(m_);
        if (st_.running) return false;
    }
    if (thread_.joinable()) thread_.join(); /* the job before has ended: running is cleared by its last step */
    {
        std::lock_guard<std::mutex> lock(m_);
        st_ = CopyStatus();
        st_.running = true;
        st_.label = drive_label;
        if (!mount_dir.empty()) st_.target = folder_of(mount_dir) + "/" + base_name(src_path);
        bytes_.store(0);
        cancel_.store(false);
    }
    thread_ = std::thread([this, src_path, mount_dir, drive_label, fs_type] { work(src_path, mount_dir, drive_label, fs_type); });
    return true;
}

void CopyJob::cancel()
{
    cancel_.store(true);
}

void CopyJob::join()
{
    if (thread_.joinable()) thread_.join();
}

CopyStatus CopyJob::status() const
{
    std::lock_guard<std::mutex> lock(m_);
    CopyStatus s = st_;
    s.bytes = s.done ? s.total : bytes_.load();
    if (s.done) {
        s.eta_s = 0;
    } else if (s.running && s.bytes_per_s > 0 && s.total > 0) {
        const double left = static_cast<double>(s.total > s.bytes ? s.total - s.bytes : 0);
        s.eta_s = static_cast<std::int64_t>(std::ceil(left / s.bytes_per_s));
    } else {
        s.eta_s = -1;
    }
    return s;
}

void CopyJob::set_total(std::uint64_t total)
{
    std::lock_guard<std::mutex> lock(m_);
    st_.total = total;
}

void CopyJob::finish(End how, const std::string &error)
{
    std::lock_guard<std::mutex> lock(m_);
    st_.running = false;
    st_.done = how == End::Done;
    st_.cancelled = how == End::Cancelled;
    st_.failed = how == End::Failed;
    st_.error = how == End::Failed ? error : std::string();
}

void CopyJob::work(const std::string src, const std::string mount, const std::string label, const std::string fs_type)
{
    set_io_idle();
    const std::string name = base_name(src);
    const std::string folder = folder_of(mount);
    const std::string final_path = folder + "/" + name;
    const std::string part = folder + "/." + name + ".part";
    const bool fat = is_fat_type(fs_type);
    const std::uint64_t t_start = clock_.mono_ms();

    std::uint64_t size = 0;
    const CopyCheck pf = preflight(ops_, src, mount, fs_type, label, size);
    if (!pf.ok) {
        std::fprintf(stderr, "[vhs] copy: %s to %s refused: %s\n", name.c_str(), label.c_str(), pf.why.c_str());
        finish(End::Failed, pf.why);
        return;
    }
    set_total(size);
    std::fprintf(stderr, "[vhs] copy: %s (%s) to %s started\n", name.c_str(), bytes_text(size).c_str(), label.c_str());
    if (cancel_.load()) {
        finish(End::Cancelled, "");
        return;
    }

    /* G7: the VHS/ folder this copy makes is removed again when the copy ends without a file and leaves it empty; a
     * folder that was there before is never touched */
    const bool folder_was_there = path_exists(ops_, folder);
    auto tidy_folder = [&] {
        if (folder_was_there) return;
        std::vector<std::string> left;
        if (ops_.list_dir(folder, left) == 0 && left.empty() && ops_.rmdir(folder) == 0)
            std::fprintf(stderr, "[vhs] copy: removed the empty folder %s it made\n", folder.c_str());
    };
    auto fail_early = [&](const std::string &why) {
        std::fprintf(stderr, "[vhs] copy: %s to %s failed: %s\n", name.c_str(), label.c_str(), why.c_str());
        tidy_folder();
        finish(End::Failed, why);
    };
    const int mk = ops_.mkdir(folder);
    if (mk < 0) return fail_early(write_why(-mk, label, fat));
    const int in = ops_.open(src, O_RDONLY, 0);
    if (in < 0) return fail_early(read_why(-in));
    const int out = ops_.open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        ops_.close(in);
        return fail_early(write_why(-out, label, fat));
    }

    End how = End::Done;
    std::string error;
    std::vector<char> buf(kChunk);
    std::uint64_t off = 0, unsynced = 0;
    std::uint64_t win_ms = clock_.mono_ms(), win_bytes = 0;
    double rate = 0;
    while (off < size) {
        if (cancel_.load()) {
            how = End::Cancelled;
            break;
        }
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, size - off));
        const ssize_t got = ops_.pread(in, buf.data(), want, off);
        if (got < 0) {
            how = End::Failed;
            error = read_why(static_cast<int>(-got));
            break;
        }
        if (got == 0) {
            how = End::Failed;
            error = "the file changed while it was copied";
            break;
        }
        const int w = write_all(ops_, out, buf.data(), static_cast<std::size_t>(got));
        if (w < 0) {
            how = End::Failed;
            error = write_why(-w, label, fat);
            break;
        }
        off += static_cast<std::uint64_t>(got);
        unsynced += static_cast<std::uint64_t>(got);
        bytes_.store(off);
        const std::uint64_t now = clock_.mono_ms();
        if (now >= win_ms + kWindowMs) {
            const double r = static_cast<double>(off - win_bytes) * 1000.0 / static_cast<double>(now - win_ms);
            rate = rate > 0 ? rate * 0.7 + r * 0.3 : r;
            win_ms = now;
            win_bytes = off;
            std::lock_guard<std::mutex> lock(m_);
            st_.bytes_per_s = rate;
        }
        if (unsynced >= kSyncEvery) {
            const int s = ops_.fdatasync(out);
            if (s < 0) {
                how = End::Failed;
                error = write_why(-s, label, fat);
                break;
            }
            unsynced = 0;
        }
    }
    if (how == End::Done) {
        if (cancel_.load()) {
            how = End::Cancelled;
        } else {
            const int s = ops_.fsync(out);
            if (s < 0) {
                how = End::Failed;
                error = write_why(-s, label, fat);
            }
        }
    }
    ops_.close(in);
    const int cl = ops_.close(out);
    if (how == End::Done && cl < 0) {
        how = End::Failed;
        error = write_why(-cl, label, fat);
    }
    if (how == End::Done && cancel_.load()) how = End::Cancelled; /* the last chance: after this the file is committed */
    if (how == End::Done && path_exists(ops_, final_path)) {
        how = End::Failed;
        error = "a file of that name is on the drive";
    }
    if (how == End::Done) {
        const int rn = ops_.rename(part, final_path);
        if (rn < 0) {
            how = End::Failed;
            error = write_why(-rn, label, fat);
        }
    }
    if (how != End::Done) {
        if (ops_.unlink(part) < 0) std::fprintf(stderr, "[vhs] copy: could not remove %s\n", part.c_str());
        tidy_folder();
        std::fprintf(stderr, "[vhs] copy: %s to %s %s%s\n", name.c_str(), label.c_str(), how == End::Cancelled ? "cancelled" : "failed: ",
                     error.c_str());
        finish(how, error);
        return;
    }
    if (ops_.fsync_dir(folder) < 0) std::fprintf(stderr, "[vhs] copy: fsync of %s failed (the file is in place)\n", folder.c_str());
    std::fprintf(stderr, "[vhs] copy: %s to %s done: %s in %llu s\n", name.c_str(), label.c_str(), bytes_text(size).c_str(),
                 static_cast<unsigned long long>((clock_.mono_ms() - t_start) / 1000));
    bytes_.store(size);
    finish(End::Done, "");
}

} // namespace vhs
