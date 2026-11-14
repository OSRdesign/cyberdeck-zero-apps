/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl database: SQLite (public domain, package libsqlite3-0) loaded at run time with dlopen("libsqlite3.so.0"), the
 * pattern of cp0_lvgl's map engine (cp0_map_sqlite.c) and of Vinyl's own codec and sound-server tables: RTLD_NOW |
 * RTLD_LOCAL, every symbol checked, a readable reason when something is missing, loaded once per process and never
 * unloaded. The binary's NEEDED list never names libsqlite3 (report 060 3a, P2b).
 *
 * On top of the function table, two small RAII classes: Db (one connection) and Stmt (one prepared statement, finalized
 * by its destructor). Both are move-only. A connection is used by one thread at a time (the library worker's or the
 * scanner's); interrupt() is the one call another thread may make (sqlite3_interrupt is documented as safe for that).
 * LVGL-free.
 */
#pragma once

#include <cstdint>
#include <string>

namespace vinyl {

namespace sqlite {
struct sqlite3;      /* opaque, as in sqlite3.h */
struct sqlite3_stmt; /* opaque */

/* the result codes and flags Vinyl uses (values of sqlite3.h, stable since 3.0) */
constexpr int kOk = 0;
constexpr int kError = 1;
constexpr int kBusy = 5;
constexpr int kLocked = 6;
constexpr int kInterrupt = 9;
constexpr int kCorrupt = 11;
constexpr int kNotADb = 26;
constexpr int kRow = 100;
constexpr int kDone = 101;
constexpr int kOpenReadOnly = 0x01;
constexpr int kOpenReadWrite = 0x02;
constexpr int kOpenCreate = 0x04;
constexpr int kOpenUri = 0x40;
constexpr int kOpenFullMutex = 0x10000;
/* column types */
constexpr int kInteger = 1;
constexpr int kFloat = 2;
constexpr int kText = 3;
constexpr int kBlob = 4;
constexpr int kNull = 5;
}  // namespace sqlite

struct SqliteLib {
    using Db = sqlite::sqlite3;
    using St = sqlite::sqlite3_stmt;
    using Destructor = void (*)(void *);
    std::string name;    /* the file that was loaded ("libsqlite3.so.0") */
    std::string version; /* sqlite3_libversion() ("3.46.1") */
    int (*open_v2)(const char *, Db **, int, const char *) = nullptr;
    int (*close_v2)(Db *) = nullptr;
    int (*prepare_v2)(Db *, const char *, int, St **, const char **) = nullptr;
    int (*step)(St *) = nullptr;
    int (*reset)(St *) = nullptr;
    int (*finalize)(St *) = nullptr;
    int (*clear_bindings)(St *) = nullptr;
    int (*bind_int)(St *, int, int) = nullptr;
    int (*bind_int64)(St *, int, long long) = nullptr;
    int (*bind_double)(St *, int, double) = nullptr;
    int (*bind_text)(St *, int, const char *, int, Destructor) = nullptr;
    int (*bind_blob)(St *, int, const void *, int, Destructor) = nullptr;
    int (*bind_null)(St *, int) = nullptr;
    int (*column_count)(St *) = nullptr;
    int (*column_int)(St *, int) = nullptr;
    long long (*column_int64)(St *, int) = nullptr;
    double (*column_double)(St *, int) = nullptr;
    const unsigned char *(*column_text)(St *, int) = nullptr;
    const void *(*column_blob)(St *, int) = nullptr;
    int (*column_bytes)(St *, int) = nullptr;
    int (*column_type)(St *, int) = nullptr;
    int (*exec)(Db *, const char *, int (*)(void *, int, char **, char **), void *, char **) = nullptr;
    void (*free_mem)(void *) = nullptr; /* sqlite3_free */
    const char *(*errmsg)(Db *) = nullptr;
    int (*extended_errcode)(Db *) = nullptr;
    int (*busy_timeout)(Db *, int) = nullptr;
    const char *(*libversion)() = nullptr;
    int (*libversion_number)() = nullptr;
    int (*changes)(Db *) = nullptr;
    long long (*last_insert_rowid)(Db *) = nullptr;
    void (*interrupt)(Db *) = nullptr;
    int (*get_autocommit)(Db *) = nullptr;
    bool complete() const; /* every function is set */
};

/* Loads `file` (dlopen RTLD_NOW | RTLD_LOCAL) and fills `out`; false with `why` ("SQLite not installed (libsqlite3-0):
 * ...", "libsqlite3.so.0 lacks sqlite3_..."), the handle closed again. Not cached: real_sqlite_lib() is the cached
 * production entry; the tests call this with a name that does not exist to see the missing-library path. */
bool load_sqlite_lib(const char *file, SqliteLib &out, std::string &why);

/* The production table: libsqlite3.so.0 loaded on the first call, then cached for the life of the process. Null with
 * `why` filled when the library is missing or lacks a symbol. Thread-safe. */
const SqliteLib *real_sqlite_lib(std::string *why = nullptr);

class Db;

/* One prepared statement. Bind indices start at 1, column indices at 0 (as SQLite's). Text is bound as a copy
 * (SQLITE_TRANSIENT), so the caller's string may go away before step(). */
class Stmt {
public:
    Stmt() = default;
    ~Stmt();
    Stmt(Stmt &&o) noexcept;
    Stmt &operator=(Stmt &&o) noexcept;
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    bool ok() const { return st_ != nullptr; }
    Stmt &bind(int i, long long v);
    Stmt &bind(int i, int v) { return bind(i, static_cast<long long>(v)); }
    Stmt &bind(int i, unsigned v) { return bind(i, static_cast<long long>(v)); }
    Stmt &bind(int i, unsigned long long v) { return bind(i, static_cast<long long>(v)); }
    Stmt &bind(int i, long v) { return bind(i, static_cast<long long>(v)); }
    Stmt &bind(int i, unsigned long v) { return bind(i, static_cast<long long>(v)); }
    Stmt &bind(int i, double v);
    Stmt &bind(int i, const std::string &v);
    Stmt &bind(int i, const char *v); /* null binds NULL */
    Stmt &bind_null(int i);
    /* sqlite::kRow, sqlite::kDone, or an error code (kInterrupt when interrupted) */
    int step();
    /* Steps to the end (an INSERT / UPDATE / DELETE): true when it reached kDone. */
    bool run();
    /* Ready to run again with new bindings (the old ones cleared). */
    void reset();

    int columns() const;
    bool is_null(int c) const;
    long long i64(int c) const;
    int i32(int c) const;
    double f64(int c) const;
    std::string text(int c) const;

private:
    friend class Db;
    Stmt(const SqliteLib *lib, sqlite::sqlite3_stmt *st) : lib_(lib), st_(st) {}
    const SqliteLib *lib_ = nullptr;
    sqlite::sqlite3_stmt *st_ = nullptr;
};

/* One connection. */
class Db {
public:
    Db() = default;
    ~Db();
    Db(Db &&o) noexcept;
    Db &operator=(Db &&o) noexcept;
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    /* Opens `path` (":memory:" for a private in-memory database) with `flags` (sqlite::kOpen*; 0 = read-write and
     * create). `lib` null = the production table. false with why (the library missing, or SQLite's message). */
    bool open(const std::string &path, int flags = 0, const SqliteLib *lib = nullptr, std::string *why = nullptr);
    void close();
    bool is_open() const { return db_ != nullptr; }
    const SqliteLib *lib() const { return lib_; }

    /* One or more statements without results: true on success (why: SQLite's message). */
    bool exec(const std::string &sql, std::string *why = nullptr);
    /* A statement; !ok() on a syntax error (errmsg() says why). */
    Stmt prepare(const std::string &sql);
    /* The first column of the first row of a query, or `fallback` (no row, NULL, an error). */
    long long scalar(const std::string &sql, long long fallback = -1);
    std::string scalar_text(const std::string &sql);

    int changes() const;
    long long last_insert_rowid() const;
    /* Stops the statement running on this connection (from any thread): its step() returns sqlite::kInterrupt. */
    void interrupt();
    bool in_transaction() const;
    std::string errmsg() const;
    int errcode() const; /* the extended code of the last failure */

private:
    const SqliteLib *lib_ = nullptr;
    sqlite::sqlite3 *db_ = nullptr;
};

/* BEGIN IMMEDIATE ... COMMIT; the destructor rolls back what was not committed. */
class Transaction {
public:
    explicit Transaction(Db &db);
    ~Transaction();
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    bool ok() const { return open_; }
    bool commit();

private:
    Db &db_;
    bool open_ = false;
};

}  // namespace vinyl
