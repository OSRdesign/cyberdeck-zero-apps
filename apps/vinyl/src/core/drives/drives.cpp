/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl drives (see drives.hpp).
 */
#include "drives.hpp"

#include "../library/roots.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vinyl {

std::string DriveInfo::name() const {
    if (!label.empty()) return label;
    const size_t slash = dev.rfind('/');
    return slash == std::string::npos ? dev : dev.substr(slash + 1);
}

std::string size_text(uint64_t bytes) {
    char b[32];
    const double gb = static_cast<double>(bytes) / 1e9;
    if (bytes >= 1000000000000ull) std::snprintf(b, sizeof b, "%.1f TB", gb / 1000.0);
    else if (bytes >= 10000000000ull) std::snprintf(b, sizeof b, "%.0f GB", gb);
    else if (bytes >= 1000000000ull) std::snprintf(b, sizeof b, "%.1f GB", gb);
    else std::snprintf(b, sizeof b, "%.0f MB", static_cast<double>(bytes) / 1e6);
    return b;
}

namespace {

std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> subdirs(const std::string &dir) {
    std::vector<std::string> out;
    DIR *d = ::opendir(dir.c_str());
    if (!d) return out;
    while (const dirent *e = ::readdir(d))
        if (e->d_name[0] != '.') out.push_back(e->d_name);
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool contains(const std::string &s, const char *t) { return s.find(t) != std::string::npos; }

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/* the reasons the boards name, for udisks's messages */
std::string known_reason(const std::string &msg, bool mounting) {
    const std::string m = lower(msg);
    if (contains(m, "notauthorized") || contains(m, "not authorized"))
        return mounting ? "this session is not allowed to mount drives" : "this session is not allowed to unmount drives";
    if (contains(m, "wrong fs type") || contains(m, "unknown filesystem") || contains(m, "unknown file system") || contains(m, "notsupported") ||
        contains(m, "no medium") || contains(m, "bad superblock"))
        return "the disk has no readable filesystem";
    if (contains(m, "devicebusy") || contains(m, "target is busy") || contains(m, "is busy")) return "it is in use";
    if (contains(m, "timed out") || contains(m, "timeout")) return "it took too long";
    return "";
}

}  // namespace

std::vector<std::pair<std::string, std::string>> parse_udev_data(const std::string &text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 2, "E:") != 0) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(line.substr(2, eq - 2), line.substr(eq + 1));
    }
    return out;
}

std::vector<DriveInfo> list_removable_drives(const std::string &sys_root, const std::string &udev_root, const std::string &mounts_text) {
    std::vector<DriveInfo> out;
    const std::vector<MountEntry> mounts = parse_mounts(mounts_text);
    const std::string blocks = sys_root + "/block";
    for (const std::string &name : subdirs(blocks)) {
        const std::string dir = blocks + "/" + name;
        if (trim(slurp(dir + "/removable")) != "1") continue;
        std::vector<std::string> parts;
        for (const std::string &sub : subdirs(dir))
            if (sub.compare(0, name.size(), name) == 0 && ::access((dir + "/" + sub + "/partition").c_str(), F_OK) == 0) parts.push_back(sub);
        std::vector<std::pair<std::string, std::string>> devs; /* name, sysfs dir */
        if (parts.empty()) devs.emplace_back(name, dir);
        for (const std::string &p : parts) devs.emplace_back(p, dir + "/" + p);
        for (const auto &d : devs) {
            const std::string majmin = trim(slurp(d.second + "/dev"));
            if (majmin.empty()) continue;
            DriveInfo info;
            bool usage_ok = true;
            for (const auto &kv : parse_udev_data(slurp(udev_root + "/b" + majmin))) {
                if (kv.first == "ID_FS_TYPE") info.fstype = kv.second;
                else if (kv.first == "ID_FS_LABEL") info.label = kv.second;
                else if (kv.first == "ID_FS_UUID") info.uuid = kv.second;
                else if (kv.first == "ID_FS_USAGE") usage_ok = kv.second == "filesystem";
            }
            if (info.fstype.empty() || !usage_ok) continue; /* no file system (a blank disk, a partition table, a RAID member) */
            info.dev = "/dev/" + d.first;
            info.size = static_cast<uint64_t>(std::strtoull(trim(slurp(d.second + "/size")).c_str(), nullptr, 10)) * 512ull;
            for (const MountEntry &m : mounts)
                if (m.dev == info.dev) {
                    info.mounted = true;
                    info.mount_dir = m.dir;
                    break;
                }
            out.push_back(info);
        }
    }
    return out;
}

std::string shorten_reason(const std::string &msg) {
    std::string s = trim(msg);
    const size_t nl = s.rfind('\n');
    if (nl != std::string::npos) s = trim(s.substr(nl + 1));
    const size_t colon = s.rfind(": ");
    if (colon != std::string::npos && s.size() - colon - 2 >= 4) s = s.substr(colon + 2);
    while (!s.empty() && (s.back() == '.' || std::isspace(static_cast<unsigned char>(s.back())))) s.pop_back();
    if (s.empty()) return "unknown reason";
    if (std::isupper(static_cast<unsigned char>(s[0])) && !(s.size() > 1 && std::isupper(static_cast<unsigned char>(s[1]))))
        s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    if (s.size() > 60) {
        s.resize(60);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back(); /* not in the middle of a character */
        if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
    }
    return s;
}

MountReply parse_mount_reply(int status, const std::string &out, const std::string &err) {
    MountReply r;
    const std::string all = err + "\n" + out;
    if (status == 0) {
        const size_t at = out.find(" at ");
        if (at != std::string::npos && out.compare(0, 7, "Mounted") == 0) {
            std::string dir = trim(out.substr(at + 4));
            while (!dir.empty() && dir.back() == '.') dir.pop_back();
            if (!dir.empty()) {
                r.ok = true;
                r.mount_dir = dir;
                return r;
            }
        }
    }
    if (contains(all, "AlreadyMounted") || contains(all, "already mounted")) { /* "... is already mounted at `/media/u/X'." */
        const size_t a = all.find('`');
        const size_t b = a == std::string::npos ? a : all.find('\'', a);
        if (b != std::string::npos && b > a + 1) {
            r.ok = true;
            r.mount_dir = all.substr(a + 1, b - a - 1);
            return r;
        }
    }
    r.reason = known_reason(all, true);
    if (r.reason.empty()) r.reason = status == 127 ? "udisks2 is missing" : shorten_reason(err.empty() ? out : err);
    return r;
}

MountReply parse_unmount_reply(int status, const std::string &out, const std::string &err) {
    MountReply r;
    const std::string all = err + "\n" + out;
    if (status == 0 || contains(all, "NotMounted") || contains(all, "not mounted")) {
        r.ok = true;
        return r;
    }
    r.reason = known_reason(all, false);
    if (r.reason.empty()) r.reason = status == 127 ? "udisks2 is missing" : shorten_reason(err.empty() ? out : err);
    return r;
}

namespace {

/* runs argv (PATH lookup), its output and error read until it ends or `timeout_ms` passes (then it is killed: status 124);
 * status 127: not found */
int run_command(const std::vector<std::string> &argv, int timeout_ms, std::string &out, std::string &err) {
    int po[2], pe[2];
    if (::pipe(po) != 0) return 127;
    if (::pipe(pe) != 0) {
        ::close(po[0]);
        ::close(po[1]);
        return 127;
    }
    const pid_t pid = ::fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        ::dup2(po[1], 1);
        ::dup2(pe[1], 2);
        ::close(po[0]);
        ::close(po[1]);
        ::close(pe[0]);
        ::close(pe[1]);
        const int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) ::dup2(null, 0);
        ::setenv("LC_ALL", "C", 1);
        std::vector<char *> a;
        for (const std::string &s : argv) a.push_back(const_cast<char *>(s.c_str()));
        a.push_back(nullptr);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    ::close(po[1]);
    ::close(pe[1]);
    const auto t0 = std::chrono::steady_clock::now();
    bool open_o = true, open_e = true, killed = false;
    char buf[1024];
    while (open_o || open_e) {
        const int left = timeout_ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (left <= 0) {
            ::kill(pid, SIGKILL);
            killed = true;
            break;
        }
        pollfd fds[2] = {{po[0], POLLIN, 0}, {pe[0], POLLIN, 0}};
        const int n = ::poll(fds, 2, std::min(left, 200));
        if (n < 0) break;
        for (int i = 0; i < 2; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP))) continue;
            const ssize_t k = ::read(fds[i].fd, buf, sizeof buf);
            if (k > 0) (i == 0 ? out : err).append(buf, static_cast<size_t>(k));
            else (i == 0 ? open_o : open_e) = false;
        }
    }
    ::close(po[0]);
    ::close(pe[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    if (killed) return 124;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

bool program_in_path(const char *prog) {
    const char *p = std::getenv("PATH");
    const std::string path = p && *p ? p : "/usr/bin:/bin:/usr/sbin:/sbin";
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        const std::string cand = path.substr(i, j - i) + "/" + prog;
        if (::access(cand.c_str(), X_OK) == 0) return true;
        i = j + 1;
    }
    return false;
}

class UdisksBackend : public DriveBackend {
public:
    bool available() override { return program_in_path("udisksctl"); }
    std::vector<DriveInfo> list() override { return list_removable_drives("/sys", "/run/udev/data", slurp("/proc/self/mounts")); }
    MountReply mount(const std::string &dev) override {
        if (!available()) return {false, "", "udisks2 is missing"};
        std::string out, err;
        const int st = run_command({"udisksctl", "mount", "-b", dev, "--no-user-interaction"}, 20000, out, err);
        if (st == 124) return {false, "", "it took too long"};
        return parse_mount_reply(st, out, err);
    }
    MountReply unmount(const std::string &dev) override {
        if (!available()) return {false, "", "udisks2 is missing"};
        std::string out, err;
        const int st = run_command({"udisksctl", "unmount", "-b", dev, "--no-user-interaction"}, 20000, out, err);
        if (st == 124) return {false, "", "it took too long"};
        return parse_unmount_reply(st, out, err);
    }
};

}  // namespace

std::unique_ptr<DriveBackend> make_udisks_backend() { return std::make_unique<UdisksBackend>(); }

std::vector<DriveInfo> FakeDrives::list() { return devices; }

MountReply FakeDrives::mount(const std::string &dev) {
    calls.push_back("mount " + dev);
    if (!has_udisks) return {false, "", "udisks2 is missing"};
    MountReply r;
    if (on_mount) {
        r = on_mount(dev);
    } else {
        for (const DriveInfo &d : devices)
            if (d.dev == dev) {
                r.ok = true;
                r.mount_dir = mount_base + "/" + d.name();
            }
        if (!r.ok) r.reason = "no such device";
    }
    if (r.ok)
        for (DriveInfo &d : devices)
            if (d.dev == dev) {
                d.mounted = true;
                d.mount_dir = r.mount_dir;
            }
    return r;
}

MountReply FakeDrives::unmount(const std::string &dev) {
    calls.push_back("unmount " + dev);
    MountReply r;
    if (on_unmount) r = on_unmount(dev);
    else r.ok = true;
    if (r.ok)
        for (DriveInfo &d : devices)
            if (d.dev == dev) {
                d.mounted = false;
                d.mount_dir.clear();
            }
    return r;
}

/* ------------------------------------------------------------------------------------------------ DriveManager */

DriveManager::DriveManager(std::unique_ptr<DriveBackend> backend) : be_(std::move(backend)) { th_ = std::thread([this] { run(); }); }

DriveManager::~DriveManager() { shutdown(); }

bool DriveManager::available() const { return be_ && be_->available(); }

std::vector<DriveInfo> DriveManager::list() {
    std::vector<DriveInfo> v = be_ ? be_->list() : std::vector<DriveInfo>();
    std::lock_guard<std::mutex> lk(mu_);
    for (DriveInfo &d : v) d.by_vinyl = d.mounted && ours_.count(d.dev) != 0;
    /* a drive that left (pulled) or is mounted no more is no longer ours: a later mount by the system is the system's */
    for (auto it = ours_.begin(); it != ours_.end();) {
        const bool there = std::any_of(v.begin(), v.end(), [&](const DriveInfo &d) { return d.dev == *it && d.mounted; });
        it = there ? std::next(it) : ours_.erase(it);
    }
    return v;
}

void DriveManager::note_mounted(const std::string &dev) {
    std::lock_guard<std::mutex> lk(mu_);
    ours_.insert(dev);
}

bool DriveManager::by_vinyl(const std::string &dev) const {
    std::lock_guard<std::mutex> lk(mu_);
    return ours_.count(dev) != 0;
}

bool DriveManager::busy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return busy_ || !ops_.empty();
}

void DriveManager::mount_async(const std::string &dev, const std::string &name) {
    std::lock_guard<std::mutex> lk(mu_);
    ops_.push_back({true, dev, name});
    cv_.notify_all();
}

void DriveManager::unmount_async(const std::string &dev, const std::string &name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ours_.count(dev) == 0) return; /* never a drive the system mounted */
    ops_.push_back({false, dev, name});
    cv_.notify_all();
}

bool DriveManager::take(Result &out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (done_.empty()) return false;
    out = done_.front();
    done_.pop_front();
    return true;
}

void DriveManager::run() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [this] { return quit_ || !ops_.empty(); });
        if (quit_) break;
        const Op op = ops_.front();
        ops_.pop_front();
        busy_ = true;
        lk.unlock();
        const MountReply r = op.mount ? be_->mount(op.dev) : be_->unmount(op.dev);
        lk.lock();
        Result res;
        res.dev = op.dev;
        res.name = op.name;
        res.mount_dir = r.mount_dir;
        res.reason = r.reason;
        if (op.mount) {
            res.kind = r.ok ? Result::Kind::Mounted : Result::Kind::MountFailed;
            if (r.ok) ours_.insert(op.dev);
        } else {
            res.kind = r.ok ? Result::Kind::Unmounted : Result::Kind::UnmountFailed;
            if (r.ok) ours_.erase(op.dev);
        }
        done_.push_back(res);
        busy_ = false;
        idle_cv_.notify_all();
    }
    idle_cv_.notify_all();
}

void DriveManager::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_cv_.wait(lk, [this] { return ops_.empty() && !busy_; });
}

int DriveManager::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return 0;
        quit_ = true;
        cv_.notify_all();
    }
    if (th_.joinable()) th_.join();
    int n = 0;
    std::set<std::string> mine;
    {
        std::lock_guard<std::mutex> lk(mu_);
        mine = ours_;
        ops_.clear();
    }
    for (const std::string &dev : mine) {
        const MountReply r = be_->unmount(dev);
        std::fprintf(stderr, "[vinyl] drives: unmount %s at exit: %s%s\n", dev.c_str(), r.ok ? "ok" : "failed: ", r.ok ? "" : r.reason.c_str());
        if (r.ok) ++n;
    }
    return n;
}

}  // namespace vinyl
