/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the queries, the writer and the worker thread (see library.hpp).
 */
#include "library.hpp"

#include "schema.hpp"
#include "sortkey.hpp"

#include <algorithm>
#include <memory>

namespace vinyl {

namespace {

/* a track row's columns (read by read_track) and the joins they need */
const char *const kTrackCols =
    "t.id, r.path || case when f.rel = '' then '' else '/' || f.rel end || '/' || t.name, coalesce(t.title, ''), "
    "coalesce(a.name, ''), coalesce(al.title, ''), coalesce(t.artist_id, 0), coalesce(t.album_id, 0), "
    "coalesce(t.track, 0), coalesce(t.disc, 0), coalesce(t.duration_ms, 0), coalesce(t.codec, '')";
const int kTrackColCount = 11;
const char *const kTrackJoins = " join folders f on f.id = t.folder_id join roots r on r.id = f.root_id"
                                " left join artists a on a.id = t.artist_id left join albums al on al.id = t.album_id";

void read_track(const Stmt &s, TrackRow &r) {
    r.id = s.i64(0);
    r.path = s.text(1);
    r.title = s.text(2);
    r.artist = s.text(3);
    r.album = s.text(4);
    r.artist_id = s.i64(5);
    r.album_id = s.i64(6);
    r.track = s.i32(7);
    r.disc = s.i32(8);
    r.duration_ms = static_cast<uint64_t>(std::max<long long>(0, s.i64(9)));
    r.codec = s.text(10);
}

void read_album(const Stmt &s, AlbumRow &r) {
    r.id = s.i64(0);
    r.title = s.text(1);
    r.artist_id = s.i64(2);
    r.artist = s.text(3);
    r.year = s.i32(4);
    r.tracks = s.i32(5);
}

/* runs a list query: every row through `read`; false on an error or an interrupt */
template <typename Row, typename Read> bool collect(Stmt &s, std::vector<Row> &out, Read read) {
    out.clear();
    if (!s.ok()) return false;
    int rc;
    while ((rc = s.step()) == sqlite::kRow) {
        out.emplace_back();
        read(s, out.back());
    }
    return rc == sqlite::kDone;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

/* an FTS5 query for `q` as one phrase in the given columns: "{col ...} : "q"" with the quotes doubled */
std::string fts_phrase(const std::string &cols, const std::string &q) {
    std::string p = "{" + cols + "} : \"";
    for (char c : q) {
        if (static_cast<unsigned char>(c) < 0x20) continue;
        if (c == '"') p += '"';
        p += c;
    }
    p += '"';
    return p;
}

}  // namespace

/* ------------------------------------------------------------------------------------------------ Library */

Library::Library(Db &db) : db_(db) { refresh_roots(); }

void Library::refresh_roots() {
    roots_.clear();
    offline_filter_.clear();
    Stmt s = db_.prepare("select id, path, coalesce(online, 1) from roots order by id");
    std::string ids;
    while (s.step() == sqlite::kRow) {
        roots_.push_back({s.i64(0), s.text(1), s.i32(2) != 0});
        if (!roots_.back().online) ids += (ids.empty() ? "" : ",") + std::to_string(roots_.back().id);
    }
    if (!ids.empty()) offline_filter_ = " and t.root_id not in (" + ids + ")";
}

bool Library::artists(int offset, int limit, std::vector<ArtistRow> &out) {
    const std::string vis = "exists (select 1 from tracks t where t.album_id = al.id" + offline_filter_ + ")";
    Stmt s = db_.prepare("select a.id, coalesce(a.name, ''), (select count(*) from albums al where al.artist_id = a.id and " +
                         vis + ") from artists a where exists (select 1 from albums al where al.artist_id = a.id and " +
                         vis + ") order by a.sort, a.id limit ? offset ?");
    s.bind(1, limit).bind(2, offset);
    return collect(s, out, [](const Stmt &st, ArtistRow &r) {
        r.id = st.i64(0);
        r.name = st.text(1);
        r.albums = st.i32(2);
    });
}

namespace {
std::string album_select(const std::string &f) {
    return "select al.id, coalesce(al.title, ''), coalesce(al.artist_id, 0), coalesce(a.name, ''), coalesce(al.year, 0), "
           "(select count(*) from tracks t where t.album_id = al.id" +
           f + ") from albums al left join artists a on a.id = al.artist_id where exists (select 1 from tracks t where "
               "t.album_id = al.id" +
           f + ")";
}
}  // namespace

bool Library::albums(int offset, int limit, std::vector<AlbumRow> &out) {
    Stmt s = db_.prepare(album_select(offline_filter_) + " order by al.sort, a.sort, al.id limit ? offset ?");
    s.bind(1, limit).bind(2, offset);
    return collect(s, out, read_album);
}

bool Library::songs(int offset, int limit, std::vector<TrackRow> &out) {
    Stmt s = db_.prepare(std::string("select ") + kTrackCols + " from tracks t" + kTrackJoins + " where 1" + offline_filter_ +
                         " order by t.sort, t.id limit ? offset ?");
    s.bind(1, limit).bind(2, offset);
    return collect(s, out, read_track);
}

bool Library::genres(int offset, int limit, std::vector<GenreRow> &out) {
    const std::string &f = offline_filter_;
    Stmt s = db_.prepare("select g.id, coalesce(g.name, ''), (select count(distinct t.album_id) from tracks t where "
                         "t.genre_id = g.id" +
                         f + "), (select count(*) from tracks t where t.genre_id = g.id" + f + ") from genres g where exists (select 1 from tracks t where t.genre_id = g.id" + f +
                         ") order by g.sort, g.id limit ? offset ?");
    s.bind(1, limit).bind(2, offset);
    return collect(s, out, [](const Stmt &st, GenreRow &r) {
        r.id = st.i64(0);
        r.name = st.text(1);
        r.albums = st.i32(2);
        r.tracks = st.i32(3);
    });
}

bool Library::artist_albums(int64_t artist_id, std::vector<AlbumRow> &out) {
    Stmt s = db_.prepare(album_select(offline_filter_) +
                         " and (al.artist_id = ?1 or al.id in (select t.album_id from tracks t where t.artist_id = ?1" +
                         offline_filter_ + ")) order by (coalesce(al.year, 0) = 0), al.year, al.sort, al.id");
    s.bind(1, static_cast<long long>(artist_id));
    return collect(s, out, read_album);
}

bool Library::artist_tracks(int64_t artist_id, std::vector<TrackRow> &out) {
    Stmt s = db_.prepare(std::string("select ") + kTrackCols + " from tracks t" + kTrackJoins +
                         " where (t.artist_id = ?1 or al.artist_id = ?1)" + offline_filter_ +
                         " order by (coalesce(al.year, 0) = 0), al.year, al.sort, al.id, t.disc, f.rel, t.track, t.sort, t.id");
    s.bind(1, static_cast<long long>(artist_id));
    return collect(s, out, read_track);
}

bool Library::album_tracks(int64_t album_id, std::vector<TrackRow> &out) {
    Stmt s = db_.prepare(std::string("select ") + kTrackCols + " from tracks t" + kTrackJoins + " where t.album_id = ?1" +
                         offline_filter_ + " order by t.disc, f.rel, t.track, t.sort, t.id");
    s.bind(1, static_cast<long long>(album_id));
    return collect(s, out, read_track);
}

bool Library::genre_albums(int64_t genre_id, int offset, int limit, std::vector<AlbumRow> &out) {
    Stmt s = db_.prepare(album_select(offline_filter_) + " and al.id in (select t.album_id from tracks t where t.genre_id = ?1" +
                         offline_filter_ + ") order by al.sort, a.sort, al.id limit ?2 offset ?3");
    s.bind(1, static_cast<long long>(genre_id)).bind(2, limit).bind(3, offset);
    return collect(s, out, read_album);
}

bool Library::album(int64_t album_id, AlbumRow &out) {
    std::vector<AlbumRow> v;
    Stmt s = db_.prepare(album_select(offline_filter_) + " and al.id = ?1");
    s.bind(1, static_cast<long long>(album_id));
    if (!collect(s, v, read_album) || v.empty()) return false;
    out = v[0];
    return true;
}

bool Library::artist(int64_t artist_id, ArtistRow &out) {
    Stmt s = db_.prepare("select a.id, coalesce(a.name, ''), (select count(*) from albums al where al.artist_id = a.id and "
                         "exists (select 1 from tracks t where t.album_id = al.id" +
                         offline_filter_ + ")) from artists a where a.id = ?1");
    s.bind(1, static_cast<long long>(artist_id));
    if (s.step() != sqlite::kRow) return false;
    out.id = s.i64(0);
    out.name = s.text(1);
    out.albums = s.i32(2);
    return true;
}

bool Library::track(int64_t track_id, TrackDetails &out) {
    out = TrackDetails();
    Stmt s = db_.prepare(std::string("select ") + kTrackCols +
                         ", coalesce(aa.name, ''), coalesce(g.name, ''), coalesce(r.label, ''), coalesce(al.year, 0), "
                         "coalesce(t.rate, 0), coalesce(t.bits, 0), coalesce(t.channels, 0), coalesce(t.kbps, 0), "
                         "t.rg_tg, t.rg_tp, t.rg_ag, t.rg_ap, coalesce(t.has_art, 0), coalesce(t.size, 0), "
                         "coalesce(t.mtime, 0), coalesce(st.plays, 0), coalesce(st.skips, 0), coalesce(st.last_played, 0), "
                         "coalesce(r.online, 1) from tracks t" +
                         kTrackJoins +
                         " left join artists aa on aa.id = al.artist_id left join genres g on g.id = t.genre_id"
                         " left join stats st on st.track_id = t.id where t.id = ?1");
    s.bind(1, static_cast<long long>(track_id));
    if (s.step() != sqlite::kRow) return false;
    read_track(s, out.row);
    int c = kTrackColCount;
    out.album_artist = s.text(c++);
    out.genre = s.text(c++);
    out.root_label = s.text(c++);
    out.year = s.i32(c++);
    out.rate = static_cast<uint32_t>(s.i64(c++));
    out.bits = static_cast<unsigned>(s.i32(c++));
    out.channels = static_cast<unsigned>(s.i32(c++));
    out.kbps = static_cast<unsigned>(s.i32(c++));
    auto real = [&](int col, bool &has, float &v) {
        has = !s.is_null(col);
        v = has ? static_cast<float>(s.f64(col)) : 0.0f;
    };
    real(c++, out.rg.has_track, out.rg.track_db);
    real(c++, out.rg.has_track_peak, out.rg.track_peak);
    real(c++, out.rg.has_album, out.rg.album_db);
    real(c++, out.rg.has_album_peak, out.rg.album_peak);
    out.has_art = s.i32(c++) != 0;
    out.size = static_cast<uint64_t>(std::max<long long>(0, s.i64(c++)));
    out.mtime = s.i64(c++);
    out.plays = s.i32(c++);
    out.skips = s.i32(c++);
    out.last_played = s.i64(c++);
    out.online = s.i32(c++) != 0;
    return true;
}

int64_t Library::find_track(const std::string &path) {
    const RootRef *best = nullptr;
    for (const RootRef &r : roots_) {
        if (path.size() > r.path.size() + 1 && path.compare(0, r.path.size(), r.path) == 0 && path[r.path.size()] == '/' &&
            (!best || r.path.size() > best->path.size()))
            best = &r;
    }
    if (!best) return -1;
    const size_t slash = path.rfind('/');
    const std::string name = path.substr(slash + 1);
    const std::string rel = slash > best->path.size() ? path.substr(best->path.size() + 1, slash - best->path.size() - 1) : "";
    Stmt s = db_.prepare("select t.id from tracks t join folders f on f.id = t.folder_id where f.root_id = ?1 and f.rel = ?2 "
                         "and t.name = ?3");
    s.bind(1, static_cast<long long>(best->id)).bind(2, rel).bind(3, name);
    return s.step() == sqlite::kRow ? s.i64(0) : -1;
}

bool Library::track_by_path(const std::string &path, TrackRow &out) {
    out = TrackRow();
    const int64_t id = find_track(path);
    if (id < 0) return false;
    Stmt s = db_.prepare(std::string("select ") + kTrackCols + " from tracks t" + kTrackJoins + " where t.id = ?1");
    s.bind(1, static_cast<long long>(id));
    if (s.step() != sqlite::kRow) return false;
    read_track(s, out);
    return true;
}

LibraryCounts Library::counts() {
    LibraryCounts c;
    const std::string &f = offline_filter_;
    const std::string vis = "exists (select 1 from tracks t where t.album_id = al.id" + f + ")";
    c.tracks = static_cast<int>(db_.scalar("select count(*) from tracks t where 1" + f, 0));
    c.albums = static_cast<int>(db_.scalar("select count(*) from albums al where " + vis, 0));
    c.artists = static_cast<int>(
        db_.scalar("select count(*) from artists a where exists (select 1 from albums al where al.artist_id = a.id and " + vis +
                       ")",
                   0));
    c.genres = static_cast<int>(
        db_.scalar("select count(*) from genres g where exists (select 1 from tracks t where t.genre_id = g.id" + f + ")", 0));
    return c;
}

LibraryStats Library::stats() {
    LibraryStats st;
    st.counts = counts();
    Stmt s = db_.prepare("select coalesce(sum(t.duration_ms), 0), coalesce(sum(t.size), 0) from tracks t where 1" +
                         offline_filter_);
    if (s.step() == sqlite::kRow) {
        st.duration_ms = static_cast<uint64_t>(std::max<long long>(0, s.i64(0)));
        st.bytes = static_cast<uint64_t>(std::max<long long>(0, s.i64(1)));
    }
    st.last_scan = db_.scalar("select coalesce(max(last_scan), 0) from roots", 0);
    st.roots = static_cast<int>(db_.scalar("select count(*) from roots", 0));
    st.offline_roots = static_cast<int>(db_.scalar("select count(*) from roots where online = 0", 0));
    st.db_bytes = static_cast<uint64_t>(std::max<long long>(0, db_.scalar("pragma page_count", 0) *
                                                                   db_.scalar("pragma page_size", 0)));
    return st;
}

bool Library::roots_info(std::vector<RootInfo> &out) {
    out.clear();
    Stmt s = db_.prepare("select r.id, r.path, coalesce(r.label, ''), coalesce(r.mount, ''), coalesce(r.online, 1), "
                         "(select count(*) from tracks t where t.root_id = r.id), coalesce(r.last_scan, 0) from roots r "
                         "order by r.id");
    return collect(s, out, [](const Stmt &st, RootInfo &r) {
        r.id = st.i64(0);
        r.path = st.text(1);
        r.label = st.text(2);
        r.mount = st.text(3);
        r.online = st.i32(4) != 0;
        r.tracks = st.i32(5);
        r.last_scan = st.i64(6);
    });
}

bool Library::record_play(int64_t track_id, int64_t when) {
    Stmt s = db_.prepare("insert into stats(track_id, plays, skips, last_played) values(?1, 1, 0, ?2) on conflict(track_id) "
                         "do update set plays = coalesce(plays, 0) + 1, last_played = excluded.last_played");
    s.bind(1, static_cast<long long>(track_id)).bind(2, static_cast<long long>(when));
    return s.run();
}

bool Library::record_skip(int64_t track_id) {
    Stmt s = db_.prepare("insert into stats(track_id, plays, skips, last_played) values(?1, 0, 1, 0) on conflict(track_id) "
                         "do update set skips = coalesce(skips, 0) + 1");
    s.bind(1, static_cast<long long>(track_id));
    return s.run();
}

bool Library::search(const std::string &query, SearchResults &out, int limit) {
    out = SearchResults();
    out.query = trim(query);
    if (fold_text(out.query).empty()) return true;
    const std::string &f = offline_filter_;
    const std::string vis = "exists (select 1 from tracks t where t.album_id = al.id" + f + ")";
    const std::string artist_cols =
        "select a.id, coalesce(a.name, ''), (select count(*) from albums al where al.artist_id = a.id and " + vis +
        ") from artists a where ";
    auto read_artist = [](const Stmt &st, ArtistRow &r) {
        r.id = st.i64(0);
        r.name = st.text(1);
        r.albums = st.i32(2);
    };
    const std::string genre_cols = "select g.id, coalesce(g.name, ''), (select count(distinct t.album_id) from tracks t "
                                   "where t.genre_id = g.id" +
                                   f + ") from genres g where ";
    auto read_genre = [](const Stmt &st, GenreRow &r) {
        r.id = st.i64(0);
        r.name = st.text(1);
        r.albums = st.i32(2);
    };
    /* an artist shows when it has a visible album or a visible track */
    const std::string artist_vis = "(exists (select 1 from albums al where al.artist_id = a.id and " + vis +
                                   ") or exists (select 1 from tracks t where t.artist_id = a.id" + f + "))";
    const std::string genre_vis = "exists (select 1 from tracks t where t.genre_id = g.id" + f + ")";

    if (utf8_length(out.query) < 3) {
        /* FTS5 trigrams match nothing this short: the start of the sort keys (an index range) */
        out.prefix = true;
        std::string lo, hi;
        prefix_range(out.query, lo, hi);
        if (lo.empty()) return true;
        Stmt a = db_.prepare(artist_cols + "a.sort >= ?1 and a.sort < ?2 and " + artist_vis + " order by a.sort, a.id limit ?3");
        a.bind(1, lo).bind(2, hi).bind(3, limit);
        Stmt al = db_.prepare(album_select(f) + " and al.sort >= ?1 and al.sort < ?2 order by al.sort, a.sort, al.id limit ?3");
        al.bind(1, lo).bind(2, hi).bind(3, limit);
        Stmt so = db_.prepare(std::string("select ") + kTrackCols + " from tracks t" + kTrackJoins +
                              " where t.sort >= ?1 and t.sort < ?2" + f + " order by t.sort, t.id limit ?3");
        so.bind(1, lo).bind(2, hi).bind(3, limit);
        Stmt g = db_.prepare(genre_cols + "g.sort >= ?1 and g.sort < ?2 and " + genre_vis + " order by g.sort, g.id limit ?3");
        g.bind(1, lo).bind(2, hi).bind(3, limit);
        return collect(a, out.artists, read_artist) && collect(al, out.albums, read_album) &&
               collect(so, out.songs, read_track) && collect(g, out.genres, read_genre);
    }
    const std::string q = out.query;
    Stmt a = db_.prepare(artist_cols +
                         "a.id in (select t.artist_id from fts join tracks t on t.id = fts.rowid where fts match ?1" + f +
                         " union select al.artist_id from fts join tracks t on t.id = fts.rowid join albums al on al.id = "
                         "t.album_id where fts match ?2" +
                         f + ") order by a.sort, a.id limit ?3");
    a.bind(1, fts_phrase("artist", q)).bind(2, fts_phrase("album_artist", q)).bind(3, limit);
    Stmt al = db_.prepare(album_select(f) + " and al.id in (select t.album_id from fts join tracks t on t.id = fts.rowid "
                                            "where fts match ?1" +
                          f + ") order by al.sort, a.sort, al.id limit ?2");
    al.bind(1, fts_phrase("album", q)).bind(2, limit);
    Stmt so = db_.prepare(std::string("select ") + kTrackCols + " from fts join tracks t on t.id = fts.rowid" + kTrackJoins +
                          " where fts match ?1" + f + " order by t.sort, t.id limit ?2");
    so.bind(1, fts_phrase("title", q)).bind(2, limit);
    Stmt g = db_.prepare(genre_cols + "g.id in (select t.genre_id from fts join tracks t on t.id = fts.rowid where fts "
                                      "match ?1" +
                         f + ") order by g.sort, g.id limit ?2");
    g.bind(1, fts_phrase("genre", q)).bind(2, limit);
    return collect(a, out.artists, read_artist) && collect(al, out.albums, read_album) &&
           collect(so, out.songs, read_track) && collect(g, out.genres, read_genre);
}

/* ------------------------------------------------------------------------------------------------ LibraryWriter */

LibraryWriter::LibraryWriter(Db &db) : db_(db) {}

Stmt &LibraryWriter::prep(Stmt &s, const char *sql) {
    if (s.ok()) s.reset();
    else s = db_.prepare(sql);
    return s;
}

int64_t LibraryWriter::root(const std::string &path, const std::string &label, const std::string &uuid,
                            const std::string &mount) {
    Stmt ins = db_.prepare("insert into roots(path, label, uuid, online, mount) values(?1, ?2, ?3, 1, ?4) "
                           "on conflict(path) do nothing");
    ins.bind(1, path).bind(2, label).bind(3, uuid).bind(4, mount);
    if (!ins.run()) return -1;
    Stmt q = db_.prepare("select id from roots where path = ?1");
    q.bind(1, path);
    return q.step() == sqlite::kRow ? q.i64(0) : -1;
}

int64_t LibraryWriter::find_folder(int64_t root_id, const std::string &rel) {
    Stmt &q = prep(find_folder_, "select id, coalesce(mtime, 0) from folders where root_id = ?1 and rel = ?2");
    q.bind(1, static_cast<long long>(root_id)).bind(2, rel);
    return q.step() == sqlite::kRow ? q.i64(0) : -1;
}

int64_t LibraryWriter::folder(int64_t root_id, const std::string &rel, int64_t mtime) {
    Stmt &q = prep(find_folder_, "select id, coalesce(mtime, 0) from folders where root_id = ?1 and rel = ?2");
    q.bind(1, static_cast<long long>(root_id)).bind(2, rel);
    if (q.step() == sqlite::kRow) {
        const int64_t id = q.i64(0);
        if (q.i64(1) != mtime) {
            Stmt &u = prep(folder_mtime_, "update folders set mtime = ?1 where id = ?2");
            u.bind(1, static_cast<long long>(mtime)).bind(2, static_cast<long long>(id));
            u.run();
        }
        return id;
    }
    Stmt &ins = prep(add_folder_, "insert into folders(root_id, rel, mtime) values(?1, ?2, ?3)");
    ins.bind(1, static_cast<long long>(root_id)).bind(2, rel).bind(3, static_cast<long long>(mtime));
    if (!ins.run()) return -1;
    return db_.last_insert_rowid();
}

int64_t LibraryWriter::artist_id(const std::string &name) {
    const std::string key = sort_key(name);
    Stmt &q = prep(find_artist_, "select id from artists where sort = ?1");
    q.bind(1, key);
    if (q.step() == sqlite::kRow) return q.i64(0);
    Stmt &ins = prep(add_artist_, "insert into artists(name, sort) values(?1, ?2)");
    ins.bind(1, name).bind(2, key);
    if (!ins.run()) return -1;
    return db_.last_insert_rowid();
}

int64_t LibraryWriter::album_id(const std::string &title, int64_t artist, int year) {
    const std::string key = sort_key(title);
    Stmt &q = prep(find_album_, "select id, coalesce(year, 0) from albums where sort = ?1 and artist_id = ?2");
    q.bind(1, key).bind(2, static_cast<long long>(artist));
    if (q.step() == sqlite::kRow) {
        const int64_t id = q.i64(0);
        if (q.i32(1) == 0 && year > 0) { /* the first track with a year gives the album its year */
            Stmt &u = prep(album_year_, "update albums set year = ?1 where id = ?2");
            u.bind(1, year).bind(2, static_cast<long long>(id));
            u.run();
        }
        return id;
    }
    Stmt &ins = prep(add_album_, "insert into albums(title, sort, artist_id, year) values(?1, ?2, ?3, ?4)");
    ins.bind(1, title).bind(2, key).bind(3, static_cast<long long>(artist)).bind(4, year);
    if (!ins.run()) return -1;
    return db_.last_insert_rowid();
}

int64_t LibraryWriter::genre_id(const std::string &name) {
    if (name.empty()) return 0;
    const std::string key = sort_key(name);
    Stmt &q = prep(find_genre_, "select id from genres where sort = ?1");
    q.bind(1, key);
    if (q.step() == sqlite::kRow) return q.i64(0);
    Stmt &ins = prep(add_genre_, "insert into genres(name, sort) values(?1, ?2)");
    ins.bind(1, name).bind(2, key);
    if (!ins.run()) return -1;
    return db_.last_insert_rowid();
}

namespace {
const std::string &album_artist_of(const TrackTags &t) { return t.album_artist.empty() ? t.artist : t.album_artist; }
int year_of(const TrackTags &t) {
    const std::string y = t.year();
    return y.empty() ? 0 : std::stoi(y);
}
}  // namespace

bool LibraryWriter::bind_track(Stmt &s, const TrackFile &f, const TrackTags &t, std::string &title) {
    title = t.title.empty() ? title_from_path(f.name) : t.title;
    const int64_t artist = artist_id(t.artist);
    const int64_t album_artist = album_artist_of(t) == t.artist ? artist : artist_id(album_artist_of(t));
    if (artist < 0 || album_artist < 0) return false;
    const int64_t album = album_id(t.album, album_artist, year_of(t));
    const int64_t genre = genre_id(t.genre);
    if (album < 0 || genre < 0) return false;
    s.bind(1, static_cast<long long>(f.folder_id)).bind(2, static_cast<long long>(f.root_id)).bind(3, f.name);
    s.bind(4, static_cast<long long>(f.mtime)).bind(5, static_cast<long long>(f.size)).bind(6, title).bind(7, sort_key(title));
    s.bind(8, static_cast<long long>(artist)).bind(9, static_cast<long long>(album));
    if (genre > 0) s.bind(10, static_cast<long long>(genre));
    else s.bind_null(10);
    s.bind(11, t.track).bind(12, t.disc).bind(13, static_cast<long long>(t.duration_ms)).bind(14, t.codec);
    s.bind(15, static_cast<long long>(t.rate)).bind(16, t.bits).bind(17, t.channels).bind(18, t.bitrate_kbps);
    auto real = [&s](int i, bool has, float v) {
        if (has) s.bind(i, static_cast<double>(v));
        else s.bind_null(i);
    };
    real(19, t.rg.has_track, t.rg.track_db);
    real(20, t.rg.has_track_peak, t.rg.track_peak);
    real(21, t.rg.has_album, t.rg.album_db);
    real(22, t.rg.has_album_peak, t.rg.album_peak);
    s.bind(23, t.has_picture ? 1 : 0);
    return true;
}

bool LibraryWriter::fts_insert(int64_t id, const std::string &title, const TrackTags &t) {
    Stmt &s = prep(add_fts_, "insert into fts(rowid, title, artist, album, album_artist, genre) values(?1, ?2, ?3, ?4, ?5, ?6)");
    s.bind(1, static_cast<long long>(id)).bind(2, title).bind(3, t.artist).bind(4, t.album).bind(5, album_artist_of(t));
    s.bind(6, t.genre);
    return s.run();
}

int64_t LibraryWriter::add_track(const TrackFile &f, const TrackTags &t) {
    std::string title;
    Stmt &s = prep(add_track_, "insert into tracks(folder_id, root_id, name, mtime, size, title, sort, artist_id, album_id, "
                               "genre_id, track, disc, duration_ms, codec, rate, bits, channels, kbps, rg_tg, rg_tp, rg_ag, "
                               "rg_ap, has_art) values(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, "
                               "?17, ?18, ?19, ?20, ?21, ?22, ?23)");
    if (!bind_track(s, f, t, title) || !s.run()) return -1;
    const int64_t id = db_.last_insert_rowid();
    if (!fts_insert(id, title, t)) return -1;
    ++writes_;
    return id;
}

bool LibraryWriter::update_track(int64_t id, const TrackFile &f, const TrackTags &t) {
    std::string title;
    Stmt &s = prep(update_track_, "update tracks set folder_id = ?1, root_id = ?2, name = ?3, mtime = ?4, size = ?5, "
                                  "title = ?6, sort = ?7, artist_id = ?8, album_id = ?9, genre_id = ?10, track = ?11, "
                                  "disc = ?12, duration_ms = ?13, codec = ?14, rate = ?15, bits = ?16, channels = ?17, "
                                  "kbps = ?18, rg_tg = ?19, rg_tp = ?20, rg_ag = ?21, rg_ap = ?22, has_art = ?23 "
                                  "where id = ?24");
    if (!bind_track(s, f, t, title)) return false;
    s.bind(24, static_cast<long long>(id));
    if (!s.run()) return false;
    Stmt &d = prep(del_fts_, "delete from fts where rowid = ?1");
    d.bind(1, static_cast<long long>(id));
    if (!d.run() || !fts_insert(id, title, t)) return false;
    ++writes_;
    return true;
}

bool LibraryWriter::delete_track(int64_t id) {
    Stmt &t = prep(del_track_, "delete from tracks where id = ?1");
    t.bind(1, static_cast<long long>(id));
    Stmt &f = prep(del_fts_, "delete from fts where rowid = ?1");
    f.bind(1, static_cast<long long>(id));
    Stmt &s = prep(del_stats_, "delete from stats where track_id = ?1");
    s.bind(1, static_cast<long long>(id));
    const bool ok = t.run() && f.run() && s.run();
    if (ok) ++writes_;
    return ok;
}

bool LibraryWriter::delete_folder(int64_t folder_id) {
    const std::string id = std::to_string(folder_id);
    const std::string sel = "(select id from tracks where folder_id = " + id + ")";
    bool ok = db_.exec("delete from fts where rowid in " + sel) && db_.exec("delete from stats where track_id in " + sel);
    ok = ok && db_.exec("delete from tracks where folder_id = " + id);
    if (ok) writes_ += db_.changes();
    return ok && db_.exec("delete from folders where id = " + id);
}

bool LibraryWriter::delete_root(int64_t root_id) {
    const std::string id = std::to_string(root_id);
    const std::string sel = "(select id from tracks where root_id = " + id + ")";
    bool ok = db_.exec("delete from fts where rowid in " + sel) && db_.exec("delete from stats where track_id in " + sel);
    ok = ok && db_.exec("delete from tracks where root_id = " + id);
    if (ok) writes_ += db_.changes();
    return ok && db_.exec("delete from folders where root_id = " + id) && db_.exec("delete from roots where id = " + id);
}

bool LibraryWriter::prune() {
    return db_.exec("delete from albums where not exists (select 1 from tracks t where t.album_id = albums.id)") &&
           db_.exec("delete from artists where not exists (select 1 from tracks t where t.artist_id = artists.id) and not "
                    "exists (select 1 from albums al where al.artist_id = artists.id)") &&
           db_.exec("delete from genres where not exists (select 1 from tracks t where t.genre_id = genres.id)");
}

/* ------------------------------------------------------------------------------------------------ LibraryWorker */

LibraryWorker::~LibraryWorker() { stop(); }

void LibraryWorker::start(const std::string &db_path, int ram_mb) {
    if (th_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = false;
    }
    th_ = std::thread(&LibraryWorker::run, this, db_path, ram_mb);
}

void LibraryWorker::post(Job job, Done done, const std::string &key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!key.empty()) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Item &i) { return i.key == key; }), queue_.end());
        if (busy_ && running_key_ == key) {
            running_cancelled_ = true;
            if (db_) db_->interrupt();
        }
    }
    queue_.push_back({std::move(job), std::move(done), key, 0});
    cv_.notify_all();
}

int LibraryWorker::drain() {
    std::deque<Done> ready;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ready.swap(finished_);
    }
    int n = 0;
    for (Done &d : ready) {
        d();
        ++n;
    }
    return n;
}

void LibraryWorker::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_cv_.wait(lk, [&] { return quit_ || !th_.joinable() || (queue_.empty() && !busy_); });
}

std::string LibraryWorker::open_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return open_error_;
}

void LibraryWorker::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
        queue_.clear();
        if (db_) db_->interrupt();
    }
    cv_.notify_all();
    idle_cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void LibraryWorker::run(std::string path, int ram_mb) {
    Db db;
    std::string why;
    const bool ok = open_library_db(db, path, ram_mb, &why);
    std::unique_ptr<Library> lib;
    if (ok) lib.reset(new Library(db));
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ok) open_error_ = why;
        db_ = ok ? &db : nullptr;
    }
    while (true) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            it = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            running_key_ = it.key;
            running_cancelled_ = false;
        }
        if (lib) lib->refresh_roots();
        it.job(lib.get());
        {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = false;
            if (!running_cancelled_ && it.done) finished_.push_back(std::move(it.done));
            running_key_.clear();
            ++gen_;
        }
        idle_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        db_ = nullptr;
    }
    lib.reset();
    db.close();
    idle_cv_.notify_all();
}

}  // namespace vinyl
