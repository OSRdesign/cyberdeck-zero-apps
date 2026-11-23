# SPDX-License-Identifier: MIT
"""Integration: the real bridge (RNS + LXMF) and a plain RNS node (peer.py), linked over TCP on 127.0.0.1.

What is checked: nothing is sent on a fresh configuration (every interface off: zero tx bytes, the peer sees no
connection); an interface added through the bridge is written off; switched on with outgoing = False it receives the
peer's announce and sends no announce and no path request; announce is refused until a display name is set and an
interface may transmit; the LXMF router sends nothing by itself; an explicit announce reaches the peer; a busy serial
port is refused; hand edits survive; one bridge per run folder; a clean stop removes the socket and the state file.
Each test runs in order on one bridge (numbered names). About a minute.
"""
import base64
import json
import os
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE_DIR = os.path.dirname(HERE)
sys.path.insert(0, BRIDGE_DIR)
from reticulum_bridge.client import Connection  # noqa: E402

try:
    import LXMF
    import RNS
except ImportError:                                         # pragma: no cover
    RNS = None

HAND_CONFIG = """\
# hand-written test configuration: this comment must survive
[reticulum]
  enable_transport = No
  share_instance = Yes
  instance_name = %s

[logging]
  loglevel = 3

[interfaces]
  # a comment inside [interfaces]
  [[Default Interface]]
    type = AutoInterface
    enabled = no
    outgoing = False
"""
HOLD = "import os, sys, time; fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY); print('ok', flush=True); time.sleep(60)"
# An announce reached the other node's handler 0.5 to 18 s after the request on loopback (RNS 1.6.1, 7 runs; the
# sender's announce byte counter moved at once and it spent no CPU on stamps; the cause was not investigated).
ANNOUNCE_WAIT = 60


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def rd(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def wait_for(cond, timeout, step=0.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return cond()


def start_bridge(cfg, data, run, log):
    env = dict(os.environ, PYTHONPATH=BRIDGE_DIR, PYTHONUNBUFFERED="1")
    env.pop("XDG_RUNTIME_DIR", None)                      # never the real one
    return subprocess.Popen([sys.executable, "-m", "reticulum_bridge", "--config", cfg, "--data", data, "--run", run],
                            env=env, stdout=log, stderr=subprocess.STDOUT)


@unittest.skipIf(RNS is None, "rns and lxmf are not installed (run tests/run_tests.sh)")
class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="rnsbridge-")
        cls.port = free_port()
        cls.cfg, cls.data, cls.rundir = (os.path.join(cls.tmp, n) for n in ("cfg", "data", "run"))
        os.makedirs(cls.cfg)
        cls.config = os.path.join(cls.cfg, "config")
        with open(cls.config, "w") as f:
            f.write(HAND_CONFIG % ("rnsbridgetest%d" % cls.port))
        cls.config_before = rd(cls.config, "rb")
        cls.peer = subprocess.Popen([sys.executable, os.path.join(HERE, "peer.py"), os.path.join(cls.tmp, "peer"),
                                     str(cls.port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        cls.peer_dest = json.loads(cls.peer.stdout.readline())["destination"]
        cls.log = open(os.path.join(cls.tmp, "bridge.log"), "w")
        cls.bridge = start_bridge(cls.cfg, cls.data, cls.rundir, cls.log)
        cls.sock = os.path.join(cls.rundir, "bridge.sock")
        if not wait_for(lambda: os.path.exists(cls.sock), 30):
            raise RuntimeError("the bridge did not start: " + rd(cls.log.name))
        cls.c = Connection(cls.sock)

    @classmethod
    def tearDownClass(cls):
        cls.c.close()
        if cls.bridge.poll() is None:
            cls.bridge.kill()
        cls.bridge.wait()
        cls.peer.stdin.write("quit\n")
        cls.peer.stdin.close()
        cls.peer.wait(10)
        cls.peer.stdout.close()
        cls.log.close()
        sys.stderr.write("\n--- bridge log ---\n" + rd(cls.log.name) + "--- end ---\n")

    def peer_cmd(self, cmd):
        self.peer.stdin.write(cmd + "\n")
        self.peer.stdin.flush()
        return json.loads(self.peer.stdout.readline())

    def peer_server(self):
        return next(i for i in self.peer_cmd("stats") if i["short_name"] == "Peer server")

    def iface(self, name):
        return next((i for i in self.c.call("ifaces")["interfaces"] if i["name"] == name), None)

    def rnstatus_tx(self):
        """Independent check through upstream's rnstatus (a client of the bridge's shared instance)."""
        out = subprocess.run([os.path.join(os.path.dirname(sys.executable), "rnstatus"), "--config", self.cfg, "-j"],
                             capture_output=True, text=True, timeout=30)
        stats = json.loads(out.stdout)
        return {i["short_name"]: i["txb"] for i in stats["interfaces"] if i.get("type") not in (
            "LocalServerInterface", "LocalClientInterface")}

    def test_01_started(self):
        h = self.c.call("hello")
        self.assertEqual(h["protocol"], 1)
        self.assertEqual(h["rns"], RNS.__version__)
        self.assertIn("iface.outgoing", h["verbs"])
        s = self.c.call("status")
        self.assertEqual((s["state"], s["mode"], s["transport"]), ("up", "master", False))
        self.assertEqual(len(s["lxmf_address"]), 32)
        self.assertIsNone(s["display_name"])
        state = dict(line.split("=", 1) for line in rd(os.path.join(self.rundir, "bridge.state")).splitlines())
        self.assertEqual(state["pid"], str(self.bridge.pid))
        self.assertEqual(state["lxmf_address"], s["lxmf_address"])
        self.assertEqual(state["ifaces_up"], "0")
        for key in ("bridge", "rns", "lxmf", "identity", "mode", "ifaces_up_names"):
            self.assertIn(key, state)
        self.assertEqual(stat.S_IMODE(os.stat(self.sock).st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.data, "identity")).st_mode), 0o600)
        self.assertEqual(rd(self.config, "rb"), self.config_before)              # an existing config is kept
        self.assertEqual(rd("/proc/%d/comm" % self.bridge.pid).strip(), "reticulum")

    def test_02_fresh_config_sends_nothing(self):
        time.sleep(3)
        d = self.iface("Default Interface")
        self.assertEqual((d["enabled"], d["outgoing"], d["online"]), (False, False, False))
        self.assertEqual(self.c.call("status")["interfaces_up"], [])
        self.assertEqual(self.rnstatus_tx(), {})                # no interface attached at all: zero tx bytes
        srv = self.peer_server()
        self.assertEqual((srv["clients"], srv["rxb"]), (0, 0))

    def test_03_types(self):
        t = {x["type"]: x for x in self.c.call("iface.types")["types"]}
        self.assertEqual(len(t), 12)
        freq = next(o for o in t["RNodeInterface"]["options"] if o["name"] == "frequency")
        self.assertEqual((freq["min"], freq["max"], freq["default"], freq["required"]),
                         (137000000, 3000000000, None, True))

    def test_04_add_is_written_off(self):
        e = self.c.call("iface.add", {"name": "Peer link", "type": "TCPClientInterface",
                                      "options": {"target_host": "127.0.0.1", "target_port": self.port}})
        self.assertEqual((e["enabled"], e["outgoing"], e["online"], e["missing"]), (False, False, False, []))
        text = rd(self.config)
        self.assertIn("  [[Peer link]]\n    type = TCPClientInterface\n    enabled = no\n    outgoing = False\n", text)
        self.assertIn("# hand-written test configuration: this comment must survive", text)
        self.assertIn("  # a comment inside [interfaces]", text)
        self.assertEqual(rd(self.config + ".bak", "rb"), self.config_before)
        time.sleep(2)
        self.assertEqual(self.peer_server()["clients"], 0)
        self.assertEqual(self.rnstatus_tx(), {})

    def test_05_validation(self):
        r = self.c.request("iface.edit", {"name": "Peer link", "options": {"target_port": 70000}})
        self.assertEqual((r["error"]["code"], r["error"]["field"]), ("invalid", "target_port"))
        r = self.c.request("iface.add", {"name": "X", "type": "UDPInterface", "options": {"enabled": True}})
        self.assertEqual(r["error"]["code"], "invalid")
        r = self.c.request("iface.add", {"name": "Radio", "type": "RNodeInterface", "options": {"frequency": 100}})
        self.assertEqual(r["error"]["field"], "frequency")
        e = self.c.call("iface.add", {"name": "Radio", "type": "RNodeInterface",
                                      "options": {"port": "/dev/does-not-exist-rnode"}})
        self.assertEqual(e["missing"], ["frequency", "bandwidth", "txpower", "spreadingfactor", "codingrate"])
        self.assertEqual(e["lock"], "not held")
        r = self.c.request("iface.enable", {"name": "Radio"})
        self.assertEqual(r["error"]["code"], "incomplete")
        self.assertEqual(self.c.call("iface.remove", {"name": "Radio"}), {"removed": "Radio"})
        self.assertEqual(self.c.request("iface.remove", {"name": "Radio"})["error"]["code"], "not_found")
        self.assertEqual(self.c.request("iface.add", {"name": "Peer link", "type": "UDPInterface"})["error"]["code"],
                         "exists")

    def test_06_listen_only(self):
        sub = self.c.call("announces.subscribe")
        self.assertEqual(sub["subscribed"], True)
        e = self.c.call("iface.enable", {"name": "Peer link"}, timeout=30)
        self.assertTrue(e["enabled"])
        self.assertIn("enabled = yes", rd(self.config))
        self.assertTrue(wait_for(lambda: self.iface("Peer link")["online"], 15), "the TCP link did not come up")
        self.peer_cmd("announce hello from the peer")
        ev = self.c.wait_event("announce", ANNOUNCE_WAIT, lambda d: d["destination"] == self.peer_dest)
        self.assertEqual(ev["kind"], "other")
        self.assertEqual(ev["hops"], 1)
        self.assertEqual(ev["interface"], "Peer link")
        self.assertIn(self.peer_dest, [p["hash"] for p in self.c.call("paths")["paths"]])
        time.sleep(5)
        st = self.iface("Peer link")["stats"]
        self.assertEqual((st["atxb"], st["ptxb"]), (0, 0))       # no announce, no path request
        self.assertEqual(self.peer_cmd("heard"), [])             # the peer heard nothing from the bridge
        sys.stderr.write("\n[listen-only] Peer link tx %d bytes (link set-up only), rx %d bytes\n" % (
            st["txb"], st["rxb"]))

    def test_07_announce_refused(self):
        self.assertEqual(self.c.request("announce")["error"]["code"], "no_display_name")
        self.assertEqual(self.c.request("announce", {"display_name": "Test deck"})["error"]["code"], "no_transmit")
        self.assertEqual(self.peer_cmd("heard"), [])

    def test_08_lxmf_sends_nothing_by_itself(self):
        e = self.c.call("iface.outgoing", {"name": "Peer link", "outgoing": True}, timeout=30)
        self.assertTrue(e["outgoing"])
        self.assertIn("outgoing = True", rd(self.config))
        self.assertTrue(wait_for(lambda: self.iface("Peer link")["online"], 15))
        time.sleep(10)
        st = self.iface("Peer link")["stats"]
        self.assertEqual((st["atxb"], st["ptxb"]), (0, 0))
        self.assertEqual(self.peer_cmd("heard"), [])

    def test_09_announce_on_request(self):
        r = self.c.call("announce", {"display_name": "Test deck"})
        self.assertEqual((r["announced"], r["via"]), (True, ["Peer link"]))
        addr = self.c.call("status")["lxmf_address"]
        heard = wait_for(lambda: [h for h in self.peer_cmd("heard") if h["destination"] == addr], ANNOUNCE_WAIT, 0.5)
        self.assertTrue(heard, "the peer did not hear the announce")
        self.assertEqual(LXMF.display_name_from_app_data(bytes.fromhex(heard[0]["app_data"])), "Test deck")
        self.assertEqual(self.c.call("status")["display_name"], "Test deck")

    def test_10_identity(self):
        info = self.c.call("identity.info")
        self.assertEqual(info["lxmf_address"], self.c.call("status")["lxmf_address"])
        self.assertEqual(self.c.request("identity.export")["error"]["code"], "confirm")
        x = self.c.call("identity.export", {"confirm": True})
        key = base64.b32decode(x["key"])
        self.assertEqual(len(key), 64)
        self.assertEqual(RNS.Identity.from_bytes(key).hash.hex(), info["identity_hash"])
        self.assertNotIn(x["key"], rd(self.log.name))            # keys never in the log

    def test_10b_identity_name(self):
        # the app's Settings > Name: a get/set verb that stores the name (only `announce` ever sends it)
        self.assertEqual(self.c.call("identity.name")["display_name"], self.c.call("status")["display_name"])
        self.assertEqual(self.c.call("identity.name", {"name": " Bench deck "})["display_name"], "Bench deck")
        self.assertEqual(self.c.call("status")["display_name"], "Bench deck")
        self.assertEqual(self.c.call("identity.info")["display_name"], "Bench deck")
        self.assertEqual(self.c.request("identity.name", {"name": "a\nb"})["error"]["code"], "invalid")
        self.assertEqual(self.c.call("identity.name")["display_name"], "Bench deck")

    def test_11_busy_serial_port(self):
        master, slave = os.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        holder = subprocess.Popen([sys.executable, "-c", HOLD, path], stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "ok")
            self.c.call("iface.add", {"name": "Serial test", "type": "SerialInterface", "options": {"port": path}})
            r = self.c.request("iface.enable", {"name": "Serial test"})
            self.assertEqual(r["error"]["code"], "busy")
            self.assertIn("is used by python", r["error"]["message"])
            self.assertEqual(r["error"]["holders"][0]["pid"], holder.pid)
            self.assertFalse(self.iface("Serial test")["enabled"])
            self.c.call("iface.remove", {"name": "Serial test"})
        finally:
            holder.kill()
            holder.wait()
            holder.stdout.close()
            os.close(master)

    def test_12_switch_off_and_remove(self):
        e = self.c.call("iface.enable", {"name": "Peer link", "enabled": False})
        self.assertFalse(e["enabled"])
        self.assertTrue(wait_for(lambda: self.peer_server()["clients"] == 0, 10))
        self.c.call("iface.remove", {"name": "Peer link"})
        text = rd(self.config)
        self.assertNotIn("Peer link", text)
        self.assertIn("# hand-written test configuration: this comment must survive", text)
        self.assertIn("  # a comment inside [interfaces]", text)

    def test_13_one_bridge_per_user(self):
        with open(os.path.join(self.tmp, "second.log"), "w") as log:
            second = start_bridge(self.cfg, self.data, self.rundir, log)
            self.assertEqual(second.wait(30), 3)
        self.assertIn("already running", rd(os.path.join(self.tmp, "second.log")))
        self.assertEqual(self.c.call("hello")["protocol"], 1)

    def test_14_stop(self):
        self.assertEqual(self.c.call("service.stop"), {"stopping": True})
        self.assertEqual(self.bridge.wait(20), 0)
        self.assertFalse(os.path.exists(self.sock))
        self.assertFalse(os.path.exists(os.path.join(self.rundir, "bridge.state")))


@unittest.skipIf(RNS is None, "rns and lxmf are not installed (run tests/run_tests.sh)")
class FirstRunConfigAndStaleSocket(unittest.TestCase):
    def test_first_run_text_runs_and_stale_socket(self):
        from reticulum_bridge import rnsconfig
        tmp = tempfile.mkdtemp(prefix="rnsbridge-")
        cfg, data, run = (os.path.join(tmp, n) for n in ("cfg", "data", "run"))
        os.makedirs(cfg)
        os.makedirs(run, mode=0o700)
        text = rnsconfig.FIRST_RUN.replace("  share_instance = Yes\n", "  share_instance = Yes\n  instance_name = "
                                           "rnsbridgefirst%d\n" % os.getpid())
        with open(os.path.join(cfg, "config"), "w") as f:
            f.write(text)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(os.path.join(run, "bridge.sock"))
        s.close()                                           # left behind by a bridge that was killed
        with open(os.path.join(tmp, "bridge.log"), "w") as log:
            p = start_bridge(cfg, data, run, log)
            try:
                c = wait_for(lambda: self._connect(os.path.join(run, "bridge.sock")), 30)
                self.assertTrue(c, rd(log.name))
                ifs = c.call("ifaces")["interfaces"]
                self.assertEqual([(i["name"], i["type"], i["enabled"], i["outgoing"]) for i in ifs],
                                 [("Default Interface", "AutoInterface", False, False)])
                c.call("announces.subscribe")
                p.send_signal(signal.SIGTERM)                   # a clean exit on SIGTERM too
                self.assertEqual(c.wait_event("stopping", 10), {})
                c.close()
                self.assertEqual(p.wait(20), 0)
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait()
        self.assertIn("removed a stale socket", rd(os.path.join(tmp, "bridge.log")))
        self.assertEqual(rd(os.path.join(cfg, "config")), text)
        self.assertEqual(sorted(os.listdir(run)), ["bridge.lock"])

    @staticmethod
    def _connect(path):
        try:
            c = Connection(path)
            c.call("hello")
            return c
        except OSError:
            return None


if __name__ == "__main__":
    unittest.main()
