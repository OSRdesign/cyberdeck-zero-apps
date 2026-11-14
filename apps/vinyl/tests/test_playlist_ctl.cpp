/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the playlists in the Library controller (P2c, report 060 2.4): the Playlists page, New playlist, Add to playlist from
 * the actions sheets, the edit page (Move up / down, Remove, Add tracks in pick mode, Rename, Export M3U, Delete after a
 * question), the queue saved as a playlist, a .m3u imported from the Folders browser, missing entries (dimmed, kept, skipped),
 * and the index rebuilt from the files. A temporary library of real (empty) files and a recording host.
 */
#include "check.hpp"

#include "library/lib_model.hpp"
#include "library/library.hpp"
#include "library/library_ctl.hpp"
#include "library/schema.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

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
    std::vector<std::vector<std::string>> plays, adds;
    std::vector<int> starts;
    std::vector<std::string> sources;
    std::vector<std::pair<Notice::Kind, std::string>> notices;
    std::vector<SheetView> sheets;
    std::vector<NameAsk> names;
    std::vector<std::string> confirms, exports;
    std::vector<Page> shown;
    std::vector<RootSpec> wanted;
    std::string playing;
    int sheets_hidden = 0;
    std::string last_notice() const { return notices.empty() ? std::string() : notices.back().second; }
    bool last_danger() const { return !notices.empty() && notices.back().first == Notice::Kind::Danger; }
};

LibHost make_host(Rec &r) {
    LibHost h;
    h.playing_path = [&r] { return r.playing; };
    h.play = [&r](const std::vector<std::string> &p, int start) {
        r.plays.push_back(p);
        r.starts.push_back(start);
        r.sources.push_back("");
    };
    h.play_source = [&r](const std::vector<std::string> &p, int start, const std::string &src) {
        r.plays.push_back(p);
        r.starts.push_back(start);
        r.sources.push_back(src);
    };
    h.play_next = [](const std::vector<std::string> &) {};
    h.add_to_queue = [&r](const std::vector<std::string> &p) { r.adds.push_back(p); };
    h.notice = [&r](Notice::Kind k, const std::string &t) { r.notices.emplace_back(k, t); };
    h.show = [&r](Page p) { r.shown.push_back(p); };
    h.open_folders = [] {};
    h.choose_folder = [] {};
    h.choose_export = [&r](const std::string &n) { r.exports.push_back(n); };
    h.ask_name = [&r](const NameAsk &a) { r.names.push_back(a); };
    h.ask_confirm = [&r](const std::string &t, const std::string &b, const std::string &ok) { r.confirms.push_back(t + "|" + b + "|" + ok); };
    h.wanted = [&r] { return r.wanted; };
    h.folders = [] { return std::vector<std::string>(); };
    h.set_folders = [](const std::vector<std::string> &) {};
    h.mini = [] { return MiniView(); };
    h.scroll = [](Page, int) {};
    h.scroll_y = [] { return 0; };
    h.search_open = [](const std::string &, bool) {};
    h.search_edit = [] {};
    h.search_end_edit = [] {};
    h.show_sheet = [&r](const SheetView &v) { r.sheets.push_back(v); };
    h.hide_sheet = [&r] { ++r.sheets_hidden; };
    h.sheet_select = [](int) {};
    return h;
}

uint64_t g_now = 5000;
void pump(LibraryCtl &c, int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
        c.wait_idle();
        g_now += 30;
        c.tick(g_now);
    }
}

bool exists_file(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

/* Music/Ann/Alpha: Aone, Atwo, Athree (tracks 1-3, A-Z order differs); Music/Bob/Beta: Bone, Btwo; real files */
void make_library(const std::string &music, const std::string &db_path) {
    Db db;
    std::string why;
    CHECK(open_library_db(db, db_path, 416, &why));
    LibraryWriter w(db);
    const int64_t root = w.root(music, "Music");
    Transaction tx(db);
    struct T {
        const char *artist, *album, *title, *file;
        int track;
    };
    const T tr[] = {{"Ann", "Alpha", "Aone", "1.mp3", 1},   {"Ann", "Alpha", "Atwo", "2.mp3", 2}, {"Ann", "Alpha", "Athree", "3.mp3", 3},
                    {"Bob", "Beta", "Bone", "1.mp3", 1},    {"Bob", "Beta", "Btwo", "2.mp3", 2}};
    for (const T &t : tr) {
        const std::string folder = std::string(t.artist) + " - " + t.album;
        ::mkdir((music + "/" + folder).c_str(), 0755);
        write_file(music + "/" + folder + "/" + t.file, "x");
        TrackTags tg;
        tg.title = t.title;
        tg.artist = t.artist;
        tg.album = t.album;
        tg.track = t.track;
        tg.codec = "MP3";
        tg.duration_ms = 61000;
        tg.ok = true;
        TrackFile f;
        f.root_id = root;
        f.folder_id = w.folder(root, folder, 1);
        f.name = t.file;
        f.mtime = 1;
        f.size = 1;
        w.add_track(f, tg);
    }
    CHECK(tx.commit());
}

}  // namespace

void test_playlist_ctl() {
    if (!have_sqlite()) return;
    const std::string dir = make_temp_dir("plctl");
    const std::string music = dir + "/Music";
    ::mkdir(music.c_str(), 0755);
    const std::string dbp = dir + "/library.db";
    make_library(music, dbp);
    const std::string pldir = music + "/Playlists";
    Rec rec;
    rec.wanted = {RootSpec{music, "Music"}};
    LibConfig cfg;
    cfg.db_path = dbp;
    cfg.ram_mb = 416;
    cfg.home = dir;
    cfg.demo = true;
    {
        LibraryCtl c(make_host(rec), cfg);
        CHECK(c.available());
        CHECK_EQ(c.playlists_dir(), pldir);
        c.enter();
        pump(c);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(6));

        /* the hub's Playlists row opens the page: only "+ New playlist", the message under it */
        c.row(Page::LibHub, 1);
        pump(c);
        CHECK_EQ(c.page(), Page::LibPlaylists);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(1));
        CHECK_EQ(c.rows()[0].text, std::string("+ New playlist"));
        CHECK(c.rows()[0].mark == libm::Row::Mark::Dot);
        LibView v = c.view();
        CHECK_EQ(v.heading, std::string("PLAYLISTS, 0"));
        CHECK(v.empty);
        CHECK_EQ(v.empty_after_rows, 1);
        CHECK_EQ(v.empty_title, std::string("No playlists yet"));
        CHECK(v.empty_sub.find("Music/Playlists") != std::string::npos);

        /* New playlist: the name dialog, a bad name and a good one */
        c.row(Page::LibPlaylists, 0);
        CHECK_EQ(rec.names.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.names.back().title, std::string("New playlist"));
        CHECK_EQ(rec.names.back().text, std::string("Playlist"));
        CHECK_EQ(rec.names.back().ok, std::string("Create"));
        c.name_answer(true, "no/pe");
        pump(c);
        CHECK(rec.last_danger());
        CHECK_EQ(rec.last_notice(), std::string("Letters, digits, space, - and _ only."));
        CHECK_EQ(rec.names.size(), static_cast<size_t>(2)); /* asked again, with what was typed */
        CHECK_EQ(rec.names.back().text, std::string("no/pe"));
        c.name_answer(true, "   ");
        CHECK_EQ(rec.last_notice(), std::string("Type a name first."));
        c.name_answer(true, std::string(30, 'x'));
        CHECK_EQ(rec.last_notice(), std::string("Use 24 characters or fewer."));
        c.name_answer(true, "Evening");
        pump(c, 5);
        CHECK(exists_file(pldir + "/Evening.m3u8"));
        CHECK_EQ(c.rows().size(), static_cast<size_t>(2));
        CHECK_EQ(c.rows()[1].text, std::string("Evening"));
        CHECK_EQ(c.rows()[1].value, std::string("0 tracks"));
        CHECK(c.rows()[1].mark == libm::Row::Mark::Chevron);
        /* a taken name (without case): the notice, the dialog again */
        const size_t asks = rec.names.size();
        c.row(Page::LibPlaylists, 0);
        c.name_answer(true, "evening");
        pump(c);
        CHECK(rec.last_danger());
        CHECK_EQ(rec.last_notice(), std::string("That name is taken. Pick another."));
        CHECK_EQ(rec.names.size(), asks + 2);
        c.name_answer(false, "");
        c.name_answer(true, "Ignored"); /* no dialog is open: nothing happens */
        pump(c);
        CHECK(!exists_file(pldir + "/Ignored.m3u8"));
        CHECK_EQ(rec.names.back().text, std::string("evening"));

        /* Add to playlist from a song's sheet: Songs is A-Z: Aone Athree Atwo Bone Btwo */
        c.back();
        pump(c);
        c.row(Page::LibHub, 4); /* Songs */
        pump(c);
        CHECK_EQ(c.page(), Page::LibSongs);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(5));
        c.hold(Page::LibSongs, 0); /* Aone */
        CHECK(c.sheet_open());
        CHECK(rec.sheets.back().rows[3].enabled); /* Add to playlist is real now */
        CHECK(c.letter(libm::kKeyP, 0));
        pump(c);
        CHECK(c.sheet_open());
        {
            const SheetView &sv = rec.sheets.back();
            CHECK_EQ(sv.title, std::string("ADD AONE TO"));
            CHECK_EQ(sv.rows.size(), static_cast<size_t>(2));
            CHECK_EQ(sv.rows[0].label, std::string("+ New playlist"));
            CHECK_EQ(sv.rows[0].key, std::string("N"));
            CHECK_EQ(sv.rows[1].label, std::string("Evening"));
            CHECK_EQ(sv.rows[1].key, std::string("0 tracks"));
            CHECK(sv.hint.find("added again") != std::string::npos);
        }
        c.sheet_pick(1);
        pump(c, 5);
        CHECK_EQ(rec.last_notice(), std::string("Added to Evening."));
        CHECK(!rec.last_danger());
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            CHECK(st.read("Evening", e) == PlResult::Ok);
            CHECK_EQ(e.size(), static_cast<size_t>(1));
            if (!e.empty()) {
                CHECK_EQ(e[0].path, music + "/Ann - Alpha/1.mp3");
                CHECK_EQ(e[0].text, std::string("Ann - Aone"));
                CHECK_EQ(e[0].seconds, 61);
            }
        }
        /* the same song again: duplicates are allowed */
        c.hold(Page::LibSongs, 0);
        c.letter(libm::kKeyP, 0);
        pump(c);
        c.sheet_pick(1);
        pump(c, 5);
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            st.read("Evening", e);
            CHECK_EQ(e.size(), static_cast<size_t>(2));
        }
        /* "+ New playlist" in the picker (the N key): the name dialog, then the song is in it */
        c.hold(Page::LibSongs, 3); /* Bone */
        c.letter(libm::kKeyP, 0);
        pump(c);
        CHECK(c.letter(libm::kKeyN, 0)); /* N: New playlist */
        CHECK(!c.sheet_open());
        CHECK_EQ(rec.names.back().title, std::string("New playlist"));
        CHECK_EQ(rec.names.back().text, std::string("Playlist"));
        CHECK_EQ(rec.names.back().sub, std::string("1 track. Name:"));
        c.name_answer(true, "Road trip");
        pump(c, 5);
        CHECK_EQ(rec.last_notice(), std::string("Added to Road trip."));
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            CHECK(st.read("Road trip", e) == PlResult::Ok);
            CHECK_EQ(e.size(), static_cast<size_t>(1));
        }
        /* an album adds all its tracks in album order (not A-Z): Aone, Atwo, Athree */
        c.back();
        pump(c);
        c.row(Page::LibHub, 3); /* Albums */
        pump(c);
        CHECK_EQ(c.page(), Page::LibAlbums);
        c.hold(Page::LibAlbums, 0); /* Alpha */
        CHECK(c.sheet_open());
        CHECK(rec.sheets.back().rows[3].enabled);
        c.letter(libm::kKeyP, 0);
        pump(c);
        CHECK_EQ(rec.sheets.back().title, std::string("ADD ALPHA TO"));
        CHECK_EQ(rec.sheets.back().rows.size(), static_cast<size_t>(3)); /* New, Evening, Road trip */
        c.sheet_pick(2); /* Road trip: A-Z after Evening */
        pump(c, 5);
        CHECK_EQ(rec.last_notice(), std::string("Added to Road trip."));
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            st.read("Road trip", e);
            CHECK_EQ(e.size(), static_cast<size_t>(4));
            if (e.size() == 4) {
                CHECK_EQ(e[1].text, std::string("Ann - Aone"));
                CHECK_EQ(e[2].text, std::string("Ann - Atwo"));
                CHECK_EQ(e[3].text, std::string("Ann - Athree"));
            }
        }
        /* an artist: all the albums' tracks */
        c.back();
        pump(c);
        c.row(Page::LibHub, 2); /* Artists */
        pump(c);
        c.hold(Page::LibArtists, 1); /* Bob */
        CHECK(c.sheet_open());
        c.letter(libm::kKeyP, 0);
        pump(c);
        CHECK_EQ(rec.sheets.back().title, std::string("ADD BOB TO"));
        c.sheet_pick(1);
        pump(c, 5);
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            st.read("Evening", e);
            CHECK_EQ(e.size(), static_cast<size_t>(4)); /* Aone, Aone, Bone, Btwo */
        }
        c.back();
        pump(c);
        CHECK(!c.sheet_open());

        /* the list: names with counts, A-Z */
        c.row(Page::LibHub, 1);
        pump(c);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(3));
        CHECK_EQ(c.rows()[1].text, std::string("Evening"));
        CHECK_EQ(c.rows()[1].value, std::string("4 tracks"));
        CHECK_EQ(c.rows()[2].text, std::string("Road trip"));
        CHECK_EQ(c.rows()[2].value, std::string("4 tracks"));
        CHECK_EQ(c.view().heading, std::string("PLAYLISTS, 2"));
        CHECK(!c.view().empty);

        /* the index is only a cache: a playlist added outside Vinyl shows at the next open */
        {
            PlaylistStore st(pldir, {music});
            CHECK(st.create("Dropped in", {}) == PlResult::Ok);
            m3u::Entry e;
            e.path = music + "/Bob - Beta/1.mp3";
            CHECK(st.append("Dropped in", {e}) == PlResult::Ok);
        }
        c.back();
        pump(c);
        c.row(Page::LibHub, 1);
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(4));
        CHECK_EQ(c.rows()[1].text, std::string("Dropped in"));
        CHECK_EQ(c.rows()[1].value, std::string("1 track"));

        /* ---- the edit page: open Evening (Aone, Aone, Bone, Btwo) */
        c.row(Page::LibPlaylists, 2);
        pump(c);
        CHECK_EQ(c.page(), Page::LibPlaylist);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(4));
        CHECK_EQ(c.rows()[0].text, std::string("Ann, Aone"));
        CHECK_EQ(c.rows()[0].value, std::string("01:01"));
        CHECK(c.rows()[0].mark == libm::Row::Mark::Dot);
        CHECK(!c.rows()[0].dimmed);
        v = c.view();
        CHECK_EQ(v.title, std::string("Evening"));
        CHECK_EQ(v.heading, std::string("4 TRACKS, 4 MIN"));
        CHECK_EQ(v.chips.size(), static_cast<size_t>(2));
        CHECK_EQ(v.chips[0], std::string("Rename"));
        CHECK_EQ(v.chips[1], std::string("Export M3U"));
        CHECK_EQ(v.buttons.size(), static_cast<size_t>(4));
        CHECK_EQ(v.buttons[3], std::string("Add tracks"));
        CHECK(!v.button_enabled[0]); /* the first row: no Move up */
        CHECK(v.button_enabled[1] && v.button_enabled[2] && v.button_enabled[3]);
        c.key(18); /* Down: row 1 */
        c.key(18);
        pump(c);
        CHECK_EQ(c.selected(), 2);
        v = c.view();
        CHECK(v.button_enabled[0] && v.button_enabled[1]);
        /* Move up: Bone goes above the second Aone, the selection follows it, the order holds on disk */
        c.button(Page::LibPlaylist, 0);
        pump(c, 5);
        CHECK_EQ(c.selected(), 1);
        CHECK_EQ(c.rows()[1].text, std::string("Bob, Bone"));
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            st.read("Evening", e);
            CHECK_EQ(e.size(), static_cast<size_t>(4));
            CHECK_EQ(e[1].text, std::string("Bob - Bone"));
        }
        /* Move down twice with U and D keys; the last row has no Move down */
        CHECK(c.letter(32, 0)); /* D */
        pump(c, 5);
        CHECK_EQ(c.selected(), 2);
        c.letter(32, 0);
        pump(c, 5);
        CHECK_EQ(c.selected(), 3);
        CHECK(!c.view().button_enabled[1]);
        c.button(Page::LibPlaylist, 1); /* does nothing at the end */
        pump(c);
        CHECK_EQ(c.selected(), 3);
        CHECK(c.letter(22, 0)); /* U */
        pump(c, 5);
        CHECK_EQ(c.selected(), 2);
        /* Remove: the row goes, the selection stays at that place; with the Delete key too */
        c.button(Page::LibPlaylist, 2);
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(3));
        CHECK_EQ(c.selected(), 2);
        CHECK(c.key(127)); /* Delete */
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(2));
        CHECK_EQ(c.selected(), 1);
        CHECK(c.letter(45, 0)); /* X */
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(1));
        CHECK_EQ(c.selected(), 0);
        CHECK(!c.view().button_enabled[0] && !c.view().button_enabled[1]);
        c.button(Page::LibPlaylist, 2);
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(0));
        v = c.view();
        CHECK(v.empty); /* no tracks yet */
        CHECK_EQ(v.heading, std::string("0 TRACKS"));
        CHECK(!v.button_enabled[2]);
        c.button(Page::LibPlaylist, 2); /* nothing to remove */
        pump(c);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(0));

        /* ---- Add tracks: the Library in pick mode */
        c.button(Page::LibPlaylist, 3);
        pump(c);
        CHECK(c.pick_mode());
        CHECK_EQ(c.page(), Page::LibHub);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(4)); /* Artists, Albums, Songs, Genres */
        CHECK_EQ(c.rows()[0].key, std::string("artists"));
        CHECK_EQ(c.rows()[1].key, std::string("albums"));
        CHECK_EQ(c.rows()[2].key, std::string("songs"));
        CHECK_EQ(c.rows()[3].key, std::string("genres"));
        CHECK_EQ(c.view().heading, std::string("ADD TRACKS TO EVENING"));
        CHECK_EQ(c.view().chip, std::string("Search"));
        c.row(Page::LibHub, 2); /* Songs */
        pump(c);
        c.hold(Page::LibSongs, 0); /* no sheet in pick mode */
        CHECK(!c.sheet_open());
        c.row(Page::LibSongs, 4); /* Btwo: appended, not played */
        pump(c, 5);
        CHECK_EQ(rec.last_notice(), std::string("Added to Evening."));
        const size_t plays_before = rec.plays.size();
        c.row(Page::LibSongs, 0); /* Aone */
        pump(c, 5);
        CHECK_EQ(rec.plays.size(), plays_before); /* nothing plays while picking */
        c.back(); /* Esc: Songs -> the hub */
        pump(c);
        CHECK(c.pick_mode());
        c.row(Page::LibHub, 1); /* Albums */
        pump(c);
        c.row(Page::LibAlbums, 1); /* Beta */
        pump(c);
        CHECK_EQ(c.page(), Page::LibAlbum);
        CHECK_EQ(c.view().chip, std::string("Add album"));
        c.chip(Page::LibAlbum);
        pump(c, 5);
        CHECK_EQ(rec.plays.size(), plays_before);
        c.back();
        c.back();
        pump(c);
        CHECK(c.pick_mode());
        CHECK(c.back()); /* Esc at the hub: back on the playlist */
        pump(c, 5);
        CHECK(!c.pick_mode());
        CHECK_EQ(c.page(), Page::LibPlaylist);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(4)); /* Btwo, Aone, Bone, Btwo */
        CHECK_EQ(c.rows()[0].text, std::string("Bob, Btwo"));
        CHECK_EQ(c.rows()[1].text, std::string("Ann, Aone"));
        CHECK_EQ(c.rows()[3].text, std::string("Bob, Btwo"));

        /* ---- play: Enter on an entry plays the playlist from there, the queue's source is the playlist file */
        c.row(Page::LibPlaylist, 2);
        CHECK_EQ(rec.plays.size(), plays_before + 1);
        CHECK_EQ(rec.plays.back().size(), static_cast<size_t>(4));
        CHECK_EQ(rec.starts.back(), 2);
        CHECK_EQ(rec.sources.back(), pldir + "/Evening.m3u8");
        CHECK_EQ(rec.plays.back()[2], music + "/Bob - Beta/1.mp3");

        /* ---- missing entries: dimmed, kept, skipped, inert; Remove takes them out */
        {
            PlaylistStore st(pldir, {music});
            m3u::Entry gone;
            gone.path = music + "/Gone/old.mp3";
            gone.text = "Old Band - Old song";
            gone.seconds = 30;
            m3u::Entry win;
            win.path = "D:\\Music\\Away.mp3";
            win.resolved = false;
            CHECK(st.append("Evening", {gone, win}) == PlResult::Ok);
        }
        c.back(); /* the list, then the playlist again: it reads the file */
        pump(c);
        c.row(Page::LibPlaylists, 2);
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(6));
        CHECK(c.rows()[4].dimmed);
        CHECK_EQ(c.rows()[4].text, std::string("Old Band, Old song"));
        CHECK_EQ(c.rows()[4].value, std::string("file not found"));
        CHECK(!c.rows()[4].mono_value);
        CHECK(c.rows()[5].dimmed);
        CHECK_EQ(c.rows()[5].text, std::string("Away.mp3"));
        CHECK(!c.rows()[0].dimmed);
        CHECK_EQ(c.view().heading, std::string("6 TRACKS, 5 MIN. 2 NOT FOUND"));
        const size_t p2 = rec.plays.size();
        c.row(Page::LibPlaylist, 4); /* a dimmed row is inert */
        CHECK_EQ(rec.plays.size(), p2);
        c.row(Page::LibPlaylist, 3); /* play from the last good one: the missing are skipped */
        CHECK_EQ(rec.plays.size(), p2 + 1);
        CHECK_EQ(rec.plays.back().size(), static_cast<size_t>(4));
        CHECK_EQ(rec.starts.back(), 3);
        /* the missing entry can be selected and removed; the other missing one stays until it is removed too */
        c.key(18);
        c.key(18);
        pump(c);
        CHECK_EQ(c.selected(), 5);
        c.button(Page::LibPlaylist, 1); /* last row: no Move down */
        pump(c);
        c.button(Page::LibPlaylist, 2);
        pump(c, 5);
        CHECK_EQ(c.rows().size(), static_cast<size_t>(5));
        CHECK(c.rows()[4].dimmed);
        CHECK_EQ(c.rows()[4].text, std::string("Old Band, Old song"));
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            st.read("Evening", e);
            CHECK_EQ(e.size(), static_cast<size_t>(5)); /* a move or an add did not drop it */
        }

        /* ---- the playlist sheet of an entry (Ctrl+O) has Add tracks; the Play row plays all the files that are there */
        c.hold(Page::LibPlaylist, 0);
        CHECK(c.sheet_open());
        {
            const SheetView &sv = rec.sheets.back();
            CHECK_EQ(sv.title, std::string("EVENING, 5 TRACKS"));
            CHECK_EQ(sv.rows.size(), static_cast<size_t>(5));
            CHECK_EQ(sv.rows[0].label, std::string("Play"));
            CHECK_EQ(sv.rows[1].label, std::string("Add tracks"));
            CHECK_EQ(sv.rows[2].label, std::string("Rename"));
            CHECK_EQ(sv.rows[3].label, std::string("Export M3U"));
            CHECK_EQ(sv.rows[4].label, std::string("Delete"));
        }
        const size_t p3 = rec.plays.size();
        c.sheet_pick(0);
        pump(c, 5);
        CHECK_EQ(rec.plays.size(), p3 + 1);
        CHECK_EQ(rec.plays.back().size(), static_cast<size_t>(4));
        CHECK_EQ(rec.starts.back(), 0);

        /* ---- Rename: the dialog, a taken name keeps it open with the notice, then the new name sticks */
        c.button(Page::LibPlaylist, 10);
        CHECK_EQ(rec.names.back().title, std::string("Rename playlist"));
        CHECK_EQ(rec.names.back().text, std::string("Evening"));
        CHECK_EQ(rec.names.back().sub, std::string("5 tracks. New name:"));
        CHECK_EQ(rec.names.back().ok, std::string("Rename"));
        c.name_answer(true, "road TRIP");
        pump(c);
        CHECK_EQ(rec.last_notice(), std::string("That name is taken. Pick another."));
        CHECK(rec.last_danger());
        c.name_answer(true, "Evening run");
        pump(c, 5);
        CHECK(exists_file(pldir + "/Evening run.m3u8"));
        CHECK(!exists_file(pldir + "/Evening.m3u8"));
        CHECK_EQ(c.view().title, std::string("Evening run"));
        CHECK_EQ(c.rows().size(), static_cast<size_t>(5)); /* the page still shows it, under its new name */

        /* ---- Export M3U: the Browser in choose mode, then the file where the folder is */
        const std::string usb = dir + "/usb";
        ::mkdir(usb.c_str(), 0755);
        c.button(Page::LibPlaylist, 11);
        CHECK_EQ(rec.exports.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.exports.back(), std::string("Evening run"));
        CHECK(c.exporting());
        c.export_to(usb + "/missing-dir");
        pump(c);
        CHECK(rec.last_danger());
        CHECK(c.exporting()); /* still choosing */
        c.export_to(usb);
        pump(c, 5);
        CHECK(!c.exporting());
        CHECK(exists_file(usb + "/Evening run.m3u8"));
        CHECK_EQ(rec.last_notice(), std::string("Exported to usb: Evening run.m3u8."));
        CHECK_EQ(c.page(), Page::LibPlaylist);
        CHECK_EQ(libm::where_text("/media/olivier/SANDISK/Music/"), std::string("SANDISK"));
        CHECK_EQ(libm::where_text("/home/o/Music"), std::string("Music"));

        /* ---- Delete: the question first (Cancel keeps it), then the file goes and the page with it */
        CHECK(rec.confirms.empty());
        c.hold(Page::LibPlaylist, 0);
        c.letter(libm::kKeyD, 0);
        CHECK_EQ(rec.confirms.size(), static_cast<size_t>(1));
        CHECK_EQ(rec.confirms.back(), std::string("Delete \"Evening run\"?|5 tracks. The playlist file is deleted; the songs stay in your library.|Delete"));
        c.confirm_answer(false);
        pump(c);
        CHECK(exists_file(pldir + "/Evening run.m3u8"));
        CHECK_EQ(c.page(), Page::LibPlaylist);
        c.hold(Page::LibPlaylist, 0);
        c.letter(libm::kKeyD, 0);
        c.confirm_answer(true);
        pump(c, 6);
        CHECK(!exists_file(pldir + "/Evening run.m3u8"));
        CHECK(exists_file(music + "/Ann - Alpha/1.mp3")); /* the songs stay */
        CHECK_EQ(rec.last_notice(), std::string("Playlist deleted."));
        CHECK_EQ(c.page(), Page::LibPlaylists); /* back on the list */
        CHECK_EQ(c.rows().size(), static_cast<size_t>(3)); /* New, Dropped in, Road trip */
        CHECK(exists_file(usb + "/Evening run.m3u8")); /* the exported copy is the user's */

        /* a playlist row's sheet on the list: Play, Rename, Delete, Export; the letters */
        c.hold(Page::LibPlaylists, 0); /* "+ New playlist": no sheet */
        CHECK(!c.sheet_open());
        c.hold(Page::LibPlaylists, 2); /* Road trip */
        CHECK(c.sheet_open());
        {
            const SheetView &sv = rec.sheets.back();
            CHECK_EQ(sv.title, std::string("ROAD TRIP, 4 TRACKS"));
            CHECK_EQ(sv.rows.size(), static_cast<size_t>(4));
            CHECK_EQ(sv.rows[1].label, std::string("Rename"));
            CHECK_EQ(sv.rows[2].label, std::string("Delete"));
            CHECK_EQ(sv.rows[3].label, std::string("Export M3U"));
        }
        const size_t p4 = rec.plays.size();
        c.sheet_pick(0);
        pump(c, 5);
        CHECK_EQ(rec.plays.size(), p4 + 1);
        CHECK_EQ(rec.sources.back(), pldir + "/Road trip.m3u8");
        CHECK_EQ(rec.plays.back().size(), static_cast<size_t>(4));
        c.hold(Page::LibPlaylists, 2);
        CHECK(c.letter(libm::kKeyR, 0));
        CHECK_EQ(rec.names.back().title, std::string("Rename playlist"));
        CHECK_EQ(rec.names.back().text, std::string("Road trip"));
        c.name_answer(false, "");
        c.hold(Page::LibPlaylists, 2);
        CHECK(c.letter(libm::kKeyE, 0));
        CHECK_EQ(rec.exports.back(), std::string("Road trip"));
        c.choose_cancelled();
        CHECK(!c.exporting());
        CHECK_EQ(c.page(), Page::LibPlaylists);

        /* ---- Save the queue: the name dialog (Queue, then the next free), the playlist has the queue's order */
        const std::vector<std::string> queue = {music + "/Bob - Beta/2.mp3", music + "/Ann - Alpha/3.mp3", music + "/gone-file.mp3"};
        c.save_queue(queue);
        pump(c);
        CHECK_EQ(rec.names.back().title, std::string("Save queue as playlist"));
        CHECK_EQ(rec.names.back().sub, std::string("3 tracks. Name:"));
        CHECK_EQ(rec.names.back().text, std::string("Queue"));
        CHECK_EQ(rec.names.back().ok, std::string("Save"));
        c.name_answer(true, "Queue");
        pump(c, 5);
        CHECK_EQ(rec.last_notice(), std::string("Playlist saved."));
        CHECK(!rec.last_danger());
        {
            std::vector<m3u::Entry> e;
            PlaylistStore st(pldir, {music});
            CHECK(st.read("Queue", e) == PlResult::Ok);
            CHECK_EQ(e.size(), static_cast<size_t>(3));
            if (e.size() == 3) {
                CHECK_EQ(e[0].path, music + "/Bob - Beta/2.mp3");
                CHECK_EQ(e[1].path, music + "/Ann - Alpha/3.mp3");
                CHECK_EQ(e[2].path, music + "/gone-file.mp3"); /* a file the library does not know is kept too */
            }
            const std::string text = read_file(pldir + "/Queue.m3u8");
            CHECK(text.find("\n../Bob - Beta/2.mp3\n") != std::string::npos); /* relative: same drive */
        }
        c.save_queue(queue);
        pump(c);
        CHECK_EQ(rec.names.back().text, std::string("Queue 2"));
        c.name_answer(true, "queue"); /* taken, without case */
        pump(c);
        CHECK(rec.last_danger());
        c.name_answer(false, "");
        c.save_queue({});
        CHECK_EQ(rec.last_notice(), std::string("The queue is empty."));
        CHECK_EQ(c.rows().size(), static_cast<size_t>(4)); /* New, Dropped in, Queue, Road trip */
    }
    rm_tree(dir);
}
