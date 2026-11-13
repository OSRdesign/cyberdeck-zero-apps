/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl database: the libsqlite3 table and the RAII wrappers (see sqlite_lib.hpp).
 */
#include "sqlite_lib.hpp"

#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <utility>

namespace vinyl {

bool SqliteLib::complete() const {
    return open_v2 && close_v2 && prepare_v2 && step && reset && finalize && clear_bindings && bind_int && bind_int64 &&
           bind_double && bind_text && bind_blob && bind_null && column_count && column_int && column_int64 &&
           column_double && column_text && column_blob && column_bytes && column_type && exec && free_mem && errmsg &&
           extended_errcode && busy_timeout && libversion && libversion_number && changes && last_insert_rowid &&
           interrupt && get_autocommit;
}

namespace {

template <typename Fn> bool sym(void *h, Fn &fn, const char *name, const std::string &lib, std::string &why) {
    void *p = ::dlsym(h, name);
    fn = reinterpret_cast<Fn>(p);
    if (!p && why.empty()) why = lib + " lacks " + name;
    return p != nullptr;
}

struct Holder {
    std::once_flag once;
    SqliteLib lib;
    bool ok = false;
    std::string why;
};

Holder &holder() {
    static Holder h;
    return h;
}

SqliteLib::Destructor transient() { return reinterpret_cast<SqliteLib::Destructor>(static_cast<intptr_t>(-1)); }

}  // namespace

bool load_sqlite_lib(const char *file, SqliteLib &out, std::string &why) {
    out = SqliteLib();
    why.clear();
    void *h = ::dlopen(file, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = ::dlerror();
        why = std::string("SQLite not installed (libsqlite3-0): ") + file + ": " + (e ? e : "dlopen failed");
        return false;
    }
    SqliteLib &l = out;
    l.name = file;
    bool all = true;
    all &= sym(h, l.open_v2, "sqlite3_open_v2", l.name, why);
    all &= sym(h, l.close_v2, "sqlite3_close_v2", l.name, why);
    all &= sym(h, l.prepare_v2, "sqlite3_prepare_v2", l.name, why);
    all &= sym(h, l.step, "sqlite3_step", l.name, why);
    all &= sym(h, l.reset, "sqlite3_reset", l.name, why);
    all &= sym(h, l.finalize, "sqlite3_finalize", l.name, why);
    all &= sym(h, l.clear_bindings, "sqlite3_clear_bindings", l.name, why);
    all &= sym(h, l.bind_int, "sqlite3_bind_int", l.name, why);
    all &= sym(h, l.bind_int64, "sqlite3_bind_int64", l.name, why);
    all &= sym(h, l.bind_double, "sqlite3_bind_double", l.name, why);
    all &= sym(h, l.bind_text, "sqlite3_bind_text", l.name, why);
    all &= sym(h, l.bind_blob, "sqlite3_bind_blob", l.name, why);
    all &= sym(h, l.bind_null, "sqlite3_bind_null", l.name, why);
    all &= sym(h, l.column_count, "sqlite3_column_count", l.name, why);
    all &= sym(h, l.column_int, "sqlite3_column_int", l.name, why);
    all &= sym(h, l.column_int64, "sqlite3_column_int64", l.name, why);
    all &= sym(h, l.column_double, "sqlite3_column_double", l.name, why);
    all &= sym(h, l.column_text, "sqlite3_column_text", l.name, why);
    all &= sym(h, l.column_blob, "sqlite3_column_blob", l.name, why);
    all &= sym(h, l.column_bytes, "sqlite3_column_bytes", l.name, why);
    all &= sym(h, l.column_type, "sqlite3_column_type", l.name, why);
    all &= sym(h, l.exec, "sqlite3_exec", l.name, why);
    all &= sym(h, l.free_mem, "sqlite3_free", l.name, why);
    all &= sym(h, l.errmsg, "sqlite3_errmsg", l.name, why);
    all &= sym(h, l.extended_errcode, "sqlite3_extended_errcode", l.name, why);
    all &= sym(h, l.busy_timeout, "sqlite3_busy_timeout", l.name, why);
    all &= sym(h, l.libversion, "sqlite3_libversion", l.name, why);
    all &= sym(h, l.libversion_number, "sqlite3_libversion_number", l.name, why);
    all &= sym(h, l.changes, "sqlite3_changes", l.name, why);
    all &= sym(h, l.last_insert_rowid, "sqlite3_last_insert_rowid", l.name, why);
    all &= sym(h, l.interrupt, "sqlite3_interrupt", l.name, why);
    all &= sym(h, l.get_autocommit, "sqlite3_get_autocommit", l.name, why);
    if (!all) {
        ::dlclose(h);
        out = SqliteLib();
        return false;
    }
    const char *v = l.libversion();
    l.version = v ? v : "";
    return true;
}

const SqliteLib *real_sqlite_lib(std::string *why) {
    Holder &h = holder();
    std::call_once(h.once, [&h] { h.ok = load_sqlite_lib("libsqlite3.so.0", h.lib, h.why); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

/* ------------------------------------------------------------------------------------------------ Stmt */

Stmt::~Stmt() {
    if (st_) lib_->finalize(st_);
}

Stmt::Stmt(Stmt &&o) noexcept : lib_(o.lib_), st_(o.st_) { o.st_ = nullptr; }

Stmt &Stmt::operator=(Stmt &&o) noexcept {
    if (this != &o) {
        if (st_) lib_->finalize(st_);
        lib_ = o.lib_;
        st_ = o.st_;
        o.st_ = nullptr;
    }
    return *this;
}

Stmt &Stmt::bind(int i, long long v) {
    if (st_) lib_->bind_int64(st_, i, v);
    return *this;
}

Stmt &Stmt::bind(int i, double v) {
    if (st_) lib_->bind_double(st_, i, v);
    return *this;
}

Stmt &Stmt::bind(int i, const std::string &v) {
    if (st_) lib_->bind_text(st_, i, v.data(), static_cast<int>(v.size()), transient());
    return *this;
}

Stmt &Stmt::bind(int i, const char *v) {
    if (!v) return bind_null(i);
    return bind(i, std::string(v));
}

Stmt &Stmt::bind_null(int i) {
    if (st_) lib_->bind_null(st_, i);
    return *this;
}

int Stmt::step() { return st_ ? lib_->step(st_) : sqlite::kError; }

bool Stmt::run() {
    int rc;
    do rc = step();
    while (rc == sqlite::kRow);
    return rc == sqlite::kDone;
}

void Stmt::reset() {
    if (!st_) return;
    lib_->reset(st_);
    lib_->clear_bindings(st_);
}

int Stmt::columns() const { return st_ ? lib_->column_count(st_) : 0; }
bool Stmt::is_null(int c) const { return !st_ || lib_->column_type(st_, c) == sqlite::kNull; }
long long Stmt::i64(int c) const { return st_ ? lib_->column_int64(st_, c) : 0; }
int Stmt::i32(int c) const { return st_ ? lib_->column_int(st_, c) : 0; }
double Stmt::f64(int c) const { return st_ ? lib_->column_double(st_, c) : 0.0; }

std::string Stmt::text(int c) const {
    if (!st_) return "";
    const unsigned char *p = lib_->column_text(st_, c);
    const int n = lib_->column_bytes(st_, c);
    return p && n > 0 ? std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(n)) : std::string();
}

/* ------------------------------------------------------------------------------------------------ Db */

Db::~Db() { close(); }

Db::Db(Db &&o) noexcept : lib_(o.lib_), db_(o.db_) { o.db_ = nullptr; }

Db &Db::operator=(Db &&o) noexcept {
    if (this != &o) {
        close();
        lib_ = o.lib_;
        db_ = o.db_;
        o.db_ = nullptr;
    }
    return *this;
}

bool Db::open(const std::string &path, int flags, const SqliteLib *lib, std::string *why) {
    close();
    std::string w;
    if (!lib) lib = real_sqlite_lib(&w);
    if (!lib) {
        if (why) *why = w;
        return false;
    }
    lib_ = lib;
    if (flags == 0) flags = sqlite::kOpenReadWrite | sqlite::kOpenCreate;
    flags |= sqlite::kOpenFullMutex;
    sqlite::sqlite3 *db = nullptr;
    const int rc = lib_->open_v2(path.c_str(), &db, flags, nullptr);
    if (rc != sqlite::kOk) {
        if (why) *why = std::string("cannot open ") + path + ": " + (db ? lib_->errmsg(db) : "out of memory");
        if (db) lib_->close_v2(db);
        return false;
    }
    db_ = db;
    lib_->busy_timeout(db_, 2000);
    return true;
}

void Db::close() {
    if (db_) lib_->close_v2(db_);
    db_ = nullptr;
}

bool Db::exec(const std::string &sql, std::string *why) {
    if (!db_) {
        if (why) *why = "no database";
        return false;
    }
    char *err = nullptr;
    const int rc = lib_->exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != sqlite::kOk && why) *why = err ? err : lib_->errmsg(db_);
    if (err) lib_->free_mem(err);
    return rc == sqlite::kOk;
}

Stmt Db::prepare(const std::string &sql) {
    if (!db_) return Stmt();
    sqlite::sqlite3_stmt *st = nullptr;
    if (lib_->prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &st, nullptr) != sqlite::kOk) {
        if (st) lib_->finalize(st);
        return Stmt();
    }
    return Stmt(lib_, st);
}

long long Db::scalar(const std::string &sql, long long fallback) {
    Stmt s = prepare(sql);
    if (s.step() != sqlite::kRow || s.is_null(0)) return fallback;
    return s.i64(0);
}

std::string Db::scalar_text(const std::string &sql) {
    Stmt s = prepare(sql);
    if (s.step() != sqlite::kRow) return "";
    return s.text(0);
}

int Db::changes() const { return db_ ? lib_->changes(db_) : 0; }
long long Db::last_insert_rowid() const { return db_ ? lib_->last_insert_rowid(db_) : 0; }

void Db::interrupt() {
    if (db_) lib_->interrupt(db_);
}

bool Db::in_transaction() const { return db_ && lib_->get_autocommit(db_) == 0; }
std::string Db::errmsg() const { return db_ ? lib_->errmsg(db_) : "no database"; }
int Db::errcode() const { return db_ ? lib_->extended_errcode(db_) : sqlite::kError; }

/* ------------------------------------------------------------------------------------------------ Transaction */

Transaction::Transaction(Db &db) : db_(db) { open_ = db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (open_) db_.exec("ROLLBACK");
}

bool Transaction::commit() {
    if (!open_) return false;
    open_ = false;
    if (db_.exec("COMMIT")) return true;
    db_.exec("ROLLBACK");
    return false;
}

}  // namespace vinyl
