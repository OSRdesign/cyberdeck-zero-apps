/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the library's sort keys, its views and paging on 1,000 synthetic tracks, its search (report 060 2.3:
 * test_sortkey, test_library, test_search), on in-memory SQLite. Without libsqlite3.so.0 the database groups skip.
 */
#include "check.hpp"

#include "library/browse.hpp"
#include "library/library.hpp"
#include "library/schema.hpp"
#include "library/sortkey.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>

using namespace vinyl;

namespace {

bool have_sqlite() {
    std::string why;
    if (real_sqlite_lib(&why)) return true;
    SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
    return false;
}

TrackTags tags(const std::string &title, const std::string &artist, const std::string &album, int track = 0, int disc = 0,
               const std::string &genre = "", const std::string &date = "", const std::string &album_artist = "") {
    TrackTags t;
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.track = track;
    t.disc = disc;
    t.genre = genre;
    t.date = date;
    t.album_artist = album_artist;
    t.codec = "MP3";
    t.duration_ms = 200000;
    t.rate = 44100;
    t.channels = 2;
    t.bitrate_kbps = 192;
    t.ok = true;
    return t;
}

/* adds one track under root/rel; returns its id */
int64_t add(LibraryWriter &w, int64_t root, const std::string &rel, const std::string &name, const TrackTags &t,
            uint64_t size = 1000) {
    TrackFile f;
    f.root_id = root;
    f.folder_id = w.folder(root, rel, 1);
    f.name = name;
    f.mtime = 1700000000;
    f.size = size;
    return w.add_track(f, t);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool sorted_by_key(const std::vector<std::string> &names) {
    for (size_t i = 1; i < names.size(); ++i)
        if (sort_key(names[i - 1]) > sort_key(names[i])) return false;
    return true;
}

}  // namespace

void test_sortkey() {
    /* case and diacritics folded; ligatures and sharp s spelled out; decomposed (NFD) names too */
    CHECK_EQ(sort_key("Beyoncé"), sort_key("BEYONCE"));
    CHECK_EQ(sort_key("Mötley Crüe"), std::string("motley crue"));
    CHECK_EQ(sort_key("Straße"), std::string("strasse"));
    CHECK_EQ(sort_key("Æon Flux"), std::string("aeon flux"));
    CHECK_EQ(sort_key("Œuvre"), std::string("oeuvre"));
    CHECK_EQ(sort_key("Łódź"), std::string("lodz"));
    CHECK_EQ(sort_key("Sigur Ro\xCC\x81s"), std::string("sigur ros")); /* o + U+0301 */
    CHECK_EQ(sort_key("Şerban Ţuţea"), std::string("serban tutea"));
    CHECK_EQ(sort_key("ＡＢＣ"), std::string("abc")); /* full-width */
    CHECK_EQ(sort_key("КИНО"), sort_key("кино"));
    CHECK_EQ(sort_key("ΑΒΓ"), sort_key("αβγ"));
    /* spaces made one, leading punctuation skipped */
    CHECK_EQ(sort_key("  Pink   Floyd "), std::string("pink floyd"));
    CHECK_EQ(sort_key("\"Heroes\""), std::string("heroes\""));
    CHECK_EQ(sort_key("...And Justice for All"), std::string("and justice for all"));
    CHECK_EQ(sort_key("!!!"), std::string("!!!")); /* nothing but punctuation keeps it */
    /* articles: a leading "The " moves behind the name; "Beatles" first, then "The Beatles", then "Beatles and..." */
    CHECK_EQ(sort_key("The Beatles"), std::string("beatles\x01the"));
    CHECK(sort_key("Beatles") < sort_key("The Beatles"));
    CHECK(sort_key("The Beatles") < sort_key("Beatles and Wings"));
    CHECK(sort_key("The Beatles") < sort_key("Blur"));
    CHECK_EQ(sort_key("The"), std::string("the"));          /* the word alone stays */
    CHECK_EQ(sort_key("Theatre"), std::string("theatre")); /* not an article */
    CHECK_EQ(sort_key("THE CURE"), sort_key("The Cure"));
    /* natural numbers */
    CHECK(sort_key("Track 2") < sort_key("Track 10"));
    CHECK(sort_key("Track 02") == sort_key("Track 2"));
    CHECK(sort_key("9 Crimes") < sort_key("10,000 Days"));
    CHECK(sort_key("1999") < sort_key("Abba"));
    CHECK_EQ(sort_key("Op. 0"), std::string("op. 00000000"));
    CHECK(!(sort_key("Disc 123456789012") < sort_key("Disc 2x"))); /* a run longer than 8 digits stays as written */
    /* kana: katakana folded to hiragana; kana after Latin, Han after kana, Hangul after Han */
    CHECK_EQ(sort_key("カタカナ"), sort_key("かたかな"));
    CHECK_EQ(sort_key("ヒカル"), std::string("ひかる"));
    CHECK(sort_key("Zebra") < sort_key("あ"));
    CHECK(sort_key("ん") < sort_key("一"));
    CHECK(sort_key("龍") < sort_key("가"));
    /* Hangul in code-point (dictionary) order; Han in code-point order */
    CHECK(sort_key("가방") < sort_key("나무"));
    CHECK(sort_key("나무") < sort_key("다리"));
    CHECK(sort_key("아이유") < sort_key("이하이"));
    CHECK(sort_key("一") < sort_key("二"));
    CHECK(sort_key("坂本龍一") < sort_key("宇多田"));
    /* unknown names last */
    CHECK_EQ(sort_key(""), std::string(kUnknownSortKey));
    CHECK_EQ(sort_key("   "), std::string(kUnknownSortKey));
    CHECK(sort_key("zzz") < sort_key(""));
    CHECK(sort_key("힣") < sort_key(""));
    /* invalid UTF-8 never breaks the key */
    CHECK(!sort_key(std::string("caf\xE9", 4)).empty());
    /* lengths in characters */
    CHECK_EQ(utf8_length("abc"), static_cast<size_t>(3));
    CHECK_EQ(utf8_length("夜明け"), static_cast<size_t>(3));
    CHECK_EQ(utf8_length("艺术"), static_cast<size_t>(2));
    CHECK_EQ(utf8_length(""), static_cast<size_t>(0));
    /* the prefix range of a short query */
    std::string lo, hi;
    prefix_range("Ab", lo, hi);
    CHECK_EQ(lo, std::string("ab"));
    CHECK_EQ(hi, std::string("ab\xFF"));
    CHECK(sort_key("ABBA") >= lo && sort_key("ABBA") < hi);
    CHECK(!(sort_key("Aaron") >= lo && sort_key("Aaron") < hi));
    prefix_range("  ", lo, hi);
    CHECK(lo.empty() && hi.empty());
    prefix_range("2", lo, hi);
    CHECK(sort_key("2 Become 1") >= lo && sort_key("2 Become 1") < hi);
    CHECK(!(sort_key("25 Minutes") >= lo && sort_key("25 Minutes") < hi)); /* a number matches whole */
}

void test_library() {
    if (!have_sqlite()) return;
    Db db;
    std::string why;
    CHECK(open_library_db(db, ":memory:", 416, &why));
    CHECK_EQ(library_schema_version(db), kLibrarySchemaVersion);
    CHECK(create_library_schema(db, &why)); /* again: nothing to do */
    CHECK(meta_set(db, "k", "v1"));
    CHECK(meta_set(db, "k", "v2"));
    CHECK_EQ(meta_get(db, "k"), std::string("v2"));
    CHECK(meta_erase(db, "k"));
    CHECK_EQ(meta_get(db, "k"), std::string(""));

    /* 1,000 synthetic tracks: 50 artists, 100 albums of 10 tracks (every 5th on two discs), 8 genres; albums 90-99 on a
     * USB drive */
    static const char *const kGenres[] = {"Rock", "Jazz", "Électronique", "Folk", "Classical", "Hip-Hop", "Ambient", "Pop"};
    LibraryWriter w(db);
    const int64_t home = w.root("/music", "Music");
    const int64_t usb = w.root("/media/usb", "USB", "ABCD-1234", "/media/usb");
    CHECK(home > 0 && usb > 0 && home != usb);
    CHECK_EQ(w.root("/music", "Music"), home);
    const auto t0 = std::chrono::steady_clock::now();
    {
        Transaction tx(db);
        for (int a = 0; a < 100; ++a) {
            const std::string artist = "Artist " + std::to_string(a % 50 + 1);
            const std::string album = "Album " + std::to_string(a + 1);
            const int64_t root = a >= 90 ? usb : home;
            const std::string date = a % 7 == 0 ? "" : std::to_string(1990 + a % 30) + "-05-01";
            for (int k = 0; k < 10; ++k) {
                const int i = (k * 7) % 10; /* added out of order */
                const bool two = a % 5 == 0;
                const int disc = two ? 1 + i / 5 : 0;
                const int track = two ? i % 5 + 1 : i + 1;
                const std::string title = "Song " + std::to_string(a * 10 + i);
                add(w, root, artist + "/" + album, std::to_string(i) + " " + title + ".mp3",
                    tags(title, artist, album, track, disc, kGenres[a % 8], date));
            }
        }
        CHECK(tx.commit());
    }
    std::fprintf(stderr, "  1,000 tracks written in %.0f ms\n", ms_since(t0));
    CHECK_EQ(w.writes(), 1000);

    Library lib(db);
    CHECK(lib.all_online());
    LibraryCounts c = lib.counts();
    CHECK_EQ(c.tracks, 1000);
    CHECK_EQ(c.albums, 100);
    CHECK_EQ(c.artists, 50);
    CHECK_EQ(c.genres, 8);

    /* Artists: pages of 40, natural order ("Artist 2" before "Artist 10") */
    std::vector<ArtistRow> ar;
    CHECK(lib.artists(0, kLibraryPage, ar));
    CHECK_EQ(ar.size(), static_cast<size_t>(40));
    CHECK_EQ(ar[0].name, std::string("Artist 1"));
    CHECK_EQ(ar[1].name, std::string("Artist 2"));
    CHECK_EQ(ar[9].name, std::string("Artist 10"));
    CHECK_EQ(ar[0].albums, 2);
    std::vector<std::string> names;
    for (const ArtistRow &r : ar) names.push_back(r.name);
    std::vector<ArtistRow> ar2;
    CHECK(lib.artists(40, kLibraryPage, ar2));
    CHECK_EQ(ar2.size(), static_cast<size_t>(10));
    for (const ArtistRow &r : ar2) names.push_back(r.name);
    CHECK(sorted_by_key(names));
    CHECK(lib.artists(50, kLibraryPage, ar2));
    CHECK(ar2.empty());

    /* Albums: 40, 40, 20 */
    std::vector<AlbumRow> al, all_albums;
    for (int off = 0; off < 120; off += kLibraryPage) {
        CHECK(lib.albums(off, kLibraryPage, al));
        all_albums.insert(all_albums.end(), al.begin(), al.end());
    }
    CHECK_EQ(all_albums.size(), static_cast<size_t>(100));
    CHECK_EQ(all_albums.front().title, std::string("Album 1"));
    CHECK_EQ(all_albums[1].title, std::string("Album 2"));
    CHECK_EQ(all_albums.back().title, std::string("Album 100"));
    CHECK_EQ(all_albums.front().tracks, 10);
    CHECK_EQ(all_albums.front().artist, std::string("Artist 1"));
    CHECK_EQ(all_albums[1].year, 1991);
    CHECK_EQ(all_albums.front().year, 0); /* album 1 (a = 0) has no date */

    /* Songs: pages of 40 over all 1,000, in title order */
    std::vector<TrackRow> so;
    int seen = 0;
    std::vector<std::string> titles;
    auto t1 = std::chrono::steady_clock::now();
    for (int off = 0; off < 1000; off += kLibraryPage) {
        CHECK(lib.songs(off, kLibraryPage, so));
        CHECK_EQ(so.size(), static_cast<size_t>(40));
        seen += static_cast<int>(so.size());
        for (const TrackRow &r : so) titles.push_back(r.title);
    }
    std::fprintf(stderr, "  25 song pages in %.1f ms\n", ms_since(t1));
    CHECK_EQ(seen, 1000);
    CHECK(sorted_by_key(titles));
    CHECK_EQ(titles[0], std::string("Song 0"));
    CHECK_EQ(titles[1], std::string("Song 1"));
    CHECK_EQ(titles[10], std::string("Song 10"));
    CHECK(lib.songs(1000, kLibraryPage, so));
    CHECK(so.empty());
    CHECK(lib.songs(990, kLibraryPage, so));
    CHECK_EQ(so.size(), static_cast<size_t>(10));

    /* Genres: 8, each with its albums */
    std::vector<GenreRow> ge;
    CHECK(lib.genres(0, kLibraryPage, ge));
    CHECK_EQ(ge.size(), static_cast<size_t>(8));
    CHECK_EQ(ge[0].name, std::string("Ambient"));
    CHECK_EQ(ge[2].name, std::string("Électronique")); /* sorted as "electronique" */
    int genre_albums = 0;
    for (const GenreRow &g : ge) genre_albums += g.albums;
    CHECK_EQ(genre_albums, 100);
    const int64_t rock = std::find_if(ge.begin(), ge.end(), [](const GenreRow &g) { return g.name == "Rock"; })->id;
    CHECK(lib.genre_albums(rock, 0, kLibraryPage, al));
    CHECK_EQ(al.size(), static_cast<size_t>(13)); /* albums 1, 9, 17 ... 97 */
    CHECK_EQ(al[0].title, std::string("Album 1"));
    CHECK(lib.genre_albums(rock, 10, kLibraryPage, al));
    CHECK_EQ(al.size(), static_cast<size_t>(3));

    /* an artist's albums by year (unknown last); an album's tracks in disc and track order */
    const int64_t a1 = ar[0].id; /* Artist 1: albums 1 (no year) and 51 (2010) */
    CHECK(lib.artist_albums(a1, al));
    CHECK_EQ(al.size(), static_cast<size_t>(2));
    CHECK_EQ(al[0].title, std::string("Album 51"));
    CHECK_EQ(al[0].year, 2010);
    CHECK_EQ(al[1].title, std::string("Album 1"));
    std::vector<TrackRow> tr;
    CHECK(lib.artist_tracks(a1, tr));
    CHECK_EQ(tr.size(), static_cast<size_t>(20));
    CHECK_EQ(tr[0].album, std::string("Album 51"));
    CHECK(lib.album_tracks(al[1].id, tr)); /* album 1: two discs of 5 */
    CHECK_EQ(tr.size(), static_cast<size_t>(10));
    bool ordered = true;
    for (size_t i = 0; i < tr.size(); ++i)
        ordered = ordered && tr[i].disc == static_cast<int>(1 + i / 5) && tr[i].track == static_cast<int>(i % 5 + 1);
    CHECK(ordered);
    CHECK_EQ(tr[0].path, std::string("/music/Artist 1/Album 1/0 Song 0.mp3"));
    CHECK_EQ(tr[0].artist, std::string("Artist 1"));
    CHECK_EQ(tr[0].codec, std::string("MP3"));
    CHECK_EQ(tr[0].duration_ms, static_cast<uint64_t>(200000));
    AlbumRow one;
    CHECK(lib.album(al[1].id, one));
    CHECK_EQ(one.tracks, 10);
    ArtistRow artist_one;
    CHECK(lib.artist(a1, artist_one));
    CHECK_EQ(artist_one.name, std::string("Artist 1"));

    /* track info, the path lookup, play counts */
    const int64_t id = tr[2].id;
    CHECK_EQ(lib.find_track(tr[2].path), id);
    CHECK_EQ(lib.find_track("/music/Artist 1/Album 1/none.mp3"), -1LL);
    CHECK_EQ(lib.find_track("/elsewhere/x.mp3"), -1LL);
    for (int i = 0; i < 5; ++i) CHECK(lib.record_play(id, 1763000000 + i));
    CHECK(lib.record_skip(id));
    TrackInfo info;
    CHECK(lib.track(id, info));
    CHECK_EQ(info.plays, 5);
    CHECK_EQ(info.skips, 1);
    CHECK_EQ(info.last_played, 1763000004LL);
    CHECK_EQ(info.genre, std::string("Rock"));
    CHECK_EQ(info.album_artist, std::string("Artist 1"));
    CHECK_EQ(info.root_label, std::string("Music"));
    CHECK_EQ(info.rate, 44100u);
    CHECK_EQ(info.kbps, 192u);
    CHECK_EQ(info.size, static_cast<uint64_t>(1000));
    CHECK(!info.rg.has_track);
    CHECK(info.online);
    CHECK(!lib.track(999999, info));

    LibraryStats st = lib.stats();
    CHECK_EQ(st.counts.tracks, 1000);
    CHECK_EQ(st.duration_ms, static_cast<uint64_t>(1000) * 200000);
    CHECK_EQ(st.bytes, static_cast<uint64_t>(1000) * 1000);
    CHECK_EQ(st.roots, 2);
    CHECK_EQ(st.offline_roots, 0);
    CHECK(st.db_bytes > 0);

    /* an offline drive: its rows hidden from every view, not deleted */
    CHECK(db.exec("update roots set online = 0 where id = " + std::to_string(usb)));
    lib.refresh_roots();
    CHECK(!lib.all_online());
    c = lib.counts();
    CHECK_EQ(c.tracks, 900);
    CHECK_EQ(c.albums, 90);
    CHECK_EQ(c.artists, 50);
    CHECK(lib.albums(80, kLibraryPage, al));
    CHECK_EQ(al.size(), static_cast<size_t>(10));
    CHECK(lib.songs(880, kLibraryPage, so));
    CHECK_EQ(so.size(), static_cast<size_t>(20));
    CHECK_EQ(lib.stats().offline_roots, 1);
    CHECK_EQ(db.scalar("select count(*) from tracks"), 1000LL);
    CHECK(db.exec("update roots set online = 1"));
    lib.refresh_roots();
    CHECK_EQ(lib.counts().tracks, 1000);

    /* writes: a changed tag moves a track; artists, albums and genres left empty are pruned */
    {
        Transaction tx(db);
        TrackFile f;
        f.root_id = home;
        f.folder_id = w.find_folder(home, "Artist 1/Album 1");
        CHECK(f.folder_id > 0);
        f.name = "0 Song 0.mp3";
        f.mtime = 1700000001;
        f.size = 2000;
        CHECK(w.update_track(tr[0].id, f, tags("Renamed", "Newcomer", "Debut", 1, 0, "Zydeco", "2024")));
        CHECK(tx.commit());
    }
    lib.refresh_roots();
    CHECK(lib.track(tr[0].id, info));
    CHECK_EQ(info.row.title, std::string("Renamed"));
    CHECK_EQ(info.row.artist, std::string("Newcomer"));
    CHECK_EQ(info.year, 2024);
    c = lib.counts();
    CHECK_EQ(c.artists, 51);
    CHECK_EQ(c.albums, 101);
    CHECK_EQ(c.genres, 9);
    SearchResults sr;
    CHECK(lib.search("Renamed", sr));
    CHECK_EQ(sr.songs.size(), static_cast<size_t>(1));
    CHECK(lib.search("Song 0", sr)); /* the old FTS row went with the update */
    CHECK(std::none_of(sr.songs.begin(), sr.songs.end(), [&](const TrackRow &r) { return r.id == tr[0].id; }));
    {
        Transaction tx(db);
        CHECK(w.delete_track(tr[0].id));
        CHECK(w.prune());
        CHECK(tx.commit());
    }
    c = lib.counts();
    CHECK_EQ(c.tracks, 999);
    CHECK_EQ(c.artists, 50);
    CHECK_EQ(c.genres, 8);
    CHECK_EQ(db.scalar("select count(*) from artists where name = 'Newcomer'"), 0LL);
    CHECK_EQ(db.scalar("select count(*) from stats where track_id = " + std::to_string(tr[0].id)), 0LL);
    {
        Transaction tx(db);
        CHECK(w.delete_folder(w.find_folder(home, "Artist 1/Album 1")));
        CHECK(w.delete_root(usb));
        CHECK(w.prune());
        CHECK(tx.commit());
    }
    lib.refresh_roots();
    c = lib.counts();
    CHECK_EQ(c.tracks, 999 - 9 - 100);
    CHECK_EQ(c.albums, 89);
    CHECK_EQ(db.scalar("select count(*) from roots"), 1LL);
    CHECK(lib.search("Song 905", sr)); /* album 91, on the drive */
    CHECK(sr.songs.empty());

    /* the worker: jobs in order on its own thread, done callbacks on ours; a newer keyed job replaces a waiting one */
    const std::string dir = make_temp_dir("libworker");
    {
        LibraryWorker wk;
        wk.start(dir + "/sub/library.db", 416);
        std::promise<void> gate;
        std::shared_future<void> open_gate = gate.get_future().share();
        int ran = 0, first = 0, second = 0, third = 0;
        bool had_lib = false;
        wk.post([open_gate, &had_lib](Library *l) {
            had_lib = l != nullptr;
            open_gate.wait();
        });
        wk.post([](Library *) {}, [&] { ++first; }, "search");
        wk.post([](Library *) {}, [&] { ++second; }, "search"); /* replaces the waiting one */
        auto counts_out = std::make_shared<LibraryCounts>();
        wk.post([counts_out](Library *l) { *counts_out = l->counts(); }, [&] { ++third; });
        gate.set_value();
        wk.wait_idle();
        ran = wk.drain();
        CHECK(had_lib);
        CHECK_EQ(ran, 2);
        CHECK_EQ(first, 0);
        CHECK_EQ(second, 1);
        CHECK_EQ(third, 1);
        CHECK_EQ(counts_out->tracks, 0);
        CHECK(wk.gen() >= 3u);
        CHECK(wk.open_error().empty());
        CHECK_EQ(wk.drain(), 0);
    }
    CHECK(file_exists(dir + "/sub/library.db"));
    {
        /* a database that cannot be made: every job gets no library, and the reason is kept */
        LibraryWorker wk;
        wk.start("/proc/vinyl-no-such/library.db", 416);
        bool null_lib = false;
        wk.post([&](Library *l) { null_lib = l == nullptr; });
        wk.wait_idle();
        CHECK(null_lib);
        CHECK(!wk.open_error().empty());
    }
    /* a damaged file is moved aside and made again */
    write_file(dir + "/bad.db", std::string(4096, 'x'));
    {
        Db d2;
        CHECK(open_library_db(d2, dir + "/bad.db", 416, &why));
        CHECK_EQ(library_schema_version(d2), kLibrarySchemaVersion);
    }
    CHECK(file_exists(dir + "/bad.db.bad"));
    /* a newer schema is left alone */
    {
        Db d3;
        CHECK(d3.open(dir + "/newer.db"));
        CHECK(d3.exec("pragma user_version = 99"));
    }
    {
        Db d4;
        CHECK(!open_library_db(d4, dir + "/newer.db", 416, &why));
        CHECK(why.find("newer") != std::string::npos);
    }
    rm_tree(dir);
}

void test_search() {
    if (!have_sqlite()) return;
    Db db;
    std::string why;
    CHECK(open_library_db(db, ":memory:", 416, &why));
    LibraryWriter w(db);
    const int64_t root = w.root("/m", "Music");
    const int64_t usb = w.root("/media/usb", "USB");
    {
        Transaction tx(db);
        add(w, root, "a", "1.mp3", tags("Crazy in Love", "Beyoncé", "Dangerously in Love", 1, 0, "R&B"));
        add(w, root, "b", "1.mp3", tags("Kickstart My Heart", "Mötley Crüe", "Dr. Feelgood", 1, 0, "Rock"));
        add(w, root, "c", "1.mp3", tags("Caecilia", "Fennesz", "Endless Summer", 1, 0, "Électronique"));
        add(w, root, "d", "1.mp3", tags("Dancing Queen", "ABBA", "Arrival", 1, 0, "Pop"));
        add(w, root, "e", "1.mp3", tags("Come Together", "The Beatles", "Abbey Road", 1, 0, "Rock"));
        add(w, root, "e", "2.mp3", tags("Abracadabra", "Steve Miller Band", "Abracadabra", 1, 0, "Rock"));
        add(w, root, "f", "1.mp3", tags("夜明けのスキャット", "由紀さおり", "夜明けのスキャット", 1, 0, "歌謡曲"));
        add(w, root, "g", "1.mp3", tags("艺术家", "坂本龍一", "音楽図鑑", 1, 0));
        add(w, root, "h", "1.mp3", tags("좋은 날", "아이유", "Real", 1, 0, "K-Pop"));
        add(w, root, "i", "1.mp3", tags("Crème brûlée", "Les Chefs", "Desserts", 1, 0));
        add(w, root, "j", "1.mp3", tags("Say \"hi\"", "Quote Band", "Quotes", 1, 0));
        add(w, root, "k", "1.mp3", tags("Hidden Gem", "Offline Artist", "Offline Album", 1, 0, "Drone"));
        for (int i = 0; i < 120; ++i)
            add(w, root, "loops", std::to_string(i) + ".mp3", tags("Loop " + std::to_string(i), "Looper", "Loops", i + 1));
        add(w, usb, "", "1.mp3", tags("Fenland", "Drive Band", "On the stick", 1, 0));
        CHECK(tx.commit());
    }
    Library lib(db);
    SearchResults r;
    auto has_artist = [&](const std::string &n) {
        return std::any_of(r.artists.begin(), r.artists.end(), [&](const ArtistRow &a) { return a.name == n; });
    };
    auto has_song = [&](const std::string &n) {
        return std::any_of(r.songs.begin(), r.songs.end(), [&](const TrackRow &a) { return a.title == n; });
    };
    auto has_album = [&](const std::string &n) {
        return std::any_of(r.albums.begin(), r.albums.end(), [&](const AlbumRow &a) { return a.title == n; });
    };

    /* trigram: case and accents ignored */
    CHECK(lib.search("beyonce", r));
    CHECK(!r.prefix);
    CHECK(has_artist("Beyoncé"));
    CHECK(r.songs.empty()); /* Songs match titles */
    CHECK(lib.search("MOTLEY", r));
    CHECK(has_artist("Mötley Crüe"));
    CHECK(lib.search("fen", r)); /* three characters: the shortest trigram */
    CHECK(has_artist("Fennesz"));
    CHECK(has_song("Fenland"));
    CHECK(lib.search("creme", r));
    CHECK(has_song("Crème brûlée"));
    CHECK(lib.search("BRULEE", r));
    CHECK(has_song("Crème brûlée"));
    CHECK(lib.search("dancing", r));
    CHECK(has_song("Dancing Queen"));
    CHECK(lib.search("summer", r));
    CHECK(has_album("Endless Summer"));
    CHECK(lib.search("electro", r));
    CHECK_EQ(r.genres.size(), static_cast<size_t>(1));
    CHECK_EQ(r.genres[0].name, std::string("Électronique"));
    CHECK_EQ(r.genres[0].albums, 1);
    CHECK(lib.search("beatles", r)); /* the article stays searchable */
    CHECK(has_artist("The Beatles"));
    /* CJK runs of three characters or more */
    CHECK(lib.search("明けの", r));
    CHECK(has_song("夜明けのスキャット"));
    CHECK(has_album("夜明けのスキャット"));
    CHECK(lib.search("夜明け", r));
    CHECK(has_song("夜明けのスキャット"));
    CHECK(lib.search("由紀さ", r));
    CHECK(has_artist("由紀さおり"));
    CHECK(lib.search("음악 없음", r));
    CHECK(r.empty());
    /* the prefix fallback for 1-2 characters: the start of the names */
    CHECK(lib.search("ab", r));
    CHECK(r.prefix);
    CHECK(has_artist("ABBA"));
    CHECK(has_album("Abbey Road"));
    CHECK(has_song("Abracadabra"));
    CHECK(!has_song("Crazy in Love"));
    CHECK(lib.search("Be", r));
    CHECK(has_artist("Beyoncé"));
    CHECK(has_artist("The Beatles")); /* "Beatles, the" */
    CHECK(lib.search("艺术", r));
    CHECK(r.prefix);
    CHECK(has_song("艺术家"));
    CHECK(lib.search("術", r)); /* not at the start: no match under three characters */
    CHECK(r.songs.empty());
    CHECK(lib.search("아", r));
    CHECK(has_artist("아이유"));
    CHECK(lib.search("é", r)); /* "e": Endless Summer */
    CHECK(has_album("Endless Summer"));
    CHECK(lib.search("カ", r)); /* katakana query, hiragana-folded keys: nothing starts with か here */
    CHECK(r.empty());
    /* 50 results per section */
    CHECK(lib.search("loop", r));
    CHECK_EQ(r.songs.size(), static_cast<size_t>(50));
    CHECK_EQ(r.songs[0].title, std::string("Loop 0"));
    CHECK_EQ(r.songs[1].title, std::string("Loop 1"));
    CHECK(lib.search("loop", r, 10));
    CHECK_EQ(r.songs.size(), static_cast<size_t>(10));
    CHECK(lib.search("lo", r));
    CHECK_EQ(r.songs.size(), static_cast<size_t>(50));
    /* odd queries: quotes, FTS syntax, empty */
    CHECK(lib.search("say \"hi\"", r));
    CHECK(has_song("Say \"hi\""));
    CHECK(lib.search("AND OR NOT *", r));
    CHECK(r.empty());
    CHECK(lib.search("   ", r));
    CHECK(r.empty());
    CHECK(lib.search("", r));
    CHECK(r.empty());
    /* an offline drive's tracks leave the results */
    CHECK(db.exec("update roots set online = 0 where id = " + std::to_string(usb)));
    lib.refresh_roots();
    CHECK(lib.search("fen", r));
    CHECK(!has_song("Fenland"));
    CHECK(has_artist("Fennesz"));
    CHECK(lib.search("drive band", r));
    CHECK(r.artists.empty());
    /* timing at this size (the board numbers come from B41) */
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) lib.search("love", r);
    std::fprintf(stderr, "  search: %.2f ms a query on 133 tracks\n", ms_since(t0) / 20);
}
