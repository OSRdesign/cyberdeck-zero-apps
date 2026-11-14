/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the playlist rows, headings and sheets of core/library/lib_model (P2c): what the Playlists page, the edit page, the
 * add-to picker, the playlist sheets and the import sheet list, in plain rows.
 */
#include "check.hpp"

#include "library/lib_model.hpp"

using namespace vinyl;
using namespace vinyl::libm;

namespace {

PlaylistRow pr(int64_t id, const std::string &name, int n) {
    PlaylistRow r;
    r.id = id;
    r.name = name;
    r.tracks = n;
    return r;
}

PlaylistEntryRow er(const std::string &artist, const std::string &title, uint64_t ms, bool exists, const std::string &path = "/m/x.mp3") {
    PlaylistEntryRow r;
    r.artist = artist;
    r.title = title;
    r.duration_ms = ms;
    r.exists = exists;
    r.entry.path = path;
    return r;
}

}  // namespace

void test_playlist_model() {
    /* the list: "+ New playlist" first (the action dot), then each with "N tracks" and a chevron */
    {
        const std::vector<Row> rows = playlist_list_rows({pr(1, "Evening", 18), pr(2, "Solo", 1), pr(3, "Empty", 0)});
        CHECK_EQ(rows.size(), static_cast<size_t>(4));
        CHECK(rows[0].type == Row::Type::NewPlaylist && rows[0].mark == Row::Mark::Dot);
        CHECK_EQ(rows[0].text, std::string("+ New playlist"));
        CHECK(rows[1].type == Row::Type::Playlist && rows[1].mark == Row::Mark::Chevron);
        CHECK_EQ(rows[1].text, std::string("Evening"));
        CHECK_EQ(rows[1].value, std::string("18 tracks"));
        CHECK_EQ(rows[1].index, 0);
        CHECK_EQ(rows[2].value, std::string("1 track"));
        CHECK_EQ(rows[3].value, std::string("0 tracks"));
        CHECK_EQ(rows[3].index, 2);
        CHECK_EQ(playlist_list_rows({}).size(), static_cast<size_t>(1));
        CHECK_EQ(playlists_heading(4), std::string("PLAYLISTS, 4"));
        CHECK_EQ(playlists_heading(0), std::string("PLAYLISTS, 0"));
    }
    /* the entries: a dot, "Artist, Title", the length; a missing file dimmed with "file not found" (never dropped) */
    {
        const std::vector<Row> rows = playlist_entry_rows({er("Phoenix", "Lisztomania", 241000, true), er("", "Old song.mp3", 0, false),
                                                           er("Amerie", "1 Thing", 221000, true, "/m/y.mp3")},
                                                          "/m/y.mp3");
        CHECK_EQ(rows.size(), static_cast<size_t>(3));
        CHECK(rows[0].type == Row::Type::Entry && rows[0].mark == Row::Mark::Dot);
        CHECK_EQ(rows[0].text, std::string("Phoenix, Lisztomania"));
        CHECK_EQ(rows[0].value, std::string("04:01"));
        CHECK(rows[0].mono_value && !rows[0].dimmed && !rows[0].playing);
        CHECK(rows[1].dimmed && !rows[1].mono_value);
        CHECK_EQ(rows[1].text, std::string("Old song.mp3"));
        CHECK_EQ(rows[1].value, std::string("file not found"));
        CHECK(rows[2].playing); /* the track heard shows its blue dot */
        CHECK_EQ(rows[2].index, 2);
    }
    CHECK_EQ(length_words(0), std::string(""));
    CHECK_EQ(length_words(60000), std::string("1 MIN"));
    CHECK_EQ(length_words(29000), std::string("0 MIN"));
    CHECK_EQ(length_words(45 * 60000), std::string("45 MIN"));
    CHECK_EQ(length_words(72 * 60000), std::string("1 H 12 MIN"));
    CHECK_EQ(length_words(120 * 60000), std::string("2 H 0 MIN"));
    {
        PlaylistStats s;
        s.tracks = 18;
        s.ms = 72 * 60000;
        CHECK_EQ(playlist_heading(s), std::string("18 TRACKS, 1 H 12 MIN"));
        s.missing = 2;
        CHECK_EQ(playlist_heading(s), std::string("18 TRACKS, 1 H 12 MIN. 2 NOT FOUND"));
        s.tracks = 1;
        s.ms = 0;
        s.missing = 0;
        CHECK_EQ(playlist_heading(s), std::string("1 TRACK"));
        s.tracks = 0;
        CHECK_EQ(playlist_heading(s), std::string("0 TRACKS"));
    }

    /* the hub while picking: only the places that hold songs */
    {
        LibraryCounts c;
        c.tracks = 10;
        c.albums = 2;
        c.artists = 3;
        c.genres = 4;
        const std::vector<Row> rows = hub_pick_rows(c, true);
        CHECK_EQ(rows.size(), static_cast<size_t>(4));
        CHECK_EQ(rows[0].key, std::string("artists"));
        CHECK_EQ(rows[3].key, std::string("genres"));
        CHECK_EQ(rows[2].value, std::string("10"));
        CHECK_EQ(hub_pick_heading("Evening"), std::string("ADD TRACKS TO EVENING"));
    }

    /* names */
    CHECK_EQ(free_name_among({}, "Queue"), std::string("Queue"));
    CHECK_EQ(free_name_among({"Queue"}, "Queue"), std::string("Queue 2"));
    CHECK_EQ(free_name_among({"queue", "QUEUE 2"}, "Queue"), std::string("Queue 3"));
    CHECK_EQ(free_name_among({"Playlist"}, "***"), std::string("Playlist 2"));
    CHECK_EQ(name_problem_text(NameProblem::Empty), std::string("Type a name first."));
    CHECK_EQ(name_problem_text(NameProblem::TooLong), std::string("Use 24 characters or fewer."));
    CHECK_EQ(name_problem_text(NameProblem::BadChar), std::string("Letters, digits, space, - and _ only."));
    CHECK_EQ(name_problem_text(NameProblem::None), std::string(""));
    CHECK_EQ(where_text("/media/u/SANDISK"), std::string("SANDISK"));
    CHECK_EQ(where_text("/media/u/SANDISK/Playlists/"), std::string("SANDISK"));
    CHECK_EQ(where_text("/mnt/stick/a/b"), std::string("stick"));
    CHECK_EQ(where_text("/home/pi/Music"), std::string("Music"));

    /* the sheets */
    {
        const Sheet ps = playlist_sheet("Evening", 18, false);
        CHECK(ps.kind == Sheet::Kind::Playlist);
        CHECK_EQ(ps.title, std::string("EVENING, 18 TRACKS"));
        CHECK_EQ(ps.rows.size(), static_cast<size_t>(4));
        CHECK_EQ(ps.rows[0].label, std::string("Play"));
        CHECK_EQ(ps.rows[0].key, std::string("Enter"));
        CHECK_EQ(ps.rows[1].label, std::string("Rename"));
        CHECK_EQ(ps.rows[2].label, std::string("Delete"));
        CHECK_EQ(ps.rows[3].label, std::string("Export M3U"));
        CHECK(ps.rows[0].enabled && ps.rows[3].enabled);
        const Sheet empty = playlist_sheet("Evening", 0, false); /* nothing to play or export: dimmed, inert */
        CHECK(!empty.rows[0].enabled && !empty.rows[3].enabled);
        CHECK(empty.rows[1].enabled && empty.rows[2].enabled);
        const Sheet es = playlist_sheet("Evening", 18, true);
        CHECK_EQ(es.rows.size(), static_cast<size_t>(5));
        CHECK_EQ(es.rows[1].label, std::string("Add tracks"));
        CHECK_EQ(sheet_action_for_key(kKeyR, ps), std::string("rename"));
        CHECK_EQ(sheet_action_for_key(kKeyD, ps), std::string("delete"));
        CHECK_EQ(sheet_action_for_key(kKeyE, ps), std::string("export"));
        CHECK_EQ(sheet_action_for_key(kKeyT, es), std::string("addtracks"));
        CHECK_EQ(sheet_action_for_key(kKeyP, ps), std::string("")); /* not this sheet's letter */
        CHECK(sheet_index(es, "addtracks") == 1 && sheet_index(ps, "addtracks") == -1);

        const Sheet is = import_sheet("Road trip.m3u8", 52);
        CHECK(is.kind == Sheet::Kind::Import);
        CHECK_EQ(is.title, std::string("ROAD TRIP.M3U8, 52 ENTRIES"));
        CHECK_EQ(is.rows.size(), static_cast<size_t>(3));
        CHECK_EQ(is.rows[0].label, std::string("Play"));
        CHECK_EQ(is.rows[1].label, std::string("Import as a playlist"));
        CHECK_EQ(is.rows[1].key, std::string("I"));
        CHECK_EQ(is.rows[2].label, std::string("Add to the queue"));
        CHECK_EQ(is.rows[2].key, std::string("A"));
        CHECK_EQ(sheet_action_for_key(kKeyI, is), std::string("import"));
        CHECK_EQ(sheet_action_for_key(kKeyA, is), std::string("add"));
        CHECK_EQ(sheet_action_for_key(kKeyN, is), std::string(""));
        CHECK_EQ(import_sheet("a.m3u", 1).title, std::string("A.M3U, 1 ENTRY"));

        const Sheet as = add_to_sheet("Lisztomania", {pr(1, "Morning run", 24), pr(2, "Evening", 18)});
        CHECK(as.kind == Sheet::Kind::AddTo);
        CHECK_EQ(as.title, std::string("ADD LISZTOMANIA TO"));
        CHECK_EQ(as.rows.size(), static_cast<size_t>(3));
        CHECK_EQ(as.rows[0].label, std::string("+ New playlist"));
        CHECK_EQ(as.rows[0].key, std::string("N"));
        CHECK_EQ(as.rows[0].id, std::string("new"));
        CHECK_EQ(as.rows[1].label, std::string("Morning run"));
        CHECK_EQ(as.rows[1].key, std::string("24 tracks"));
        CHECK_EQ(as.rows[2].id, std::string("pl:1"));
        CHECK_EQ(sheet_action_for_key(kKeyN, as), std::string("new"));
        CHECK_EQ(sheet_action_for_key(kKeyA, as), std::string(""));
        CHECK_EQ(add_to_sheet("x", {}).rows.size(), static_cast<size_t>(1));
        /* the Track, Album and Artist sheets keep their letters */
        const Sheet ts = track_sheet("T", "A");
        CHECK_EQ(sheet_action_for_key(kKeyP, ts), std::string("playlist"));
        CHECK_EQ(sheet_action_for_key(kKeyR, ts), std::string(""));
    }
}
