/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: core/playlist (P2c, report 060 2.4): the store (names, create, rename, delete, move, remove, add, import, export,
 * atomic writes) and the database index (rebuilt from the files: added, changed and deleted outside Vinyl).
 */
#include "check.hpp"

#include "library/library.hpp"
#include "library/schema.hpp"
#include "playlist/playlist_index.hpp"
#include "playlist/playlists.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

using namespace vinyl;

namespace {

bool exists_file(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

m3u::Entry E(const std::string &path, const std::string &text = "", int sec = -1) {
    m3u::Entry e;
    e.path = path;
    e.text = text;
    e.seconds = sec;
    return e;
}

std::vector<std::string> paths_of(const std::vector<m3u::Entry> &e) {
    std::vector<std::string> p;
    for (const m3u::Entry &x : e) p.push_back(x.path);
    return p;
}

}  // namespace

void test_playlists() {
    const std::string dir = make_temp_dir("pl");
    const std::string music = dir + "/Music";
    ::mkdir(music.c_str(), 0755);
    for (const char *n : {"a.mp3", "b.mp3", "c.mp3", "d.mp3"}) write_file(music + "/" + n, "x");
    PlaylistStore st(music + "/Playlists", {music});

    /* names: the allowed characters, 24 at most, trimmed */
    CHECK(PlaylistStore::name_problem("Evening") == NameProblem::None);
    CHECK(PlaylistStore::name_problem("  Road trip 2-b_c ") == NameProblem::None);
    CHECK(PlaylistStore::name_problem("") == NameProblem::Empty);
    CHECK(PlaylistStore::name_problem("   ") == NameProblem::Empty);
    CHECK(PlaylistStore::name_problem("a/b") == NameProblem::BadChar);
    CHECK(PlaylistStore::name_problem("a.b") == NameProblem::BadChar);
    CHECK(PlaylistStore::name_problem("..") == NameProblem::BadChar);
    CHECK(PlaylistStore::name_problem("Qu\"ote") == NameProblem::BadChar);
    CHECK(PlaylistStore::name_problem("tab\there") == NameProblem::BadChar);
    CHECK(PlaylistStore::name_problem("\xC3\x89t\xC3\xA9") == NameProblem::None); /* Été: letters of every alphabet */
    CHECK(PlaylistStore::name_problem("\xE5\xA4\x9C\xE6\x98\x8E") == NameProblem::None);
    CHECK(PlaylistStore::name_problem(std::string("\xC3", 1)) == NameProblem::BadChar); /* a cut sequence */
    CHECK(PlaylistStore::name_problem(std::string(24, 'a')) == NameProblem::None);
    CHECK(PlaylistStore::name_problem(std::string(25, 'a')) == NameProblem::TooLong);
    CHECK(PlaylistStore::name_problem("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                      "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9") == NameProblem::None); /* 24 letters, 48 bytes */
    CHECK_EQ(PlaylistStore::name_length("\xC3\xA9" "a"), 2);
    CHECK_EQ(PlaylistStore::trim_name("  x y  "), std::string("x y"));
    /* sanitising: bad characters become one space, the length is cut, an empty name gets a default */
    CHECK_EQ(PlaylistStore::clean_name("Road trip (2019).m3u"), std::string("Road trip 2019 m3u"));
    CHECK_EQ(PlaylistStore::clean_name("a//b\\c"), std::string("a b c"));
    CHECK_EQ(PlaylistStore::clean_name("***"), std::string("Playlist"));
    CHECK_EQ(PlaylistStore::clean_name(""), std::string("Playlist"));
    CHECK_EQ(PlaylistStore::clean_name(std::string(40, 'x')), std::string(24, 'x'));
    CHECK_EQ(PlaylistStore::clean_name("  lead and trail  "), std::string("lead and trail"));
    CHECK(PlaylistStore::name_problem(PlaylistStore::clean_name("we!rd:name?")) == NameProblem::None);

    /* an empty store: no folder yet */
    CHECK(st.names().empty());
    CHECK(!exists_file(st.dir()));
    {
        std::vector<m3u::Entry> none;
        CHECK(st.read("Nothing", none) == PlResult::NotFound);
    }

    /* create: the folder is made, the file is M3U8, the name is kept as typed, a taken name is refused without case */
    CHECK(st.create("Evening", {E(music + "/a.mp3", "A - One", 60), E(music + "/b.mp3")}) == PlResult::Ok);
    CHECK(exists_file(st.dir() + "/Evening.m3u8"));
    const std::string text = read_file(st.dir() + "/Evening.m3u8");
    CHECK(text.compare(0, 8, "#EXTM3U\n") == 0);
    CHECK(text.find("\n../a.mp3\n") != std::string::npos); /* relative to Music/Playlists */
    CHECK(st.create("Evening") == PlResult::Taken);
    CHECK(st.create("evening") == PlResult::Taken);
    CHECK(st.create("EVENING ") == PlResult::Taken);
    CHECK(st.create("bad/name") == PlResult::Invalid);
    CHECK(st.create("") == PlResult::Invalid);
    CHECK(st.create("Morning run") == PlResult::Ok);
    CHECK(st.create("Road trip 10") == PlResult::Ok);
    CHECK(st.create("Road trip 9") == PlResult::Ok);
    {
        const std::vector<std::string> n = st.names(); /* A-Z, natural order */
        CHECK_EQ(n.size(), static_cast<size_t>(4));
        if (n.size() == 4) {
            CHECK_EQ(n[0], std::string("Evening"));
            CHECK_EQ(n[1], std::string("Morning run"));
            CHECK_EQ(n[2], std::string("Road trip 9"));
            CHECK_EQ(n[3], std::string("Road trip 10"));
        }
    }
    CHECK_EQ(st.free_name("Evening"), std::string("Evening 2"));
    CHECK_EQ(st.free_name("Queue"), std::string("Queue"));
    CHECK(st.create("Evening 2") == PlResult::Ok);
    CHECK_EQ(st.free_name("Evening"), std::string("Evening 3"));
    CHECK_EQ(st.free_name(std::string(24, 'q')), std::string(24, 'q'));
    CHECK(st.create(std::string(24, 'q')) == PlResult::Ok);
    {
        const std::string f = st.free_name(std::string(24, 'q')); /* cut so the number fits within 24 */
        CHECK_EQ(PlaylistStore::name_length(f), 24);
        CHECK(f.size() > 2 && f.substr(f.size() - 2) == " 2");
        CHECK(PlaylistStore::name_problem(f) == NameProblem::None);
    }

    /* read back: the entries, the #EXTINF */
    {
        std::vector<m3u::Entry> e;
        CHECK(st.read("evening", e) == PlResult::Ok); /* without case */
        CHECK_EQ(e.size(), static_cast<size_t>(2));
        if (e.size() == 2) {
            CHECK_EQ(e[0].path, music + "/a.mp3");
            CHECK_EQ(e[0].text, std::string("A - One"));
            CHECK_EQ(e[0].seconds, 60);
            CHECK_EQ(e[1].path, music + "/b.mp3");
        }
    }

    /* append (duplicates allowed), move, remove */
    CHECK(st.append("Evening", {E(music + "/c.mp3"), E(music + "/a.mp3")}) == PlResult::Ok);
    {
        std::vector<m3u::Entry> e;
        st.read("Evening", e);
        CHECK(paths_of(e) == (std::vector<std::string>{music + "/a.mp3", music + "/b.mp3", music + "/c.mp3", music + "/a.mp3"}));
    }
    int ni = -1;
    CHECK(st.move("Evening", 1, -1, &ni) == PlResult::Ok);
    CHECK_EQ(ni, 0);
    CHECK(st.move("Evening", 0, -1) == PlResult::Range);
    CHECK(st.move("Evening", 3, 1) == PlResult::Range);
    CHECK(st.move("Evening", 9, -1) == PlResult::Range);
    CHECK(st.move("Evening", 2, 1, &ni) == PlResult::Ok);
    CHECK_EQ(ni, 3);
    {
        std::vector<m3u::Entry> e;
        st.read("Evening", e);
        CHECK(paths_of(e) == (std::vector<std::string>{music + "/b.mp3", music + "/a.mp3", music + "/a.mp3", music + "/c.mp3"}));
        CHECK_EQ(e[1].text, std::string("A - One")); /* the #EXTINF travels with its entry */
    }
    CHECK(st.remove_at("Evening", 0) == PlResult::Ok);
    CHECK(st.remove_at("Evening", 7) == PlResult::Range);
    CHECK(st.remove_at("Evening", -1) == PlResult::Range);
    CHECK(st.append("Nothing", {E("/x")}) == PlResult::NotFound);
    CHECK(st.move("Nothing", 0, 1) == PlResult::NotFound);
    {
        std::vector<m3u::Entry> e;
        st.read("Evening", e);
        CHECK_EQ(e.size(), static_cast<size_t>(3));
    }

    /* a missing file is KEPT through every edit, never dropped; only Remove takes it out */
    CHECK(st.append("Morning run", {E(music + "/gone.mp3", "Old - Song", 30), E(music + "/a.mp3")}) == PlResult::Ok);
    CHECK(st.move("Morning run", 1, -1) == PlResult::Ok);
    {
        std::vector<m3u::Entry> e;
        st.read("Morning run", e);
        CHECK_EQ(e.size(), static_cast<size_t>(2));
        CHECK_EQ(e[0].path, music + "/a.mp3");
        CHECK_EQ(e[1].path, music + "/gone.mp3");
        CHECK_EQ(e[1].text, std::string("Old - Song"));
    }
    CHECK(st.remove_at("Morning run", 1) == PlResult::Ok);

    /* rename: Taken, Invalid, the same one under another case, NotFound */
    CHECK(st.rename("Evening", "Morning run") == PlResult::Taken);
    CHECK(st.rename("Evening", "morning RUN") == PlResult::Taken);
    CHECK(st.rename("Evening", "no/pe") == PlResult::Invalid);
    CHECK(st.rename("Nothing", "Whatever") == PlResult::NotFound);
    CHECK(st.rename("Evening", "evening") == PlResult::Ok); /* the same playlist, another case */
    CHECK(exists_file(st.dir() + "/evening.m3u8"));
    CHECK(!exists_file(st.dir() + "/Evening.m3u8") || st.find("Evening") == "evening");
    CHECK(st.rename("evening", "Evening run") == PlResult::Ok);
    CHECK(st.exists("Evening run"));
    CHECK(!st.exists("Evening"));
    {
        std::vector<m3u::Entry> e;
        CHECK(st.read("Evening run", e) == PlResult::Ok);
        CHECK_EQ(e.size(), static_cast<size_t>(3));
    }

    /* delete */
    CHECK(st.remove("Evening 2") == PlResult::Ok);
    CHECK(st.remove("Evening 2") == PlResult::NotFound);
    CHECK(!exists_file(st.dir() + "/Evening 2.m3u8"));
    CHECK(music.size() > 0 && exists_file(music + "/a.mp3")); /* the songs stay */

    /* an interrupted write leaves the old file whole; the leftover is swept */
    {
        const std::string before = read_file(st.dir() + "/Evening run.m3u8");
        st.test_hook = [](const char *) { return false; }; /* the power goes after the temporary file is written */
        CHECK(st.append("Evening run", {E(music + "/d.mp3")}) == PlResult::Io);
        CHECK(st.write("Brand new", {E(music + "/d.mp3")}) == PlResult::Io);
        st.test_hook = nullptr;
        CHECK_EQ(read_file(st.dir() + "/Evening run.m3u8"), before);
        CHECK(!exists_file(st.dir() + "/Brand new.m3u8")); /* no half file under the real name */
        CHECK(exists_file(st.dir() + "/.Evening run.m3u8.tmp")); /* what was left */
        const std::vector<std::string> n = st.names();
        CHECK(std::find(n.begin(), n.end(), "Brand new") == n.end());
        CHECK(!exists_file(st.dir() + "/.Evening run.m3u8.tmp")); /* swept */
        CHECK(st.append("Evening run", {E(music + "/d.mp3")}) == PlResult::Ok);
        std::vector<m3u::Entry> e;
        st.read("Evening run", e);
        CHECK_EQ(e.size(), static_cast<size_t>(4));
    }

    /* an unwritable folder: Io, nothing changes */
    {
        PlaylistStore bad("/proc/vinyl-nope/Playlists");
        CHECK(bad.create("X") == PlResult::Io);
    }

    /* import: a Windows .m3u with Latin-1 names and backslashes from another folder becomes a playlist whose paths are written
     * relative / absolute by the writer's rules; the name must be free */
    {
        const std::string src = dir + "/Import";
        ::mkdir(src.c_str(), 0755);
        write_file(src + "/Caf\xC3\xA9.mp3", "x");
        write_file(src + "/plain.mp3", "x");
        write_file(src + "/old.m3u", "#EXTM3U\r\n#EXTINF:90,Caf\xE9 Band - Caf\xE9\r\nCaf\xE9.mp3\r\nD:\\Gone\\x.mp3\r\n.\\plain.mp3\r\n");
        CHECK(st.import_file(src + "/old.m3u", "Evening run") == PlResult::Taken);
        CHECK(st.import_file(src + "/old.m3u", "bad/name") == PlResult::Invalid);
        CHECK(st.import_file(src + "/nope.m3u", "Imported") == PlResult::NotFound);
        CHECK(st.import_file(src + "/old.m3u", "Imported") == PlResult::Ok);
        std::vector<m3u::Entry> e;
        CHECK(st.read("Imported", e) == PlResult::Ok);
        CHECK_EQ(e.size(), static_cast<size_t>(3));
        if (e.size() == 3) {
            CHECK_EQ(e[0].path, src + "/Caf\xC3\xA9.mp3");
            CHECK_EQ(e[0].text, std::string("Caf\xC3\xA9 Band - Caf\xC3\xA9"));
            CHECK_EQ(e[0].seconds, 90);
            CHECK(!e[1].resolved); /* kept, as read */
            CHECK_EQ(e[2].path, src + "/plain.mp3");
        }
        const std::string t = read_file(st.dir() + "/Imported.m3u8");
        CHECK(m3u::valid_utf8(t)); /* the file is UTF-8 now */
        CHECK(t.find("D:\\Gone\\x.mp3\n") != std::string::npos);
    }

    /* export: to a folder (a drive's root): the file there, tracks of that folder relative, the others absolute */
    {
        const std::string usb = dir + "/usb";
        ::mkdir(usb.c_str(), 0755);
        write_file(usb + "/own.mp3", "x");
        CHECK(st.create("Mixed", {E(usb + "/own.mp3", "O - O", 5), E(music + "/a.mp3")}) == PlResult::Ok);
        std::string out;
        CHECK(st.export_to("Mixed", usb, &out) == PlResult::Ok);
        CHECK_EQ(out, usb + "/Mixed.m3u8");
        const std::string t = read_file(out);
        CHECK(t.find("\nown.mp3\n") != std::string::npos);
        CHECK(t.find(music + "/a.mp3") != std::string::npos || t.find("../Music/a.mp3") != std::string::npos);
        /* exporting again replaces the file */
        CHECK(st.export_to("mixed", usb, &out) == PlResult::Ok);
        CHECK(st.export_to("Nothing", usb) == PlResult::NotFound);
        CHECK(st.export_to("Mixed", usb + "/missing-folder") == PlResult::Io);
        /* the exported file reads back with the same tracks */
        PlaylistStore other(usb + "/Playlists", {music});
        std::vector<m3u::Entry> e = m3u::parse(t, other.parse_options(usb));
        CHECK_EQ(e.size(), static_cast<size_t>(2));
        if (e.size() == 2) CHECK_EQ(e[0].path, usb + "/own.mp3");
    }

    /* the stem and the file test */
    CHECK_EQ(playlist_stem("/a/b/Road trip.m3u8"), std::string("Road trip"));
    CHECK_EQ(playlist_stem("/a/b/x.M3U"), std::string("x"));
    CHECK_EQ(playlist_stem("/a/b/noext"), std::string("noext"));
    CHECK(is_playlist_file_name("A.M3U8") && is_playlist_file_name("a.m3u") && !is_playlist_file_name("a.mp3"));
    CHECK(std::string(pl_result_text(PlResult::Taken)).size() > 0);

    /* ---------------------------------------------------------------- the database index */
    std::string why;
    if (!real_sqlite_lib(&why)) {
        SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
        rm_tree(dir);
        return;
    }
    Db db;
    CHECK(open_library_db(db, ":memory:", 416, &why));
    {
        LibraryWriter w(db);
        const int64_t root = w.root(music, "Music");
        Transaction tx(db);
        const char *titles[] = {"One", "Two", "Three", "Four"};
        const char *files[] = {"a.mp3", "b.mp3", "c.mp3", "d.mp3"};
        for (int i = 0; i < 4; ++i) {
            TrackTags t;
            t.title = titles[i];
            t.artist = "Band";
            t.album = "Album";
            t.track = i + 1;
            t.codec = "MP3";
            t.duration_ms = 60000 * (i + 1);
            t.ok = true;
            TrackFile f;
            f.root_id = root;
            f.folder_id = w.folder(root, "", 1);
            f.name = files[i];
            f.mtime = 1;
            f.size = 1;
            w.add_track(f, t);
        }
        CHECK(tx.commit());
    }
    Library lib(db);
    CHECK(playlist_sync(lib, st));
    std::vector<PlaylistRow> rows;
    CHECK(playlist_list(lib, rows));
    CHECK_EQ(rows.size(), st.names().size()); /* the index = the files */
    auto row_of = [&](const std::string &name) -> const PlaylistRow * {
        for (const PlaylistRow &r : rows)
            if (r.name == name) return &r;
        return nullptr;
    };
    const PlaylistRow *ev = row_of("Evening run");
    CHECK(ev != nullptr);
    if (ev) {
        CHECK_EQ(ev->tracks, 4);
        CHECK_EQ(ev->ms, static_cast<uint64_t>(60000 + 60000 + 180000 + 240000)); /* a, a, c, d: the library's lengths */
    }
    const long long items = db.scalar("select count(*) from playlist_items", -1);
    long long expect_items = 0;
    for (const PlaylistRow &r : rows) expect_items += r.tracks;
    CHECK_EQ(items, expect_items);
    /* every row names a known track (a library file) except the unresolved one */
    CHECK_EQ(db.scalar("select count(*) from playlist_items i join playlists p on p.id = i.playlist_id where p.name = 'Evening run' and i.track_id is not null", -1), 4LL);

    /* nothing changed: nothing is written (the rowids stay) */
    const long long max_before = db.scalar("select max(id) from playlists", -1);
    const long long chg = db.scalar("select total_changes()", -1);
    CHECK(playlist_sync(lib, st));
    CHECK_EQ(db.scalar("select total_changes()", -1), chg);
    CHECK_EQ(db.scalar("select max(id) from playlists", -1), max_before);

    /* a file changed outside Vinyl: seen at the next sync (the mtime is made different on purpose) */
    write_file(st.dir() + "/Mixed.m3u8", "#EXTM3U\n" + std::string("../a.mp3\n") + "../b.mp3\n../c.mp3\n");
    {
        struct timespec ts[2] = {{1700000000, 0}, {1700000000, 0}};
        ::utimensat(AT_FDCWD, (st.dir() + "/Mixed.m3u8").c_str(), ts, 0);
    }
    CHECK(playlist_sync(lib, st));
    playlist_list(lib, rows);
    const PlaylistRow *mx = row_of("Mixed");
    CHECK(mx != nullptr);
    if (mx) CHECK_EQ(mx->tracks, 3);
    /* a file added by hand (a name Vinyl would not make: it is listed under its own name) */
    write_file(st.dir() + "/Hand made.m3u8", "../d.mp3\n");
    CHECK(playlist_sync(lib, st));
    playlist_list(lib, rows);
    CHECK(row_of("Hand made") != nullptr);
    CHECK_EQ(rows.size(), st.names().size());
    /* an edit of a file with such a name keeps the name */
    CHECK(st.append("Hand made", {E(music + "/a.mp3")}) == PlResult::Ok);
    CHECK(exists_file(st.dir() + "/Hand made.m3u8"));
    /* a file deleted outside Vinyl: gone from the index, items too */
    ::unlink((st.dir() + "/Hand made.m3u8").c_str());
    CHECK(playlist_sync(lib, st));
    playlist_list(lib, rows);
    CHECK(row_of("Hand made") == nullptr);
    CHECK_EQ(rows.size(), st.names().size());
    CHECK_EQ(db.scalar("select count(*) from playlist_items where playlist_id not in (select id from playlists)", -1), 0LL);
    /* the index after Vinyl's own edits (forced, one playlist) and a rename */
    CHECK(st.rename("Mixed", "Mixed up") == PlResult::Ok);
    CHECK(playlist_reindex(lib, st, "Mixed up"));
    CHECK(playlist_reindex(lib, st, "Mixed")); /* the old name: its row goes */
    playlist_list(lib, rows);
    CHECK(row_of("Mixed") == nullptr);
    CHECK(row_of("Mixed up") != nullptr);
    CHECK_EQ(rows.size(), st.names().size());
    /* a rebuild from nothing gives the same list: the index is only a cache */
    db.exec("delete from playlist_items; delete from playlists;");
    CHECK(playlist_sync(lib, st));
    std::vector<PlaylistRow> again;
    playlist_list(lib, again);
    CHECK_EQ(again.size(), rows.size());
    for (size_t i = 0; i < again.size() && i < rows.size(); ++i) {
        CHECK_EQ(again[i].name, rows[i].name);
        CHECK_EQ(again[i].tracks, rows[i].tracks);
    }

    /* the edit page's rows: from the file, named from the library; a missing file is dimmed (exists false) and kept */
    {
        CHECK(st.append("Evening run", {E(music + "/gone.mp3", "Old Band - Old song", 33), E("D:\\Music\\x.mp3")}) == PlResult::Ok);
        std::vector<PlaylistEntryRow> pr;
        PlaylistStats ps;
        CHECK(playlist_rows(lib, st, "Evening run", pr, &ps));
        CHECK_EQ(pr.size(), static_cast<size_t>(6));
        CHECK_EQ(ps.tracks, 6);
        CHECK_EQ(ps.missing, 2);
        if (pr.size() == 6) {
            CHECK_EQ(pr[0].label(), std::string("Band, One"));
            CHECK(pr[0].exists && pr[0].track_id > 0);
            CHECK_EQ(pr[0].duration_ms, static_cast<uint64_t>(60000));
            CHECK(!pr[4].exists);
            CHECK_EQ(pr[4].label(), std::string("Old Band, Old song"));
            CHECK_EQ(pr[4].duration_ms, static_cast<uint64_t>(33000));
            CHECK(!pr[5].exists);
            CHECK(!pr[5].entry.resolved);
            CHECK_EQ(pr[5].title, std::string("x.mp3")); /* the file name of a Windows path */
        }
        /* playing: the missing are skipped and the start index is mapped; a missing start has none */
        int start = 0;
        const std::vector<std::string> play = playlist_playable(pr, 3, &start);
        CHECK_EQ(play.size(), static_cast<size_t>(4));
        CHECK_EQ(start, 3);
        playlist_playable(pr, 4, &start);
        CHECK_EQ(start, -1);
        CHECK_EQ(playlist_playable(pr, 0, &start).size(), static_cast<size_t>(4));
        CHECK_EQ(start, 0);
        /* a playlist that is entirely unplayable is still listed */
        CHECK(st.create("Ghosts", {E(music + "/gone1.mp3"), E(music + "/gone2.mp3")}) == PlResult::Ok);
        CHECK(playlist_sync(lib, st));
        playlist_list(lib, rows);
        CHECK(row_of("Ghosts") != nullptr && row_of("Ghosts")->tracks == 2);
        CHECK(playlist_rows(lib, st, "Ghosts", pr, &ps));
        CHECK_EQ(ps.missing, 2);
        CHECK(playlist_playable(pr, 0, &start).empty());
        /* a drive that is away: its entries stay, dimmed, in the file */
        std::vector<m3u::Entry> keep;
        st.read("Ghosts", keep);
        CHECK_EQ(keep.size(), static_cast<size_t>(2));
    }

    /* entries from tracks and from paths (the library knows a, not other) */
    {
        const std::vector<m3u::Entry> e = entries_from_paths(lib, {music + "/a.mp3", "/elsewhere/x.mp3"});
        CHECK_EQ(e.size(), static_cast<size_t>(2));
        if (e.size() == 2) {
            CHECK_EQ(e[0].text, std::string("Band - One"));
            CHECK_EQ(e[0].seconds, 60);
            CHECK(e[1].text.empty() && e[1].seconds == -1);
        }
        TrackRow t;
        t.path = "/m/x.mp3";
        t.title = "T";
        t.artist = "A";
        t.duration_ms = 61400;
        const std::vector<m3u::Entry> f = entries_from_tracks({t});
        CHECK_EQ(f[0].text, std::string("A - T"));
        CHECK_EQ(f[0].seconds, 61);
    }
    rm_tree(dir);
}
