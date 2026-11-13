/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the database file and schema v1 (see schema.hpp).
 */
#include "schema.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace vinyl {

namespace {

const char *const kSchemaV1 = R"SQL(
create table if not exists meta(key text primary key, value text);
create table if not exists roots(id integer primary key, path text unique, label text, uuid text, online int default 1,
                                 last_scan int, mount text);
create table if not exists folders(id integer primary key, root_id int, rel text, mtime int, unique(root_id, rel));
create table if not exists artists(id integer primary key, name text, sort text unique);
create table if not exists albums(id integer primary key, title text, sort text, artist_id int, year int, art text,
                                  unique(sort, artist_id));
create table if not exists genres(id integer primary key, name text, sort text unique);
create table if not exists tracks(id integer primary key, folder_id int, root_id int, name text, mtime int, size int,
  title text, sort text, artist_id int, album_id int, genre_id int, track int, disc int, duration_ms int,
  codec text, rate int, bits int, channels int, kbps int, rg_tg real, rg_tp real, rg_ag real, rg_ap real,
  has_art int, unique(folder_id, name));
create table if not exists playlists(id integer primary key, path text unique, name text, mtime int, n int, ms int);
create table if not exists playlist_items(playlist_id int, pos int, track_id int, path text, primary key(playlist_id, pos));
create table if not exists stats(track_id integer primary key, plays int, skips int, last_played int, rating int);
create index if not exists tracks_album on tracks(album_id, disc, track);
create index if not exists tracks_artist on tracks(artist_id);
create index if not exists tracks_genre on tracks(genre_id);
create index if not exists tracks_sort on tracks(sort);
create index if not exists albums_artist on albums(artist_id, year);
create virtual table if not exists fts using fts5(title, artist, album, album_artist, genre, content='',
                                                  contentless_delete=1, tokenize='trigram remove_diacritics 1');
)SQL";

int mkdir_p(const std::string &dir) {
    if (dir.empty()) return 0;
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
    const size_t s = dir.rfind('/');
    if (s != std::string::npos && s > 0) {
        const int rc = mkdir_p(dir.substr(0, s));
        if (rc < 0) return rc;
    }
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return -errno;
    return 0;
}

}  // namespace

std::string library_db_path() {
    const char *x = std::getenv("XDG_DATA_HOME");
    if (x && *x) return std::string(x) + "/vinyl/library.db";
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp") + "/.local/share/vinyl/library.db";
}

int system_ram_mb() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    long long kb = 0;
    std::string unit;
    while (f >> key >> kb >> unit) {
        if (key == "MemTotal:") return static_cast<int>(kb / 1024);
    }
    return 0;
}

bool apply_library_pragmas(Db &db, int ram_mb) {
    const bool small = ram_mb <= 512;
    bool ok = db.exec("pragma journal_mode=WAL");
    ok &= db.exec("pragma synchronous=NORMAL");
    ok &= db.exec("pragma secure_delete=OFF");
    ok &= db.exec(small ? "pragma cache_size=-2048" : "pragma cache_size=-8192");
    ok &= db.exec("pragma temp_store=MEMORY");
    return ok;
}

int library_schema_version(Db &db) { return static_cast<int>(db.scalar("pragma user_version", 0)); }

bool create_library_schema(Db &db, std::string *why) {
    const int v = library_schema_version(db);
    if (v > kLibrarySchemaVersion) {
        if (why) *why = "the library was made by a newer Vinyl (schema " + std::to_string(v) + ")";
        return false;
    }
    if (v == kLibrarySchemaVersion) return true;
    Transaction tx(db);
    std::string w;
    if (!tx.ok()) {
        if (why) *why = "cannot write the library: " + db.errmsg();
        return false;
    }
    /* v0 (a new file) to v1; later versions add their steps here, each from the one before */
    if (!db.exec(kSchemaV1, &w) || !db.exec("pragma user_version=" + std::to_string(kLibrarySchemaVersion), &w)) {
        if (why) *why = "cannot create the library: " + w;
        return false;
    }
    if (!tx.commit()) {
        if (why) *why = "cannot create the library: " + db.errmsg();
        return false;
    }
    return true;
}

bool open_library_db(Db &db, const std::string &path, int ram_mb, std::string *why, const SqliteLib *lib) {
    const bool memory = path == ":memory:";
    if (!memory) {
        const size_t s = path.rfind('/');
        if (s != std::string::npos && s > 0) {
            const int rc = mkdir_p(path.substr(0, s));
            if (rc < 0) {
                if (why) *why = "cannot make " + path.substr(0, s) + ": " + std::strerror(-rc);
                return false;
            }
        }
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!db.open(path, 0, lib, why)) return false;
        Stmt probe = db.prepare("select count(*) from sqlite_master");
        const int rc = probe.ok() ? probe.step() : db.errcode();
        probe = Stmt();
        if (rc != sqlite::kRow) {
            const int code = db.errcode() & 0xFF;
            const std::string msg = db.errmsg();
            db.close();
            if (!memory && attempt == 0 && (code == sqlite::kCorrupt || code == sqlite::kNotADb)) {
                /* a damaged file: aside, and a new library (only the play counts are lost; the tags come back) */
                std::rename(path.c_str(), (path + ".bad").c_str());
                std::remove((path + "-wal").c_str());
                std::remove((path + "-shm").c_str());
                std::fprintf(stderr, "[vinyl] library: %s damaged (%s), moved to %s.bad\n", path.c_str(), msg.c_str(),
                             path.c_str());
                continue;
            }
            if (why) *why = "cannot read " + path + ": " + msg;
            return false;
        }
        apply_library_pragmas(db, ram_mb);
        if (!create_library_schema(db, why)) {
            db.close();
            return false;
        }
        return true;
    }
    return false;
}

std::string meta_get(Db &db, const std::string &key) {
    Stmt s = db.prepare("select value from meta where key = ?");
    s.bind(1, key);
    return s.step() == sqlite::kRow ? s.text(0) : "";
}

bool meta_set(Db &db, const std::string &key, const std::string &value) {
    Stmt s = db.prepare("insert into meta(key, value) values(?, ?) on conflict(key) do update set value = excluded.value");
    s.bind(1, key).bind(2, value);
    return s.run();
}

bool meta_erase(Db &db, const std::string &key) {
    Stmt s = db.prepare("delete from meta where key = ?");
    s.bind(1, key);
    return s.run();
}

}  // namespace vinyl
