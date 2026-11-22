/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the scanner and its thread (see scanner.hpp).
 */
#include "scanner.hpp"

#include "../decode/track_decoder.hpp"
#include "library.hpp"
#include "net_share.hpp"
#include "schema.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace vinyl {

const char *scan_state_name(ScanProgress::State s) {
    switch (s) {
    case ScanProgress::State::Scanning: return "scanning";
    case ScanProgress::State::Paused: return "paused";
    case ScanProgress::State::Done: return "done";
    case ScanProgress::State::Cancelled: return "cancelled";
    case ScanProgress::State::Failed: return "failed";
    default: return "idle";
    }
}

ScanHooks default_scan_hooks() {
    ScanHooks h;
    h.read_tags = [](const std::string &p, TrackTags &t) { return read_tags_scan(p, t); };
    h.temp_mc = [] {
        std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
        long v = -1;
        return (f >> v) ? static_cast<int>(v) : -1;
    };
    h.mono_ms = [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    };
    h.sleep_ms = [](uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    h.mounts = [] { return read_mounts(); };
    return h;
}

ScanOptions scan_options_for(int ram_mb) {
    ScanOptions o;
    if (ram_mb > 512) o.batch_files = 1000;
    return o;
}

void lower_thread_priority() {
    const long tid = ::syscall(SYS_gettid);
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 19);
#ifdef SYS_ioprio_set
    constexpr int kWhoProcess = 1, kClassIdle = 3, kClassShift = 13; /* linux/ioprio.h */
    ::syscall(SYS_ioprio_set, kWhoProcess, static_cast<int>(tid), kClassIdle << kClassShift);
#endif
}

namespace {

std::vector<std::string> split(const std::string &rel) {
    std::vector<std::string> v;
    size_t a = 0;
    while (a <= rel.size() && !rel.empty()) {
        const size_t b = rel.find('/', a);
        v.push_back(rel.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return v;
}

/* the walk's order: by name, component by component, a folder before its sub-folders */
int preorder_cmp(const std::string &a, const std::string &b) {
    if (a == b) return 0;
    const std::vector<std::string> pa = split(a), pb = split(b);
    for (size_t i = 0; i < pa.size() && i < pb.size(); ++i)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return pa.size() < pb.size() ? -1 : 1;
}

/* a strictly above b */
bool is_ancestor(const std::string &a, const std::string &b) {
    if (a == b) return false;
    return a.empty() || (b.size() > a.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == '/');
}

std::string join(const std::string &root, const std::string &rel) { return rel.empty() ? root : root + "/" + rel; }
std::string child(const std::string &rel, const std::string &name) { return rel.empty() ? name : rel + "/" + name; }

/* the errors of a drive that went away */
bool gone_error(int e) {
    return e == ENOENT || e == EIO || e == ENOTDIR || e == ENODEV || e == ENXIO || e == ESTALE ||
           e == ETIMEDOUT || e == EHOSTDOWN || e == EHOSTUNREACH || e == ENETUNREACH || e == ENOTCONN || e == ECONNRESET || e == ECONNREFUSED; /* a share that dropped */
}

}  // namespace

/* ------------------------------------------------------------------------------------------------ Scanner */

Scanner::Scanner(Db &db, ScanHooks hooks, ScanOptions opt)
    : db_(db), hooks_(std::move(hooks)), opt_(opt), w_(new LibraryWriter(db)) {
    const ScanHooks d = default_scan_hooks();
    if (!hooks_.read_tags) hooks_.read_tags = d.read_tags;
    if (!hooks_.temp_mc) hooks_.temp_mc = d.temp_mc;
    if (!hooks_.mono_ms) hooks_.mono_ms = d.mono_ms;
    if (!hooks_.sleep_ms) hooks_.sleep_ms = d.sleep_ms;
    if (!hooks_.mounts) hooks_.mounts = d.mounts;
}

Scanner::~Scanner() {
    if (in_tx_) db_.exec("ROLLBACK");
}

ScanProgress Scanner::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    return p_;
}

void Scanner::publish() {
    if (!on_progress) return;
    on_progress(progress());
}

void Scanner::set_state(ScanProgress::State s) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        p_.state = s;
    }
    publish();
}

Scanner::Walk Scanner::fail() {
    std::lock_guard<std::mutex> lk(mu_);
    if (p_.error.empty()) p_.error = db_.errmsg();
    return Walk::Failed;
}

bool Scanner::begin() {
    if (in_tx_) return true;
    if (!db_.exec("BEGIN IMMEDIATE")) return false;
    in_tx_ = true;
    dirty_ = false;
    batch_start_ = hooks_.mono_ms();
    w_->reset_writes();
    return true;
}

bool Scanner::commit() {
    if (!in_tx_) return true;
    bool ok = !dirty_ || w_->prune();
    if (ok && full_ && !resume_value_.empty()) ok = meta_set(db_, "scan_resume", resume_value_);
    if (ok) ok = db_.exec("COMMIT");
    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (p_.error.empty()) p_.error = db_.errmsg();
        }
        db_.exec("ROLLBACK");
    }
    in_tx_ = false;
    dirty_ = false;
    w_->reset_writes();
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++p_.commits;
        }
        if (on_commit) on_commit();
    }
    return ok;
}

bool Scanner::maybe_commit(bool boundary) {
    if (!in_tx_) return true;
    if (w_->writes() >= opt_.batch_files || (boundary && hooks_.mono_ms() - batch_start_ >= opt_.batch_ms)) return commit();
    return true;
}

bool Scanner::on_share(const RootRow &root) { return share_mount_of(parse_mounts(hooks_.mounts()), root.path) != nullptr; }

bool Scanner::still_online(const RootRow &root) {
    /* a share is "there" only when it answers (bounded wait) and is really mounted: an unreachable share deletes nothing */
    if (on_share(root)) return share_reachable(root.path, hooks_.mounts);
    return root_online(root, parse_mounts(hooks_.mounts()), hooks_.by_uuid_dir);
}

bool Scanner::heat(const std::atomic<bool> &cancel) {
    const uint64_t now = hooks_.mono_ms();
    if (last_heat_ != 0 && now - last_heat_ < opt_.heat_check_ms) return true;
    last_heat_ = now;
    int t = hooks_.temp_mc();
    if (t < opt_.hot_mc) return true;
    commit(); /* never hold the write lock while paused */
    {
        std::lock_guard<std::mutex> lk(mu_);
        p_.temp_c = t / 1000;
    }
    std::fprintf(stderr, "[vinyl] library scan paused: %d C\n", t / 1000);
    set_state(ScanProgress::State::Paused);
    while (true) {
        hooks_.sleep_ms(opt_.heat_poll_ms);
        if (cancel.load()) return false;
        t = hooks_.temp_mc();
        {
            std::lock_guard<std::mutex> lk(mu_);
            p_.temp_c = t / 1000;
        }
        if (t < 0 || t <= opt_.cool_mc) break;
    }
    set_state(ScanProgress::State::Scanning);
    last_heat_ = hooks_.mono_ms();
    return true;
}

Scanner::Walk Scanner::folder_files(const RootRow &root, const std::string &rel, int64_t dir_mtime,
                                    std::vector<FileEntry> &files, const std::atomic<bool> &cancel) {
    std::sort(files.begin(), files.end(), [](const FileEntry &a, const FileEntry &b) { return a.name < b.name; });
    const std::string abs = join(root.path, rel);
    int64_t fid = w_->find_folder(root.id, rel);
    struct Row {
        int64_t id;
        int64_t mtime;
        uint64_t size;
    };
    std::unordered_map<std::string, Row> rows;
    if (fid > 0) {
        Stmt q = db_.prepare("select id, name, coalesce(mtime, 0), coalesce(size, 0) from tracks where folder_id = ?1");
        q.bind(1, static_cast<long long>(fid));
        while (q.step() == sqlite::kRow) rows[q.text(1)] = {q.i64(0), q.i64(2), static_cast<uint64_t>(q.i64(3))};
    }
    for (const FileEntry &f : files) {
        if (cancel.load()) return Walk::Cancelled;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++p_.files;
        }
        auto it = rows.find(f.name);
        if (it != rows.end() && it->second.mtime == f.mtime && it->second.size == f.size) { /* unchanged: no read */
            rows.erase(it);
            continue;
        }
        TrackTags t;
        uint64_t reads;
        {
            std::lock_guard<std::mutex> lk(mu_);
            reads = ++p_.read;
        }
        const bool ok = hooks_.read_tags(abs + "/" + f.name, t);
        if (!ok) { /* not readable now (gone, no access): the row stays as it was */
            if (it != rows.end()) rows.erase(it);
            continue;
        }
        if (!begin()) return fail();
        if (fid <= 0 && (fid = w_->folder(root.id, rel, dir_mtime)) <= 0) return fail();
        TrackFile tf;
        tf.root_id = root.id;
        tf.folder_id = fid;
        tf.name = f.name;
        tf.mtime = f.mtime;
        tf.size = f.size;
        if (it != rows.end()) {
            if (!w_->update_track(it->second.id, tf, t)) return fail();
            dirty_ = true;
            rows.erase(it);
            std::lock_guard<std::mutex> lk(mu_);
            ++p_.updated;
        } else {
            if (w_->add_track(tf, t) < 0) return fail();
            std::lock_guard<std::mutex> lk(mu_);
            ++p_.added;
        }
        if (reads % 64 == 0) publish();
        if (!maybe_commit(false)) return fail();
    }
    /* rows whose file is not listed: gone only when stat says so and the root is still there */
    bool checked = false;
    for (const auto &kv : rows) {
        struct stat st;
        if (::lstat((abs + "/" + kv.first).c_str(), &st) == 0 || errno != ENOENT) continue;
        if (!checked) {
            if (!still_online(root)) return Walk::Offline;
            checked = true;
        }
        if (!begin() || !w_->delete_track(kv.second.id)) return fail();
        dirty_ = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++p_.removed;
        }
        if (!maybe_commit(false)) return fail();
    }
    return Walk::Ok;
}

Scanner::Walk Scanner::walk(const RootRow &root, const std::string &start, bool recursive, const Resume *rs,
                            const std::atomic<bool> &cancel) {
    std::set<std::pair<dev_t, ino_t>> seen; /* the loop guard (bind mounts; symlinks are never followed) */
    std::vector<std::string> stack{start};
    const bool resuming = rs && rs->active && rs->root == root.id && rs->has_rel;
    const bool share = on_share(root);
    if (share && !share_reachable(root.path, hooks_.mounts)) return Walk::Offline; /* not answering: nothing is read, nothing is deleted */
    while (!stack.empty()) {
        if (cancel.load() || !heat(cancel)) return Walk::Cancelled;
        const std::string rel = stack.back();
        stack.pop_back();
        const std::string abs = join(root.path, rel);
        /* a share is asked on a thread of its own, with a bounded wait, before the folder is opened here: a hard NFS mount that lost
         * its server would hold this thread (and the exit that waits for it) for good */
        if (share && !share_answers(abs, root.path)) return Walk::Offline;
        const int fd = ::open(abs.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            const int e = errno;
            if (gone_error(e) && !still_online(root)) return Walk::Offline;
            if (e == ENOENT && !rel.empty()) { /* the folder went away, its root is there: its rows go */
                const int64_t fid = w_->find_folder(root.id, rel);
                if (fid > 0) {
                    if (!begin() || !w_->delete_folder(fid)) return fail();
                    dirty_ = true;
                }
            }
            continue;
        }
        struct stat st;
        if (::fstat(fd, &st) != 0 || !seen.insert({st.st_dev, st.st_ino}).second) {
            ::close(fd);
            continue;
        }
        DIR *d = ::fdopendir(fd);
        if (!d) {
            ::close(fd);
            continue;
        }
        std::vector<std::string> dirs;
        std::vector<FileEntry> files;
        int read_err = 0;
        while (true) {
            errno = 0;
            dirent *e = ::readdir(d);
            if (!e) {
                read_err = errno;
                break;
            }
            const char *n = e->d_name;
            if (n[0] == '.') continue; /* ".", "..", hidden files and folders */
            unsigned char type = e->d_type;
            struct stat fs;
            bool have = false;
            if (type == DT_UNKNOWN) { /* file systems without d_type: one fstatat */
                if (::fstatat(::dirfd(d), n, &fs, AT_SYMLINK_NOFOLLOW) != 0) continue;
                have = true;
                type = S_ISDIR(fs.st_mode) ? DT_DIR : S_ISREG(fs.st_mode) ? DT_REG : DT_LNK;
            }
            if (type == DT_DIR) {
                if (recursive) dirs.push_back(n);
                continue;
            }
            if (type != DT_REG || !is_audio_name(n)) continue; /* symlinks are not followed */
            if (!have && ::fstatat(::dirfd(d), n, &fs, AT_SYMLINK_NOFOLLOW) != 0) continue;
            if (!S_ISREG(fs.st_mode)) continue;
            files.push_back({n, static_cast<int64_t>(fs.st_mtime), static_cast<uint64_t>(fs.st_size)});
        }
        ::closedir(d);
        if (read_err && gone_error(read_err) && !still_online(root)) return Walk::Offline;
        const bool files_done = resuming && preorder_cmp(rel, rs->rel) <= 0;
        if (!files_done) {
            const Walk r = folder_files(root, rel, static_cast<int64_t>(st.st_mtime), files, cancel);
            if (r != Walk::Ok) return r;
        }
        std::sort(dirs.begin(), dirs.end());
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) { /* reversed: the first name comes off the stack first */
            const std::string c = child(rel, *it);
            if (resuming && preorder_cmp(c, rs->rel) < 0 && !is_ancestor(c, rs->rel)) continue; /* done before */
            stack.push_back(c);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++p_.folders;
        }
        if (full_) resume_value_ = std::to_string(root.id) + "\t" + rel;
        publish();
        if (!maybe_commit(true)) return fail();
    }
    return Walk::Ok;
}

bool Scanner::sweep(const RootRow &root) {
    std::vector<std::pair<int64_t, std::string>> folders;
    {
        Stmt q = db_.prepare("select id, rel from folders where root_id = ?1 and rel != ''");
        q.bind(1, static_cast<long long>(root.id));
        while (q.step() == sqlite::kRow) folders.emplace_back(q.i64(0), q.text(1));
    }
    bool checked = false;
    for (const auto &f : folders) {
        struct stat st;
        if (::lstat(join(root.path, f.second).c_str(), &st) == 0 || errno != ENOENT) continue;
        if (!checked) {
            if (!still_online(root)) return true; /* the drive left: nothing goes */
            checked = true;
        }
        if (!begin() || !w_->delete_folder(f.first)) return false;
        dirty_ = true;
        if (!maybe_commit(false)) return false;
    }
    return true;
}

ScanProgress::State Scanner::finish(ScanProgress::State s) {
    if (in_tx_) {
        if (s == ScanProgress::State::Failed) {
            db_.exec("ROLLBACK");
            in_tx_ = false;
        } else if (!commit()) {
            s = ScanProgress::State::Failed;
        }
    }
    full_ = false;
    set_state(s);
    if (on_commit) on_commit();
    return s;
}

ScanProgress::State Scanner::scan_all(const std::vector<RootSpec> &wanted, const std::atomic<bool> &cancel) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        p_ = ScanProgress();
        p_.state = ScanProgress::State::Scanning;
    }
    publish();
    full_ = false;
    last_heat_ = 0;
    if (!sync_roots(db_, wanted, parse_mounts(hooks_.mounts()), hooks_.by_uuid_dir)) {
        fail();
        return finish(ScanProgress::State::Failed);
    }
    Resume rs;
    const std::string rv = meta_get(db_, "scan_resume");
    if (!rv.empty()) {
        rs.active = true;
        const size_t tab = rv.find('\t');
        rs.root = std::atoll(rv.substr(0, tab).c_str());
        if (tab != std::string::npos) {
            rs.has_rel = true;
            rs.rel = rv.substr(tab + 1);
        }
        std::lock_guard<std::mutex> lk(mu_);
        p_.resumed = true;
    }
    full_ = true;
    for (const RootRow &r : load_roots(db_)) {
        if (!r.online || (rs.active && r.id < rs.root)) continue;
        {
            std::lock_guard<std::mutex> lk(mu_);
            p_.root = r.label;
        }
        publish();
        resume_value_ = std::to_string(r.id);
        if (rs.active && r.id == rs.root && rs.has_rel) resume_value_ += "\t" + rs.rel;
        const Walk w = walk(r, "", true, &rs, cancel);
        if (w == Walk::Cancelled) return finish(ScanProgress::State::Cancelled);
        if (w == Walk::Failed) return finish(ScanProgress::State::Failed);
        if (w == Walk::Offline && on_share(r)) { /* a share that does not answer: skipped, its rows stay as they are and stay visible */
            std::fprintf(stderr, "[vinyl] library: %s does not answer: skipped, its tracks are kept\n", r.path.c_str());
            continue;
        }
        if (w == Walk::Offline) { /* the drive left during the walk: hidden, nothing deleted */
            std::fprintf(stderr, "[vinyl] library: %s went away during the scan\n", r.path.c_str());
            if (!begin() || !set_root_online(db_, r.id, false)) return finish(ScanProgress::State::Failed);
            continue;
        }
        if (!sweep(r) || !begin()) return finish(ScanProgress::State::Failed);
        Stmt u = db_.prepare("update roots set last_scan = ?1 where id = ?2");
        u.bind(1, static_cast<long long>(std::time(nullptr))).bind(2, static_cast<long long>(r.id));
        if (!u.run()) return finish(ScanProgress::State::Failed);
        resume_value_ = std::to_string(r.id + 1); /* every root up to this one is done */
        if (!maybe_commit(true)) return finish(ScanProgress::State::Failed);
    }
    full_ = false;
    if (rs.active || !meta_get(db_, "scan_resume").empty()) {
        if (!begin() || !meta_erase(db_, "scan_resume")) return finish(ScanProgress::State::Failed);
    }
    return finish(ScanProgress::State::Done);
}

ScanProgress::State Scanner::scan_folder(const std::string &path_in, bool recursive, const std::atomic<bool> &cancel) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        p_ = ScanProgress();
        p_.state = ScanProgress::State::Scanning;
    }
    publish();
    full_ = false;
    last_heat_ = 0;
    std::string path = path_in;
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const RootRow *best = nullptr;
    const std::vector<RootRow> roots = load_roots(db_);
    for (const RootRow &r : roots) {
        const bool under = path == r.path || (path.size() > r.path.size() && path.compare(0, r.path.size(), r.path) == 0 &&
                                              path[r.path.size()] == '/');
        if (r.online && under && (!best || r.path.size() > best->path.size())) best = &r;
    }
    if (!best) return finish(ScanProgress::State::Done);
    {
        std::lock_guard<std::mutex> lk(mu_);
        p_.root = best->label;
    }
    const std::string rel = path == best->path ? "" : path.substr(best->path.size() + 1);
    const Walk w = walk(*best, rel, recursive, nullptr, cancel);
    if (w == Walk::Cancelled) return finish(ScanProgress::State::Cancelled);
    if (w == Walk::Failed) return finish(ScanProgress::State::Failed);
    if (w == Walk::Offline && on_share(*best)) {
        std::fprintf(stderr, "[vinyl] library: %s does not answer: skipped, its tracks are kept\n", best->path.c_str());
    } else if (w == Walk::Offline && (!begin() || !set_root_online(db_, best->id, false))) {
        return finish(ScanProgress::State::Failed);
    }
    return finish(ScanProgress::State::Done);
}

/* ------------------------------------------------------------------------------------------------ ScanThread */

ScanThread::ScanThread(std::string db_path, int ram_mb, ScanHooks hooks)
    : options(scan_options_for(ram_mb)), path_(std::move(db_path)), ram_mb_(ram_mb), hooks_(std::move(hooks)) {}

ScanThread::~ScanThread() {
    cancel();
    wait();
}

bool ScanThread::start(std::function<ScanProgress::State(Scanner &)> job) {
    if (running_.load()) return false;
    if (th_.joinable()) th_.join();
    cancel_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_ = ScanProgress();
        last_.state = ScanProgress::State::Scanning;
    }
    th_ = std::thread([this, job] {
        lower_thread_priority();
        Db db;
        std::string why;
        if (!open_library_db(db, path_, ram_mb_, &why)) {
            std::lock_guard<std::mutex> lk(mu_);
            last_ = ScanProgress();
            last_.state = ScanProgress::State::Failed;
            last_.error = why;
        } else {
            Scanner sc(db, hooks_, options);
            sc.on_commit = [this] { ++gen_; };
            sc.on_progress = [this](const ScanProgress &p) {
                std::lock_guard<std::mutex> lk(mu_);
                last_ = p;
            };
            job(sc);
            std::lock_guard<std::mutex> lk(mu_);
            last_ = sc.progress();
        }
        ++gen_;
        running_ = false;
    });
    return true;
}

bool ScanThread::start_all(std::vector<RootSpec> wanted) {
    return start([this, w = std::move(wanted)](Scanner &s) { return s.scan_all(w, cancel_); });
}

bool ScanThread::start_folder(std::string path, bool recursive) {
    return start([this, p = std::move(path), recursive](Scanner &s) { return s.scan_folder(p, recursive, cancel_); });
}

void ScanThread::cancel() { cancel_ = true; }

void ScanThread::wait() {
    if (th_.joinable()) th_.join();
}

ScanProgress ScanThread::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_;
}

}  // namespace vinyl
