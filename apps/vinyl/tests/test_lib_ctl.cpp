/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the Library controller (src/core/library/library_ctl) on a temporary database of 100 artists (200 tracks)
 * with a recording host: the hub and the places Esc steps back through, the 40-row paging of Artists, an album and its
 * Play album / track taps, the actions sheet (N, A, I; P inert), the debounced search and its empty states, the play
 * count, the Music folders (Add, Remove) and owner decision D27 (the launch scan, left alone when nobody visited the
 * Library, stopped when the user leaves it; a folder opened from the Library is scanned).
 */
#include "check.hpp"

#include "library/lib_model.hpp"
#include "library/library.hpp"
#include "library/library_ctl.hpp"
#include "library/schema.hpp"

#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <thread>

using namespace vinyl;
using namespace vinyl::ui;

namespace {

bool have_sqlite() {
    std::string why;
    if (real_sqlite_lib(&why)) return true;
    SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
    return false;
}

struct Rec {
    std::vector<Page> shown;
    std::vector<std::vector<std::string>> plays, nexts, adds;
    std::vector<int> starts;
    std::vector<std::string> notices;
    int sheets = 0, sheets_hidden = 0, opened_folders = 0, chose = 0;
    std::vector<std::string> folders;
    std::vector<std::vector<std::string>> folder_calls;
    std::vector<RootSpec> wanted;
    std::string playing;
    std::vector<std::string> searched;
    int top = 0; /* where the list is scrolled to (px), as the view would tell */
    bool with_drives = false, udisks = true;
    std::vector<DriveInfo> drive_list;
    std::vector<std::string> ejected;
};

LibHost make_host(Rec &r) {
    LibHost h;
    h.playing_path = [&r] { return r.playing; };
    h.play = [&r](const std::vector<std::string> &p, int start) {
        r.plays.push_back(p);
        r.starts.push_back(start);
    };
    h.play_next = [&r](const std::vector<std::string> &p) { r.nexts.push_back(p); };
    h.add_to_queue = [&r](const std::vector<std::string> &p) { r.adds.push_back(p); };
    h.notice = [&r](Notice::Kind, const std::string &t) { r.notices.push_back(t); };
    h.show = [&r](Page p) { r.shown.push_back(p); };
    h.open_folders = [&r] { ++r.opened_folders; };
    h.choose_folder = [&r] { ++r.chose; };
    h.wanted = [&r] { return r.wanted; };
    h.folders = [&r] { return r.folders; };
    h.set_folders = [&r](const std::vector<std::string> &f) {
        r.folder_calls.push_back(f);
        r.folders = f;
    };
    h.mini = [] { return MiniView(); };
    h.scroll = [](Page, int) {};
    h.scroll_y = [&r] { return r.top; };
    if (r.with_drives) {
        h.drives = [&r] { return r.drive_list; };
        h.drives_available = [&r] { return r.udisks; };
        h.eject = [&r](const std::string &dev, const std::string &) { r.ejected.push_back(dev); };
    }
    h.search_open = [&r](const std::string &t, bool edit) { r.searched.push_back(t + (edit ? "+edit" : "")); };
    h.search_edit = [] {};
    h.search_end_edit = [] {};
    h.show_sheet = [&r](const SheetView &) { ++r.sheets; };
    h.hide_sheet = [&r] { ++r.sheets_hidden; };
    h.sheet_select = [](int) {};
    return h;
}

uint64_t g_now = 1000;

void adv(LibraryCtl &c, uint64_t ms) {
    g_now += ms;
    c.tick(g_now);
}

void pump(LibraryCtl &c, int rounds = 3) {
    for (int i = 0; i < rounds; ++i) {
        c.wait_idle();
        g_now += 30;
        c.tick(g_now);
    }
}

void make_library(const std::string &db_path, const std::string &root2 = "") {
    Db db;
    std::string why;
    CHECK(open_library_db(db, db_path, 416, &why));
    LibraryWriter w(db);
    const int64_t root = w.root("/music", "Music");
    Transaction tx(db);
    for (int a = 1; a <= 100; ++a) {
        char name[32];
        std::snprintf(name, sizeof name, "Artist %03d", a);
        for (int t = 1; t <= 2; ++t) {
            TrackTags tg;
            tg.title = std::string("Song ") + name + " " + std::to_string(t);
            tg.artist = name;
            tg.album = std::string("Album ") + name;
            tg.track = t;
            tg.date = "2001";
            tg.codec = "MP3";
            tg.duration_ms = 200000;
            tg.ok = true;
            TrackFile f;
            f.root_id = root;
            f.folder_id = w.folder(root, name, 1);
            f.name = std::to_string(t) + ".mp3";
            f.mtime = 1;
            f.size = 100;
            w.add_track(f, tg);
        }
    }
    if (!root2.empty()) {
        const int64_t r2 = w.root(root2, "Extra", "AAAA-0000", root2);
        TrackTags tg;
        tg.title = "Extra song";
        tg.artist = "Extra artist";
        tg.album = "Extra album";
        tg.codec = "MP3";
        tg.ok = true;
        TrackFile f;
        f.root_id = r2;
        f.folder_id = w.folder(r2, "x", 1);
        f.name = "1.mp3";
        f.mtime = 1;
        f.size = 1;
        w.add_track(f, tg);
    }
    CHECK(tx.commit());
}

int count_rows(LibraryCtl &c, libm::Row::Kind k) {
    int n = 0;
    for (const libm::Row &r : c.rows()) n += r.kind == k;
    return n;
}

/* a place is given back as it was left: the selected row and the first row in view, for Track info, an album, an artist and
 * search results (owner, B44: leaving Track info jumped to the top) */
void restore_test() {
    const std::string dir = make_temp_dir("libctl3");
    make_library(dir + "/library.db");
    Rec rec;
    LibConfig cfg;
    cfg.db_path = dir + "/library.db";
    cfg.ram_mb = 416;
    cfg.home = "/home/o";
    cfg.demo = true;
    LibraryCtl c(make_host(rec), cfg);
    c.enter();
    pump(c);
    /* Songs -> Track info -> Esc */
    c.row(Page::LibHub, 4);
    pump(c);
    CHECK_EQ(c.page(), Page::LibSongs);
    for (int i = 0; i < 12; ++i) c.key(18);
    CHECK_EQ(c.selected(), 12);
    rec.top = 7;
    c.hold(Page::LibSongs, 12);
    CHECK(c.letter(libm::kKeyI, 0));
    pump(c);
    CHECK_EQ(c.page(), Page::LibInfo);
    rec.top = 0;
    CHECK(c.back());
    CHECK_EQ(c.page(), Page::LibSongs);
    CHECK_EQ(c.view().scroll_to, -1); /* the rows are not in yet: nothing is scrolled, the saved place is kept */
    CHECK_EQ(c.selected(), -1);
    pump(c);
    CHECK_EQ(c.selected(), 12);
    CHECK_EQ(c.view().scroll_to, 7); /* the top row, once */
    CHECK_EQ(c.view().scroll_to, -1);
    CHECK_EQ(c.selected(), 12);
    CHECK(c.back());
    pump(c);
    CHECK_EQ(c.page(), Page::LibHub);
    /* Artists (on the second page) -> an artist -> an album -> Esc twice */
    c.row(Page::LibHub, 2);
    pump(c);
    for (int i = 0; i < 50; ++i) {
        c.key(18);
        if (i % 10 == 9) pump(c);
    }
    pump(c);
    const int artist_sel = c.selected();
    CHECK(artist_sel >= 40);
    rec.top = 44;
    c.row(Page::LibArtists, artist_sel);
    pump(c);
    CHECK_EQ(c.page(), Page::LibArtist);
    rec.top = 0;
    c.key(10);
    pump(c);
    CHECK_EQ(c.page(), Page::LibAlbum);
    CHECK(c.back());
    pump(c);
    CHECK_EQ(c.page(), Page::LibArtist);
    CHECK(c.back());
    pump(c);
    CHECK_EQ(c.page(), Page::LibArtists);
    CHECK(c.rows().size() > static_cast<size_t>(artist_sel)); /* the page the highlight was on is loaded again at once */
    CHECK_EQ(c.selected(), artist_sel);
    CHECK_EQ(c.view().scroll_to, 44);
    CHECK(c.back());
    pump(c);
    /* Search results -> an album -> Esc */
    c.chip(Page::LibHub);
    pump(c);
    adv(c, 10);
    c.query_changed("artist 05");
    adv(c, 400);
    pump(c);
    c.submit();
    int album_row = -1;
    for (size_t i = 0; i < c.rows().size(); ++i)
        if (c.rows()[i].type == libm::Row::Type::Album) {
            album_row = static_cast<int>(i);
            break;
        }
    CHECK(album_row > 2);
    rec.top = 2;
    c.row(Page::LibSearch, album_row);
    pump(c);
    CHECK_EQ(c.page(), Page::LibAlbum);
    rec.top = 0;
    CHECK(c.back());
    pump(c, 4);
    CHECK_EQ(c.page(), Page::LibSearch);
    CHECK_EQ(c.selected(), album_row);
    CHECK_EQ(c.view().scroll_to, 2);
    rm_tree(dir);
}

/* the Music folders page with drives (owner decision 2026-11-14: mount on request) and a root that is gone (nothing of it shown) */
void drives_ui_test() {
    const std::string dir = make_temp_dir("libctl4");
    make_library(dir + "/library.db", "/media/stick");
    Rec rec;
    rec.with_drives = true;
    DriveInfo v, n;
    v.dev = "/dev/sda1";
    v.label = "STICK";
    v.mounted = true;
    v.by_vinyl = true;
    v.mount_dir = "/media/stick";
    n.dev = "/dev/sdc1";
    n.label = "Generic Flash Disk";
    rec.drive_list = {v, n};
    LibConfig cfg;
    cfg.db_path = dir + "/library.db";
    cfg.ram_mb = 416;
    cfg.home = "/home/o";
    cfg.demo = true;
    LibraryCtl c(make_host(rec), cfg);
    c.enter_settings();
    pump(c);
    c.row(Page::LibSettings, 0);
    pump(c);
    CHECK_EQ(c.page(), Page::LibFolders);
    CHECK_EQ(c.rows().size(), static_cast<size_t>(3));
    CHECK_EQ(c.rows()[1].key, std::string("vdrive:/dev/sda1"));
    CHECK_EQ(c.rows()[1].value, std::string("mounted by Vinyl"));
    CHECK_EQ(c.rows()[2].key, std::string("ndrive:/dev/sdc1"));
    CHECK(c.rows()[2].dimmed);
    LibView lv = c.view();
    CHECK_EQ(lv.heading, std::string("MUSIC FOLDERS, 3"));
    CHECK_EQ(lv.buttons.size(), static_cast<size_t>(3));
    CHECK_EQ(lv.buttons[1], std::string("Eject")); /* Music is selected: nothing to eject */
    CHECK(!lv.button_enabled[1]);
    c.button(Page::LibFolders, 1);
    CHECK(rec.ejected.empty());
    c.key(18); /* the drive Vinyl mounted */
    lv = c.view();
    CHECK_EQ(lv.buttons[1], std::string("Eject STICK"));
    CHECK(lv.button_enabled[1]);
    c.button(Page::LibFolders, 1);
    CHECK_EQ(rec.ejected.size(), static_cast<size_t>(1));
    CHECK_EQ(rec.ejected[0], std::string("/dev/sda1"));
    c.key(18); /* a drive that is not mounted: Eject and Remove are inert / explain */
    lv = c.view();
    CHECK_EQ(lv.buttons[1], std::string("Eject Generic Flash Disk"));
    CHECK(!lv.button_enabled[1]);
    c.button(Page::LibFolders, 1);
    CHECK_EQ(rec.ejected.size(), static_cast<size_t>(1));
    c.button(Page::LibFolders, 2);
    CHECK_EQ(rec.notices.back(), std::string("Music and USB drives are always in the library. Only folders you added can be removed."));
    /* the system mounted it: Eject is dimmed */
    rec.drive_list[0].by_vinyl = false;
    c.mounts_changed();
    pump(c);
    c.key(17);
    CHECK_EQ(c.rows()[1].key, std::string("sdrive:/dev/sda1"));
    lv = c.view();
    CHECK_EQ(lv.buttons[1], std::string("Eject STICK"));
    CHECK(!lv.button_enabled[1]);
    c.button(Page::LibFolders, 1);
    CHECK_EQ(rec.ejected.size(), static_cast<size_t>(1));
    /* the drive is gone (pulled; or a stale mount, unreadable): nothing of it is shown, nowhere */
    rec.drive_list = {n};
    c.mounts_changed();
    pump(c, 4);
    CHECK_EQ(c.page(), Page::LibFolders);
    CHECK_EQ(c.rows().size(), static_cast<size_t>(2)); /* Music and the drive that is not mounted: no row for STICK */
    for (const libm::Row &r : c.rows()) CHECK(r.text.find("STICK") == std::string::npos && r.text.find("stick") == std::string::npos);
    CHECK_EQ(c.view().heading, std::string("MUSIC FOLDERS, 2"));
    c.enter();
    pump(c);
    LibView hv = c.view();
    CHECK(hv.note.kind == LibNote::Kind::None); /* no "is not connected" note */
    CHECK_EQ(hv.heading, std::string("LIBRARY, 200 TRACKS")); /* its track is not counted */
    c.row(Page::LibHub, 4); /* Songs: none of its tracks */
    pump(c);
    for (const libm::Row &r : c.rows()) CHECK(r.text != "Extra song");
    CHECK_EQ(c.view().heading, std::string("SONGS, 200"));
    /* back: the rows are there again, nothing was deleted (no rescan) */
    rec.drive_list = {v, n};
    {
        Db db;
        std::string why;
        CHECK(open_library_db(db, dir + "/library.db", 416, &why));
        Library lib(db);
        std::vector<RootInfo> ri;
        CHECK(lib.roots_info(ri));
        CHECK_EQ(ri.size(), static_cast<size_t>(2));
        CHECK(!ri[1].online);
        CHECK_EQ(ri[1].tracks, 1); /* kept, hidden */
    }
    c.back();
    c.enter_settings();
    c.row(Page::LibSettings, 0);
    c.mounts_changed();
    pump(c, 4);
    CHECK_EQ(c.rows().size(), static_cast<size_t>(3));
    CHECK_EQ(c.rows()[1].key, std::string("vdrive:/dev/sda1")); /* by_vinyl is back in the list: the drive is there again */
    /* no udisks: no Eject */
    rec.udisks = false;
    CHECK_EQ(c.view().buttons.size(), static_cast<size_t>(2));
    rm_tree(dir);
}

}  // namespace

void test_lib_ctl() {
    drives_ui_test();
    restore_test();
    if (!have_sqlite()) return;
    const std::string dir = make_temp_dir("libctl");
    const std::string dbp = dir + "/library.db";
    make_library(dbp, "/music2");
    Rec rec;
    LibConfig cfg;
    cfg.db_path = dbp;
    cfg.ram_mb = 416;
    cfg.home = "/home/o";
    cfg.demo = true; /* no mounts, as the scenes */
    rec.wanted = {RootSpec{dir + "/Music", "Music"}};
    {
        LibraryCtl c(make_host(rec), cfg);
        CHECK(c.available());

        /* the hub: Folders, Playlists, Artists ... with the counts, the heading */
        c.enter();
        pump(c);
        CHECK(c.in_library());
        CHECK(!rec.shown.empty() && rec.shown.back() == Page::LibHub);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(6));
        CHECK_EQ(c.rows()[2].value, std::string("101")); /* Artists: Extra artist is the 101st */
        LibView hv = c.view();
        CHECK_EQ(hv.heading, std::string("LIBRARY, 201 TRACKS"));
        CHECK_EQ(hv.chip, std::string("Search"));
        CHECK(!hv.empty);
        CHECK(!c.back()); /* at the hub: the host decides */

        /* Folders: the host opens the Browser; Playlists: a notice, nothing opens */
        c.row(Page::LibHub, 0);
        CHECK_EQ(rec.opened_folders, 1);
        c.row(Page::LibHub, 1);
        CHECK_EQ(rec.notices.back(), std::string("Playlists arrive in a later version."));
        CHECK_EQ(c.page(), Page::LibHub);

        /* Artists: 40 rows, the next page when the highlight nears the end (once), 101 in the end */
        c.row(Page::LibHub, 2);
        pump(c);
        CHECK_EQ(c.page(), Page::LibArtists);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(40));
        CHECK_EQ(c.view().heading, std::string("ARTISTS, 101"));
        for (int i = 0; i < 31; ++i) c.key(18); /* Down */
        pump(c);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(40)); /* row 31: not yet near the end */
        c.key(18);
        c.key(18);
        pump(c);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(80));
        for (int i = 0; i < 80; ++i) c.key(18);
        pump(c, 4);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(101)); /* the last page has 21 rows */
        CHECK_EQ(c.rows()[100].text, std::string("Extra artist"));
        c.key(18);
        pump(c);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(101)); /* no page asked for beyond the end */

        /* an artist, its album, the tracks */
        c.row(Page::LibArtists, 4); /* Artist 005 */
        pump(c);
        CHECK_EQ(c.page(), Page::LibArtist);
        CHECK_EQ(c.view().heading, std::string("ARTIST 005, 1 ALBUM, 2 TRACKS"));
        CHECK_EQ(c.view().title, std::string("Artist 005"));
        c.key(10); /* Enter on the first album */
        pump(c);
        CHECK_EQ(c.page(), Page::LibAlbum);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(2));
        CHECK_EQ(c.view().chip, std::string("Play album"));
        c.chip(Page::LibAlbum);
        CHECK_EQ(rec.plays.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.plays[0].size(), static_cast<size_t>(2));
        CHECK_EQ(rec.plays[0][0], std::string("/music/Artist 005/1.mp3"));
        CHECK_EQ(rec.starts[0], 0);
        c.row(Page::LibAlbum, 1); /* the second track: the album from there */
        CHECK_EQ(rec.plays.size(), static_cast<size_t>(2));
        CHECK_EQ(rec.starts[1], 1);

        /* the sheet of a track: a hold opens it; N and A act, P is drawn and does nothing, I opens Track info */
        const int sheets0 = rec.sheets;
        c.hold(Page::LibAlbum, 0);
        CHECK(c.sheet_open());
        CHECK_EQ(rec.sheets, sheets0 + 1);
        CHECK_EQ(c.sheet_view().rows.size(), static_cast<size_t>(5));
        CHECK(!c.sheet_view().rows[3].enabled);
        CHECK(c.letter(libm::kKeyN, 0));
        CHECK(!c.sheet_open());
        CHECK_EQ(rec.nexts.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.nexts[0][0], std::string("/music/Artist 005/1.mp3"));
        CHECK_EQ(rec.notices.back(), std::string("Will play next."));
        c.hold(Page::LibAlbum, 1);
        CHECK(c.letter(libm::kKeyA, 0));
        CHECK_EQ(rec.adds.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.adds[0][0], std::string("/music/Artist 005/2.mp3"));
        c.hold(Page::LibAlbum, 0);
        CHECK(c.letter(libm::kKeyP, 0)); /* consumed, nothing happens (P2c) */
        CHECK(c.sheet_open());
        CHECK_EQ(rec.nexts.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.adds.size(), static_cast<size_t>(1));
        CHECK(!c.letter(16, 0)); /* Q: not the sheet's, the host's */
        c.key(18); /* Down: the next row that can act (Play next) */
        c.key(18);
        c.key(18); /* Add to queue, then past the inert row to Track info */
        c.key(10);
        pump(c);
        CHECK(!c.sheet_open());
        CHECK_EQ(c.page(), Page::LibInfo);
        CHECK_EQ(c.view().heading, std::string("TRACK INFO"));
        CHECK_EQ(c.view().info.size(), static_cast<size_t>(10));
        CHECK_EQ(c.view().info[0].second, std::string("Song Artist 005 1"));
        CHECK_EQ(c.view().info[9].second, std::string("/music/Artist 005/1.mp3"));
        CHECK_EQ(c.view().info[8].second, std::string("Never"));

        /* Esc steps back: Track info, album, artist, Artists, the hub; a sheet takes the first Esc */
        CHECK(c.back());
        pump(c); /* the album's rows come again from the worker */
        CHECK_EQ(c.page(), Page::LibAlbum);
        c.hold(Page::LibAlbum, 0);
        CHECK(c.back());
        CHECK(!c.sheet_open());
        CHECK_EQ(c.page(), Page::LibAlbum);
        CHECK(c.back());
        CHECK(c.back());
        CHECK_EQ(c.page(), Page::LibArtists);
        CHECK(c.back());
        CHECK_EQ(c.page(), Page::LibHub);
        CHECK(!c.back());

        /* the play count: the track played to its end */
        c.note_played("/music/Artist 003/2.mp3");
        pump(c);
        {
            Db db;
            std::string why;
            CHECK(open_library_db(db, dbp, 416, &why));
            Library lib(db);
            const int64_t id = lib.find_track("/music/Artist 003/2.mp3");
            CHECK(id > 0);
            TrackDetails d;
            CHECK(lib.track(id, d));
            CHECK_EQ(d.plays, 1);
            CHECK(d.last_played > 0);
        }

        /* Search: 250 ms after the last key; empty query clears; nothing found says so */
        c.chip(Page::LibHub);
        pump(c);
        CHECK_EQ(c.page(), Page::LibSearch);
        CHECK(!rec.searched.empty() && rec.searched.back() == "+edit");
        CHECK(c.view().empty);
        CHECK_EQ(c.view().empty_title, std::string("Search your music"));
        adv(c, 10);
        c.query_changed("artist 05");
        adv(c, 100);
        c.wait_idle();
        adv(c, 50);
        CHECK(c.rows().empty()); /* not asked yet */
        adv(c, 300);
        pump(c);
        CHECK(!c.rows().empty());
        CHECK_EQ(c.rows()[0].text, std::string("SONGS"));
        CHECK(c.rows()[0].kind == libm::Row::Kind::Heading);
        CHECK(count_rows(c, libm::Row::Kind::Item) >= 3);
        CHECK(!c.view().empty);
        c.query_changed("zzzzzz");
        adv(c, 100);
        adv(c, 300);
        pump(c);
        CHECK(c.view().empty);
        CHECK_EQ(c.view().empty_title, std::string("Nothing found for \"zzzzzz\""));
        c.query_changed("");
        CHECK(c.rows().empty());
        CHECK_EQ(c.view().empty_title, std::string("Search your music"));
        c.query_changed("artist 050");
        adv(c, 100);
        adv(c, 300);
        pump(c);
        c.submit(); /* Enter: the keys go to the results */
        CHECK_EQ(c.selected(), 1);
        c.key(10); /* Enter on the first song: play the song list from there */
        CHECK(rec.plays.back()[static_cast<size_t>(rec.starts.back())].find("/music/Artist 050/") == 0);
        CHECK(c.back());
        CHECK_EQ(c.page(), Page::LibHub);

        /* "/" and Ctrl+F open Search; Ctrl+O is the sheet on a row that has one and the Menu's otherwise */
        CHECK(c.letter(53, 0));
        CHECK_EQ(c.page(), Page::LibSearch);
        CHECK(c.back());
        CHECK(!c.letter(24, 1u << 1)); /* Ctrl+O on a hub row: the host's Menu */

        /* the Library settings, Music folders, Add and Remove */
        c.enter_settings();
        pump(c);
        CHECK_EQ(c.page(), Page::LibSettings);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(3));
        CHECK_EQ(c.rows()[0].text, std::string("Music folders"));
        CHECK_EQ(c.rows()[0].value, std::string("2"));
        c.row(Page::LibSettings, 0);
        pump(c);
        CHECK_EQ(c.page(), Page::LibFolders);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(2));
        CHECK_EQ(c.rows()[1].text, std::string("/music2"));
        LibView fv = c.view();
        CHECK_EQ(fv.buttons.size(), static_cast<size_t>(2));
        CHECK_EQ(fv.heading, std::string("MUSIC FOLDERS, 2"));
        c.button(Page::LibFolders, 0);
        CHECK_EQ(rec.chose, 1);
        c.button(Page::LibFolders, 1); /* Music is selected: it cannot be removed */
        CHECK_EQ(rec.notices.back(), std::string("Music and USB drives are always in the library. Only folders you added can be removed."));
        CHECK(rec.folder_calls.empty());
        c.key(18);
        rec.folders = {"/music2"};
        c.button(Page::LibFolders, 1); /* /music2 is one of the user's: it goes, its tracks with it */
        pump(c, 4);
        CHECK_EQ(rec.folder_calls.size(), static_cast<size_t>(1));
        CHECK(rec.folder_calls[0].empty());
        CHECK_EQ(c.rows().size(), static_cast<size_t>(1));
        {
            Db db;
            std::string why;
            CHECK(open_library_db(db, dbp, 416, &why));
            Library lib(db);
            CHECK_EQ(lib.counts().tracks, 200);
        }
        /* Esc from the settings (opened from the Menu over the hub) leaves them and the hub is the Library's place again */
        CHECK(c.back()); /* Music folders -> the settings */
        CHECK(!c.back()); /* the settings were the first place: the host goes back to the Menu */
        CHECK_EQ(c.page(), Page::LibHub);
        /* Add: a folder inside a root is in the library already; a new one joins and a scan starts */
        c.add_folder(dir + "/Music/sub");
        CHECK_EQ(rec.notices.back(), std::string("That folder is in the library already."));
        CHECK(rec.folder_calls.size() == 1);
        c.add_folder(dir + "/Elsewhere");
        CHECK_EQ(rec.notices.back(), std::string("Folder added. Scanning."));
        CHECK_EQ(rec.folder_calls.size(), static_cast<size_t>(2));
        CHECK_EQ(rec.folder_calls[1][0], dir + "/Elsewhere");
        if (c.scan_thread()) c.scan_thread()->wait();
    }

    /* D27: the scan at launch; left alone when nobody went into the Library; stopped when the user leaves it; a folder
     * opened from the Library is scanned */
    const std::string dir2 = make_temp_dir("libctl2");
    const std::string music = dir2 + "/Music";
    ::mkdir(music.c_str(), 0755);
    for (int i = 0; i < 150; ++i) {
        char n[32];
        std::snprintf(n, sizeof n, "/f%03d.mp3", i);
        write_file(music + n, "x");
    }
    Rec r2;
    r2.wanted = {RootSpec{music, "Music"}};
    std::atomic<int> reads{0};
    LibConfig c2;
    c2.ram_mb = 416;
    c2.demo = true;
    c2.home = "/home/o";
    c2.scan_hooks.read_tags = [&reads](const std::string &path, TrackTags &t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        ++reads;
        t = TrackTags();
        t.title = path.substr(path.rfind('/') + 1);
        t.artist = "A";
        t.album = "B";
        t.codec = "MP3";
        t.ok = true;
        return true;
    };
    auto settle = [](LibraryCtl &c) {
        for (int i = 0; i < 200 && c.scan_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (c.scan_thread()) c.scan_thread()->wait();
    };
    {
        /* nobody went into the Library: the launch scan runs to its end, leaving changes nothing */
        c2.db_path = dir2 + "/lib1.db";
        reads = 0;
        LibraryCtl c(make_host(r2), c2);
        c.start_launch_scan();
        pump(c);
        CHECK(c.scan_running());
        c.left();
        settle(c);
        CHECK_EQ(reads.load(), 150);
        CHECK(c.scan_thread()->progress().state == ScanProgress::State::Done);
    }
    {
        /* the user went in and left: the scan stops (it resumes at the next start) */
        c2.db_path = dir2 + "/lib2.db";
        reads = 0;
        LibraryCtl c(make_host(r2), c2);
        c.start_launch_scan();
        pump(c);
        CHECK(c.scan_running());
        c.enter();
        c.left();
        settle(c);
        CHECK(reads.load() < 150);
        CHECK(c.scan_thread()->progress().state == ScanProgress::State::Cancelled);
        CHECK(!c.in_library());
    }
    {
        /* a folder opened from the Library is scanned; outside the Library it is not */
        c2.db_path = dir2 + "/lib3.db";
        reads = 0;
        LibraryCtl c(make_host(r2), c2);
        c.start_launch_scan(); /* the roots and the first 150 files */
        pump(c);
        settle(c);
        CHECK_EQ(reads.load(), 150);
        for (int i = 150; i < 155; ++i) {
            char n[32];
            std::snprintf(n, sizeof n, "/f%03d.mp3", i);
            write_file(music + n, "x");
        }
        c.browser_opened(music); /* not in the Library: nothing */
        CHECK(!c.scan_running());
        c.enter();
        pump(c);
        c.browser_opened(music);
        CHECK(c.scan_running());
        settle(c);
        CHECK_EQ(reads.load(), 155); /* the five new files */
        c.left();
    }
    rm_tree(dir);
    rm_tree(dir2);
}
