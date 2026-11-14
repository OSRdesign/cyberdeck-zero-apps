/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the library scanner and its roots (report 060 2.3, test_scanner) on a temporary fixture tree, with a
 * counting tag reader, fake mounts and a fake /dev/disk/by-uuid: the first scan; an unchanged rescan reads no tags; a
 * touched file is read again; deleted files and folders go; a pulled drive is hidden (rows kept), then restored, also
 * under another mount point (its UUID); cancel and resume; the heat pause; scanning one folder; the thread.
 */
#include "check.hpp"

#include "library/library.hpp"
#include "library/roots.hpp"
#include "library/scanner.hpp"
#include "library/schema.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <future>
#include <sys/stat.h>
#include <unistd.h>

using namespace vinyl;

namespace {

std::string base(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string dir_of(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}

void mkdirs(const std::string &p) {
    if (p.empty() || ::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST) return;
    mkdirs(dir_of(p));
    ::mkdir(p.c_str(), 0755);
}

void put(const std::string &path, const std::string &bytes = "x") {
    mkdirs(dir_of(path));
    write_file(path, bytes);
}

void set_mtime(const std::string &path, long sec) {
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = sec;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
}

/* the fake environment of one test run */
struct Env {
    std::string tmp;
    std::string mounts;      /* the fake /proc/self/mounts */
    int reads = 0;           /* tag reads */
    std::vector<std::string> read_paths;
    std::function<void(const std::string &)> on_read; /* a test's hook into a read */
    int temp_mc = 40000;
    std::vector<int> temps; /* temperatures returned first, then temp_mc */
    int sleeps = 0;
    std::function<void()> on_sleep;
    uint64_t clock = 1000;

    ScanHooks hooks() {
        ScanHooks h;
        h.read_tags = [this](const std::string &path, TrackTags &t) {
            ++reads;
            read_paths.push_back(path);
            if (on_read) on_read(path);
            if (::access(path.c_str(), R_OK) != 0) return false;
            if (read_file(path) == "unreadable") return false;
            t = TrackTags();
            std::string stem = base(path);
            stem = stem.substr(0, stem.rfind('.'));
            t.title = stem.size() > 3 ? stem.substr(3) : stem; /* "01 One" -> "One" */
            t.album = base(dir_of(path));
            t.artist = base(dir_of(dir_of(path)));
            t.track = std::atoi(stem.c_str());
            t.codec = "MP3";
            t.duration_ms = 1000;
            t.ok = true;
            return true;
        };
        h.temp_mc = [this] {
            if (!temps.empty()) {
                const int t = temps.front();
                temps.erase(temps.begin());
                return t;
            }
            return temp_mc;
        };
        h.mono_ms = [this] { return clock += 10; };
        h.sleep_ms = [this](uint32_t) {
            ++sleeps;
            if (on_sleep) on_sleep();
        };
        h.mounts = [this] { return mounts; };
        h.by_uuid_dir = tmp + "/by-uuid";
        return h;
    }
};

int visible_tracks(Db &db) {
    Library lib(db);
    return lib.counts().tracks;
}

}  // namespace

void test_scanner() {
    /* mounts, UUIDs and the wanted roots, without a database */
    {
        const std::vector<MountEntry> m = parse_mounts("/dev/root / ext4 rw 0 0\n"
                                                       "/dev/sda1 /media/pi/USB\\040STICK vfat rw 0 0\n"
                                                       "/dev/sdb1 /media/pi/USB vfat rw 0 0\n"
                                                       "broken line\n");
        CHECK_EQ(m.size(), static_cast<size_t>(3));
        CHECK_EQ(m[1].dir, std::string("/media/pi/USB STICK"));
        CHECK_EQ(mount_of(m, "/media/pi/USB STICK/Jazz")->dev, std::string("/dev/sda1"));
        CHECK_EQ(mount_of(m, "/media/pi/USB/Jazz")->dev, std::string("/dev/sdb1"));
        CHECK_EQ(mount_of(m, "/media/pi/USBX")->dir, std::string("/"));
        CHECK_EQ(mount_of(m, "/home/pi/Music")->dir, std::string("/"));
        CHECK(mount_of({}, "/x") == nullptr);
        const std::vector<RootSpec> r = library_roots("/home/pi", "pi",
                                                      "/dev/root / ext4 rw 0 0\n/dev/sda1 /media/pi/STICK vfat rw 0 0\n",
                                                      {"/srv/music/", "/home/pi/Music/Jazz", "/media/pi", "relative", "/srv/music"});
        CHECK_EQ(r.size(), static_cast<size_t>(3)); /* Music, STICK, /srv/music; the others overlap or are not absolute */
        CHECK_EQ(r[0].path, std::string("/home/pi/Music"));
        CHECK_EQ(r[1].path, std::string("/media/pi/STICK"));
        CHECK_EQ(r[1].label, std::string("STICK"));
        CHECK_EQ(r[2].path, std::string("/srv/music"));
        CHECK_EQ(r[2].label, std::string("music"));
        /* the settings keep the folders (and only absolute ones) */
        Settings s;
        s.library_folders = {"/srv/music", "/data/flac"};
        Settings back;
        json::Value raw;
        CHECK(settings_from_json(settings_to_json(s, nullptr), back, &raw));
        CHECK(back == s);
        CHECK(settings_from_json("{\"library\": {\"folders\": [\"/a\", 3, \"b\"], \"later\": 1}}", back, &raw));
        CHECK_EQ(back.library_folders.size(), static_cast<size_t>(1));
        back.library_folders.clear();
        CHECK(settings_to_json(back, &raw).find("\"later\"") != std::string::npos); /* a newer version's key kept */
        CHECK(settings_to_json(Settings(), nullptr).find("library") == std::string::npos);
        CHECK_EQ(scan_options_for(416).batch_files, 500);
        CHECK_EQ(scan_options_for(4096).batch_files, 1000);
        CHECK_EQ(std::string(scan_state_name(ScanProgress::State::Paused)), std::string("paused"));
    }

    std::string why;
    if (!real_sqlite_lib(&why)) {
        SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
        return;
    }
    Env env;
    env.tmp = make_temp_dir("scanner");
    const std::string T = env.tmp;
    const std::string music = T + "/music", usb = T + "/usb";
    put(music + "/Artist A/Album 1/01 One.mp3");
    put(music + "/Artist A/Album 1/02 Two.mp3");
    put(music + "/Artist A/Album 1/cover.jpg");
    put(music + "/Artist A/Album 1/.03 Hidden.mp3");
    put(music + "/Artist A/Album 2/01 Three.flac");
    put(music + "/Artist B/Single/01 Four.ogg");
    put(music + "/.hidden/Album/05 Secret.mp3");
    CHECK_EQ(::symlink((music + "/Artist A/Album 1/01 One.mp3").c_str(), (music + "/link.mp3").c_str()), 0);
    CHECK_EQ(::symlink((music + "/Artist A").c_str(), (music + "/linkdir").c_str()), 0);
    put(usb + "/Album X/01 X.mp3");
    put(usb + "/Album X/02 Y.mp3");
    /* the fake drive: a device node stand-in, its UUID link, its mount line */
    put(T + "/dev/fakeusb", "");
    mkdirs(T + "/by-uuid");
    CHECK_EQ(::symlink("../dev/fakeusb", (T + "/by-uuid/1234-ABCD").c_str()), 0);
    CHECK_EQ(device_uuid(T + "/dev/fakeusb", T + "/by-uuid"), std::string("1234-ABCD"));
    CHECK_EQ(device_uuid(T + "/dev/none", T + "/by-uuid"), std::string(""));
    CHECK_EQ(device_uuid("tmpfs", T + "/by-uuid"), std::string(""));
    const std::string root_line = "/dev/root / ext4 rw 0 0\n";
    auto mount_usb = [&](const std::string &dir) { env.mounts = root_line + T + "/dev/fakeusb " + dir + " vfat rw 0 0\n"; };
    mount_usb(usb);
    std::vector<RootSpec> wanted = {{music, "Music"}, {usb, "USB"}};

    Db db;
    CHECK(open_library_db(db, ":memory:", 416, &why));
    ScanOptions opt;
    opt.batch_files = 2; /* many commits on a small tree */
    opt.heat_check_ms = 0;
    std::atomic<bool> cancel{false};

    /* the first scan: six tracks; hidden files and folders, symlinks and covers left out */
    {
        Scanner sc(db, env.hooks(), opt);
        int commits = 0;
        sc.on_commit = [&] { ++commits; };
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        const ScanProgress p = sc.progress();
        CHECK_EQ(env.reads, 6);
        CHECK_EQ(p.read, static_cast<uint64_t>(6));
        CHECK_EQ(p.files, static_cast<uint64_t>(6));
        CHECK_EQ(p.added, static_cast<uint64_t>(6));
        CHECK(p.commits >= 3);
        CHECK(commits >= 3);
        CHECK(!p.resumed);
        CHECK(std::none_of(env.read_paths.begin(), env.read_paths.end(),
                           [](const std::string &s) { return s.find("Hidden") != std::string::npos || s.find("Secret") != std::string::npos || s.find("link") != std::string::npos; }));
    }
    CHECK_EQ(visible_tracks(db), 6);
    CHECK_EQ(db.scalar("select count(*) from roots where online = 1"), 2LL);
    CHECK_EQ(db.scalar_text("select uuid from roots where path = '" + usb + "'"), std::string("1234-ABCD"));
    CHECK_EQ(db.scalar_text("select mount from roots where path = '" + usb + "'"), std::string(usb));
    CHECK_EQ(db.scalar_text("select mount from roots where path = '" + music + "'"), std::string(""));
    CHECK(db.scalar("select max(last_scan) from roots") > 0);
    CHECK_EQ(meta_get(db, "scan_resume"), std::string(""));
    {
        Library lib(db);
        std::vector<AlbumRow> al;
        CHECK(lib.albums(0, 40, al));
        CHECK_EQ(al.size(), static_cast<size_t>(4));
        std::vector<TrackRow> tr;
        CHECK(lib.album_tracks(al[0].id, tr)); /* "Album 1": One, Two */
        CHECK_EQ(tr.size(), static_cast<size_t>(2));
        CHECK_EQ(tr[0].path, music + "/Artist A/Album 1/01 One.mp3");
        CHECK_EQ(lib.find_track(music + "/Artist B/Single/01 Four.ogg") > 0, true);
    }

    /* an unchanged rescan reads no tag and changes no row */
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 0);
        CHECK_EQ(sc.progress().files, static_cast<uint64_t>(6));
        CHECK_EQ(sc.progress().added + sc.progress().updated + sc.progress().removed, static_cast<uint64_t>(0));
    }

    /* a touched file is read again (mtime), so is a rewritten one (size) */
    set_mtime(music + "/Artist A/Album 1/02 Two.mp3", 1600000000);
    put(usb + "/Album X/01 X.mp3", "longer content");
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 2);
        CHECK_EQ(sc.progress().updated, static_cast<uint64_t>(2));
    }
    CHECK_EQ(visible_tracks(db), 6);

    /* a deleted file goes (and its artist with it); a deleted folder goes */
    ::unlink((music + "/Artist B/Single/01 Four.ogg").c_str());
    ::unlink((music + "/Artist A/Album 2/01 Three.flac").c_str());
    ::rmdir((music + "/Artist A/Album 2").c_str());
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 0);
        CHECK_EQ(sc.progress().removed, static_cast<uint64_t>(1));
    }
    CHECK_EQ(visible_tracks(db), 4);
    CHECK_EQ(db.scalar("select count(*) from artists where name = 'Artist B'"), 0LL);
    CHECK_EQ(db.scalar("select count(*) from folders where rel = 'Artist A/Album 2'"), 0LL);
    CHECK_EQ(db.scalar("select count(*) from tracks"), db.scalar("select count(*) from fts_docsize"));

    /* the drive pulled (its mount line gone, the empty mount point left): hidden, nothing deleted */
    env.mounts = root_line;
    wanted = {{music, "Music"}};
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 0);
    }
    CHECK_EQ(visible_tracks(db), 2);
    CHECK_EQ(db.scalar("select count(*) from tracks"), 4LL);
    CHECK_EQ(db.scalar("select online from roots where path = '" + usb + "'"), 0LL);
    /* still wanted (the list made before the pull) but not mounted: offline as well */
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all({{music, "Music"}, {usb, "USB"}}, cancel) == ScanProgress::State::Done);
    }
    CHECK_EQ(visible_tracks(db), 2);
    /* plugged in again: back, nothing read */
    mount_usb(usb);
    wanted = {{music, "Music"}, {usb, "USB"}};
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 0);
    }
    CHECK_EQ(visible_tracks(db), 4);
    /* mounted again under another name (USB1): matched by its UUID, the same root, nothing read */
    const long long usb_id = db.scalar("select id from roots where path = '" + usb + "'");
    const std::string usb1 = T + "/usb1";
    CHECK_EQ(std::rename(usb.c_str(), usb1.c_str()), 0);
    mount_usb(usb1);
    wanted = {{music, "Music"}, {usb1, "USB1"}};
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 0);
    }
    CHECK_EQ(db.scalar("select id from roots where path = '" + usb1 + "'"), usb_id);
    CHECK_EQ(db.scalar("select count(*) from roots"), 2LL);
    CHECK_EQ(visible_tracks(db), 4);
    {
        Library lib(db);
        CHECK(lib.find_track(usb1 + "/Album X/02 Y.mp3") > 0);
    }

    /* pulled during a scan (a read in the first root pulls it): the walk of its root finds it gone and hides it */
    set_mtime(music + "/Artist A/Album 1/01 One.mp3", 1600000001);
    const std::string away = T + "/away";
    env.on_read = [&](const std::string &) {
        if (::access(usb1.c_str(), F_OK) == 0) std::rename(usb1.c_str(), away.c_str());
        env.mounts = root_line;
    };
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
    }
    env.on_read = nullptr;
    CHECK_EQ(db.scalar("select online from roots where id = " + std::to_string(usb_id)), 0LL);
    CHECK_EQ(db.scalar("select count(*) from tracks"), 4LL);
    CHECK_EQ(visible_tracks(db), 2);
    std::rename(away.c_str(), usb1.c_str());
    mount_usb(usb1);
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
    }
    CHECK_EQ(visible_tracks(db), 4);

    /* mounted but unreadable: a stick pulled while its mount is still listed (a stale mount, every read fails) is gone as a
     * pulled one is: offline, hidden, nothing shown (owner, 2026-11-14, B45); readable again, it is back */
    set_root_probe([&](const std::string &p) { return p != usb1 && root_readable(p); });
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
    }
    CHECK_EQ(db.scalar("select online from roots where id = " + std::to_string(usb_id)), 0LL);
    CHECK_EQ(visible_tracks(db), 2);
    set_root_probe(nullptr);
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
    }
    CHECK_EQ(visible_tracks(db), 4);
    CHECK(root_readable(music));
    CHECK(!root_readable(T + "/no-such-folder"));

    /* cancel and resume: 6 folders of 3 files; cancelled at the 7th read (in the third folder) */
    const std::string big = T + "/big";
    for (int f = 1; f <= 6; ++f)
        for (int i = 1; i <= 3; ++i) put(big + "/f" + std::to_string(f) + "/0" + std::to_string(i) + " T.mp3");
    wanted.push_back({big, "Big"});
    env.reads = 0;
    env.on_read = [&](const std::string &) {
        if (env.reads == 7) cancel = true;
    };
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Cancelled);
        CHECK_EQ(env.reads, 7);
    }
    env.on_read = nullptr;
    cancel = false;
    const std::string resume = meta_get(db, "scan_resume");
    CHECK(resume.find("\tf2") != std::string::npos); /* the last folder done */
    CHECK_EQ(db.scalar("select count(*) from tracks t join roots r on r.id = t.root_id where r.label = 'Big'"), 7LL);
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        const ScanProgress p = sc.progress();
        CHECK(p.resumed);
        CHECK_EQ(env.reads, 11);                       /* the 11 not read before */
        CHECK_EQ(p.files, static_cast<uint64_t>(12)); /* f1 and f2 (and the roots before) not walked again */
    }
    CHECK_EQ(meta_get(db, "scan_resume"), std::string(""));
    CHECK_EQ(visible_tracks(db), 4 + 18);
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
        CHECK(!sc.progress().resumed);
        CHECK_EQ(sc.progress().files, static_cast<uint64_t>(22));
        CHECK_EQ(env.reads, 0);
    }

    /* the heat pause: above 75 C the scan waits (its batch committed), below 70 C it goes on */
    set_mtime(big + "/f6/01 T.mp3", 1600000002);
    env.temps = {80000, 77000, 72000, 69000};
    env.sleeps = 0;
    std::vector<ScanProgress::State> states;
    {
        Scanner sc(db, env.hooks(), opt);
        sc.on_progress = [&](const ScanProgress &p) {
            if (states.empty() || states.back() != p.state) states.push_back(p.state);
        };
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Done);
    }
    CHECK_EQ(env.sleeps, 3);
    CHECK(std::find(states.begin(), states.end(), ScanProgress::State::Paused) != states.end());
    CHECK(states.back() == ScanProgress::State::Done);
    /* cancelled while paused */
    env.temp_mc = 90000;
    env.sleeps = 0;
    env.on_sleep = [&] {
        if (env.sleeps == 2) cancel = true;
    };
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_all(wanted, cancel) == ScanProgress::State::Cancelled);
        CHECK_EQ(sc.progress().temp_c, 90);
    }
    env.on_sleep = nullptr;
    env.temp_mc = 40000;
    cancel = false;

    /* one folder (opened from the Library): only it is walked */
    put(music + "/Artist A/Album 1/03 New.mp3");
    put(music + "/Artist C/Elsewhere/01 Later.mp3");
    env.reads = 0;
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_folder(music + "/Artist A/Album 1/", false, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 1);
        CHECK_EQ(sc.progress().added, static_cast<uint64_t>(1));
        CHECK(sc.scan_folder("/nowhere/at/all", true, cancel) == ScanProgress::State::Done);
        CHECK_EQ(env.reads, 1);
        CHECK(sc.scan_folder(music, true, cancel) == ScanProgress::State::Done); /* the whole root: Artist C found */
        CHECK_EQ(env.reads, 2);
    }
    /* an unreadable file is skipped (no row), the rest go on */
    put(music + "/Artist A/Album 1/04 Bad.mp3", "unreadable");
    {
        Scanner sc(db, env.hooks(), opt);
        CHECK(sc.scan_folder(music + "/Artist A/Album 1", false, cancel) == ScanProgress::State::Done);
    }
    CHECK_EQ(db.scalar("select count(*) from tracks where name = '04 Bad.mp3'"), 0LL);
    /* removing a folder from the library deletes its rows */
    const long long big_id = db.scalar("select id from roots where label = 'Big'");
    CHECK(remove_root(db, big_id));
    CHECK_EQ(db.scalar("select count(*) from tracks where root_id = " + std::to_string(big_id)), 0LL);
    CHECK_EQ(db.scalar("select count(*) from roots where id = " + std::to_string(big_id)), 0LL);
    CHECK_EQ(db.scalar("select count(*) from tracks"), db.scalar("select count(*) from fts_docsize"));

    /* the thread: its own connection to a file; one scan at a time; cancel */
    {
        ScanThread th(T + "/data/library.db", 416, env.hooks());
        th.options.heat_check_ms = 0;
        CHECK(!th.running());
        CHECK(th.start_all({{music, "Music"}}));
        th.wait();
        CHECK(th.progress().state == ScanProgress::State::Done);
        CHECK(th.gen() > 0);
        CHECK_EQ(th.progress().added, static_cast<uint64_t>(4)); /* One, Two, New, Later (Bad unreadable) */
        /* a scan held in a read: a second start is refused, cancel stops it */
        std::promise<void> release;
        std::shared_future<void> gate = release.get_future().share();
        std::atomic<bool> in_read{false};
        set_mtime(music + "/Artist A/Album 1/01 One.mp3", 1600000003);
        set_mtime(music + "/Artist A/Album 1/02 Two.mp3", 1600000003);
        env.on_read = [&](const std::string &) {
            in_read = true;
            gate.wait();
        };
        CHECK(th.start_folder(music + "/Artist A/Album 1", false));
        while (!in_read) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(th.running());
        CHECK(!th.start_all({{music, "Music"}}));
        th.cancel();
        release.set_value();
        th.wait();
        env.on_read = nullptr;
        CHECK(th.progress().state == ScanProgress::State::Cancelled);
        CHECK(!th.running());
        Db check;
        CHECK(check.open(T + "/data/library.db"));
        CHECK_EQ(check.scalar("select count(*) from tracks"), 4LL);
    }
    rm_tree(T);
}
