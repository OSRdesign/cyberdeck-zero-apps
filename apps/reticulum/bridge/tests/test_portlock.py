# SPDX-License-Identifier: MIT
"""Serial port arbitration on pseudo terminals and a fake /proc (no RNS, no hardware)."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from reticulum_bridge import BridgeError, portlock  # noqa: E402

HOLD = "import os, sys, time; fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY); print('ok', flush=True); time.sleep(60)"


class PortLock(unittest.TestCase):
    def setUp(self):
        self.master, slave = os.openpty()
        self.path = os.ttyname(slave)
        os.close(slave)

    def tearDown(self):
        os.close(self.master)

    def test_is_device(self):
        self.assertTrue(portlock.is_device("/dev/ttyACM0"))
        for p in ("ble://RNode 3B87", "tcp://10.0.0.5", "", None, "ttyACM0"):
            self.assertFalse(portlock.is_device(p))

    def test_port_held_by_another_process(self):
        p = subprocess.Popen([sys.executable, "-c", HOLD, self.path], stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(p.stdout.readline().strip(), "ok")
            self.assertIn(p.pid, [pid for pid, _ in portlock.holders(self.path)])
            locks = portlock.PortLocks()
            with self.assertRaises(BridgeError) as cm:
                locks.acquire("Radio", self.path)
            self.assertEqual(cm.exception.code, "busy")
            self.assertIn("Close that program first", cm.exception.message)
            self.assertEqual(locks.held, {})
        finally:
            p.kill()
            p.wait()
            p.stdout.close()
        locks.acquire("Radio", self.path)
        self.assertIn("Radio", locks.held)

    def test_flock_between_owners(self):
        a, b = portlock.PortLocks(), portlock.PortLocks()
        a.acquire("Radio", self.path)
        with self.assertRaises(BridgeError) as cm:               # a second owner (here: another open file)
            b.acquire("Other", self.path)
        self.assertEqual(cm.exception.code, "busy")
        a.acquire("Radio", self.path)                            # same name, same port: no change
        a.release("Radio")
        b.acquire("Other", self.path)
        b.release_all()
        self.assertEqual(b.held, {})

    def test_missing_device(self):
        with self.assertRaises(BridgeError) as cm:
            portlock.PortLocks().acquire("Radio", "/dev/does-not-exist-rnode")
        self.assertEqual(cm.exception.code, "not_found")

    def test_names_the_holder_from_proc(self):
        proc = tempfile.mkdtemp()
        os.makedirs(os.path.join(proc, "4321", "fd"))
        os.symlink(self.path, os.path.join(proc, "4321", "fd", "7"))
        with open(os.path.join(proc, "4321", "comm"), "w") as f:
            f.write("mesh-hop\n")
        os.makedirs(os.path.join(proc, "self"))
        self.assertEqual(portlock.holders(self.path, proc=proc), [(4321, "mesh-hop")])
        with self.assertRaises(BridgeError) as cm:
            portlock.PortLocks(proc=proc).acquire("Radio", self.path)
        self.assertEqual(cm.exception.message, "%s is used by mesh-hop. Close that program first." % self.path)
        self.assertEqual(cm.exception.extra["holders"], [{"pid": 4321, "name": "mesh-hop"}])


if __name__ == "__main__":
    unittest.main()
