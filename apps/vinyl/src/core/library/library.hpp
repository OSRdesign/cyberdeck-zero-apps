/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: what the Library pages show, read from the database (report 060 3a, "Views" and "Search"; P2b).
 *
 *   Artists   album artists, else the track artist (each album's artist_id), A-Z by sort key, pages of 40
 *   artist    the artist's albums by year (unknown years last), with the albums the artist appears on (a compilation's
 *             track artist); artist_tracks() for playing them all
 *   Albums    A-Z (then by artist), pages of 40; keyed by sort key and album artist, so CD1 / CD2 folders join
 *   album     its tracks in disc, folder, track order
 *   Songs     every track A-Z by title, pages of 40 (LIMIT / OFFSET: 2.6 ms at offset 5,000 on zero8)
 *   Genres    A-Z, pages of 40; a genre leads to its albums
 *   Search    FTS5 trigram over title, artist, album, album artist and genre (case and accents ignored, CJK runs of three
 *             characters or more); a query of 1-2 characters matches the start of the sort keys instead (an index
 *             range). Up to 50 results per section: Artists, Albums, Songs (by title), Genres.
 *   counts, stats, track info (tags, file, play count), play counts (record_play / record_skip)
 * A drive that is not mounted keeps its rows, hidden from every view (roots.online, read by refresh_roots()).
 *
 * Threads: a Library runs its queries on the calling thread, on its own connection. The UI never calls it directly: it
 * posts jobs to a LibraryWorker (one thread, its own connection) and takes the results back on its own thread with
 * drain(), so no page waits for an SD card (D-63). Playback never touches the database. LibraryWriter is the one place
 * rows are written (the scanner; the tests build synthetic libraries with it). LVGL-free.
 */
#pragma once

#include "../db/sqlite_lib.hpp"
#include "../tags/tag_reader.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {

constexpr int kLibraryPage = 40;        /* rows per page of the long lists */
constexpr int kSearchSectionLimit = 50; /* results per search section */

struct ArtistRow {
    int64_t id = 0;
    std::string name; /* "" = unknown artist (the UI names it) */
    int albums = 0;   /* albums with this album artist */
};

struct AlbumRow {
    int64_t id = 0;
    std::string title; /* "" = unknown album */
    int64_t artist_id = 0;
    std::string artist; /* the album artist */
    int year = 0;       /* 0 = unknown */
    int tracks = 0;
};

struct GenreRow {
    int64_t id = 0;
    std::string name;
    int albums = 0;
    int tracks = 0;
};

/* A library folder as the Music folders page lists it (P2b UI). */
struct RootInfo {
    int64_t id = 0;
    std::string path, label, mount;
    bool online = true;
    int tracks = 0;       /* the rows kept for it (hidden while it is offline) */
    int64_t last_scan = 0;
};

struct TrackRow {
    int64_t id = 0;
    std::string path;  /* absolute: what the queue plays */
    std::string title; /* the tag, else the file name without its extension */
    std::string artist, album;
    int64_t artist_id = 0, album_id = 0;
    int track = 0, disc = 0;
    uint64_t duration_ms = 0;
    std::string codec;
};

struct TrackDetails {
    TrackRow row;
    std::string album_artist, genre, root_label;
    int year = 0;
    uint32_t rate = 0;
    unsigned bits = 0, channels = 0, kbps = 0;
    ReplayGainTags rg;
    bool has_art = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    int plays = 0, skips = 0;
    int64_t last_played = 0; /* seconds since the epoch, 0 = never */
    bool online = true;      /* its drive is mounted */
};

struct LibraryCounts {
    int tracks = 0, albums = 0, artists = 0, genres = 0;
};

struct LibraryStats {
    LibraryCounts counts;
    uint64_t duration_ms = 0; /* of the visible tracks */
    uint64_t bytes = 0;       /* their files */
    int64_t last_scan = 0;    /* the latest finished scan of a root, seconds since the epoch, 0 = never */
    int roots = 0, offline_roots = 0;
    uint64_t db_bytes = 0; /* the database's own size (pages) */
};

struct SearchResults {
    std::string query;   /* as searched (trimmed) */
    bool prefix = false; /* the 1-2 character fallback was used */
    std::vector<ArtistRow> artists;
    std::vector<AlbumRow> albums;
    std::vector<TrackRow> songs;
    std::vector<GenreRow> genres;
    bool empty() const { return artists.empty() && albums.empty() && songs.empty() && genres.empty(); }
};

/* The queries. Every list call clears `out`, fills it and returns false on a database error (or an interrupt). */
class Library {
public:
    explicit Library(Db &db);
    Db &db() { return db_; }

    /* Reads which roots are online (call after a scan or a change of drives; the worker does before every job). */
    void refresh_roots();
    bool all_online() const { return offline_filter_.empty(); }

    bool artists(int offset, int limit, std::vector<ArtistRow> &out);
    bool albums(int offset, int limit, std::vector<AlbumRow> &out);
    bool songs(int offset, int limit, std::vector<TrackRow> &out);
    bool genres(int offset, int limit, std::vector<GenreRow> &out);

    bool artist_albums(int64_t artist_id, std::vector<AlbumRow> &out);
    bool artist_tracks(int64_t artist_id, std::vector<TrackRow> &out);
    bool album_tracks(int64_t album_id, std::vector<TrackRow> &out);
    bool genre_albums(int64_t genre_id, int offset, int limit, std::vector<AlbumRow> &out);
    bool album(int64_t album_id, AlbumRow &out);
    bool artist(int64_t artist_id, ArtistRow &out);

    bool track(int64_t track_id, TrackDetails &out);
    /* The track of an absolute path (the queue's), or -1. */
    int64_t find_track(const std::string &path);
    /* The row of the track at an absolute path, even when its drive is away (a playlist shows it dimmed); false when the
     * library does not know the file. */
    bool track_by_path(const std::string &path, TrackRow &out);

    LibraryCounts counts();
    LibraryStats stats();
    /* Every root with its track count and online flag, in the order they were first seen. */
    bool roots_info(std::vector<RootInfo> &out);

    /* Play counts: a track played to its end (when: seconds since the epoch); a track skipped. One small write each. */
    bool record_play(int64_t track_id, int64_t when);
    bool record_skip(int64_t track_id);

    /* Search (see the top of the file); limit per section. */
    bool search(const std::string &query, SearchResults &out, int limit = kSearchSectionLimit);

private:
    struct RootRef {
        int64_t id;
        std::string path;
        bool online;
    };
    Db &db_;
    std::vector<RootRef> roots_;
    std::string offline_filter_; /* " and t.root_id not in (...)" when a root is offline */
};

/* A file as the scanner found it. */
struct TrackFile {
    int64_t root_id = 0, folder_id = 0;
    std::string name; /* in its folder */
    int64_t mtime = 0;
    uint64_t size = 0;
};

/* Every write of the library: rows of tracks with their artists, albums and genres (made on first use, one per sort
 * key) and their FTS rows, in the caller's transaction. */
class LibraryWriter {
public:
    explicit LibraryWriter(Db &db);
    Db &db() { return db_; }

    /* A root by path (made when new). -1 on error. */
    int64_t root(const std::string &path, const std::string &label, const std::string &uuid = "",
                 const std::string &mount = "");
    /* A folder of a root ("" = the root's top), made when new; its mtime kept up to date. -1 on error. */
    int64_t folder(int64_t root_id, const std::string &rel, int64_t mtime);
    /* The folder's id or -1 (never makes one). */
    int64_t find_folder(int64_t root_id, const std::string &rel);

    int64_t add_track(const TrackFile &f, const TrackTags &t);
    bool update_track(int64_t id, const TrackFile &f, const TrackTags &t);
    bool delete_track(int64_t id);
    bool delete_folder(int64_t folder_id);
    bool delete_root(int64_t root_id);
    /* Albums, artists and genres no track uses any more. */
    bool prune();

    /* Rows written (tracks added, changed or removed) since the last reset_writes(): the scanner's batch counter. */
    int writes() const { return writes_; }
    void reset_writes() { writes_ = 0; }

private:
    int64_t artist_id(const std::string &name);
    int64_t album_id(const std::string &title, int64_t artist, int year);
    int64_t genre_id(const std::string &name);
    bool fts_insert(int64_t id, const std::string &title, const TrackTags &t);
    bool bind_track(Stmt &s, const TrackFile &f, const TrackTags &t, std::string &title);
    Stmt &prep(Stmt &s, const char *sql);

    Db &db_;
    int writes_ = 0;
    Stmt find_artist_, add_artist_, find_album_, add_album_, album_year_, find_genre_, add_genre_;
    Stmt add_track_, update_track_, del_track_, del_fts_, del_stats_, add_fts_, find_folder_, add_folder_, folder_mtime_;
};

/* The library's worker thread: opens the database on its thread and runs the posted jobs one at a time, in order. */
class LibraryWorker {
public:
    /* lib is null when the database could not be opened (open_error() says why). */
    using Job = std::function<void(Library *lib)>;
    using Done = std::function<void()>;

    LibraryWorker() = default;
    ~LibraryWorker();
    LibraryWorker(const LibraryWorker &) = delete;
    LibraryWorker &operator=(const LibraryWorker &) = delete;

    /* Starts the thread, which opens `db_path` (made when missing). Returns at once. */
    void start(const std::string &db_path, int ram_mb);
    /* Queues a job; `done` (may be empty) is run by drain() on the caller's thread after the job. A non-empty `key`
     * replaces a waiting job of the same key (a newer search replaces an older one) and interrupts a running one; the
     * replaced job's `done` never runs. */
    void post(Job job, Done done = nullptr, const std::string &key = "");
    /* Runs the `done` of the finished jobs, in order, on this thread (the UI's timer). Returns how many ran. */
    int drain();
    /* Changes each time a job finishes (a cheap check before drain()). */
    uint64_t gen() const { return gen_.load(); }
    /* Waits until every job posted so far has run (the tests). */
    void wait_idle();
    std::string open_error() const;
    /* Stops the thread (the destructor does); waiting jobs are dropped. */
    void stop();

private:
    struct Item {
        Job job;
        Done done;
        std::string key;
        uint64_t seq = 0;
    };
    void run(std::string path, int ram_mb);

    mutable std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    std::deque<Item> queue_;
    std::deque<Done> finished_;
    std::string running_key_;
    bool running_cancelled_ = false;
    bool busy_ = false;
    bool quit_ = false;
    std::string open_error_;
    Db *db_ = nullptr; /* the thread's connection (interrupt only) */
    std::atomic<uint64_t> gen_{0};
    std::thread th_;
};

}  // namespace vinyl
