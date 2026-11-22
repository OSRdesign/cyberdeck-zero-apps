/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the scanner (report 060 3a, "Incremental scan", "Priority", "Heat and drives that go away"; owner
 * decisions D27 and D28 as changed on 2026-11-13; P2b).
 *
 *   walk      readdir per root with d_type (fstatat only where it says nothing), no symlinks followed, a (dev, ino) guard
 *             against loops, hidden files and folders skipped; folders in a fixed order (by name, parents before their
 *             sub-folders), so a cancelled scan can resume after the last folder it committed.
 *   diff      per folder, the (name, mtime, size) of each audio file against the folder's rows (one query a folder:
 *             memory stays bounded, no global path map). Tags are read (scan mode: no picture body, never FFmpeg) only
 *             for new or changed files. A row is deleted only when its root is online and stat() says ENOENT; a folder
 *             that is gone goes the same way at the end of a full scan of its root.
 *   commits   one transaction for many files, FTS rows inside it: committed at a folder boundary once 500 files were
 *             written (1,000 on boards with more than 512 MB) or 2 s passed, inside a big folder at the file count. An
 *             unchanged rescan reads no tag and writes only the roots' last-scan times. Albums, artists and genres left
 *             empty are pruned in the same commit.
 *   heat      paused above 75 °C (the open batch committed first), resumed below 70 °C.
 *   drives    EIO / ENOENT under a root whose mount left /proc/self/mounts marks the root offline: the walk of that root
 *             stops and its rows stay, hidden (never deleted).
 *   priority  the ScanThread runs at nice 19 with ioprio_set(IOPRIO_CLASS_IDLE): the SD boards' mq-deadline honours it.
 *   when      the owner's D27 (2026-11-13): at app launch and when a folder is opened from the Library, never in the
 *             background or on a timer, stopped when the user leaves the Library (the UI calls cancel()); a cancelled
 *             full scan resumes where it stopped at the next start_all().
 * Playback never waits for the scanner: it has its own thread and its own database connection. LVGL-free.
 */
#pragma once

#include "../db/sqlite_lib.hpp"
#include "../tags/tag_reader.hpp"
#include "roots.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {

class LibraryWriter;

struct ScanProgress {
    enum class State { Idle, Scanning, Paused, Done, Cancelled, Failed };
    State state = State::Idle;
    int temp_c = 0; /* the SoC temperature when paused for heat */
    uint64_t folders = 0, files = 0; /* audio files seen */
    uint64_t read = 0;               /* tags read (new or changed files) */
    uint64_t added = 0, updated = 0, removed = 0, commits = 0;
    std::string root;  /* the label of the root being walked */
    std::string error; /* why it Failed */
    bool resumed = false;
};
const char *scan_state_name(ScanProgress::State s); /* "idle", "scanning", "paused", "done", "cancelled", "failed" */

/* The seams the tests replace. */
struct ScanHooks {
    std::function<bool(const std::string &path, TrackTags &out)> read_tags; /* scan mode */
    std::function<int()> temp_mc;               /* millidegrees C, < 0 unknown */
    std::function<uint64_t()> mono_ms;          /* a monotonic clock */
    std::function<void(uint32_t ms)> sleep_ms;  /* while paused for heat */
    std::function<std::string()> mounts;        /* the text of /proc/self/mounts */
    std::string by_uuid_dir = "/dev/disk/by-uuid";
};
/* read_tags_scan, /sys/class/thermal/thermal_zone0/temp, the steady clock, /proc/self/mounts */
ScanHooks default_scan_hooks();

struct ScanOptions {
    int batch_files = 500;      /* rows written before a commit */
    uint64_t batch_ms = 2000;   /* or this long since the batch began (at a folder boundary) */
    int hot_mc = 75000;         /* pause at or above */
    int cool_mc = 70000;        /* resume at or below */
    uint64_t heat_check_ms = 2000;
    uint32_t heat_poll_ms = 1000;
};
/* 500 files on boards with 512 MB or less, 1,000 above (report 060 section 4) */
ScanOptions scan_options_for(int ram_mb);

/* The scan itself, on the calling thread, on the caller's connection. */
class Scanner {
public:
    Scanner(Db &db, ScanHooks hooks = default_scan_hooks(), ScanOptions opt = ScanOptions());
    ~Scanner();
    Scanner(const Scanner &) = delete;
    Scanner &operator=(const Scanner &) = delete;

    /* The full scan: the roots brought in line with `wanted`, every online root walked, the folders that are gone
     * removed; resumes a cancelled one after the last folder it committed. */
    ScanProgress::State scan_all(const std::vector<RootSpec> &wanted, const std::atomic<bool> &cancel);
    /* One folder of an online root (its sub-folders too when `recursive`): a folder opened from the Library. A path
     * under no root does nothing. */
    ScanProgress::State scan_folder(const std::string &path, bool recursive, const std::atomic<bool> &cancel);

    /* Thread-safe. */
    ScanProgress progress() const;
    /* Called after every commit (the thread's generation counter) and whenever the progress changes. */
    std::function<void()> on_commit;
    std::function<void(const ScanProgress &)> on_progress;

private:
    enum class Walk { Ok, Cancelled, Offline, Failed };
    struct Resume {
        bool active = false;
        int64_t root = 0;
        bool has_rel = false;
        std::string rel;
    };
    struct FileEntry {
        std::string name;
        int64_t mtime;
        uint64_t size;
    };
    Walk walk(const RootRow &root, const std::string &start, bool recursive, const Resume *resume,
              const std::atomic<bool> &cancel);
    Walk folder_files(const RootRow &root, const std::string &rel, int64_t dir_mtime, std::vector<FileEntry> &files,
                      const std::atomic<bool> &cancel);
    Walk fail();
    bool sweep(const RootRow &root);
    bool still_online(const RootRow &root);
    bool on_share(const RootRow &root); /* its mount is a network share (net_share.hpp): probed bounded, never marked offline */
    bool heat(const std::atomic<bool> &cancel);
    bool begin();
    bool commit();
    bool maybe_commit(bool boundary);
    void set_state(ScanProgress::State s);
    void publish();
    ScanProgress::State finish(ScanProgress::State s);

    Db &db_;
    ScanHooks hooks_;
    ScanOptions opt_;
    std::unique_ptr<LibraryWriter> w_;
    bool in_tx_ = false;
    bool dirty_ = false; /* rows changed or removed in this batch: prune at its commit */
    uint64_t batch_start_ = 0;
    uint64_t last_heat_ = 0;
    std::string resume_value_; /* written with each commit of a full scan */
    bool full_ = false;
    mutable std::mutex mu_;
    ScanProgress p_;
};

/* The scanner's thread: its own connection, nice 19, I/O class idle. One scan at a time. */
class ScanThread {
public:
    explicit ScanThread(std::string db_path, int ram_mb, ScanHooks hooks = default_scan_hooks());
    ~ScanThread();
    ScanThread(const ScanThread &) = delete;
    ScanThread &operator=(const ScanThread &) = delete;

    /* false when a scan is running already */
    bool start_all(std::vector<RootSpec> wanted);
    bool start_folder(std::string path, bool recursive);
    /* Asks the scan to stop at the next file (returns at once; the open batch is committed). */
    void cancel();
    /* Waits for the running scan to end. */
    void wait();
    bool running() const { return running_.load(); }
    ScanProgress progress() const;
    /* Changes at every commit and at the end of a scan: the Library pages ask for their rows again. */
    uint64_t gen() const { return gen_.load(); }
    /* The options the thread scans with (scan_options_for(ram_mb); the tests shorten them). */
    ScanOptions options;

private:
    bool start(std::function<ScanProgress::State(Scanner &)> job);

    std::string path_;
    int ram_mb_;
    ScanHooks hooks_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> gen_{0};
    mutable std::mutex mu_;
    ScanProgress last_;
};

/* Lowers the calling thread: nice 19 and the idle I/O class (both best effort). */
void lower_thread_priority();

}  // namespace vinyl
