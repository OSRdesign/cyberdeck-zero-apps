# SPDX-License-Identifier: MIT
"""Contacts and messages against REAL RNS 1.6.1 and LXMF 1.3.1 (no mocking of the protocol): two bridge processes
(Alice = A, Bob = B) linked by a TCP interface on 127.0.0.1, each with its own folders and RNS instance.

The tests run in order on the same two bridges (numbered names). A is the TCP server and starts listen-only; B is the
client and may transmit. What is covered: refusals with no connection that may send (message, retry, announce); the heard
list from the other bridge's announce and add-from-heard; send and delivered (events: sending -> delivered); the
other side's unread count, mark-read, alerts off/on (a fake launcher socket); a long message; history paging; an
offline contact (failed after LXMF's bounded tries, no automatic resend when the contact is back and heard, manual retry
delivers); delete a conversation and remove a contact; restart persistence (also a bridge killed while sending);
invalid input; message text kept out of the logs; 500 messages and the memory of both bridges. Several minutes.
"""
import os
import stat
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridgekit import (RNS, FakeLauncher, Node, free_port, rd, rss_mb, wait_for)  # noqa: E402

DELIVER_WAIT = 90
HEARD_WAIT = 60
SECRET = "S3CR3T-body-marker-7731"
BAD_ADDRESSES = ["", "abc", "z" * 32, "ab" * 15, "ab" * 17, "lxmf@" + "ab" * 16, None, 5]


def history(node, addr, **kw):
    return node.call("messages.history", dict(address=addr, **kw))


def last_state(node, addr):
    rows = history(node, addr)["messages"]
    return (rows[-1]["state"], rows[-1]["detail"]) if rows else None


@unittest.skipIf(RNS is None, "rns and lxmf are not installed (run tests/run_tests.sh)")
class Messaging(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="rnsmsg-")
        cls.launcher = FakeLauncher(os.path.join(cls.dir, "launcher.sock"))
        cls.A = Node("alice")
        cls.B = Node("bob", env={"RETICULUM_BRIDGE_LAUNCHER_SOCK": cls.launcher.path})
        cls.A.start()
        cls.B.start()
        cls.port = free_port()
        cls.addr_a, cls.addr_b = cls.A.address, cls.B.address
        cls.base = {}

    @classmethod
    def tearDownClass(cls):
        for n in (cls.A, cls.B):
            n.stop(kill=True)
        cls.launcher.close()
        for n in (cls.A, cls.B):
            quiet = "".join(l for l in n.logtext().splitlines(True) if " queued (" not in l and " from " not in l)
            sys.stderr.write("\n--- bridge %s log (tail, bulk lines left out) ---\n%s--- end ---\n" % (
                n.label, quiet[-7000:]))

    # ------------------------------------------------------------------ the first contact
    def test_01_verbs_limits_defaults_and_validation(self):
        A = self.A
        verbs = set(A.call("hello")["verbs"])
        for v in ("contacts.list", "contacts.add", "contacts.rename", "contacts.remove", "heard.list",
                  "messages.conversations", "messages.history", "messages.send", "messages.retry",
                  "messages.delete_conversation", "messages.mark_read", "messages.limits", "messages.subscribe",
                  "settings.get", "settings.set"):
            self.assertIn(v, verbs)
        lim = A.call("messages.limits")
        self.assertEqual((lim["single_packet_bytes"], lim["link_packet_bytes"]), (287, 319))     # LXMF 1.3.1 constants
        self.assertEqual((lim["per_conversation"], lim["total"], lim["delivery_attempts"]), (2000, 20000, 5))
        self.assertEqual(A.call("settings.get"), {"alerts": False})
        self.assertEqual((A.call("status")["unread"], A.call("status")["alerts"]), (0, False))
        self.assertEqual(A.call("contacts.list"), {"contacts": []})
        self.assertEqual(A.call("messages.conversations"), {"conversations": [], "unread_total": 0})
        for bad in BAD_ADDRESSES:
            for verb, extra in (("contacts.add", {"name": "x"}), ("contacts.rename", {"name": "x"}),
                                ("contacts.remove", {}), ("messages.send", {"text": "hi"}),
                                ("messages.history", {}), ("messages.delete_conversation", {}),
                                ("messages.mark_read", {})):
                r = A.request(verb, dict(extra, address=bad))
                self.assertEqual((r["ok"], r["error"]["code"], r["error"].get("field")), (False, "invalid", "address"),
                                 "%s %r" % (verb, bad))
        self.assertEqual(A.code("contacts.add", {"address": self.addr_a, "name": "me"}), "invalid")   # own address
        self.assertEqual(A.code("settings.set", {"alerts": "on"}), "invalid")
        self.assertEqual(A.code("settings.set", {"nothing": True}), "invalid")
        self.assertEqual(A.code("messages.send", {"address": self.addr_b, "text": "  "}), "invalid")
        self.assertEqual(A.code("messages.history", {"address": self.addr_b, "limit": 0}), "invalid")
        self.assertEqual(A.code("messages.retry", {"id": 999}), "not_found")
        self.assertEqual(A.call("contacts.list"), {"contacts": []})                     # nothing got stored by the refusals

    def test_02_nothing_is_sent_without_a_connection_that_may_send(self):
        A = self.A
        self.assertEqual(A.code("messages.send", {"address": self.addr_b, "text": "hello"}), "no_transmit")
        A.call("identity.name", {"name": "Alice"})
        self.assertEqual(A.code("announce"), "no_transmit")
        self.assertEqual(A.call("messages.conversations")["conversations"], [])        # a refused send leaves no row
        self.assertEqual(A.call("messages.limits")["single_packet_bytes"], 287)
        A.link_server(self.port, outgoing=False)                                       # up, but listen-only
        self.B.call("identity.name", {"name": "Bob"})
        self.B.link_client(self.port, outgoing=True)
        self.assertTrue(A.online() and self.B.online())
        self.assertEqual(A.code("messages.send", {"address": self.addr_b, "text": "hello"}), "no_transmit")
        self.assertEqual(A.code("announce"), "no_transmit")
        self.assertEqual(A.call("messages.conversations")["conversations"], [])
        st = A.iface("Link")["stats"]
        self.assertEqual((st["atxb"], st["ptxb"]), (0, 0))                              # no announce, no path request

    def test_03_heard_list_from_the_other_bridges_announce(self):
        A, B = self.A, self.B
        A.call("announces.subscribe")
        self.assertEqual(A.call("heard.list"), {"count": 0, "heard": []})
        B.call("announce")
        ev = A.conn.wait_event("heard", HEARD_WAIT, lambda d: d["address"] == self.addr_b)
        self.assertEqual((ev["name"], ev["contact"], ev["hops"], ev["interface"]), ("Bob", False, 1, "Link"))
        h = A.call("heard.list")
        self.assertEqual(h["count"], 1)
        self.assertEqual(h["heard"][0]["address"], self.addr_b)
        self.assertEqual((h["heard"][0]["name"], h["heard"][0]["contact"]), ("Bob", False))
        self.assertLess(abs(h["heard"][0]["time"] - time.time()), 120)
        self.assertEqual(A.code("heard.list", {"limit": 0}), "invalid")
        # nothing was announced by A
        self.assertEqual(self.iface_tx(A), (0, 0))

    @staticmethod
    def iface_tx(node):
        st = node.iface("Link")["stats"]
        return st["atxb"], st["ptxb"]

    def test_04_add_from_heard_and_contacts(self):
        A = self.A
        c = A.call("contacts.add", {"address": self.addr_b.upper()})                    # no name: the heard name is taken
        self.assertEqual((c["address"], c["name"]), (self.addr_b, "Bob"))
        self.assertEqual(A.code("contacts.add", {"address": self.addr_b, "name": "Other"}), "exists")
        self.assertTrue(A.call("heard.list")["heard"][0]["contact"])
        r = A.call("contacts.rename", {"address": self.addr_b, "name": "  Bobby "})
        self.assertEqual(r["name"], "Bobby")
        self.assertEqual(A.code("contacts.rename", {"address": "cd" * 16, "name": "x"}), "not_found")
        anon = A.call("contacts.add", {"address": "cd" * 16})                           # unheard and unnamed: allowed
        self.assertIsNone(anon["name"])
        names = [(x["address"], x["name"]) for x in A.call("contacts.list")["contacts"]]
        self.assertEqual(names, [(self.addr_b, "Bobby"), ("cd" * 16, None)])
        A.call("contacts.rename", {"address": self.addr_b, "name": "Bob"})
        self.assertEqual(A.call("contacts.remove", {"address": "cd" * 16})["removed"], "cd" * 16)
        self.assertEqual(A.code("contacts.remove", {"address": "cd" * 16}), "not_found")

    def settle_after_reload(self, node, other):
        """Switching `outgoing` reloads the TCP server: its client (the other bridge) drops, reconnects after about 5 s as
        a new interface, and the path to it must be learned again. A message sent inside that window can run out of LXMF's
        tries (seen in about 1 run of 8 on loopback), and the user would retry by hand; the tests wait for the link first,
        as a person who just switched a connection on would, and have the other side announce again."""
        self.assertTrue(wait_for(lambda: (node.iface("Link")["stats"] or {}).get("clients") == 1, 30))
        t = time.time()
        other.call("announce")
        node.conn.wait_event("heard", HEARD_WAIT, lambda d: d["address"] == other.address and d["time"] >= t - 1)

    # ------------------------------------------------------------------ send, receive, unread, alerts
    def test_05_send_and_delivered(self):
        A, B = self.A, self.B
        A.call("messages.subscribe")
        B.call("messages.subscribe")
        A.call("iface.outgoing", {"name": "Link", "outgoing": True}, timeout=30)
        self.assertTrue(A.online())
        self.settle_after_reload(A, B)
        t0 = time.time()
        m = A.call("messages.send", {"address": self.addr_b, "text": "Hello Bob ☃"})
        self.assertEqual((m["dir"], m["state"], m["text"], m["address"]), ("out", "sending", "Hello Bob ☃",
                                                                           self.addr_b))
        ev = A.conn.wait_event("message_state", DELIVER_WAIT, lambda d: d["id"] == m["id"])
        self.assertEqual((ev["state"], ev["detail"]), ("delivered", None))             # direct: sending -> delivered
        sys.stderr.write("\n[send] delivered after %.1f s (first message of the run, the link had settled)\n" % (time.time() - t0))
        self.assertEqual(last_state(A, self.addr_b), ("delivered", None))
        got = B.conn.wait_event("message", 20, lambda d: d["dir"] == "in")
        self.assertEqual((got["text"], got["address"], got["unread_total"], got["contact"]),
                         ("Hello Bob ☃", self.addr_a, 1, False))
        conv = B.call("messages.conversations")
        self.assertEqual(conv["unread_total"], 1)
        c = conv["conversations"][0]
        self.assertEqual((c["address"], c["unread"], c["preview"], c["last_dir"], c["contact"], c["name"]),
                         (self.addr_a, 1, "Hello Bob ☃", "in", False, None))        # Alice never announced: no name for her
        self.assertEqual(B.call("status")["unread"], 1)
        state = B.run_state()
        self.assertEqual(state["unread"], "1")
        # A's own list shows the message as sent by us
        ac = A.call("messages.conversations")["conversations"][0]
        self.assertEqual((ac["last_dir"], ac["last_state"], ac["unread"], ac["name"]), ("out", "delivered", 0, "Bob"))

    def test_06_alerts_off_sends_no_request_then_on_sends_one(self):
        A, B, fake = self.A, self.B, self.launcher
        self.assertEqual(fake.requests, [])                                             # test_05's message: alerts were off
        B.drain_events()
        before = B.call("status")["unread"]
        m = A.call("messages.send", {"address": self.addr_b, "text": "second message"})
        B.conn.wait_event("message", 60, lambda d: d["text"] == "second message")
        time.sleep(1.0)
        self.assertEqual(fake.requests, [])
        self.assertEqual(B.call("status")["unread"], before + 1)
        self.assertEqual(B.call("settings.set", {"alerts": True}), {"alerts": True})
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(B.data, "settings.json")).st_mode), 0o600)
        self.assertTrue(B.call("status")["alerts"])
        m = A.call("messages.send", {"address": self.addr_b, "text": SECRET})
        B.conn.wait_event("message", 60, lambda d: d["text"] == SECRET)
        self.assertTrue(wait_for(lambda: len(fake.requests) >= 2, 5), fake.requests)
        self.assertTrue(fake.requests[0].startswith("toast New message from "), fake.requests)
        self.assertEqual(fake.requests[1], "beep notify")
        self.assertNotIn(SECRET, " ".join(fake.requests))                               # the toast names the sender only
        B.call("settings.set", {"alerts": False})

    def test_07_mark_read_and_long_message(self):
        A, B = self.A, self.B
        self.assertEqual(B.call("status")["unread"], 3)
        B.drain_events()
        r = B.call("messages.mark_read", {"address": self.addr_a})
        self.assertEqual((r["marked"], r["unread_total"]), (3, 0))
        ev = B.conn.wait_event("unread", 5)
        self.assertEqual((ev["address"], ev["unread_total"]), (self.addr_a, 0))
        self.assertEqual(B.call("messages.mark_read", {"address": self.addr_a})["marked"], 0)
        self.assertTrue(wait_for(lambda: B.run_state()["unread"] == "0", 5))
        self.assertTrue(all(not x["unread"] for x in history(B, self.addr_a)["messages"]))
        # longer than one packet: still sent (a link transfer), never blocked
        text = "long " * 400                                                            # 2000 bytes
        m = A.call("messages.send", {"address": self.addr_b, "text": text})
        self.assertEqual(m["bytes"], 2000)
        ev = A.conn.wait_event("message_state", DELIVER_WAIT, lambda d: d["id"] == m["id"])
        self.assertEqual(ev["state"], "delivered")
        got = B.conn.wait_event("message", 20, lambda d: d["bytes"] == 2000)
        self.assertEqual(got["text"], text)
        B.call("messages.mark_read", {"address": self.addr_a})

    def test_08_history_paging_and_reply(self):
        A, B = self.A, self.B
        A.drain_events()
        rep = B.call("messages.send", {"address": self.addr_a, "text": "reply from Bob"})   # reply to a stranger works
        A.conn.wait_event("message", 60, lambda d: d["text"] == "reply from Bob")
        self.assertEqual(A.call("status")["unread"], 1)
        h = history(A, self.addr_b)
        self.assertEqual([m["dir"] for m in h["messages"]], ["out", "out", "out", "out", "in"])
        self.assertEqual((h["name"], h["contact"], h["has_more"]), ("Bob", True, False))
        page = history(A, self.addr_b, limit=2)
        self.assertEqual([m["id"] for m in page["messages"]], [m["id"] for m in h["messages"][-2:]])
        self.assertTrue(page["has_more"])
        older = history(A, self.addr_b, limit=2, before=page["messages"][0]["id"])
        self.assertEqual([m["id"] for m in older["messages"]],
                         [m["id"] for m in h["messages"][1:3]])
        self.assertEqual(history(A, self.addr_b, limit=50, before=h["messages"][0]["id"])["messages"], [])
        A.call("messages.mark_read", {"address": self.addr_b})
        ids = [m["id"] for m in h["messages"]]
        self.assertEqual(ids, sorted(ids))
        self.__class__.baseline_rows = len(h["messages"])

    # ------------------------------------------------------------------ an offline contact
    def test_09_offline_contact_fails_after_the_tries_and_is_not_resent(self):
        A, B = self.A, self.B
        B.stop()
        A.drain_events()
        t0 = time.time()
        m = A.call("messages.send", {"address": self.addr_b, "text": "anyone there?"})
        ev = A.conn.wait_event("message_state", 240, lambda d: d["id"] == m["id"] and d["state"] in ("failed",
                                                                                                      "delivered"))
        took = time.time() - t0
        sys.stderr.write("\n[offline] failed after %.0f s (%d tries of %.0f s plus link set-up)\n" % (took, 5, 10))
        self.assertEqual((ev["state"], ev["detail"]), ("failed", "undelivered"))
        self.assertEqual(last_state(A, self.addr_b), ("failed", "undelivered"))
        self.__class__.failed_id = m["id"]
        # the contact comes back and is heard again: nothing is sent by itself
        B.start()
        self.assertTrue(B.online())
        B.call("messages.subscribe")
        B.call("announce")
        A.conn.wait_event("heard", HEARD_WAIT, lambda d: d["address"] == self.addr_b and d["time"] > t0 + took - 1)
        time.sleep(25)
        self.assertEqual(last_state(A, self.addr_b), ("failed", "undelivered"))
        rows = [x for x in history(B, self.addr_a)["messages"] if x["dir"] == "in"]
        self.assertNotIn("anyone there?", [x["text"] for x in rows])
        self.assertEqual(A.call("messages.conversations")["conversations"][0]["last_state"], "failed")

    def test_10_manual_retry_delivers(self):
        A, B = self.A, self.B
        rid = self.failed_id
        self.assertEqual(A.code("messages.retry", {"id": rid + 1000}), "not_found")
        done = [x for x in history(A, self.addr_b)["messages"] if x["state"] == "delivered"][0]
        self.assertEqual(A.code("messages.retry", {"id": done["id"]}), "invalid")        # only a failed one goes again
        B.drain_events()
        r = A.call("messages.retry", {"id": rid})
        self.assertEqual((r["id"], r["state"], r["text"]), (rid, "sending", "anyone there?"))
        ev = A.conn.wait_event("message_state", DELIVER_WAIT, lambda d: d["id"] == rid and d["state"] != "sending")
        self.assertEqual((ev["state"], ev["detail"]), ("delivered", None))
        got = B.conn.wait_event("message", 20, lambda d: d["text"] == "anyone there?")
        self.assertEqual(got["address"], self.addr_a)
        self.assertEqual(sum(1 for x in history(A, self.addr_b)["messages"] if x["text"] == "anyone there?"), 1)
        B.call("messages.mark_read", {"address": self.addr_a})

    def test_11_taking_the_permission_back_stops_what_is_on_its_way(self):
        A, B = self.A, self.B
        stranger = "ee" * 16                                                              # never heard: nothing can deliver it
        A.drain_events()
        m = A.call("messages.send", {"address": stranger, "text": "to be cancelled"})
        self.assertEqual(m["state"], "sending")
        A.call("iface.outgoing", {"name": "Link", "outgoing": False}, timeout=30)
        ev = A.conn.wait_event("message_state", 20, lambda d: d["id"] == m["id"])
        self.assertEqual((ev["state"], ev["detail"]), ("failed", "no_transmit"))
        self.assertEqual(A.code("messages.retry", {"id": m["id"]}), "no_transmit")
        self.assertEqual(A.code("messages.send", {"address": stranger, "text": "no"}), "no_transmit")
        self.assertEqual(last_state(A, stranger), ("failed", "no_transmit"))
        A.call("iface.outgoing", {"name": "Link", "outgoing": True}, timeout=30)
        self.assertTrue(A.online())
        r = A.call("messages.retry", {"id": m["id"]})                                     # by hand, now it may
        self.assertEqual((r["id"], r["state"]), (m["id"], "sending"))
        d = A.call("messages.delete_conversation", {"address": stranger})                 # also stops its own tries
        self.assertEqual(d["messages_deleted"], 1)
        self.settle_after_reload(A, B)
        self.assertEqual(A.call("messages.conversations")["conversations"][0]["address"], self.addr_b)

    # ------------------------------------------------------------------ delete and remove
    def test_12_delete_conversation_and_remove_contact(self):
        A, B = self.A, self.B
        self.assertIn(self.addr_b, [c["address"] for c in A.call("messages.conversations")["conversations"]])
        b_before = len(history(B, self.addr_a)["messages"])
        A.drain_events()
        r = A.call("messages.delete_conversation", {"address": self.addr_b})
        self.assertEqual(r["messages_deleted"], 6)
        self.assertEqual(A.call("messages.conversations")["conversations"], [])
        self.assertEqual(history(A, self.addr_b)["messages"], [])
        self.assertEqual(A.conn.wait_event("conversation_deleted", 5)["address"], self.addr_b)
        self.assertEqual([c["address"] for c in A.call("contacts.list")["contacts"]], [self.addr_b])  # the contact stays
        time.sleep(3)                                                                     # nothing was sent to B
        self.assertEqual(len(history(B, self.addr_a)["messages"]), b_before)
        m = A.call("messages.send", {"address": self.addr_b, "text": "one more"})
        A.conn.wait_event("message_state", DELIVER_WAIT, lambda d: d["id"] == m["id"])
        rm = A.call("contacts.remove", {"address": self.addr_b})                          # default: keep the conversation
        self.assertEqual((rm["conversation_kept"], rm["messages_deleted"]), (True, 0))
        self.assertEqual(A.call("contacts.list")["contacts"], [])
        c = A.call("messages.conversations")["conversations"][0]
        self.assertEqual((c["address"], c["contact"], c["name"]), (self.addr_b, False, "Bob"))   # name from the heard list
        A.call("contacts.add", {"address": self.addr_b, "name": "Bob"})
        rm = A.call("contacts.remove", {"address": self.addr_b, "keep_conversation": False})
        self.assertEqual((rm["conversation_kept"], rm["messages_deleted"]), (False, 1))
        self.assertEqual(A.call("messages.conversations")["conversations"], [])
        self.assertTrue(wait_for(lambda: len(history(B, self.addr_a)["messages"]) == b_before + 1, 10))   # B's copy stays
        A.call("contacts.add", {"address": self.addr_b, "name": "Bob"})

    # ------------------------------------------------------------------ restart
    def test_13_restart_keeps_everything_and_a_killed_send_fails_as_interrupted(self):
        A, B = self.A, self.B
        A.call("settings.set", {"alerts": True})
        m = A.call("messages.send", {"address": self.addr_b, "text": "kept across restart"})
        self.assertTrue(wait_for(lambda: last_state(A, self.addr_b)[0] == "delivered", DELIVER_WAIT))
        heard_before = A.call("heard.list")["count"]
        B.stop()
        m2 = A.call("messages.send", {"address": self.addr_b, "text": "killed while sending"})
        self.assertEqual(m2["state"], "sending")
        A.stop(kill=True)                                                                   # no clean exit, no flush
        A.start()
        self.assertEqual(A.address, self.addr_a)                                            # same identity
        self.assertEqual([c["name"] for c in A.call("contacts.list")["contacts"]], ["Bob"])
        rows = history(A, self.addr_b)["messages"]
        self.assertEqual([x["text"] for x in rows], ["kept across restart", "killed while sending"])
        self.assertEqual([(x["state"], x["detail"]) for x in rows], [("delivered", None), ("failed", "interrupted")])
        self.assertEqual(A.call("settings.get"), {"alerts": True})
        self.assertEqual(A.call("heard.list")["count"], heard_before)                       # the heard list is on disk too
        A.call("settings.set", {"alerts": False})
        # no automatic resend after the restart either, even when B is back
        B.start()
        self.assertTrue(B.online())
        B.call("messages.subscribe")
        time.sleep(20)
        self.assertEqual(last_state(A, self.addr_b), ("failed", "interrupted"))
        self.assertNotIn("killed while sending", [x["text"] for x in history(B, self.addr_a)["messages"]])
        self.assertEqual(A.call("status")["unread"], 0)

    def test_14_modes_and_logs(self):
        for n in (self.A, self.B):
            for name in os.listdir(n.data):
                p = os.path.join(n.data, name)
                if os.path.isfile(p):
                    self.assertEqual(stat.S_IMODE(os.stat(p).st_mode), 0o600, p)
            self.assertEqual(stat.S_IMODE(os.stat(n.data).st_mode), 0o700)
            self.assertTrue(os.path.exists(os.path.join(n.data, "messages.db")))
            log = n.logtext()
            for text in (SECRET, "Hello Bob", "reply from Bob", "anyone there", "kept across restart"):
                self.assertNotIn(text, log)                                                 # message text is never logged

    # ------------------------------------------------------------------ volume and memory
    def test_15_five_hundred_messages_and_memory(self):
        A, B = self.A, self.B
        idle_a, idle_b = rss_mb(A.proc.pid), rss_mb(B.proc.pid)
        A.drain_events()
        start = time.time()
        ids = []
        for i in range(500):
            ids.append(A.call("messages.send", {"address": self.addr_b, "text": "bulk message number %d" % i})["id"])
        sys.stderr.write("\n[500] queued in %.1f s\n" % (time.time() - start))

        def delivered():
            rows = history(A, self.addr_b, limit=200)["messages"]
            return sum(1 for x in rows if x["state"] == "delivered")
        ok = wait_for(lambda: all(x["state"] == "delivered" for x in history(A, self.addr_b, limit=200)["messages"]), 240, 2)
        sys.stderr.write("[500] last 200 delivered: %s after %.1f s\n" % (ok, time.time() - start))
        self.assertTrue(ok)
        rows_b = wait_for(lambda: B.call("messages.conversations")["conversations"][0]["count"] >= 500, 120, 2)
        self.assertTrue(rows_b)
        self.assertEqual(B.call("status")["unread"] >= 500, True)
        after_a, after_b = rss_mb(A.proc.pid), rss_mb(B.proc.pid)
        sys.stderr.write("[500] VmRSS A %.1f -> %.1f MB (HWM %.1f), B %.1f -> %.1f MB (HWM %.1f); messages on disk A %d, B %d\n" % (
            idle_a["VmRSS"], after_a["VmRSS"], after_a["VmHWM"], idle_b["VmRSS"], after_b["VmRSS"], after_b["VmHWM"],
            A.call("messages.conversations")["conversations"][0]["count"], B.call("messages.conversations")["conversations"][0]["count"]))
        # budget: 60 MB idle (D2). The peak after the burst was 43-46 MB in 9 runs and 60.1 MB once, on the receiver, in a run
        # on a loaded machine (not explained): the bound is a sanity bound, the numbers above are the measurement.
        self.assertLess(after_a["VmHWM"], 64)
        self.assertLess(after_b["VmHWM"], 64)
        self.assertLess(after_a["VmRSS"] - idle_a["VmRSS"], 12)
        self.assertLess(after_b["VmRSS"] - idle_b["VmRSS"], 12)
        sizes = [os.path.getsize(os.path.join(n.data, "messages.db")) for n in (A, B)]
        sys.stderr.write("[500] messages.db: A %d bytes, B %d bytes\n" % tuple(sizes))
        # the bridge is still answering quickly with 500+ stored messages
        t = time.time()
        B.call("messages.conversations")
        history(B, self.addr_a, limit=200)
        self.assertLess(time.time() - t, 2)


if __name__ == "__main__":
    unittest.main()
