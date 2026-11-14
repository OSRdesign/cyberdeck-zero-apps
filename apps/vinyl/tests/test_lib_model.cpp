/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the data side of the Library pages (src/core/library/lib_model): number and heading text, the rows of
 * every page, the pager, the scan strip and drive note, the sheets and their keys, the navigation stack, and the two
 * queries the pages added to the library (a genre's track count, the roots with their tracks).
 */
#include "check.hpp"

#include "library/lib_model.hpp"
#include "library/library.hpp"
#include "library/schema.hpp"

using namespace vinyl;
using namespace vinyl::libm;

namespace {

TrackRow trow(int64_t id, const std::string &title, int track, int disc, uint64_t ms, const std::string &artist = "A") {
    TrackRow t;
    t.id = id;
    t.title = title;
    t.track = track;
    t.disc = disc;
    t.duration_ms = ms;
    t.artist = artist;
    t.path = "/m/" + title + ".mp3";
    return t;
}

}  // namespace

void test_lib_model() {
    /* numbers and text */
    CHECK_EQ(count_text(0), std::string("0"));
    CHECK_EQ(count_text(999), std::string("999"));
    CHECK_EQ(count_text(1204), std::string("1,204"));
    CHECK_EQ(count_text(1234567), std::string("1,234,567"));
    CHECK_EQ(plural(1, "album", "albums"), std::string("1 album"));
    CHECK_EQ(plural(3, "album", "albums"), std::string("3 albums"));
    CHECK_EQ(plural(1204, "track", "tracks"), std::string("1,204 tracks"));
    CHECK_EQ(upper_text("Phoenix"), std::string("PHOENIX"));
    CHECK_EQ(upper_text("Björk"), std::string("BJÖRK"));
    CHECK_EQ(upper_text("Motörhead çé"), std::string("MOTÖRHEAD ÇÉ"));
    CHECK_EQ(upper_text("坂本龍一 abc"), std::string("坂本龍一 ABC"));
    CHECK_EQ(upper_text("кино"), std::string("КИНО"));
    CHECK_EQ(upper_text("Łódź"), std::string("ŁÓDŹ"));
    CHECK_EQ(upper_text("bad \xE3 byte"), std::string("BAD \xE3 BYTE")); /* an invalid sequence passes through */
    CHECK_EQ(duration_text(241000), std::string("04:01"));
    CHECK_EQ(duration_text(3723000), std::string("1:02:03"));
    CHECK_EQ(duration_text(0), std::string(""));
    CHECK_EQ(length_text(241000), std::string("4:01"));
    CHECK_EQ(when_text(1731447600, true), std::string("12 Nov 21:40"));
    CHECK_EQ(when_comma_text(1731447600, true), std::string("12 Nov, 21:40"));
    CHECK_EQ(home_short("/home/o/Music/x.mp3", "/home/o"), std::string("~/Music/x.mp3"));
    CHECK_EQ(home_short("/home/other/x", "/home/o"), std::string("/home/other/x"));
    CHECK_EQ(home_short("/mnt/x", ""), std::string("/mnt/x"));

    /* the hub */
    LibraryCounts c;
    c.tracks = 1204;
    c.albums = 118;
    c.artists = 312;
    c.genres = 7;
    std::vector<Row> hub = hub_rows(c, true);
    CHECK_EQ(hub.size(), static_cast<size_t>(6));
    CHECK_EQ(hub[0].text, std::string("Folders"));
    CHECK_EQ(hub[1].text, std::string("Playlists"));
    CHECK_EQ(hub[1].mark == Row::Mark::Chevron, true);
    CHECK_EQ(hub[2].value, std::string("312"));
    CHECK_EQ(hub[4].value, std::string("1,204"));
    CHECK_EQ(hub[5].key, std::string("genres"));
    CHECK_EQ(hub_rows(c, false)[2].value, std::string(""));
    CHECK_EQ(hub_heading(1204, false), std::string("LIBRARY, 1,204 TRACKS"));
    CHECK_EQ(hub_heading(388, true), std::string("LIBRARY, 388 TRACKS SO FAR"));
    CHECK_EQ(hub_heading(1, false), std::string("LIBRARY, 1 TRACK"));
    CHECK_EQ(artists_heading(312), std::string("ARTISTS, 312"));
    CHECK_EQ(songs_heading(1204), std::string("SONGS, 1,204"));

    /* artists, albums, genres */
    ArtistRow a1, a2;
    a1.id = 4;
    a1.name = "Beach House";
    a1.albums = 3;
    a2.id = 9;
    a2.albums = 1;
    std::vector<Row> ar = artist_rows({a1, a2}, 40);
    CHECK_EQ(ar[0].text, std::string("Beach House"));
    CHECK_EQ(ar[0].value, std::string("3 albums"));
    CHECK_EQ(ar[0].index, 40);
    CHECK_EQ(ar[1].text, std::string("Unknown artist"));
    CHECK_EQ(ar[1].value, std::string("1 album"));
    CHECK_EQ(ar[0].type == Row::Type::Artist, true);
    AlbumRow al;
    al.id = 7;
    al.title = "Bankrupt!";
    al.artist = "Phoenix";
    al.year = 2013;
    CHECK_EQ(album_rows({al}, true)[0].value, std::string("Phoenix"));
    CHECK_EQ(album_rows({al}, false)[0].value, std::string("2013"));
    al.year = 0;
    CHECK_EQ(album_rows({al}, false)[0].value, std::string(""));
    al.title = "";
    CHECK_EQ(album_rows({al}, true)[0].text, std::string("Unknown album"));
    GenreRow g;
    g.id = 2;
    g.name = "";
    g.tracks = 35;
    CHECK_EQ(genre_rows({g})[0].text, std::string("No genre"));
    CHECK_EQ(genre_rows({g})[0].value, std::string("35 tracks"));
    CHECK_EQ(artist_heading("Phoenix", 5, 58), std::string("PHOENIX, 5 ALBUMS, 58 TRACKS"));
    CHECK_EQ(artist_heading("Phoenix", 1, 1), std::string("PHOENIX, 1 ALBUM, 1 TRACK"));
    CHECK_EQ(album_heading("Wolfgang Amadeus Phoenix", 2009, 14), std::string("WOLFGANG AMADEUS PHOENIX, 2009, 14 TRACKS"));
    CHECK_EQ(album_heading("X", 0, 3), std::string("X, 3 TRACKS"));
    CHECK_EQ(genre_heading("Jazz", 12), std::string("JAZZ, 12 ALBUMS"));

    /* an album's tracks: the number before the title, "DISC 2" where the disc changes, the playing one marked */
    std::vector<TrackRow> tr = {trow(1, "Lisztomania", 1, 1, 241000), trow(2, "1901", 2, 1, 193000), trow(3, "Rome", 1, 2, 261000),
                                trow(4, "Countdown", 2, 2, 267000)};
    std::vector<Row> rows = album_track_rows(tr, "/m/1901.mp3");
    CHECK_EQ(rows.size(), static_cast<size_t>(5));
    CHECK_EQ(rows[0].text, std::string("1  Lisztomania"));
    CHECK_EQ(rows[0].value, std::string("04:01"));
    CHECK_EQ(rows[1].playing, true);
    CHECK(rows[2].kind == Row::Kind::Heading);
    CHECK_EQ(rows[2].text, std::string("DISC 2"));
    CHECK_EQ(rows[3].index, 2); /* an index into the tracks, headings not counted */
    CHECK_EQ(rows[4].index, 3);
    CHECK_EQ(first_row(rows), 0);
    CHECK_EQ(step_row(rows, 1, 1), 3); /* the heading is skipped */
    CHECK_EQ(step_row(rows, 3, -1), 1);
    CHECK_EQ(step_row(rows, 0, -1), -1);
    CHECK_EQ(step_row(rows, 4, 1), -1);
    /* a single disc, or no disc tags: no heading */
    std::vector<TrackRow> one = {trow(1, "A", 0, 0, 0), trow(2, "B", 0, 0, 1000)};
    CHECK_EQ(album_track_rows(one, "").size(), static_cast<size_t>(2));
    CHECK_EQ(album_track_rows(one, "")[0].text, std::string("A"));
    CHECK_EQ(album_track_rows(one, "")[0].value, std::string(""));

    /* songs */
    std::vector<Row> sr = song_rows({trow(5, "1901", 0, 0, 1, "Phoenix")}, "", 80);
    CHECK_EQ(sr[0].value, std::string("Phoenix"));
    CHECK_EQ(sr[0].index, 80);
    CHECK(sr[0].mark == Row::Mark::Dot);

    /* search: sections with headings, empty ones left out */
    SearchResults res;
    CHECK(search_rows(res, "").empty());
    res.songs = {trow(1, "Fences", 0, 0, 0, "Phoenix"), trow(2, "Defender", 0, 0, 0, "Ghost Beach")};
    res.albums = {al};
    res.genres.push_back(g);
    rows = search_rows(res, "");
    CHECK_EQ(rows.size(), static_cast<size_t>(7)); /* SONGS 2, ALBUMS 1, GENRES 1 = 4 items + 3 headings */
    CHECK_EQ(rows[0].text, std::string("SONGS"));
    CHECK_EQ(rows[3].text, std::string("ALBUMS"));
    CHECK_EQ(rows[5].text, std::string("GENRES"));
    CHECK_EQ(rows[2].index, 1);
    CHECK(rows[4].type == Row::Type::Album);

    /* paging: the next page once, near the end of what is loaded */
    Pager p;
    p.reset(100);
    CHECK_EQ(p.next_offset(), 0);
    CHECK(p.want_more(0)); /* nothing loaded yet: asked once */
    CHECK(!p.want_more(0)); /* not twice */
    p.appended(40);
    CHECK(!p.want_more(10));
    CHECK(!p.want_more(31));
    CHECK(p.want_more(32)); /* within 8 of the 40 loaded */
    CHECK(!p.want_more(39));
    p.appended(40);
    CHECK_EQ(p.loaded(), 80);
    CHECK_EQ(p.next_offset(), 80);
    CHECK(p.want_more(79));
    p.appended(20);
    CHECK(p.complete());
    CHECK(!p.want_more(99));
    Pager q;
    q.reset(50);
    CHECK(q.want_more(0));
    q.failed();
    CHECK(q.want_more(0)); /* a failed page may be asked again */
    q.appended(0);         /* the library has no more rows than it said */
    CHECK(q.complete());
    CHECK(!q.want_more(0));
    Pager z;
    z.reset(0);
    CHECK(!z.want_more(0));

    /* the scan strip */
    ScanProgress sp;
    CHECK(!scan_note(sp, 1200).shown);
    sp.state = ScanProgress::State::Scanning;
    sp.files = 388;
    ScanNote n = scan_note(sp, 1200);
    CHECK(n.shown && !n.hot);
    CHECK_EQ(n.text, std::string("Scanning: 388 of about 1,200 files"));
    CHECK_EQ(n.pct, 32);
    n = scan_note(sp, 0); /* a first scan: no estimate, no bar */
    CHECK_EQ(n.text, std::string("Scanning: 388 files"));
    CHECK_EQ(n.pct, -1);
    sp.files = 1500;
    n = scan_note(sp, 1200); /* more files than the estimate: no "of about", the bar stays under 100 */
    CHECK_EQ(n.text, std::string("Scanning: 1,500 files"));
    CHECK(n.pct >= 0 && n.pct <= 99);
    sp.files = 735;
    sp.state = ScanProgress::State::Paused;
    n = scan_note(sp, 1200);
    CHECK(n.shown && n.hot);
    CHECK_EQ(n.text, std::string("Paused: the board is hot. Resumes when it cools."));
    CHECK_EQ(n.pct, 61);
    sp.state = ScanProgress::State::Done;
    CHECK(!scan_note(sp, 1200).shown);
    sp.state = ScanProgress::State::Cancelled;
    CHECK(!scan_note(sp, 1200).shown);
    sp.state = ScanProgress::State::Scanning;
    sp.files = 412;
    CHECK_EQ(scan_heading(sp, 1204), std::string("SCANNING, 412 OF 1,204"));
    CHECK_EQ(scan_heading(sp, 0), std::string("SCANNING, 412 FILES"));
    sp.state = ScanProgress::State::Paused;
    CHECK_EQ(scan_heading(sp, 1204), std::string("PAUSED, THE BOARD IS HOT"));

    /* the drive note, the Music folders rows, the last scan */
    RootInfo home, usb, usb2;
    home.id = 1;
    home.path = "/home/osrde/Music";
    home.label = "Music";
    home.tracks = 1204;
    usb.id = 2;
    usb.path = "/media/osrde/SANDISK";
    usb.label = "SANDISK";
    usb.online = false;
    usb.tracks = 612;
    CHECK_EQ(offline_note({home}), std::string(""));
    CHECK_EQ(offline_note({home, usb}),
             std::string("SANDISK is not connected. Its 612 tracks are hidden and come back when you plug it in."));
    usb2 = usb;
    usb2.id = 3;
    usb2.label = "STICK";
    usb2.tracks = 100;
    CHECK_EQ(offline_note({home, usb, usb2}),
             std::string("2 drives are not connected. Their 712 tracks are hidden and come back when you plug them in."));
    usb.tracks = 0; /* an offline root with no rows says nothing */
    CHECK_EQ(offline_note({home, usb}), std::string(""));
    usb.tracks = 612;
    std::vector<Row> fr = folder_rows({home, usb}, "/home/osrde");
    CHECK_EQ(fr[0].text, std::string("~/Music"));
    CHECK_EQ(fr[0].value, std::string("1,204 tracks"));
    CHECK(!fr[0].dimmed);
    CHECK_EQ(fr[1].text, std::string("SANDISK (USB drive)"));
    CHECK_EQ(fr[1].value, std::string("not connected"));
    CHECK(fr[1].dimmed);
    CHECK_EQ(fr[1].id, 2);
    CHECK(is_drive_path("/run/media/o/X"));
    CHECK(!is_drive_path("/home/o/media/x"));
    LibraryStats st;
    CHECK_EQ(last_scan_text(st, true), std::string("Not scanned yet"));
    st.last_scan = 1731450000;
    st.counts.tracks = 1204;
    st.counts.albums = 96;
    CHECK_EQ(last_scan_text(st, true), std::string("Last scan 12 Nov, 22:20: 1,204 tracks, 96 albums"));
    std::vector<Row> sett = settings_rows(2, "Last scan", false);
    CHECK_EQ(sett.size(), static_cast<size_t>(3));
    CHECK_EQ(sett[0].value, std::string("2"));
    CHECK(!sett[1].dimmed);
    CHECK(sett[2].dimmed); /* the information row is inert */
    CHECK(settings_rows(2, "x", true)[1].dimmed); /* Rescan now while scanning: inert */

    /* Track info */
    TrackDetails d;
    d.row.title = "Lisztomania";
    d.row.artist = "Phoenix";
    d.row.album = "Wolfgang Amadeus Phoenix";
    d.row.track = 1;
    d.row.disc = 1;
    d.row.duration_ms = 241000;
    d.row.codec = "mp3";
    d.row.path = "/home/osrde/Music/Phoenix/01 Lisztomania.mp3";
    d.year = 2009;
    d.genre = "Alternative";
    d.rate = 44100;
    d.kbps = 320;
    d.rg.has_track = true;
    d.rg.track_db = -7.2f;
    d.rg.has_album = true;
    d.rg.album_db = -6.8f;
    d.plays = 5;
    d.last_played = 1731447600;
    auto info = track_info(d, "/home/osrde", true);
    CHECK_EQ(info.size(), static_cast<size_t>(10));
    CHECK_EQ(info[3].second, std::string("2009, Alternative"));
    CHECK_EQ(info[4].second, std::string("1, disc 1"));
    CHECK_EQ(info[5].second, std::string("4:01"));
    CHECK_EQ(info[6].second, std::string("MP3, 44.1 kHz, 320 kbps"));
    CHECK_EQ(info[7].second, std::string("track -7.2 dB, album -6.8 dB"));
    CHECK_EQ(info[8].second, std::string("5 times, last 12 Nov 21:40"));
    CHECK_EQ(info[9].second, std::string("~/Music/Phoenix/01 Lisztomania.mp3"));
    d.row.codec = "flac";
    d.bits = 16;
    d.rate = 48000;
    CHECK_EQ(format_text(d), std::string("FLAC, 48 kHz, 16 bit"));
    d.rg = ReplayGainTags();
    d.plays = 0;
    d.row.artist = "";
    d.genre = "";
    d.year = 0;
    d.row.track = 0;
    d.row.disc = 0;
    d.row.duration_ms = 0;
    info = track_info(d, "", true);
    CHECK_EQ(info[1].second, std::string("Unknown artist"));
    CHECK_EQ(info[3].second, std::string("-"));
    CHECK_EQ(info[4].second, std::string("-"));
    CHECK_EQ(info[5].second, std::string("-"));
    CHECK_EQ(info[7].second, std::string("none"));
    CHECK_EQ(info[8].second, std::string("Never"));
    CHECK_EQ(played_text(1, 0, true), std::string("1 time"));

    /* the sheets: the rows, their keys, Add to playlist drawn and inert */
    Sheet ts = track_sheet("Lisztomania", "Phoenix");
    CHECK_EQ(ts.title, std::string("LISZTOMANIA, PHOENIX"));
    CHECK_EQ(ts.rows.size(), static_cast<size_t>(5));
    CHECK_EQ(ts.rows[0].label, std::string("Play"));
    CHECK_EQ(ts.rows[0].key, std::string("Enter"));
    CHECK_EQ(ts.rows[4].key, std::string("I"));
    CHECK(!ts.rows[sheet_index(ts, "playlist")].enabled);
    CHECK(ts.rows[sheet_index(ts, "next")].enabled);
    CHECK_EQ(ts.hint, std::string("Hold a row for 3 s, or press Ctrl+O, to open this sheet."));
    Sheet as = album_sheet("Wolfgang Amadeus Phoenix", 14);
    CHECK_EQ(as.title, std::string("WOLFGANG AMADEUS PHOENIX, 14 TRACKS"));
    CHECK_EQ(as.rows.size(), static_cast<size_t>(4));
    CHECK_EQ(as.rows[0].label, std::string("Play album"));
    CHECK_EQ(sheet_index(as, "info"), -1); /* an album has no Track info */
    Sheet rs = artist_sheet("Phoenix", 5);
    CHECK_EQ(rs.title, std::string("PHOENIX, 5 ALBUMS"));
    CHECK_EQ(sheet_action_for_key(kKeyN), std::string("next"));
    CHECK_EQ(sheet_action_for_key(kKeyA), std::string("add"));
    CHECK_EQ(sheet_action_for_key(kKeyP), std::string("playlist"));
    CHECK_EQ(sheet_action_for_key(kKeyI), std::string("info"));
    CHECK_EQ(sheet_action_for_key(16), std::string("")); /* Q */

    /* the navigation stack: Esc pops, the first place stays */
    Nav nav;
    CHECK(nav.empty());
    Loc hubl;
    hubl.place = Place::Hub;
    nav.push(hubl);
    CHECK(!nav.pop());
    Loc artist;
    artist.place = Place::Artist;
    artist.id = 4;
    artist.sel = 3;
    nav.push(artist);
    Loc album;
    album.place = Place::Album;
    nav.push(album);
    CHECK_EQ(nav.depth(), static_cast<size_t>(3));
    CHECK(nav.below() && nav.below()->place == Place::Artist);
    CHECK(nav.pop());
    CHECK(nav.top().place == Place::Artist);
    CHECK_EQ(nav.top().sel, 3);
    CHECK(nav.pop());
    CHECK(nav.top().place == Place::Hub);
    CHECK(!nav.pop());

    /* the queries the pages added: a genre's tracks, the roots with their tracks and online flags */
    std::string why;
    if (!real_sqlite_lib(&why)) {
        SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
        return;
    }
    Db db;
    CHECK(open_library_db(db, ":memory:", 416, &why));
    LibraryWriter w(db);
    const int64_t r1 = w.root("/music", "Music");
    const int64_t r2 = w.root("/media/u/STICK", "STICK", "AAAA-0000", "/media/u/STICK");
    auto add = [&](int64_t root, const std::string &name, const std::string &genre, const std::string &album) {
        TrackTags t;
        t.title = name;
        t.artist = "Ar";
        t.album = album;
        t.genre = genre;
        t.codec = "MP3";
        t.ok = true;
        TrackFile f;
        f.root_id = root;
        f.folder_id = w.folder(root, "d", 1);
        f.name = name + ".mp3";
        f.mtime = 1;
        f.size = 10;
        w.add_track(f, t);
    };
    add(r1, "a", "Rock", "X");
    add(r1, "b", "Rock", "X");
    add(r1, "c", "Rock", "Y");
    add(r2, "d", "Jazz", "Z");
    add(r2, "e", "Jazz", "Z");
    Library lib(db);
    std::vector<GenreRow> gr;
    CHECK(lib.genres(0, 40, gr));
    CHECK_EQ(gr.size(), static_cast<size_t>(2));
    for (const GenreRow &x : gr) {
        if (x.name == "Rock") CHECK(x.tracks == 3 && x.albums == 2);
        else CHECK(x.tracks == 2 && x.albums == 1);
    }
    std::vector<RootInfo> ri;
    CHECK(lib.roots_info(ri));
    CHECK_EQ(ri.size(), static_cast<size_t>(2));
    CHECK_EQ(ri[0].path, std::string("/music"));
    CHECK_EQ(ri[0].tracks, 3);
    CHECK(ri[0].online);
    CHECK_EQ(ri[1].tracks, 2);
    CHECK_EQ(ri[1].mount, std::string("/media/u/STICK"));
    CHECK(db.exec("update roots set online = 0 where id = " + std::to_string(r2)));
    CHECK(lib.roots_info(ri));
    CHECK(!ri[1].online);
    CHECK_EQ(ri[1].tracks, 2); /* the rows stay, hidden from the views */
    lib.refresh_roots();
    CHECK(lib.genres(0, 40, gr));
    CHECK_EQ(gr.size(), static_cast<size_t>(1)); /* Jazz is hidden with its drive */
}
