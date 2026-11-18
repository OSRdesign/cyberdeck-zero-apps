/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares (see net_share.hpp).
 */
#include "net_share.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace vinyl {

namespace {
std::atomic<int> g_live{0};

struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
};
}  // namespace

bool is_share_fs(const std::string &type) {
    static const char *const net[] = {"cifs", "smb3", "smbfs", "nfs", "nfs4", "ceph", "glusterfs", "9p", "afs", "lustre", "davfs", "autofs"};
    for (const char *x : net)
        if (type == x) return true;
    if (type.compare(0, 5, "fuse.") == 0) { /* fuse.sshfs, fuse.rclone ...; the FUSE drivers of a local stick are drives */
        static const char *const local[] = {"fuse.ntfs-3g", "fuse.ntfs", "fuse.exfat", "fuse.exfat-fuse", "fuse.hfsplus"};
        for (const char *x : local)
            if (type == x) return false;
        return type.size() > 5;
    }
    return false;
}

const MountEntry *share_mount_of(const std::vector<MountEntry> &mounts, const std::string &path) {
    const MountEntry *m = mount_of(mounts, path);
    return m && is_share_fs(m->type) ? m : nullptr;
}

bool path_on_share(const std::string &path) { return share_mount_of(parse_mounts(read_mounts()), path) != nullptr; }

/* ------------------------------------------------------------------------------------------------ BoundedRunner */

void BoundedRunner::configure(Config c) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = std::move(c);
}

void BoundedRunner::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    slots_.clear();
}

uint64_t BoundedRunner::now() const {
    if (cfg_.now_ms) return cfg_.now_ms();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

int BoundedRunner::live_threads() { return g_live.load(); }

BoundedRunner::Status BoundedRunner::run(const std::string &key, std::function<void()> job) {
    auto sh = std::make_shared<Shared>();
    auto busy = std::make_shared<std::atomic<bool>>(true);
    uint32_t timeout;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Slot &s = slots_[key];
        if ((s.busy && s.busy->load()) || now() < s.until) return Status::BackedOff;
        s.busy = busy;
        timeout = cfg_.timeout_ms;
    }
    ++g_live;
    try {
        std::thread([sh, busy, j = std::move(job)]() mutable {
            {
                std::function<void()> mine = std::move(j);
                if (mine) mine();
            }
            busy->store(false);
            {
                std::lock_guard<std::mutex> lk(sh->mu);
                sh->done = true;
            }
            sh->cv.notify_all();
            --g_live;
        }).detach();
    } catch (...) { /* no thread to be had: the share counts as not answering */
        --g_live;
        busy->store(false);
        return Status::TimedOut;
    }
    bool done;
    {
        std::unique_lock<std::mutex> lk(sh->mu);
        done = sh->cv.wait_for(lk, std::chrono::milliseconds(timeout), [&] { return sh->done; });
    }
    std::lock_guard<std::mutex> lk(mu_);
    Slot &s = slots_[key];
    if (done) {
        s.backoff = 0;
        s.until = 0;
        return Status::Done;
    }
    s.backoff = s.backoff == 0 ? cfg_.backoff_ms : std::min(s.backoff * 2, cfg_.backoff_max_ms);
    s.until = now() + s.backoff;
    return Status::TimedOut;
}

BoundedRunner &net_runner() {
    static BoundedRunner r;
    return r;
}

bool probe_share_root(const std::string &path, const std::string &key) {
    auto ok = std::make_shared<std::atomic<bool>>(false);
    const BoundedRunner::Status st = net_runner().run("probe:" + (key.empty() ? path : key), [path, ok] { ok->store(probe_root(path)); });
    return st == BoundedRunner::Status::Done && ok->load();
}

bool share_answers(const std::string &path, const std::string &key) {
    return net_runner().run("probe:" + (key.empty() ? path : key), [path] { (void)probe_root(path); }) == BoundedRunner::Status::Done;
}

bool share_reachable(const std::string &path, const std::function<std::string()> &mounts_text) {
    if (!probe_share_root(path)) return false;
    const std::vector<MountEntry> mounts = parse_mounts(mounts_text ? mounts_text() : read_mounts());
    const MountEntry *m = share_mount_of(mounts, path);
    return m && m->type != "autofs";
}

}  // namespace vinyl
