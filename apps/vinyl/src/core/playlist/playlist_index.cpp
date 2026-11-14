/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl playlists: the database index of the playlist files (see playlist_index.hpp).
 */
#include "playlist_index.hpp"

#include "library/browse.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <map>

namespace vinyl {

namespace {

bool is_file(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int64_t mtime_ns(const std::string &p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

std::string base_name(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

m3u::Entry entry_for(const TrackRow &t) {
    m3u::Entry e;
    e.path = t.path;
    e.seconds = static_cast<int>((t.duration_ms + 500) / 1000);
    e.text = t.artist.empty() ? t.title : t.artist + " - " + t.title;
    return e;
}

/* one playlist's entries into the index (the caller holds the transaction) */
bool index_one(Library &lib, const PlaylistStore &store, const std::string &name, int64_t mtime) {
    Db &db = lib.db();
    std::vector<PlaylistEntryRow> rows;
    PlaylistStats st;
    if (!playlist_rows(lib, store, name, rows, &st)) return false;
    const std::string path = store.path_of(name);
    int64_t id = -1;
    {
        Stmt q = db.prepare("select id from playlists where path = ?1");
        q.bind(1, path);
        id = q.step() == sqlite::kRow ? q.i64(0) : -1;
    }
    if (id < 0) {
        Stmt ins = db.prepare("insert into playlists(path, name, mtime, n, ms) values(?1, ?2, ?3, ?4, ?5)");
        ins.bind(1, path).bind(2, name).bind(3, static_cast<long long>(mtime)).bind(4, st.tracks).bind(5, static_cast<long long>(st.ms));
        if (!ins.run()) return false;
        id = db.last_insert_rowid();
    } else {
        Stmt up = db.prepare("update playlists set name = ?2, mtime = ?3, n = ?4, ms = ?5 where id = ?1");
        up.bind(1, static_cast<long long>(id)).bind(2, name).bind(3, static_cast<long long>(mtime)).bind(4, st.tracks).bind(5, static_cast<long long>(st.ms));
        if (!up.run()) return false;
        Stmt del = db.prepare("delete from playlist_items where playlist_id = ?1");
        del.bind(1, static_cast<long long>(id));
        if (!del.run()) return false;
    }
    Stmt item = db.prepare("insert into playlist_items(playlist_id, pos, track_id, path) values(?1, ?2, ?3, ?4)");
    for (size_t i = 0; i < rows.size(); ++i) {
        item.reset();
        item.bind(1, static_cast<long long>(id)).bind(2, static_cast<int>(i));
        if (rows[i].track_id > 0) item.bind(3, static_cast<long long>(rows[i].track_id));
        else item.bind_null(3);
        item.bind(4, rows[i].entry.path);
        if (!item.run()) return false;
    }
    return true;
}

}  // namespace

std::string PlaylistEntryRow::label() const {
    if (!artist.empty() && !title.empty()) return artist + ", " + title;
    return title;
}

bool playlist_rows(Library &lib, const PlaylistStore &store, const std::string &name, std::vector<PlaylistEntryRow> &out, PlaylistStats *stats) {
    out.clear();
    std::vector<m3u::Entry> entries;
    if (store.read(name, entries) != PlResult::Ok) return false;
    PlaylistStats st;
    for (const m3u::Entry &e : entries) {
        PlaylistEntryRow r;
        r.entry = e;
        r.exists = e.resolved && is_file(e.path);
        TrackRow t;
        if (e.resolved && lib.track_by_path(e.path, t)) {
            r.track_id = t.id;
            r.title = t.title;
            r.artist = t.artist;
            r.duration_ms = t.duration_ms;
        } else {
            const std::string &text = e.text;
            const size_t dash = text.find(" - ");
            if (dash != std::string::npos) {
                r.artist = text.substr(0, dash);
                r.title = text.substr(dash + 3);
            } else {
                r.title = text;
            }
            if (r.title.empty()) {
                std::string n = e.path;
                for (char &c : n)
                    if (c == '\\') c = '/';
                r.title = base_name(n);
                r.artist.clear();
            }
            if (e.seconds >= 0) r.duration_ms = static_cast<uint64_t>(e.seconds) * 1000;
        }
        ++st.tracks;
        if (!r.exists) ++st.missing;
        st.ms += r.duration_ms;
        out.push_back(std::move(r));
    }
    if (stats) *stats = st;
    return true;
}

std::vector<std::string> playlist_playable(const std::vector<PlaylistEntryRow> &rows, int start_row, int *start_out) {
    std::vector<std::string> paths;
    int start = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].exists) continue;
        if (static_cast<int>(i) == start_row) start = static_cast<int>(paths.size());
        paths.push_back(rows[i].entry.path);
    }
    if (start_out) *start_out = start;
    return paths;
}

std::vector<m3u::Entry> entries_from_tracks(const std::vector<TrackRow> &tracks) {
    std::vector<m3u::Entry> out;
    for (const TrackRow &t : tracks) out.push_back(entry_for(t));
    return out;
}

std::vector<m3u::Entry> entries_from_paths(Library &lib, const std::vector<std::string> &paths) {
    std::vector<m3u::Entry> out;
    for (const std::string &p : paths) {
        TrackRow t;
        if (lib.track_by_path(p, t)) {
            out.push_back(entry_for(t));
        } else {
            m3u::Entry e;
            e.path = p;
            out.push_back(e);
        }
    }
    return out;
}

bool playlist_reindex(Library &lib, PlaylistStore &store, const std::string &name) {
    Transaction tx(lib.db());
    if (!tx.ok()) return false;
    const std::string real = store.find(name);
    if (real.empty()) {
        Stmt q = lib.db().prepare("select id from playlists where path = ?1");
        q.bind(1, store.path_of(name));
        if (q.step() == sqlite::kRow) {
            const long long id = q.i64(0);
            lib.db().exec("delete from playlist_items where playlist_id = " + std::to_string(id));
            lib.db().exec("delete from playlists where id = " + std::to_string(id));
        }
        return tx.commit();
    }
    if (!index_one(lib, store, real, mtime_ns(store.path_of(real)))) return false;
    return tx.commit();
}

bool playlist_sync(Library &lib, PlaylistStore &store) {
    Db &db = lib.db();
    const std::vector<std::string> names = store.names();
    std::map<std::string, std::pair<int64_t, int64_t>> have; /* path -> id, mtime */
    {
        Stmt q = db.prepare("select path, id, mtime from playlists");
        while (q.step() == sqlite::kRow) have[q.text(0)] = {q.i64(1), q.i64(2)};
    }
    /* what has to change is found before the transaction: the common case is nothing, and nothing is written */
    std::vector<std::string> todo, gone;
    std::map<std::string, bool> present;
    for (const std::string &n : names) {
        const std::string p = store.path_of(n);
        present[p] = true;
        auto it = have.find(p);
        if (it == have.end() || it->second.second != mtime_ns(p)) todo.push_back(n);
    }
    for (const auto &kv : have)
        if (!present.count(kv.first)) gone.push_back(kv.first);
    if (todo.empty() && gone.empty()) return true;
    Transaction tx(db);
    if (!tx.ok()) return false;
    for (const std::string &p : gone) {
        const long long id = have[p].first;
        db.exec("delete from playlist_items where playlist_id = " + std::to_string(id));
        db.exec("delete from playlists where id = " + std::to_string(id));
    }
    for (const std::string &n : todo)
        if (!index_one(lib, store, n, mtime_ns(store.path_of(n)))) return false;
    return tx.commit();
}

bool playlist_list(Library &lib, std::vector<PlaylistRow> &out) {
    out.clear();
    Stmt q = lib.db().prepare("select id, name, path, n, ms from playlists");
    if (!q.ok()) return false;
    int rc;
    while ((rc = q.step()) == sqlite::kRow) {
        PlaylistRow r;
        r.id = q.i64(0);
        r.name = q.text(1);
        r.path = q.text(2);
        r.tracks = q.i32(3);
        r.ms = static_cast<uint64_t>(std::max<long long>(0, q.i64(4)));
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const PlaylistRow &a, const PlaylistRow &b) { return natural_less(a.name, b.name); });
    return rc == sqlite::kDone;
}

}  // namespace vinyl
