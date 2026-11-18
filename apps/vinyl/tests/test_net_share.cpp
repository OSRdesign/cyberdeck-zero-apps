/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: a network share mounted by the system is an ordinary folder (report 061 5.6, owner brief 2026-11-18): the
 * mount classifier on fake /proc/self/mounts text (USB vfat, ext4 on sd*, cifs, nfs4, tmpfs, sshfs, autofs, \040 escapes),
 * the drive rule that does not apply to a share (never "unplugged", never probed), the bounded runner (a probe that never
 * returns does not hold the caller past the timeout, is not started again while stuck, backs off), the scanner with an
 * unreachable share (rows kept, visible, nothing deleted), the Music folders row (not a USB drive) and the error text of a
 * track that cannot be reached.
 */
#include "check.hpp"

#include "decode/track_decoder.hpp"
#include "library/browse.hpp"
#include "library/library.hpp"
#include "library/lib_model.hpp"
#include "library/media_guard.hpp"
#include "library/net_share.hpp"
#include "library/roots.hpp"
#include "library/scanner.hpp"
#include "library/schema.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace vinyl;

namespace {

/* a gate a "hung" probe waits on; owned by shared_ptr so an abandoned job never touches a dead stack */
struct Gate {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    std::atomic<int> entered{0};
    void wait() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return open; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mu);
            open = true;
        }
        cv.notify_all();
    }
};

void wait_no_jobs() {
    for (int i = 0; i < 500 && BoundedRunner::live_threads() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

long long ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

void mkdirs(const std::string &p) {
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') ::mkdir(p.substr(0, i).c_str(), 0755);
}

const char *kMounts =
    "/dev/root / ext4 rw,noatime 0 0\n"
    "tmpfs /run tmpfs rw,nosuid 0 0\n"
    "/dev/sda1 /media/pi/USB\\040STICK vfat rw,uid=1000 0 0\n"
    "/dev/sdb1 /mnt/disk ext4 rw 0 0\n"
    "/dev/sdc1 /media/pi/NTFS fuseblk rw 0 0\n"
    "//nas/music /mnt/nas\\040music cifs rw,vers=3.0 0 0\n"
    "nas:/export/music /mnt/nfs nfs4 rw,soft 0 0\n"
    "pi@host:/music /mnt/ssh fuse.sshfs rw 0 0\n"
    "tmpfs /mnt/ram tmpfs rw 0 0\n"
    "systemd-1 /mnt/auto autofs rw,relatime 0 0\n"
    "//nas/auto /mnt/auto cifs rw 0 0\n"
    "systemd-1 /mnt/idle autofs rw,relatime 0 0\n"
    "/dev/sdd1 /media/pi/X\\134Y vfat rw 0 0\n"
    "broken\n";

}  // namespace

void test_net_share() {
    /* the file-system types */
    for (const char *t : {"cifs", "smb3", "nfs", "nfs4", "ceph", "9p", "fuse.sshfs", "fuse.rclone", "autofs"}) CHECK(is_share_fs(t));
    for (const char *t : {"vfat", "exfat", "ntfs", "ntfs3", "ext4", "btrfs", "tmpfs", "fuseblk", "fuse.ntfs-3g", "fuse.exfat", "fuse", "fuse.", "iso9660", "", "nfsd"})
        CHECK(!is_share_fs(t));

    /* the mounts: what lives on a share and what on a drive */
    const std::vector<MountEntry> m = parse_mounts(kMounts);
    CHECK(share_mount_of(m, "/mnt/nas music/Jazz/a.flac") != nullptr); /* \040 undone */
    CHECK_EQ(share_mount_of(m, "/mnt/nas music/Jazz/a.flac")->type, std::string("cifs"));
    CHECK(share_mount_of(m, "/mnt/nas/x.mp3") == nullptr);             /* "/mnt/nas" is not "/mnt/nas music" */
    CHECK(share_mount_of(m, "/mnt/nfs/a/b.mp3") != nullptr);
    CHECK(share_mount_of(m, "/mnt/ssh") != nullptr);
    CHECK(share_mount_of(m, "/mnt/auto/Album/1.mp3") != nullptr);
    CHECK_EQ(share_mount_of(m, "/mnt/auto/Album/1.mp3")->type, std::string("cifs")); /* the mount over the automount point */
    CHECK_EQ(share_mount_of(m, "/mnt/idle/Album/1.mp3")->type, std::string("autofs")); /* idle: the point alone */
    CHECK(share_mount_of(m, "/media/pi/USB STICK/a.mp3") == nullptr);
    CHECK(share_mount_of(m, "/mnt/disk/a.mp3") == nullptr);
    CHECK(share_mount_of(m, "/media/pi/NTFS/a.mp3") == nullptr);
    CHECK(share_mount_of(m, "/mnt/ram/a.mp3") == nullptr);
    CHECK(share_mount_of(m, "/media/pi/X\\Y/a.mp3") == nullptr); /* \134 is a backslash in the name */
    CHECK(share_mount_of(m, "/home/pi/Music/a.mp3") == nullptr);
    CHECK(share_mount_of({}, "/mnt/nas") == nullptr);

    /* the drive rule does not reach a share: present without a probe; a drive is probed; a mount that left is gone */
    int probes = 0;
    auto probe = [&](const std::string &) {
        ++probes;
        return true;
    };
    CHECK(drive_present_with("/mnt/nas music", kMounts, probe));
    CHECK(drive_present_with("/mnt/nfs", kMounts, probe));
    CHECK(drive_present_with("/mnt/ssh", kMounts, probe));
    CHECK(drive_present_with("/mnt/idle", kMounts, probe));
    CHECK_EQ(probes, 0);
    CHECK(drive_present_with("/media/pi/USB STICK", kMounts, probe));
    CHECK(drive_present_with("/mnt/disk", kMounts, probe));
    CHECK_EQ(probes, 2);
    CHECK(!drive_present_with("/media/pi/USB STICK", kMounts, [](const std::string &) { return false; })); /* stale mount */
    CHECK(!drive_present_with("/mnt/gone", kMounts, probe));                                                  /* not listed */
    CHECK(!drive_present_with("/mnt/gone", "", probe));
    CHECK_EQ(probes, 2);
    /* a queue of share tracks and stick tracks, Wi-Fi gone (the share's probe would fail): only the stick leaves */
    {
        const std::vector<std::string> q = {"/mnt/nas music/a.mp3", "/mnt/nfs/b.mp3", "/mnt/ssh/c.mp3", "/media/pi/USB STICK/d.mp3", "/mnt/disk/e.mp3"};
        auto present = [&](const std::string &d) { return drive_present_with(d, kMounts, [](const std::string &) { return false; }); };
        const std::vector<std::string> gone = gone_drive_dirs(q, present, default_drive_bases());
        CHECK_EQ(gone.size(), static_cast<size_t>(2));
        CHECK_EQ(gone[0], std::string("/media/pi/USB STICK"));
        CHECK_EQ(gone[1], std::string("/mnt/disk"));
    }

    /* the roots the Music folders list offers */
    {
        const std::vector<Root> r = list_roots("/home/pi", "pi", kMounts);
        bool nas = false, usb = false, auto_idle = false;
        for (const Root &x : r) {
            if (x.path == "/mnt/nas music") nas = x.network;
            if (x.path == "/media/pi/USB STICK") usb = !x.network;
            if (x.path == "/mnt/idle") auto_idle = x.network;
        }
        CHECK(nas);
        CHECK(usb);
        CHECK(auto_idle);
    }

    /* the Music folders row of a share is a folder's, not a USB drive's */
    {
        RootInfo share, stick;
        share.id = 1;
        share.path = "/mnt/nas";
        share.label = "nas";
        share.tracks = 10;
        share.network = true;
        stick = share;
        stick.id = 2;
        stick.path = "/mnt/stick";
        stick.label = "STICK";
        stick.network = false;
        const std::vector<libm::Row> rows = libm::folder_rows({share, stick}, "/home/pi");
        CHECK_EQ(rows.size(), static_cast<size_t>(2));
        CHECK_EQ(rows[0].text, std::string("/mnt/nas"));
        CHECK_EQ(rows[1].text, std::string("STICK (USB drive)"));
        CHECK(rows[0].key.empty()); /* nothing to eject */
    }

    /* the error of a track that cannot be reached: the queue keeps it, playback skips on (the engine's OpenFailed path) */
    CHECK(track_error_text(-ETIMEDOUT, "/mnt/nas/a.mp3").find("network share") != std::string::npos);
    CHECK(track_error_text(-EHOSTUNREACH, "/mnt/nas/a.mp3").find("network share") != std::string::npos);
    CHECK(track_error_text(-ENOENT, "/mnt/nas/a.mp3").find("gone") != std::string::npos);

    /* the bounded runner, on a clock of its own */
    {
        std::atomic<uint64_t> clock{1000};
        BoundedRunner::Config c;
        c.timeout_ms = 150;
        c.backoff_ms = 1000;
        c.backoff_max_ms = 3000;
        c.now_ms = [&] { return clock.load(); };
        BoundedRunner run(c);
        auto gate = std::make_shared<Gate>();
        auto stuck_job = [gate] {
            ++gate->entered;
            gate->wait();
        };
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(run.run("share", stuck_job) == BoundedRunner::Status::TimedOut);
        CHECK(ms_since(t0) < 1500); /* the caller got back, not held for as long as the job */
        CHECK(ms_since(t0) >= 100);
        CHECK_EQ(gate->entered.load(), 1);
        /* inside the back-off, and while the job is stuck: not started again, no wait */
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(run.run("share", stuck_job) == BoundedRunner::Status::BackedOff);
        CHECK(ms_since(t1) < 100);
        clock = 1000 + 5000; /* the back-off is over, the job still sits in the kernel: still not started again */
        CHECK(run.run("share", stuck_job) == BoundedRunner::Status::BackedOff);
        CHECK_EQ(gate->entered.load(), 1);
        /* another share is its own key */
        CHECK(run.run("other", [] {}) == BoundedRunner::Status::Done);
        /* the server answers at last: the stuck job ends, the key works again */
        gate->release();
        wait_no_jobs();
        CHECK(run.run("share", [] {}) == BoundedRunner::Status::Done);
        /* the back-off doubles: 1000, 2000, then 3000 at most */
        auto gate2 = std::make_shared<Gate>();
        auto hang = [gate2] { gate2->wait(); };
        clock = 10000;
        CHECK(run.run("slow", hang) == BoundedRunner::Status::TimedOut); /* until 11000 */
        gate2->release();
        wait_no_jobs();
        clock = 10999;
        CHECK(run.run("slow", [] {}) == BoundedRunner::Status::BackedOff);
        clock = 11000;
        auto gate3 = std::make_shared<Gate>();
        CHECK(run.run("slow", [gate3] { gate3->wait(); }) == BoundedRunner::Status::TimedOut); /* second: 2000, until 13000 */
        gate3->release();
        wait_no_jobs();
        clock = 12999;
        CHECK(run.run("slow", [] {}) == BoundedRunner::Status::BackedOff);
        clock = 13000;
        auto gate4 = std::make_shared<Gate>();
        CHECK(run.run("slow", [gate4] { gate4->wait(); }) == BoundedRunner::Status::TimedOut); /* third: 3000 (limit) */
        gate4->release();
        wait_no_jobs();
        clock = 15999;
        CHECK(run.run("slow", [] {}) == BoundedRunner::Status::BackedOff);
        clock = 16000;
        CHECK(run.run("slow", [] {}) == BoundedRunner::Status::Done);
        /* the result of an abandoned job reaches nobody: it writes into its own box */
        auto box = std::make_shared<std::atomic<int>>(0);
        auto gate5 = std::make_shared<Gate>();
        clock = 20000;
        CHECK(run.run("late", [box, gate5] {
            gate5->wait();
            box->store(7);
        }) == BoundedRunner::Status::TimedOut);
        gate5->release();
        wait_no_jobs();
        CHECK_EQ(box->load(), 7);
    }

    /* probe_share_root and list_folder_bounded through the global runner */
    {
        net_runner().reset();
        BoundedRunner::Config c;
        c.timeout_ms = 120;
        c.backoff_ms = 60000;
        net_runner().configure(c);
        const std::string T = make_temp_dir("netshare");
        mkdirs(T + "/share/Artist");
        write_file(T + "/share/Artist/01 A.mp3", "x");
        auto gate = std::make_shared<Gate>();
        std::atomic<int> calls{0};
        set_root_probe([gate, &calls, share = T + "/share"](const std::string &p) {
            if (p.compare(0, share.size(), share) != 0) return true;
            ++calls;
            gate->wait();
            return true;
        });
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!probe_share_root(T + "/share"));
        CHECK(ms_since(t0) < 1500);
        CHECK(!probe_share_root(T + "/share")); /* backed off: no second hung thread */
        CHECK(!share_answers(T + "/share/Artist", T + "/share"));
        CHECK_EQ(calls.load(), 1);
        gate->release();
        wait_no_jobs();
        set_root_probe(nullptr);
        net_runner().reset();
        CHECK(probe_share_root(T + "/share"));
        std::vector<Entry> es;
        CHECK_EQ(list_folder_bounded(T + "/share", true, es), 0);
        CHECK_EQ(es.size(), static_cast<size_t>(1));
        CHECK_EQ(list_folder_bounded(T + "/share/nope", true, es), -ENOENT);
        CHECK(es.empty());
        net_runner().configure(BoundedRunner::Config());
        net_runner().reset();
        rm_tree(T);
    }

    /* the scanner with a share that stops answering: rows kept, visible, nothing deleted; it comes back: scanned as usual */
    std::string why;
    if (!real_sqlite_lib(&why)) {
        SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
        return;
    }
    {
        const std::string T = make_temp_dir("netscan");
        const std::string share = T + "/share";
        write_file(T + "/x", "");
        mkdirs(share + "/Artist/Album");
        write_file(share + "/Artist/Album/01 One.mp3", "x");
        write_file(share + "/Artist/Album/02 Two.mp3", "x");
        std::string mounts = "/dev/root / ext4 rw 0 0\n//nas/music " + share + " cifs rw 0 0\n";
        int reads = 0;
        ScanHooks h;
        h.read_tags = [&](const std::string &path, TrackTags &t) {
            ++reads;
            const size_t s = path.rfind('/');
            std::string stem = path.substr(s + 1);
            stem = stem.substr(0, stem.rfind('.'));
            t = TrackTags();
            t.title = stem.substr(3);
            t.album = "Album";
            t.artist = "Artist";
            t.track = std::atoi(stem.c_str());
            t.codec = "MP3";
            t.duration_ms = 1000;
            t.ok = true;
            return true;
        };
        h.temp_mc = [] { return 40000; };
        uint64_t clk = 1000;
        h.mono_ms = [&] { return clk += 10; };
        h.sleep_ms = [](uint32_t) {};
        h.mounts = [&] { return mounts; };
        h.by_uuid_dir = T + "/by-uuid";
        Db db;
        CHECK(open_library_db(db, ":memory:", 416, &why));
        ScanOptions opt;
        opt.heat_check_ms = 0;
        std::atomic<bool> cancel{false};
        const std::vector<RootSpec> wanted = {{share, "share"}};
        net_runner().reset();
        BoundedRunner::Config c;
        c.timeout_ms = 120;
        c.backoff_ms = 100;
        c.backoff_max_ms = 100;
        net_runner().configure(c);
        auto tracks = [&] { return db.scalar("select count(*) from tracks"); };
        {
            Scanner sc(db, h, opt);
            CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        }
        CHECK_EQ(tracks(), 2LL);
        CHECK_EQ(db.scalar("select online from roots where path = '" + share + "'"), 1LL);
        const long long first_scan = db.scalar("select last_scan from roots where path = '" + share + "'");
        CHECK(first_scan > 0);

        /* a root_online on a share: online while mounted, never probed */
        int probed = 0;
        set_root_probe([&](const std::string &) {
            ++probed;
            return false;
        });
        {
            const std::vector<RootRow> rows = load_roots(db);
            CHECK(root_online(rows[0], parse_mounts(mounts), T + "/by-uuid"));
            CHECK_EQ(probed, 0);
            CHECK(!root_online(rows[0], parse_mounts("/dev/root / ext4 rw 0 0\n"), T + "/by-uuid")); /* unmounted altogether */
        }

        /* the share does not answer (the probe says no) while its files "disappear" from the listing: nothing is deleted, the root
         * stays online, its tracks stay visible, last_scan is not touched */
        ::unlink((share + "/Artist/Album/01 One.mp3").c_str());
        ::unlink((share + "/Artist/Album/02 Two.mp3").c_str());
        reads = 0;
        {
            Scanner sc(db, h, opt);
            CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
            CHECK_EQ(sc.progress().removed, static_cast<uint64_t>(0));
        }
        CHECK_EQ(tracks(), 2LL);
        CHECK_EQ(db.scalar("select online from roots where path = '" + share + "'"), 1LL);
        CHECK_EQ(db.scalar("select last_scan from roots where path = '" + share + "'"), first_scan);
        CHECK_EQ(reads, 0);
        {
            Library lib(db);
            std::vector<AlbumRow> al;
            CHECK(lib.albums(0, 40, al));
            CHECK_EQ(al.size(), static_cast<size_t>(1)); /* still shown */
        }
        {
            Scanner sc(db, h, opt);
            CHECK(sc.scan_folder(share + "/Artist", true, cancel) == ScanProgress::State::Done);
        }
        CHECK_EQ(tracks(), 2LL);
        CHECK_EQ(db.scalar("select online from roots where path = '" + share + "'"), 1LL);

        /* the probe hangs (a hard NFS mount): the scan returns within the bounded wait, rows kept */
        set_root_probe(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(150)); /* the back-off of the failed probes is over */
        auto gate = std::make_shared<Gate>();
        set_root_probe([gate, share](const std::string &p) {
            if (p.compare(0, share.size(), share) == 0) gate->wait();
            return true;
        });
        const auto t0 = std::chrono::steady_clock::now();
        {
            Scanner sc(db, h, opt);
            CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        }
        CHECK(ms_since(t0) < 3000);
        CHECK_EQ(tracks(), 2LL);
        gate->release();
        wait_no_jobs();

        /* reachable but still only the automount point (the mount failed): not "there" either, rows kept */
        set_root_probe(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        mounts = "/dev/root / ext4 rw 0 0\nsystemd-1 " + share + " autofs rw 0 0\n";
        {
            Scanner sc(db, h, opt);
            CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
            CHECK_EQ(sc.progress().removed, static_cast<uint64_t>(0));
        }
        CHECK_EQ(tracks(), 2LL);
        CHECK_EQ(db.scalar("select online from roots where path = '" + share + "'"), 1LL);

        /* the share is back and the files really are gone: the scan removes them as it does for any folder */
        mounts = "/dev/root / ext4 rw 0 0\n//nas/music " + share + " cifs rw 0 0\n";
        {
            Scanner sc(db, h, opt);
            CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
            CHECK_EQ(sc.progress().removed, static_cast<uint64_t>(2));
        }
        CHECK_EQ(tracks(), 0LL);
        CHECK_EQ(db.scalar("select online from roots where path = '" + share + "'"), 1LL);

        net_runner().configure(BoundedRunner::Config());
        net_runner().reset();
        rm_tree(T);
    }
}
