/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the SQLite table and its RAII wrappers (core/db), on an in-memory database of the build box's
 * libsqlite3.so.0 (the boards' 3.46.1 and the build box's 3.45 both have FTS5 with the trigram tokenizer). Without the
 * library the group says so and skips: nothing is faked.
 */
#include "check.hpp"

#include "db/sqlite_lib.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace vinyl;

void test_sqlite() {
    /* the missing-library path: a readable reason that names the package */
    SqliteLib none;
    std::string why;
    CHECK(!load_sqlite_lib("libvinyl-no-such-sqlite.so.0", none, why));
    CHECK(why.find("libsqlite3-0") != std::string::npos);
    CHECK(!none.complete());

    const SqliteLib *lib = real_sqlite_lib(&why);
    if (!lib) {
        SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
        return;
    }
    CHECK(lib->complete());
    CHECK(!lib->version.empty());
    std::fprintf(stderr, "  SQLite %s\n", lib->version.c_str());

    Db db;
    CHECK(db.open(":memory:", 0, nullptr, &why));
    CHECK(db.is_open());
    CHECK(db.exec("create table t(id integer primary key, name text, n int, x real)"));
    std::string err;
    CHECK(!db.exec("create table broken(", &err));
    CHECK(!err.empty());
    CHECK(!db.prepare("select nothing from nowhere").ok());

    /* bind: integers, text (copied), NULL, doubles; changes and last_insert_rowid */
    {
        Stmt ins = db.prepare("insert into t(name, n, x) values(?, ?, ?)");
        CHECK(ins.ok());
        {
            std::string gone = "Mötley Crüe";
            ins.bind(1, gone).bind(2, 42).bind(3, 2.5);
            gone.assign(gone.size(), 'x'); /* the statement holds its own copy */
        }
        CHECK(ins.run());
        CHECK_EQ(db.changes(), 1);
        CHECK_EQ(db.last_insert_rowid(), 1LL);
        ins.reset();
        ins.bind(1, "夜明けのスキャット").bind_null(2).bind(3, -1.0);
        CHECK(ins.run());
        CHECK_EQ(db.last_insert_rowid(), 2LL);
        ins.reset();
        ins.bind(1, static_cast<const char *>(nullptr)).bind(2, 7ULL); /* null text binds NULL; x unbound stays NULL */
        CHECK(ins.run());
    }
    {
        Stmt q = db.prepare("select id, name, n, x from t order by id");
        CHECK_EQ(q.columns(), 4);
        CHECK_EQ(q.step(), sqlite::kRow);
        CHECK_EQ(q.text(1), std::string("Mötley Crüe"));
        CHECK_EQ(q.i32(2), 42);
        CHECK_NEAR(q.f64(3), 2.5, 1e-9);
        CHECK_EQ(q.step(), sqlite::kRow);
        CHECK_EQ(q.text(1), std::string("夜明けのスキャット"));
        CHECK(q.is_null(2));
        CHECK_EQ(q.step(), sqlite::kRow);
        CHECK(q.is_null(1));
        CHECK_EQ(q.text(1), std::string(""));
        CHECK(q.is_null(3));
        CHECK_EQ(q.i64(2), 7LL);
        CHECK_EQ(q.step(), sqlite::kDone);
    }
    CHECK(db.exec("update t set n = 0"));
    CHECK_EQ(db.changes(), 3);
    CHECK_EQ(db.scalar("select count(*) from t"), 3LL);
    CHECK_EQ(db.scalar("select n from t where id = 99", -5), -5LL);
    CHECK_EQ(db.scalar_text("select name from t where id = 1"), std::string("Mötley Crüe"));

    /* statements move; the moved-from one is empty and finalizes nothing */
    {
        Stmt a = db.prepare("select count(*) from t");
        Stmt b = std::move(a);
        CHECK(!a.ok());
        CHECK(b.ok());
        CHECK_EQ(b.step(), sqlite::kRow);
        CHECK_EQ(b.i64(0), 3LL);
        Stmt c;
        c = std::move(b);
        CHECK(c.ok());
        CHECK_EQ(a.step(), sqlite::kError);
    }

    /* transactions: rolled back unless committed */
    {
        Transaction tx(db);
        CHECK(tx.ok());
        CHECK(db.in_transaction());
        CHECK(db.exec("delete from t"));
    }
    CHECK(!db.in_transaction());
    CHECK_EQ(db.scalar("select count(*) from t"), 3LL);
    {
        Transaction tx(db);
        CHECK(db.exec("delete from t where id = 3"));
        CHECK(tx.commit());
        CHECK(!tx.commit());
    }
    CHECK_EQ(db.scalar("select count(*) from t"), 2LL);

    /* FTS5 with the trigram tokenizer, contentless with deletes (report 060 3a): the boards' library has them */
    std::string fts_err;
    const bool fts = db.exec("create virtual table f using fts5(a, content='', contentless_delete=1, "
                             "tokenize='trigram remove_diacritics 1')",
                             &fts_err);
    CHECK(fts);
    if (!fts) std::fprintf(stderr, "  FTS5 trigram missing in this libsqlite3: %s\n", fts_err.c_str());
    if (fts) {
        CHECK(db.exec("insert into f(rowid, a) values(1, 'Beyoncé'), (2, 'Mötley Crüe'), (3, '夜明けのスキャット')"));
        CHECK_EQ(db.scalar("select rowid from f where f match '\"beyonce\"'"), 1LL);
        CHECK_EQ(db.scalar("select rowid from f where f match '\"MOTLEY\"'"), 2LL);
        CHECK_EQ(db.scalar("select rowid from f where f match '\"明けの\"'"), 3LL);
        CHECK_EQ(db.scalar("select count(*) from f where f match '\"ab\"'"), 0LL); /* under 3 characters: nothing */
        CHECK(db.exec("delete from f where rowid = 1"));
        CHECK_EQ(db.scalar("select count(*) from f where f match '\"beyonce\"'"), 0LL);
    }

    /* interrupt from another thread stops a running statement */
    {
        std::atomic<int> rc{-1};
        std::atomic<bool> started{false};
        std::thread th([&] {
            Stmt s = db.prepare("with recursive c(x) as (select 1 union all select x + 1 from c where x < 2000000000) "
                                "select count(*) from c");
            started = true;
            rc = s.step();
        });
        while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (int i = 0; i < 500 && rc.load() < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            db.interrupt();
        }
        th.join();
        CHECK_EQ(rc.load(), sqlite::kInterrupt);
    }
    /* the connection works again after the interrupt */
    CHECK_EQ(db.scalar("select count(*) from t"), 2LL);

    /* a moved connection keeps working; the old one is closed */
    Db other = std::move(db);
    CHECK(!db.is_open());
    CHECK(other.is_open());
    CHECK_EQ(other.scalar("select count(*) from t"), 2LL);
    CHECK(!db.prepare("select 1").ok());
    other.close();
    CHECK(!other.is_open());

    /* a path that cannot be opened */
    Db bad;
    CHECK(!bad.open("/nonexistent-dir-vinyl/x.db", 0, nullptr, &why));
    CHECK(!why.empty());
}
