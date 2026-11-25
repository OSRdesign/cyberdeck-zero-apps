# SPDX-License-Identifier: MIT
"""Messenger rules that need no network: validation, the route rule, settings, the launcher alert (fake launcher
socket), and the incoming path on a fake router (no RNS)."""
import os
import stat
import sys
import tempfile
import time
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
from reticulum_bridge import BridgeError  # noqa: E402
from reticulum_bridge import alerts, messenger  # noqa: E402
from reticulum_bridge.store import Store  # noqa: E402
from bridgekit import FakeLauncher  # noqa: E402

OWN = "0" * 32
PEER = "ab" * 16


class Validation(unittest.TestCase):
    def test_address_is_strict_and_lowercased(self):
        self.assertEqual(messenger.norm_address("  " + PEER.upper() + "\n"), PEER)
        for bad in ("", "abc", PEER[:-1], PEER + "0", "g" * 32, "ab cd" + "0" * 27, "lxmf@" + PEER, "<" + PEER + ">",
                    PEER[:16] + "-" + PEER[16:], None, 123, ["a"], True, PEER + PEER):
            with self.assertRaises(BridgeError, msg=repr(bad)) as cm:
                messenger.norm_address(bad)
            self.assertEqual((cm.exception.code, cm.exception.extra["field"]), ("invalid", "address"))

    def test_names(self):
        self.assertIsNone(messenger.check_name(None))
        self.assertIsNone(messenger.check_name(""))
        self.assertEqual(messenger.check_name("  Bea  "), "Bea")
        self.assertEqual(messenger.check_name("a" * 64), "a" * 64)
        for bad in ("a" * 65, "   ", "a\nb", "a\x00", 5):
            with self.assertRaises(BridgeError, msg=repr(bad)):
                messenger.check_name(bad)

    def test_text(self):
        self.assertEqual(messenger.check_text("a\nb\tc ☃"), "a\nb\tc ☃")
        self.assertEqual(messenger.check_text("a\r\nb\rc"), "a\nb\nc")                   # pasted Windows line ends
        for bad in ("", "  \n", "x\x07y", None, 5, "x" * (messenger.TEXT_MAX_BYTES + 1), "lone \ud800 surrogate"):
            with self.assertRaises(BridgeError, msg=repr(bad)) as cm:
                messenger.check_text(bad)
            self.assertEqual(cm.exception.code, "invalid")
        self.assertEqual(len(messenger.check_text("x" * messenger.TEXT_MAX_BYTES)), messenger.TEXT_MAX_BYTES)
        with self.assertRaises(BridgeError):                                     # the cap counts bytes, not characters
            messenger.check_text("☃" * (messenger.TEXT_MAX_BYTES // 3 + 1))


class RouteRule(unittest.TestCase):
    def test_a_path_over_a_listen_only_interface_is_refused(self):
        r = messenger.route_ok
        self.assertTrue(r(["Wi-Fi"], None, None))                                # no path known: the request is checked
        self.assertTrue(r(["Wi-Fi"], "Wi-Fi", "TCPInterface"))
        self.assertFalse(r(["Wi-Fi"], "Radio", "RNodeInterface"))                # known path over a listen-only one
        self.assertFalse(r([], "Radio", "RNodeInterface"))
        self.assertTrue(r(["Wi-Fi"], "Shared Instance[x]", "LocalClientInterface"))   # the owner of the interfaces decides


class SettingsTest(unittest.TestCase):
    def test_defaults_persistence_and_refusals(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "settings.json")
        s = messenger.Settings(path, lambda m: None)
        self.assertEqual(s.get(), {"alerts": False})
        self.assertFalse(os.path.exists(path))                                   # nothing written until a change
        self.assertEqual(s.update({"alerts": True}), {"alerts": True})
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        self.assertEqual(messenger.Settings(path, lambda m: None).get(), {"alerts": True})
        for bad in ({"alerts": "yes"}, {"alerts": 1}, {"volume": True}):
            with self.assertRaises(BridgeError):
                s.update(bad)
        self.assertEqual(s.get(), {"alerts": True})
        with open(path, "w") as f:
            f.write("{broken")
        logs = []
        self.assertEqual(messenger.Settings(path, logs.append).get(), {"alerts": False})
        self.assertTrue(logs)


class LauncherAlert(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="rnsl-")
        self.path = os.path.join(self.dir, "launcher.sock")
        self.fake = FakeLauncher(self.path)

    def tearDown(self):
        self.fake.close()

    def test_requests_are_the_contract_ones(self):
        self.assertEqual(alerts.request("toast hello", self.path), "ok queued")
        self.assertEqual(alerts.request("beep notify", self.path), "ok")
        self.assertEqual(self.fake.requests, ["toast hello", "beep notify"])

    def test_alert_sends_a_toast_and_a_beep_and_no_message_text(self):
        a = alerts.Alerter(path=self.path)
        self.assertTrue(a.alert("Bea\nthe\x07 Great"))
        deadline = time.time() + 5
        while len(self.fake.requests) < 2 and time.time() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.fake.requests, ["toast New message from Bea the  Great", "beep notify"])

    def test_at_most_one_alert_per_two_seconds(self):
        a = alerts.Alerter(path=self.path)
        self.assertTrue(a.alert("x"))
        self.assertFalse(a.alert("y"))
        a.last -= alerts.MIN_GAP + 0.1
        self.assertTrue(a.alert("z"))

    def test_no_launcher_is_silent_and_remembered(self):
        logs = []
        a = alerts.Alerter(log=logs.append, path=os.path.join(self.dir, "nothing.sock"))
        self.assertTrue(a.alert("x"))
        deadline = time.time() + 5
        while not logs and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(logs)
        a.last -= alerts.MIN_GAP + 0.1
        self.assertFalse(a.alert("y"))                                           # backed off: no retry per message

    def test_request_limits(self):
        self.assertIsNone(alerts.request("toast " + "x" * 300, self.path))      # over 254 bytes: never sent
        self.assertEqual(self.fake.requests, [])
        self.assertEqual(alerts.clean("a" * 100), "a" * 59 + "…")

    def test_path_comes_from_the_runtime_folder_or_the_test_override(self):
        old = {k: os.environ.pop(k, None) for k in ("RETICULUM_BRIDGE_LAUNCHER_SOCK", "XDG_RUNTIME_DIR")}
        try:
            self.assertIsNone(alerts.socket_path())                              # no /tmp fallback
            os.environ["XDG_RUNTIME_DIR"] = "/run/user/1000"
            self.assertEqual(alerts.socket_path(), "/run/user/1000/applaunch/launcher.sock")
            os.environ["RETICULUM_BRIDGE_LAUNCHER_SOCK"] = "/x/y"
            self.assertEqual(alerts.socket_path(), "/x/y")
        finally:
            for k, v in old.items():
                os.environ.pop(k, None)
                if v is not None:
                    os.environ[k] = v


class FakeLxm:
    def __init__(self, source, content, fields=None):
        self.source_hash = bytes.fromhex(source)
        self.content, self.fields = content, fields

    def content_as_string(self):
        return self.content


class IncomingOnFakeRouter(unittest.TestCase):
    """on_delivery with a fake router: alerts follow the setting; our own address and empty messages are ignored."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="rnsm-")
        self.fake = FakeLauncher(os.path.join(self.dir, "l.sock"))
        router = types.SimpleNamespace(register_delivery_callback=lambda cb: setattr(router, "cb", cb))
        self.events = []
        server = types.SimpleNamespace(post_event=lambda n, d, topic="announce": self.events.append((topic, n, d)))
        self.bridge = types.SimpleNamespace(
            RNS=None, LXMF=None, router=router, dest=types.SimpleNamespace(hash=bytes.fromhex(OWN)), server=server,
            log=lambda m: self.events.append(("log", m, None)), refresh_state=lambda: None)
        self.db = Store(os.path.join(self.dir, "m.db"))
        self.settings = messenger.Settings(os.path.join(self.dir, "s.json"), lambda m: None)
        self.alerter = alerts.Alerter(path=os.path.join(self.dir, "l.sock"))
        self.m = messenger.Messenger(self.bridge, self.db, self.settings, self.alerter)

    def tearDown(self):
        self.m.close()
        self.fake.close()

    def launcher_lines(self, n, wait=2.0):
        end = time.time() + wait
        while len(self.fake.requests) < n and time.time() < end:
            time.sleep(0.05)
        time.sleep(0.2)
        return list(self.fake.requests)

    def test_alerts_off_sends_no_request_on_sends_one(self):
        self.bridge.router.cb(FakeLxm(PEER, "hello"))
        self.assertEqual(self.launcher_lines(1, 1.0), [])
        self.assertEqual(self.alerter.sent, 0)
        self.settings.update({"alerts": True})
        self.bridge.router.cb(FakeLxm(PEER, "again"))
        lines = self.launcher_lines(2)
        self.assertEqual(lines, ["toast New message from Anonymous " + PEER[:8], "beep notify"])
        self.assertEqual(self.db.unread_total(), 2)

    def test_alert_names_the_contact(self):
        self.settings.update({"alerts": True})
        self.db.add_contact(PEER, "Bea")
        self.bridge.router.cb(FakeLxm(PEER, "secret body"))
        lines = self.launcher_lines(2)
        self.assertEqual(lines[0], "toast New message from Bea")
        self.assertNotIn("secret", " ".join(lines))

    def test_own_address_and_empty_are_ignored_and_long_text_is_cut(self):
        self.bridge.router.cb(FakeLxm(OWN, "from me"))
        self.bridge.router.cb(FakeLxm(PEER, "  "))
        self.assertEqual(self.db.message_count(), 0)
        self.bridge.router.cb(FakeLxm(PEER, "", fields={1: b"x"}))
        self.bridge.router.cb(FakeLxm(PEER, "x\x07" + "y" * (messenger.TEXT_MAX_BYTES + 100)))
        rows, _ = self.db.history(PEER, 10)
        self.assertEqual(rows[0]["text"], "[attachment not shown]")
        self.assertLessEqual(rows[1]["bytes"], messenger.TEXT_MAX_BYTES + 3)
        self.assertNotIn("\x07", rows[1]["text"])

    def test_events_and_unread(self):
        self.bridge.router.cb(FakeLxm(PEER, "one"))
        topic, name, data = next(e for e in self.events if e[1] == "message")
        self.assertEqual((topic, data["dir"], data["text"], data["unread_total"], data["contact"]),
                         ("messages", "in", "one", 1, False))
        logs = " ".join(e[1] for e in self.events if e[0] == "log")
        self.assertNotIn("one", logs.replace("message 1 from", ""))               # the log names sizes, not text
        self.assertEqual(self.m.v_mark_read({"address": PEER}, None)["unread_total"], 0)
        self.assertEqual([e[1] for e in self.events if e[0] == "messages"], ["message", "unread"])


if __name__ == "__main__":
    unittest.main()
