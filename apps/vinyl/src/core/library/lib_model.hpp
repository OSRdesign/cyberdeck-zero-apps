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
 *   sheets      the Track, Album and Artist actions (Enter, N, A, P, I); P2c: Add to playlist is real, and the playlist sheets
 *               (Play, Rename, Delete, Export M3U), the import sheet of a .m3u in Folders and the add-to picker
 *   playlists   (P2c) the Playlists page's rows ("+ New playlist", names with counts), the edit page's rows and heading, the
 *               hub while tracks are picked for a playlist
 *   nav         the stack of places the Library has been through (Esc pops it)
 */
#pragma once

#include "../drives/drives.hpp"
#include "../playlist/playlist_index.hpp"
#include "library.hpp"
#include "scanner.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <unordered_map>
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
    enum class Type { None, Hub, Artist, Album, Track, Genre, Playlist, NewPlaylist, Entry };
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
    bool amber = false; /* the action dot is amber: a drive that is plugged in and can be mounted from here */
    int check = 0;      /* Add tracks: the track is already in the playlist being edited this many times */
    /* 0.8.0, network shares: the glyph at the row's left (the small server: teal, or amber and slashed when the share does
     * not answer; the "+" of "Add a network folder"), a second line ("192.168.68.142 / home-share"), amber when it says the
     * share does not answer, and the teal text of the Add row */
    enum class Glyph { None, Share, ShareDown, Add };
    Glyph glyph = Glyph::None;
    std::string sub;
    bool sub_amber = false;
    bool accent = false;
    bool operator==(const Row &o) const {
        return kind == o.kind && mark == o.mark && type == o.type && text == o.text && value == o.value && key == o.key &&
               id == o.id && index == o.index && dimmed == o.dimmed && playing == o.playing && mono_value == o.mono_value && amber == o.amber && check == o.check &&
               glyph == o.glyph && sub == o.sub && sub_amber == o.sub_amber && accent == o.accent;
    }
};

/* The Hub: Folders, Playlists, Artists n, Albums n, Songs n, Genres n. The counts blank (no number) while unknown. */
std::vector<Row> hub_rows(const LibraryCounts &c, bool counts_known);
std::string hub_heading(int tracks, bool scanning);
/* the hub while tracks are being picked for a playlist (Add tracks): Artists, Albums, Songs, Genres only (Folders and Playlists
 * are not places to pick from) and its heading */
std::vector<Row> hub_pick_rows(const LibraryCounts &c, bool counts_known);
/* "ADDING TO EVENING, 18 TRACKS": every page of the pick mode that has a heading of its own for it (the board AddTracksSongs) */
std::string hub_pick_heading(const std::string &playlist, int tracks);

/* The marks of the pick mode (owner, 2026-11-15: "we need an indicator in lists that songs are already part of the playlist being
 * created/edited"; a tap adds it again and the number grows): how many times each track (by path) is in the playlist, counted once from the
 * playlist's entries (missing ones count for their path) and then kept up to date as tracks are added. A hash map: a lookup per row, never a scan.
 * The rule for the rows: a SONG row (Songs, Search, an album's tracks) shows the check, with "x2" from two; an album, artist or genre
 * row shows nothing (a mark there would need every track of the row looked up). */
class PickMarks {
public:
    void set(const std::vector<std::string> &paths);
    void add(const std::vector<std::string> &paths);
    void remove(const std::string &path); /* one occurrence (a failed add taken back) */
    int count(const std::string &path) const;
    int total() const { return total_; }
    void clear();

private:
    std::unordered_map<std::string, int> n_;
    int total_ = 0;
};
/* sets Row::check of the song rows: `list` is the track list the rows' index points into */
void apply_marks(std::vector<Row> &rows, const std::vector<TrackRow> &list, const PickMarks &marks);

/* ------------------------------------------------------------------------------------------------ playlists (P2c) */
/* the Playlists page: "+ New playlist" first (an action: the dot), then each playlist with "N tracks" and a chevron */
std::vector<Row> playlist_list_rows(const std::vector<PlaylistRow> &p);
std::string playlists_heading(int n);
/* a playlist's entries as the edit page shows them: a dot (Enter plays from here), "Artist, Title" and its length; an entry whose
 * file is not there is dimmed and says "file not found" (never dropped) */
std::vector<Row> playlist_entry_rows(const std::vector<PlaylistEntryRow> &e, const std::string &playing_path);
/* "1 H 12 MIN", "45 MIN", "" for none */
std::string length_words(uint64_t ms);
/* "18 TRACKS, 1 H 12 MIN" with ". 2 NOT FOUND" when some files are not there */
std::string playlist_heading(const PlaylistStats &s);
/* where an export went, for "Exported to SANDISK: Evening.m3u8.": the drive's name for a folder on a drive, else the folder's name */
std::string where_text(const std::string &folder);
/* the lowest free name among `names` (without case): "Queue", "Queue 2" ... within 24 characters */
std::string free_name_among(const std::vector<std::string> &names, const std::string &base);
/* the notice a bad name gets */
std::string name_problem_text(NameProblem p);

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
/* the Library settings' strip: "SCANNING, 412 OF 1,204" */
std::string scan_heading(const ScanProgress &p, int prior_tracks);
/* the Library settings' last-scan row: "Last scan 12 Nov, 22:41: 1,204 tracks, 96 albums" or "Not scanned yet" */
std::string last_scan_text(const LibraryStats &s, bool utc = false);

/* 0.8.0: what the Music folders page knows of the network shares (core/shares, filled by the host): the helper's shares
 * added from Vinyl (each at its root, /mnt/cyberdeck/<name>), the roots that do not answer (the helper's and the system's
 * network mounts alike), and whether the helper is there (Add acts) */
struct ShareRow {
    std::string name, where, root; /* "home-share", "192.168.68.142 / home-share", "/mnt/cyberdeck/home-share" */
    bool down = false;             /* it does not answer (the bounded probe), or the system's automount of it failed */
};
struct ShareInfo {
    bool known = false;       /* the helper was asked */
    bool available = false;   /* it is there and answers "ok 1" */
    std::vector<ShareRow> shares;
    std::vector<std::string> down_roots; /* every network root that does not answer now */
    uint64_t gen = 0;         /* changes with anything above */
};
/* The Music folders rows of 0.8.0's shares: a root that is a helper's share reads its name, "host / share" under it and its
 * tracks, the server glyph (teal), a chevron and the key "share:<name>"; one that does not answer reads "Not answering: N
 * tracks kept" in amber with the slashed glyph and no count. A helper's share without a root yet (just added, or its
 * automount failed) still has its row (index -1). A root on the system's own network mount keeps its path and gets the
 * glyph. The last row is "Add a network folder" (the "+" glyph, teal; key "addshare"), dimmed with the reason under it when
 * the helper is not there. */
void apply_shares(std::vector<Row> &rows, const std::vector<RootInfo> &roots, const ShareInfo &shares);

/* The Music folders rows (MusicFoldersDrives and MusicFoldersDriveMount boards): a folder's path (home as "~"), a drive "SANDISK (USB drive)" with its
 * tracks, or "mounted by Vinyl" (key "vdrive:<dev>": Eject acts on it), a drive the system mounted (key "sdrive:<dev>":
 * Eject is inert), and the drives that are plugged in and not mounted ("not mounted" with the amber action dot, key "ndrive:<dev>": the button Mount acts on it). A root
 * that is gone (offline: a pulled drive, a stale mount) has no row at all (owner ruling 2026-11-14, B45); a row's `index` is
 * its place in `roots` (-1 for a drive that is not mounted). */
std::vector<Row> folder_rows(const std::vector<RootInfo> &roots, const std::string &home, const std::vector<DriveInfo> &drives = {});
/* the roots that are there (not offline) */
int online_roots(const std::vector<RootInfo> &roots);
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
enum : uint32_t { kKeyA = 30, kKeyN = 49, kKeyP = 25, kKeyI = 23, kKeyR = 19, kKeyD = 32, kKeyE = 18, kKeyT = 20 };

struct SheetRow {
    std::string id; /* "play", "next", "add", "playlist", "info" */
    std::string label, key;
    bool enabled = true;
};
struct Sheet {
    enum class Kind { Track, Album, Artist, Playlist, Import, AddTo, PlaylistPage };
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
/* P2c: the playlist's actions (Play, Rename, Delete, Export; `editing`: the open playlist's page, which adds Add tracks), the
 * sheet of a .m3u in the Folders browser (Play, Import as a playlist, Add to the queue) and the picker "ADD <WHAT> TO" (+ New
 * playlist, then each playlist with its count). */
Sheet playlist_sheet(const std::string &name, int tracks, bool editing);
Sheet import_sheet(const std::string &file_name, int entries);
/* the options of an open playlist's page (hold a row, or Ctrl+O, in the view and in the edit mode; PlaylistOptions): Play from here (Enter),
 * Rename (N), Export M3U (E), Delete playlist (Del); `can_play_here`: the selected row is a file that is there */
Sheet playlist_page_sheet(const std::string &name, int tracks, bool can_play_here);
/* "EDITING EVENING, 18 TRACKS" */
std::string playlist_edit_heading(const std::string &name, int tracks);
/* `has[i]`: how many times the song is in playlists[i] ("in it, 18 tracks", "in it x2, 31 tracks"); empty: none (an album or an artist) */
Sheet add_to_sheet(const std::string &what, const std::vector<PlaylistRow> &playlists, const std::vector<int> &has = {});
/* the action a letter key names on the sheet ("" for none); Enter is the selected row, the caller's */
std::string sheet_action_for_key(uint32_t evdev);
/* the same for a given sheet (the playlist sheets have their own letters: R, D, E, T; the import sheet's I imports) */
std::string sheet_action_for_key(uint32_t evdev, const Sheet &s);
/* the row index of an action id, -1 when the sheet has none */
int sheet_index(const Sheet &s, const std::string &id);

/* ------------------------------------------------------------------------------------------------ navigation */

enum class Place { Hub, Artists, Artist, Albums, GenreAlbums, Album, Songs, Genres, Search, Info, Settings, Folders, Playlists, Playlist };
struct Loc {
    Place place = Place::Hub;
    int64_t id = 0;        /* artist, album, genre or track */
    std::string title;     /* what the page's heading names */
    int sel = 0;    /* the selected row, kept as it moves */
    int top = 0;    /* where the list was scrolled to (px) when the place was left (given back with the selection by Esc) */
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
