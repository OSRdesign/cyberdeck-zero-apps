# SPDX-License-Identifier: MIT
"""Test helpers: real bridge processes (RNS + LXMF) in temporary folders, linked over a TCP interface on 127.0.0.1.

A Node is one `python -m reticulum_bridge` with its own config, data and run folders and its own RNS instance name, so
several run side by side on one machine. Nothing touches ~/.reticulum or the real XDG_RUNTIME_DIR.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE_DIR = os.path.dirname(HERE)
sys.path.insert(0, BRIDGE_DIR)
from reticulum_bridge import rnsconfig  # noqa: E402
from reticulum_bridge.client import Connection  # noqa: E402

try:
    import LXMF  # noqa: F401
    import RNS  # noqa: F401
except ImportError:                                         # pragma: no cover
    RNS = None


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(cond, timeout, step=0.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return cond()


def rd(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def rss_mb(pid):
    """VmRSS and VmHWM of a process in MB (Linux)."""
    out = {}
    for line in rd("/proc/%d/status" % pid).splitlines():
        if line.startswith(("VmRSS:", "VmHWM:")):
            out[line.split(":")[0]] = int(line.split()[1]) / 1024.0
    return out


class Node:
    """One bridge. `tmp` is its folder; the same folder again after stop() is a restart with the same identity."""

    def __init__(self, label, tmp=None, env=None):
        self.label = label
        self.tmp = tmp or tempfile.mkdtemp(prefix="rnsbridge-%s-" % label)
        self.cfg, self.data, self.run = (os.path.join(self.tmp, n) for n in ("cfg", "data", "run"))
        self.sock = os.path.join(self.run, "bridge.sock")
        self.env = dict(env or {})
        self.proc = self.conn = self.log = None
        os.makedirs(self.cfg, exist_ok=True)
        cfg = os.path.join(self.cfg, "config")
        if not os.path.exists(cfg):
            with open(cfg, "w") as f:
                f.write(rnsconfig.FIRST_RUN.replace("  share_instance = Yes\n", "  share_instance = Yes\n"
                        "  instance_name = rnsbridge%s%d\n" % (label, free_port())))

    def start(self, timeout=30):
        env = dict(os.environ, PYTHONPATH=BRIDGE_DIR, PYTHONUNBUFFERED="1", **self.env)
        env.pop("XDG_RUNTIME_DIR", None)
        self.log = open(os.path.join(self.tmp, "bridge.log"), "a")
        self.proc = subprocess.Popen([sys.executable, "-m", "reticulum_bridge", "--config", self.cfg, "--data",
                                      self.data, "--run", self.run], env=env, stdout=self.log,
                                     stderr=subprocess.STDOUT)
        def connect():
            if self.proc.poll() is not None:
                return True
            try:
                self.conn = Connection(self.sock)
                return True
            except OSError:                                   # no socket yet, or a stale one of a killed bridge
                return False
        if not wait_for(connect, timeout):
            raise RuntimeError("bridge %s did not start: %s" % (self.label, self.logtext()))
        if self.proc.poll() is not None:
            raise RuntimeError("bridge %s exited: %s" % (self.label, self.logtext()))
        self.conn.call("hello")
        return self

    def stop(self, kill=False):
        if self.conn:
            self.conn.close()
            self.conn = None
        if self.proc and self.proc.poll() is None:
            if kill:
                self.proc.kill()
            else:
                self.proc.terminate()
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.log:
            self.log.close()
            self.log = None

    def logtext(self):
        return rd(os.path.join(self.tmp, "bridge.log")) if os.path.exists(os.path.join(self.tmp, "bridge.log")) else ""

    def call(self, verb, params=None, timeout=15.0):
        return self.conn.call(verb, params, timeout)

    def request(self, verb, params=None, timeout=15.0):
        return self.conn.request(verb, params, timeout)

    def code(self, verb, params=None):
        """The error code of a request that must be refused."""
        r = self.conn.request(verb, params)
        assert not r.get("ok"), "%s was accepted: %r" % (verb, r)
        return r["error"]["code"]

    @property
    def address(self):
        return self.call("identity.info")["lxmf_address"]

    def run_state(self):
        return dict(l.split("=", 1) for l in rd(os.path.join(self.run, "bridge.state")).splitlines() if "=" in l)

    def iface(self, name):
        return next((i for i in self.call("ifaces")["interfaces"] if i["name"] == name), None)

    def link_server(self, port, outgoing=True):
        self.call("iface.add", {"name": "Link", "type": "TCPServerInterface",
                                "options": {"listen_ip": "127.0.0.1", "listen_port": port}})
        self.call("iface.enable", {"name": "Link"}, timeout=30)
        self.call("iface.outgoing", {"name": "Link", "outgoing": outgoing}, timeout=30)

    def link_client(self, port, outgoing=True):
        self.call("iface.add", {"name": "Link", "type": "TCPClientInterface",
                                "options": {"target_host": "127.0.0.1", "target_port": port}})
        self.call("iface.enable", {"name": "Link"}, timeout=30)
        self.call("iface.outgoing", {"name": "Link", "outgoing": outgoing}, timeout=30)

    def online(self):
        return wait_for(lambda: (self.iface("Link") or {}).get("online"), 20)

    def drain_events(self):
        self.conn.events.clear()


class FakeLauncher:
    """A stand-in for the launcher's launcher.sock: records each request line and answers like the real one."""

    def __init__(self, path, answer=True):
        import threading
        self.path, self.answer, self.requests = path, answer, []
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        self.srv.settimeout(0.2)
        self.stop_flag = False
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while not self.stop_flag:
            try:
                c, _ = self.srv.accept()
            except (socket.timeout, OSError):
                continue
            try:
                c.settimeout(2)
                line = b""
                while b"\n" not in line:
                    chunk = c.recv(512)
                    if not chunk:
                        break
                    line += chunk
                req = line.split(b"\n")[0].decode("utf-8", "replace")
                self.requests.append(req)
                if self.answer:
                    c.sendall(b"ok queued\n" if req.startswith("toast") else b"ok\n")
            except OSError:
                pass
            finally:
                c.close()

    def close(self):
        self.stop_flag = True
        self.thread.join(2)
        self.srv.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass
