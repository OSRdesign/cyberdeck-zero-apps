# SPDX-License-Identifier: MIT
"""Contacts, messages and the heard list on disk: one SQLite file (stdlib sqlite3), mode 0600.

Write policy (the data folder is on the SD card): a new message (sent or received), every contact change and a
final delivery state (delivered, failed) are committed at once, so a power cut loses nothing the user did or needs to
act on; the rest (a "read" mark, a heard announce, the in-between state `sent`) is batched and committed by flush(),
which the bridge calls every 15 s and at exit (WAL journal, synchronous NORMAL: no fsync per commit, one checkpoint at
close). Retention: at most `per_conv` messages per conversation and
`total` messages overall, oldest first, trimmed at every insert; the heard list keeps `heard_max` rows. Message text is
never logged. A file SQLite cannot open is moved aside (messages.db.corrupt-<time>) and a new one is started.
"""
import os
import sqlite3
import threading
import time

PER_CONV = 2000
TOTAL = 20000
HEARD_MAX = 500
SCHEMA = """
CREATE TABLE IF NOT EXISTS contacts(address TEXT PRIMARY KEY, name TEXT, added REAL NOT NULL);
CREATE TABLE IF NOT EXISTS messages(
    id INTEGER PRIMARY KEY AUTOINCREMENT, address TEXT NOT NULL, dir TEXT NOT NULL, time REAL NOT NULL,
    text TEXT NOT NULL, state TEXT NOT NULL, detail TEXT, unread INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS messages_by_address ON messages(address, id);
CREATE TABLE IF NOT EXISTS heard(address TEXT PRIMARY KEY, name TEXT, time REAL NOT NULL, hops INTEGER, interface TEXT);
"""
MSG_COLS = "id, address, dir, time, text, state, detail, unread"
RANK = {"sending": 0, "sent": 1, "delivered": 2, "failed": 2}


def _msg(r):
    return {"id": r[0], "address": r[1], "dir": r[2], "time": round(r[3], 3), "text": r[4], "state": r[5],
            "detail": r[6], "unread": bool(r[7]), "bytes": len(r[4].encode("utf-8"))}


class Store:
    def __init__(self, path, per_conv=PER_CONV, total=TOTAL, heard_max=HEARD_MAX, log=lambda m: None):
        self.path, self.per_conv, self.cap_total, self.heard_max, self.log = path, per_conv, total, heard_max, log
        self.lock = threading.RLock()
        self.dirty = False
        self.heard_dirty = {}
        self.db = self._open()
        self.total = self.db.execute("SELECT COUNT(*) FROM messages").fetchone()[0]

    # --- file ---
    def _connect(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)      # created 0600 whatever the umask
        os.close(fd)
        db = sqlite3.connect(self.path, check_same_thread=False, isolation_level="")
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=NORMAL")
        db.executescript(SCHEMA)
        db.execute("PRAGMA user_version=1")
        db.commit()
        db.execute("SELECT COUNT(*) FROM messages").fetchone()      # touches the file: a damaged one fails here
        return db

    def _open(self):
        try:
            return self._connect()
        except sqlite3.DatabaseError as e:
            aside = "%s.corrupt-%d" % (self.path, int(time.time()))
            self.log("the message database is unreadable (%s): moved to %s, starting a new one" % (e, aside))
            for ext in ("", "-wal", "-shm"):
                try:
                    os.replace(self.path + ext, aside + ext)
                except OSError:
                    pass
            return self._connect()

    def close(self):
        with self.lock:
            try:
                self.flush()
                self.db.close()
            except sqlite3.Error as e:
                self.log("database close: %s" % e)

    def flush(self):
        """Commits the batched writes (delivery states, read marks, heard announces)."""
        with self.lock:
            if self.heard_dirty:
                rows, self.heard_dirty = list(self.heard_dirty.values()), {}
                self.db.executemany("INSERT OR REPLACE INTO heard(address, name, time, hops, interface) "
                                    "VALUES(?,?,?,?,?)", [(r["address"], r["name"], r["time"], r["hops"],
                                                           r["interface"]) for r in rows])
                self.db.execute("DELETE FROM heard WHERE address IN (SELECT address FROM heard ORDER BY time DESC "
                                "LIMIT -1 OFFSET ?)", (self.heard_max,))
                self.dirty = True
            if self.dirty:
                self.db.commit()
                self.dirty = False

    def _commit_now(self):
        self.db.commit()
        self.dirty = False

    # --- contacts ---
    def contacts(self):
        with self.lock:
            return [{"address": a, "name": n, "added": round(t, 3)} for a, n, t in self.db.execute(
                "SELECT address, name, added FROM contacts ORDER BY LOWER(COALESCE(NULLIF(name, ''), 'zzz')), address")]

    def contact(self, address):
        with self.lock:
            r = self.db.execute("SELECT address, name, added FROM contacts WHERE address=?", (address,)).fetchone()
        return {"address": r[0], "name": r[1], "added": round(r[2], 3)} if r else None

    def add_contact(self, address, name):
        with self.lock:
            self.db.execute("INSERT INTO contacts(address, name, added) VALUES(?,?,?)", (address, name, time.time()))
            self._commit_now()

    def rename_contact(self, address, name):
        with self.lock:
            self.db.execute("UPDATE contacts SET name=? WHERE address=?", (name, address))
            self._commit_now()

    def remove_contact(self, address):
        with self.lock:
            self.db.execute("DELETE FROM contacts WHERE address=?", (address,))
            self._commit_now()

    # --- messages ---
    def add_message(self, address, direction, text, state, unread=False, detail=None, when=None):
        with self.lock:
            cur = self.db.execute("INSERT INTO messages(address, dir, time, text, state, detail, unread) "
                                  "VALUES(?,?,?,?,?,?,?)", (address, direction, when or time.time(), text, state,
                                                            detail, 1 if unread else 0))
            rid = cur.lastrowid
            self.total += 1
            n = self.db.execute("SELECT COUNT(*) FROM messages WHERE address=?", (address,)).fetchone()[0]
            if n > self.per_conv:
                self._trim("WHERE address=? ORDER BY id LIMIT ?", (address, n - self.per_conv))
            if self.total > self.cap_total:
                self._trim("ORDER BY id LIMIT ?", (self.total - self.cap_total,))
            self._commit_now()
            return self.message(rid)

    def _trim(self, tail, args):
        cur = self.db.execute("DELETE FROM messages WHERE id IN (SELECT id FROM messages " + tail + ")", args)
        self.total -= cur.rowcount

    def message(self, rid):
        with self.lock:
            r = self.db.execute("SELECT %s FROM messages WHERE id=?" % MSG_COLS, (rid,)).fetchone()
        return _msg(r) if r else None

    def set_state(self, rid, state, detail=None):
        """Forward only: sending -> sent -> delivered | failed; a finished message is left alone. True when changed."""
        with self.lock:
            cur = self.db.execute("UPDATE messages SET state=?, detail=? WHERE id=? AND dir='out' AND state IN (%s)" %
                                  ",".join("'%s'" % s for s, r in RANK.items() if r < RANK[state]), (state, detail, rid))
            if cur.rowcount > 0:
                if state == "sent":
                    self.dirty = True                        # batched: a state that can still change
                else:
                    self._commit_now()                       # delivered / failed: final, and the user may act on it
            return cur.rowcount > 0

    def reset_for_retry(self, rid):
        """A failed outgoing message goes back to sending (the retry verb). The row keeps its place."""
        with self.lock:
            cur = self.db.execute("UPDATE messages SET state='sending', detail=NULL WHERE id=? AND dir='out' AND "
                                  "state='failed'", (rid,))
            self._commit_now()
            return cur.rowcount > 0

    def fail_interrupted(self):
        """At start: what was still sending when the last run ended will never finish (no automatic resend)."""
        with self.lock:
            cur = self.db.execute("UPDATE messages SET state='failed', detail='interrupted' WHERE dir='out' AND "
                                  "state='sending'")
            self._commit_now()
            return cur.rowcount

    def history(self, address, limit, before=None, text_budget=200000):
        """The newest `limit` messages older than id `before`, oldest first; stops early when the text adds up to
        `text_budget` bytes (one response line stays well under the server's output limit). Returns (rows, more)."""
        with self.lock:
            rows = self.db.execute("SELECT %s FROM messages WHERE address=? AND id<? ORDER BY id DESC LIMIT ?" % MSG_COLS,
                                   (address, before if before else 1 << 62, limit + 1)).fetchall()
        more = len(rows) > limit
        out, used = [], 0
        for r in rows[:limit]:
            used += len(r[4])
            if out and used > text_budget:
                more = True
                break
            out.append(_msg(r))
        out.reverse()
        return out, more

    def conversations(self, limit=200):
        with self.lock:
            groups = self.db.execute("SELECT address, COUNT(*), SUM(unread), MAX(id) FROM messages GROUP BY address "
                                     "ORDER BY MAX(id) DESC LIMIT ?", (limit,)).fetchall()
            out = []
            for address, n, unread, last in groups:
                m = self.message(last)
                out.append({"address": address, "count": n, "unread": unread or 0, "last": m})
            return out

    def unread_total(self):
        with self.lock:
            return self.db.execute("SELECT COALESCE(SUM(unread), 0) FROM messages").fetchone()[0]

    def unread_of(self, address):
        with self.lock:
            return self.db.execute("SELECT COALESCE(SUM(unread), 0) FROM messages WHERE address=?",
                                   (address,)).fetchone()[0]

    def mark_read(self, address):
        with self.lock:
            cur = self.db.execute("UPDATE messages SET unread=0 WHERE address=? AND unread=1", (address,))
            self.dirty = self.dirty or cur.rowcount > 0
            return cur.rowcount

    def delete_conversation(self, address):
        with self.lock:
            cur = self.db.execute("DELETE FROM messages WHERE address=?", (address,))
            self.total -= cur.rowcount
            self._commit_now()
            return cur.rowcount

    def message_count(self):
        with self.lock:
            return self.total

    # --- heard ---
    def load_heard(self):
        with self.lock:
            return [{"address": a, "name": n, "time": t, "hops": h, "interface": i} for a, n, t, h, i in
                    self.db.execute("SELECT address, name, time, hops, interface FROM heard ORDER BY time")]

    def put_heard(self, entry):
        with self.lock:
            self.heard_dirty[entry["address"]] = entry
