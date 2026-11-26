# SPDX-License-Identifier: MIT
"""The message store (real SQLite, no RNS): contacts, messages, retention, unread, batching, file modes, recovery."""
import os
import sqlite3
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from reticulum_bridge.store import Store  # noqa: E402

A, B = "a" * 32, "b" * 32


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="rnsstore-")
        self.path = os.path.join(self.dir, "messages.db")
        os.umask(0o077)
        self.s = Store(self.path)

    def tearDown(self):
        self.s.close()

    def peek(self, sql, *args):
        """What another process would read from the file right now (committed data only)."""
        db = sqlite3.connect(self.path)
        try:
            return db.execute(sql, args).fetchall()
        finally:
            db.close()

    def test_file_modes(self):
        self.s.add_message(A, "in", "hi", "received", unread=True)
        for name in os.listdir(self.dir):
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.dir, name)).st_mode), 0o600, name)

    def test_contacts(self):
        self.assertEqual(self.s.contacts(), [])
        self.s.add_contact(B, "Bea")
        self.s.add_contact(A, None)
        self.assertEqual([c["address"] for c in self.s.contacts()], [B, A])      # named first, anonymous last
        self.s.rename_contact(A, "Al")
        self.assertEqual(self.s.contact(A)["name"], "Al")
        with self.assertRaises(sqlite3.IntegrityError):
            self.s.add_contact(A, "again")
        self.s.remove_contact(A)
        self.assertIsNone(self.s.contact(A))

    def test_history_paging_and_preview_order(self):
        ids = [self.s.add_message(A, "out" if i % 2 else "in", "m%d" % i, "sending" if i % 2 else "received")["id"]
               for i in range(10)]
        rows, more = self.s.history(A, 4)
        self.assertEqual([r["text"] for r in rows], ["m6", "m7", "m8", "m9"])    # the newest four, oldest first
        self.assertTrue(more)
        rows, more = self.s.history(A, 4, before=rows[0]["id"])
        self.assertEqual([r["text"] for r in rows], ["m2", "m3", "m4", "m5"])
        rows, more = self.s.history(A, 4, before=rows[0]["id"])
        self.assertEqual([r["text"] for r in rows], ["m0", "m1"])
        self.assertFalse(more)
        self.assertEqual(self.s.history(B, 5), ([], False))
        self.assertEqual(ids, sorted(ids))

    def test_history_text_budget(self):
        for i in range(6):
            self.s.add_message(A, "in", "x" * 1000, "received")
        rows, more = self.s.history(A, 6, text_budget=2500)
        self.assertEqual(len(rows), 2)
        self.assertTrue(more)

    def test_conversations(self):
        self.s.add_message(A, "in", "first", "received", unread=True)
        self.s.add_message(B, "out", "to b", "sending")
        self.s.add_message(A, "in", "second", "received", unread=True)
        conv = self.s.conversations()
        self.assertEqual([c["address"] for c in conv], [A, B])                   # newest activity first
        self.assertEqual((conv[0]["count"], conv[0]["unread"], conv[0]["last"]["text"]), (2, 2, "second"))
        self.assertEqual((conv[1]["unread"], conv[1]["last"]["state"]), (0, "sending"))

    def test_unread_and_mark_read(self):
        self.s.add_message(A, "in", "1", "received", unread=True)
        self.s.add_message(A, "in", "2", "received", unread=True)
        self.s.add_message(B, "in", "3", "received", unread=True)
        self.s.add_message(B, "out", "mine", "sending")
        self.assertEqual((self.s.unread_total(), self.s.unread_of(A), self.s.unread_of(B)), (3, 2, 1))
        self.assertEqual(self.s.mark_read(A), 2)
        self.assertEqual((self.s.unread_total(), self.s.mark_read(A)), (1, 0))

    def test_state_only_moves_forward(self):
        m = self.s.add_message(A, "out", "x", "sending")["id"]
        self.assertTrue(self.s.set_state(m, "sent"))
        self.assertTrue(self.s.set_state(m, "delivered"))
        self.assertFalse(self.s.set_state(m, "failed", "late"))                  # finished: left alone
        self.assertFalse(self.s.set_state(m, "sent"))
        self.assertEqual(self.s.message(m)["state"], "delivered")
        f = self.s.add_message(A, "out", "y", "sending")["id"]
        self.assertTrue(self.s.set_state(f, "failed", "undelivered"))
        self.assertFalse(self.s.set_state(f, "delivered"))                      # no resurrection by a late callback
        i = self.s.add_message(A, "in", "z", "received")["id"]
        self.assertFalse(self.s.set_state(i, "failed"))                         # only outgoing messages have a state
        self.assertTrue(self.s.reset_for_retry(f))
        self.assertEqual((self.s.message(f)["state"], self.s.message(f)["detail"]), ("sending", None))
        self.assertFalse(self.s.reset_for_retry(m))                             # only a failed one goes again

    def test_interrupted_at_start(self):
        m = self.s.add_message(A, "out", "x", "sending")["id"]
        d = self.s.add_message(A, "out", "y", "sending")["id"]
        self.s.set_state(d, "delivered")
        self.s.flush()
        self.s.db.close()
        self.s = Store(self.path)
        self.assertEqual(self.s.fail_interrupted(), 1)
        self.assertEqual((self.s.message(m)["state"], self.s.message(m)["detail"]), ("failed", "interrupted"))
        self.assertEqual(self.s.message(d)["state"], "delivered")

    def test_retention_per_conversation_and_total(self):
        s = Store(os.path.join(self.dir, "small.db"), per_conv=5, total=12)
        try:
            for i in range(9):
                s.add_message(A, "in", "a%d" % i, "received")
            rows, _ = s.history(A, 50)
            self.assertEqual([r["text"] for r in rows], ["a4", "a5", "a6", "a7", "a8"])      # oldest went first
            for i in range(5):
                s.add_message(B, "in", "b%d" % i, "received")
            self.assertEqual(s.message_count(), 10)
            for i in range(5):
                s.add_message("c" * 32, "in", "c%d" % i, "received")
            self.assertEqual(s.message_count(), 12)                                # the total cap, oldest overall first
            self.assertEqual([r["text"] for r in s.history(A, 50)[0]], ["a7", "a8"])
            self.assertEqual(s.db.execute("SELECT COUNT(*) FROM messages").fetchone()[0], 12)
        finally:
            s.close()
        s = Store(os.path.join(self.dir, "small.db"), per_conv=5, total=12)    # the counter is right after a reopen
        self.assertEqual(s.message_count(), 12)
        s.close()

    def test_default_caps_are_the_documented_ones(self):
        self.assertEqual((self.s.per_conv, self.s.cap_total, self.s.heard_max), (2000, 20000, 500))

    def test_batching_new_messages_commit_at_once_the_rest_waits_for_flush(self):
        m = self.s.add_message(A, "out", "x", "sending")["id"]
        self.assertEqual(self.peek("SELECT state FROM messages WHERE id=?", m), [("sending",)])   # on disk at once
        self.s.set_state(m, "delivered")
        self.s.add_message(B, "in", "u", "received", unread=True)                                  # commits everything
        self.s.mark_read(B)
        self.assertEqual(self.peek("SELECT unread FROM messages WHERE address=?", B), [(1,)])      # not yet
        self.s.put_heard({"address": A, "name": "Al", "time": 5.0, "hops": 1, "interface": "x"})
        self.assertEqual(self.peek("SELECT COUNT(*) FROM heard"), [(0,)])
        self.s.flush()
        self.assertEqual(self.peek("SELECT unread FROM messages WHERE address=?", B), [(0,)])
        self.assertEqual(self.peek("SELECT name FROM heard"), [("Al",)])

    def test_heard_is_kept_and_capped(self):
        s = Store(os.path.join(self.dir, "heard.db"), heard_max=3)
        for i in range(6):
            s.put_heard({"address": "%032x" % i, "name": "n%d" % i, "time": float(i), "hops": 1, "interface": None})
        s.flush()
        s.close()
        s = Store(os.path.join(self.dir, "heard.db"), heard_max=3)
        try:
            self.assertEqual([e["name"] for e in s.load_heard()], ["n3", "n4", "n5"])
        finally:
            s.close()

    def test_delete_conversation_keeps_the_rest(self):
        self.s.add_message(A, "in", "1", "received", unread=True)
        self.s.add_message(B, "in", "2", "received", unread=True)
        self.assertEqual(self.s.delete_conversation(A), 1)
        self.assertEqual((self.s.message_count(), self.s.unread_total()), (1, 1))
        self.assertEqual([c["address"] for c in self.s.conversations()], [B])

    def test_survives_close_and_reopen(self):
        self.s.add_contact(A, "Al")
        self.s.add_message(A, "in", "kept", "received", unread=True)
        self.s.close()
        self.s = Store(self.path)
        self.assertEqual(self.s.contact(A)["name"], "Al")
        self.assertEqual(self.s.history(A, 5)[0][0]["text"], "kept")
        self.assertEqual(self.s.unread_total(), 1)

    def test_a_damaged_file_is_moved_aside(self):
        self.s.close()
        with open(self.path, "wb") as f:
            f.write(b"this is not a database " * 200)
        logs = []
        self.s = Store(self.path, log=logs.append)
        self.assertTrue(any("moved to" in m for m in logs))
        self.assertTrue([n for n in os.listdir(self.dir) if ".corrupt-" in n])
        self.assertEqual(self.s.message_count(), 0)
        self.s.add_message(A, "in", "fresh", "received")

    def test_delivered_at_is_set_when_delivered_and_cleared_by_a_retry(self):
        t0 = time.time()
        m = self.s.add_message(A, "out", "hello", "sending", when=t0 - 600)             # sent ten minutes ago
        self.assertIsNone(m["delivered_at"])
        self.s.set_state(m["id"], "failed", "undelivered")
        self.assertIsNone(self.s.message(m["id"])["delivered_at"])
        self.assertTrue(self.s.reset_for_retry(m["id"]))
        self.assertTrue(self.s.set_state(m["id"], "delivered"))
        row = self.s.message(m["id"])
        self.assertGreaterEqual(row["delivered_at"], t0 - 0.001)                              # the delivery, not the send
        self.assertAlmostEqual(row["time"], t0 - 600, places=2)                         # the order/send time is kept
        self.assertEqual(self.s.history(A, 10)[0][0]["delivered_at"], row["delivered_at"])
        self.assertEqual(self.s.conversations()[0]["last"]["delivered_at"], row["delivered_at"])
        self.assertFalse(self.s.set_state(m["id"], "failed", "x"))                      # a delivered message stays
        self.assertEqual(self.s.message(m["id"])["delivered_at"], row["delivered_at"])
        self.assertIsNone(self.s.add_message(A, "in", "hi", "received")["delivered_at"])

    def test_schema_1_database_is_migrated_and_old_rows_fall_back_to_the_send_time(self):
        self.s.close()
        os.remove(self.path)
        for ext in ("-wal", "-shm"):
            if os.path.exists(self.path + ext):
                os.remove(self.path + ext)
        db = sqlite3.connect(self.path)                                                 # what 0.3.0-0.3.3 wrote
        db.executescript("""
            CREATE TABLE contacts(address TEXT PRIMARY KEY, name TEXT, added REAL NOT NULL);
            CREATE TABLE messages(
                id INTEGER PRIMARY KEY AUTOINCREMENT, address TEXT NOT NULL, dir TEXT NOT NULL, time REAL NOT NULL,
                text TEXT NOT NULL, state TEXT NOT NULL, detail TEXT, unread INTEGER NOT NULL DEFAULT 0);
            CREATE INDEX messages_by_address ON messages(address, id);
            CREATE TABLE heard(address TEXT PRIMARY KEY, name TEXT, time REAL NOT NULL, hops INTEGER, interface TEXT);
            INSERT INTO contacts VALUES('%s', 'Bea', 100);
            INSERT INTO messages(address, dir, time, text, state, detail, unread) VALUES
                ('%s', 'out', 1000.5, 'old delivered', 'delivered', NULL, 0),
                ('%s', 'out', 1001.5, 'old failed', 'failed', 'undelivered', 0),
                ('%s', 'in', 1002.5, 'old in', 'received', NULL, 1);
            INSERT INTO heard VALUES('%s', 'Bea', 1500, 1, 'LoRa radio');
            PRAGMA user_version=1;""" % (B, A, A, A, B))
        db.commit()
        db.close()
        self.s = Store(self.path)                                                       # opens, adds the column
        rows, more = self.s.history(A, 10)
        self.assertEqual([(r["text"], r["state"], r["time"], r["delivered_at"]) for r in rows],
                         [("old delivered", "delivered", 1000.5, 1000.5),               # falls back to the send time
                          ("old failed", "failed", 1001.5, None), ("old in", "received", 1002.5, None)])
        self.assertEqual(self.s.contacts()[0]["name"], "Bea")
        self.assertEqual(self.s.load_heard()[0]["time"], 1500)
        self.assertEqual(self.s.unread_total(), 1)
        self.assertEqual(self.peek("PRAGMA user_version")[0][0], 2)
        # new deliveries on the migrated file store their own time, and a retry of an old failed row works
        failed = rows[1]["id"]
        self.assertTrue(self.s.reset_for_retry(failed))
        self.assertTrue(self.s.set_state(failed, "delivered"))
        self.assertGreater(self.s.message(failed)["delivered_at"], 1e9 + 1e8)
        self.s.close()
        self.s = Store(self.path)                                                       # reopening twice changes nothing
        self.assertEqual(self.s.message(rows[0]["id"])["delivered_at"], 1000.5)
        self.assertGreater(self.s.message(failed)["delivered_at"], 1e9 + 1e8)

    def test_text_roundtrip_unicode(self):
        t = "café ☃ \U0001F600 line\nsecond"
        m = self.s.add_message(A, "in", t, "received")
        self.assertEqual((m["text"], m["bytes"]), (t, len(t.encode("utf-8"))))


if __name__ == "__main__":
    unittest.main()
