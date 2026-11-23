# SPDX-License-Identifier: MIT
"""The JSON-lines server and the run folder, with a fake dispatcher (no RNS)."""
import json
import os
import socket
import stat
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from reticulum_bridge import BridgeError  # noqa: E402
from reticulum_bridge.client import Connection  # noqa: E402
from reticulum_bridge.server import AlreadyRunning, RunDir, Server  # noqa: E402


def dispatch(verb, params, client):
    if verb == "echo":
        return params
    if verb == "slow":
        time.sleep(params.get("s", 1.5))
        return {"slept": True}
    if verb == "fail":
        raise BridgeError("invalid", "bad value", field="x")
    if verb == "boom":
        raise ValueError("unexpected")
    if verb == "sub":
        client.subscribed = True
        return {}
    if verb == "service.stop":
        return {"stopping": True}
    raise BridgeError("unknown_verb", verb)


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.join(tempfile.mkdtemp(), "run")
        self.run = RunDir(self.dir)
        self.assertFalse(self.run.claim())
        self.run.write_state({"pid": os.getpid(), "state": "up"})
        self.server = Server(self.run, dispatch, lambda m: None)
        self.server.listen()
        self.thread = threading.Thread(target=self.server.serve, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.request_stop()
        self.thread.join(5)
        self.run.cleanup()

    def test_files_and_modes(self):
        self.assertEqual(stat.S_IMODE(os.stat(self.dir).st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(os.stat(self.run.sock_path).st_mode), 0o600)
        self.assertEqual(self.run.read_state(), {"pid": str(os.getpid()), "state": "up"})
        with self.assertRaises(AlreadyRunning):
            RunDir(self.dir).claim()

    def test_requests(self):
        c = Connection(self.run.sock_path)
        self.assertEqual(c.call("echo", {"a": 1}), {"a": 1})
        r = c.request("fail")
        self.assertEqual(r["error"], {"code": "invalid", "message": "bad value", "field": "x"})
        self.assertEqual(c.request("boom")["error"]["code"], "internal")
        self.assertEqual(c.request("nope")["error"]["code"], "unknown_verb")
        c.sock.sendall(b"not json\n{\"id\": 5}\n")
        self.assertEqual(c._line(time.monotonic() + 5)["error"]["code"], "bad_request")
        self.assertEqual(c._line(time.monotonic() + 5), {"id": None, "ok": False, "error": {
            "code": "bad_request", "message": "an object with a verb expected"}})
        c.close()

    def test_timeout_and_parallel(self):
        c = Connection(self.run.sock_path)
        r = c.request("slow", {"s": 1.5}, timeout=0.5)
        self.assertEqual(r["error"]["code"], "timeout")
        self.assertEqual(c.call("echo", {"b": 2}), {"b": 2})          # the other worker answers meanwhile
        time.sleep(1.5)
        self.assertEqual(c.call("echo", {"c": 3}), {"c": 3})          # the late reply was dropped
        c.close()

    def test_events_only_to_subscribers(self):
        a, b = Connection(self.run.sock_path), Connection(self.run.sock_path)
        a.call("sub")
        self.server.post_event("announce", {"destination": "00"})
        self.assertEqual(a.wait_event("announce", 5), {"destination": "00"})
        b.call("echo")
        self.assertEqual(b.events, [])
        a.close()
        b.close()

    def test_stop_replies_first(self):
        c = Connection(self.run.sock_path)
        self.assertEqual(c.call("service.stop"), {"stopping": True})
        c.close()
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())


class StaleSocket(unittest.TestCase):
    def test_stale_socket_removed(self):
        d = os.path.join(tempfile.mkdtemp(), "run")
        os.makedirs(d)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(os.path.join(d, "bridge.sock"))
        s.close()                                               # a socket file nobody serves
        run = RunDir(d)
        self.assertTrue(run.claim())
        self.assertFalse(os.path.exists(run.sock_path))
        run.write_state({"pid": 1})
        run.cleanup()
        self.assertEqual(sorted(os.listdir(d)), ["bridge.lock"])


if __name__ == "__main__":
    unittest.main()
