/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: the database file, its schema and its settings (report 060 3a; P2b, 0.3.0).
 *
 *   file      $XDG_DATA_HOME/vinyl/library.db (~/.local/share/vinyl/library.db): a cache of the tags plus the play counts.
 *             Deleting the folder resets the library (report 060 7.3).
 *   schema    v1 (PRAGMA user_version 1): roots, folders, artists, albums, genres, tracks, playlists (P2c), stats, meta,
 *             and the FTS5 table `fts` (trigram, remove_diacritics, contentless with deletes). Folders are split from file
 *             names, which keeps rows and the scan's per-folder diff small. Additions to the plan's DDL: tracks.root_id
 *             (hides an offline drive's rows without a join), roots.mount (the mount point a drive root was found at), and
 *             artists.sort / genres.sort unique (one row per sort key: "Beyoncé" and "BEYONCE" are one artist).
 *   pragmas   WAL with synchronous=NORMAL (consistent after a power cut, possibly without the last batch), secure_delete
 *             OFF (the boards' build turns it on: deletes would write twice), cache_size -2048 (2 MB) on boards with 512 MB
 *             or less, else -8192, temp_store MEMORY.
 *   damage    a file SQLite calls corrupt or not a database is moved aside (library.db.bad) and a new one is made; a file of
 *             a newer schema (a later Vinyl) is never touched: open fails and says so (its play counts stay).
 * Needs SQLite 3.45 or later (the trigram tokenizer's remove_diacritics); every board has 3.46.1. LVGL-free.
 */
#pragma once

#include "../db/sqlite_lib.hpp"

#include <string>

namespace vinyl {

constexpr int kLibrarySchemaVersion = 1;

/* $XDG_DATA_HOME/vinyl/library.db, else $HOME/.local/share/vinyl/library.db */
std::string library_db_path();

/* MemTotal of /proc/meminfo in MB (0 when unknown). */
int system_ram_mb();

/* The pragmas above; `ram_mb` picks the cache size (0: unknown, the small one). */
bool apply_library_pragmas(Db &db, int ram_mb);

/* The schema version of an open database (PRAGMA user_version). */
int library_schema_version(Db &db);

/* Creates or upgrades the schema to kLibrarySchemaVersion in one transaction. false with why (FTS5 missing, a newer
 * schema, a write error). */
bool create_library_schema(Db &db, std::string *why);

/* Opens the library at `path` (":memory:" for the tests): the folder made, the pragmas, the schema; a damaged file moved
 * aside and made again. `lib` null = the production SQLite. */
bool open_library_db(Db &db, const std::string &path, int ram_mb, std::string *why, const SqliteLib *lib = nullptr);

/* meta(key, value) */
std::string meta_get(Db &db, const std::string &key);
bool meta_set(Db &db, const std::string &key, const std::string &value);
bool meta_erase(Db &db, const std::string &key);

}  // namespace vinyl
