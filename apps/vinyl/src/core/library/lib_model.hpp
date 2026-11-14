/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library pages, the data side (P2b, 0.3.0; the canvas boards Library-480x320 / Library-320x480, Artists, Artist,
 * Albums, Album, Songs, Genres, Search, TrackInfo, TrackActions, AlbumActions, LibrarySettings, MusicFolders, Scanning,
 * ScanPaused, DriveOffline, EmptyNoMusic, EmptyNoResults): what each page lists, in plain rows, tested
 * (tests/test_lib_model.cpp); ui/library_ctl.cpp fetches the rows' data on the LibraryWorker and ui/library_pages.cpp
 * draws them. LVGL-free.
 *
 *   rows        Row: a heading (grey capitals, never selected), an item with its mark (a chevron opens, a dot acts, none
 *               informs: design QA D-45), its value (an album's year, an artist's album count, a song's artist, a track's
 *               length) and what it stands for (type, id, index in its list)
 *   paging      Pager: Songs and the long lists come 40 rows at a time; the next page is asked for once, when the
 *               selection gets near the end of what is loaded
 *   notes       the scan strip ("Scanning: 388 of about 1,200 files", "Paused: the board is hot. ...") and the drive
 *               note ("SANDISK is not connected. Its 612 tracks are hidden ..."), from the scanner's progress and roots
 *   sheets      the Track, Album and Artist actions (Enter, N, A, P, I); Add to playlist is drawn dimmed and does nothing
 *               in 0.3.0 (playlists are P2c)
 *   nav         the stack of places the Library has been through (Esc pops it)
 */
#pragma once

#include "library.hpp"
#include "scanner.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vinyl {
namespace libm {

/* "1,204" */
std::string count_text(int64_t n);
/* "5 albums", "1 album" */
std::string plural(int64_t n, const char *one, const char *many);
/* Upper case for the headings: ASCII, Latin-1 and Latin Extended-A letters, Greek and Cyrillic; every other character
 * (CJK, symbols) as it is. */
std::string upper_text(const std::string &s);
/* "04:01", "1:02:30" for an hour or more; "" for 0 */
std::string duration_text(uint64_t ms);
/* "4:01" (the Track info page's Length: no leading zero), "1:02:30" */
std::string length_text(uint64_t ms);
/* "12 Nov 21:40" (local time, or UTC for the tests) */
std::string when_text(int64_t epoch, bool utc = false);
/* "12 Nov, 22:41" */
std::string when_comma_text(int64_t epoch, bool utc = false);
/* `path` with a leading `home` shown as "~" */
std::string home_short(const std::string &path, const std::string &home);

struct Row {
    enum class Kind { Heading, Item };
    enum class Mark { None, Chevron, Dot };
    enum class Type { None, Hub, Artist, Album, Track, Genre };
    Kind kind = Kind::Item;
    Mark mark = Mark::Chevron;
    Type type = Type::None;
    std::string text, value;
    std::string key;     /* a hub row's name: "folders", "playlists", "artists", "albums", "songs", "genres" */
    int64_t id = 0;
    int index = -1;      /* the item's place in the data it came from (a search section, a track list) */
    bool dimmed = false;
    bool playing = false; /* the track playing: the blue dot */
    bool mono_value = false;
    bool operator==(const Row &o) const {
        return kind == o.kind && mark == o.mark && type == o.type && text == o.text && value == o.value && key == o.key &&
               id == o.id && index == o.index && dimmed == o.dimmed && playing == o.playing && mono_value == o.mono_value;
    }
};

/* The Hub: Folders, Playlists, Artists n, Albums n, Songs n, Genres n. The counts blank (no number) while unknown. */
std::vector<Row> hub_rows(const LibraryCounts &c, bool counts_known);
std::string hub_heading(int tracks, bool scanning);

std::vector<Row> artist_rows(const std::vector<ArtistRow> &a, int first_index = 0);
/* show_artist: Albums and a genre's albums (the artist right of the title); false: an artist's page (the year) */
std::vector<Row> album_rows(const std::vector<AlbumRow> &a, bool show_artist, int first_index = 0);
/* the tracks of an album: "1  Lisztomania" with its length; a "DISC 2" heading when the album has several discs */
std::vector<Row> album_track_rows(const std::vector<TrackRow> &t, const std::string &playing_path);
std::vector<Row> song_rows(const std::vector<TrackRow> &t, const std::string &playing_path, int first_index = 0);
std::vector<Row> genre_rows(const std::vector<GenreRow> &g, int first_index = 0);
/* Search: SONGS, ALBUMS, ARTISTS, GENRES, each with its heading, empty sections left out */
std::vector<Row> search_rows(const SearchResults &r, const std::string &playing_path);

std::string artists_heading(int n);
std::string albums_heading(int n);
std::string songs_heading(int n);
std::string genres_heading(int n);
std::string artist_heading(const std::string &name, int albums, int tracks);
std::string album_heading(const std::string &title, int year, int tracks);
std::string genre_heading(const std::string &name, int albums);
/* the names the UI gives to the unknown ones */
std::string artist_name(const std::string &n);
std::string album_name(const std::string &n);
std::string genre_name(const std::string &n);

/* the selectable rows: the first at or after i going in direction d (headings skipped), -1 if none */
int step_row(const std::vector<Row> &rows, int from, int d);
int first_row(const std::vector<Row> &rows);

/* ------------------------------------------------------------------------------------------------ paging */

class Pager {
public:
    static constexpr int kAhead = 8; /* rows before the end of what is loaded: the next page is asked for */
    explicit Pager(int page = kLibraryPage) : page_(page) {}
    void reset(int total);
    int total() const { return total_; }
    int loaded() const { return loaded_; }
    int page() const { return page_; }
    bool complete() const { return loaded_ >= total_; }
    /* true once for each page that is needed: the selected row is within kAhead of the end of what is loaded */
    bool want_more(int selected);
    /* the next page's offset and size (what to ask the library for) */
    int next_offset() const { return loaded_; }
    void appended(int n);
    void failed() { pending_ = false; }
    bool pending() const { return pending_; }

private:
    int page_, total_ = 0, loaded_ = 0;
    bool pending_ = false;
};

/* ------------------------------------------------------------------------------------------------ notes */

struct ScanNote {
    bool shown = false;
    bool hot = false;   /* paused for heat: the bar red */
    std::string text;
    int pct = -1;       /* 0..100, -1: no bar (the size is not known) */
};
/* prior_tracks: the tracks the library held before (the estimate "about 1,200"); 0 when none */
ScanNote scan_note(const ScanProgress &p, int prior_tracks);
/* "SANDISK is not connected. Its 612 tracks are hidden and come back when you plug it in." ("" when every root with
 * tracks is online) */
std::string offline_note(const std::vector<RootInfo> &roots);
/* the Library settings' strip: "SCANNING, 412 OF 1,204" */
std::string scan_heading(const ScanProgress &p, int prior_tracks);
/* the Library settings' last-scan row: "Last scan 12 Nov, 22:41: 1,204 tracks, 96 albums" or "Not scanned yet" */
std::string last_scan_text(const LibraryStats &s, bool utc = false);

/* The Music folders rows: a folder's path (home as "~"), a drive "SANDISK (USB drive)", its tracks or "not connected"
 * (dimmed). `removable`: the folders the user added (the others are Music and the drives, always included). */
std::vector<Row> folder_rows(const std::vector<RootInfo> &roots, const std::string &home);
bool is_drive_path(const std::string &path);

/* The Library settings rows (Music folders n, Rescan now, last scan). */
std::vector<Row> settings_rows(int folders, const std::string &last_scan, bool scanning);

/* ------------------------------------------------------------------------------------------------ track info */

/* The Track info page: label and value pairs (Title ... File), "-" where the library has nothing. */
std::vector<std::pair<std::string, std::string>> track_info(const TrackDetails &d, const std::string &home, bool utc = false);
std::string format_text(const TrackDetails &d);
std::string replaygain_text(const ReplayGainTags &rg);
std::string played_text(int plays, int64_t last, bool utc = false);

/* ------------------------------------------------------------------------------------------------ sheets */

/* evdev codes of the sheet's letters */
enum : uint32_t { kKeyA = 30, kKeyN = 49, kKeyP = 25, kKeyI = 23 };

struct SheetRow {
    std::string id; /* "play", "next", "add", "playlist", "info" */
    std::string label, key;
    bool enabled = true;
};
struct Sheet {
    enum class Kind { Track, Album, Artist };
    Kind kind = Kind::Track;
    std::string title; /* upper case */
    std::vector<SheetRow> rows;
    std::string hint;
    bool operator==(const Sheet &o) const {
        return kind == o.kind && title == o.title && hint == o.hint && rows.size() == o.rows.size();
    }
};
Sheet track_sheet(const std::string &title, const std::string &artist);
Sheet album_sheet(const std::string &title, int tracks);
Sheet artist_sheet(const std::string &name, int albums);
/* the action a letter key names on the sheet ("" for none); Enter is the selected row, the caller's */
std::string sheet_action_for_key(uint32_t evdev);
/* the row index of an action id, -1 when the sheet has none */
int sheet_index(const Sheet &s, const std::string &id);

/* ------------------------------------------------------------------------------------------------ navigation */

enum class Place { Hub, Artists, Artist, Albums, GenreAlbums, Album, Songs, Genres, Search, Info, Settings, Folders };
struct Loc {
    Place place = Place::Hub;
    int64_t id = 0;        /* artist, album, genre or track */
    std::string title;     /* what the page's heading names */
    int sel = 0;    /* the selected row, kept as it moves */
    int top = 0;    /* the first row in view when the place was left (given back with the selection by Esc) */
    int offset = 0;
};
class Nav {
public:
    void clear() { stack_.clear(); }
    void push(Loc l) { stack_.push_back(std::move(l)); }
    /* leaves the top place; false when it was the last one (or none) */
    bool pop();
    bool empty() const { return stack_.empty(); }
    size_t depth() const { return stack_.size(); }
    Loc &top() { return stack_.back(); }
    const Loc &top() const { return stack_.back(); }
    /* the place below the top, null at the first */
    const Loc *below() const { return stack_.size() >= 2 ? &stack_[stack_.size() - 2] : nullptr; }

private:
    std::vector<Loc> stack_;
};

}  // namespace libm
}  // namespace vinyl
