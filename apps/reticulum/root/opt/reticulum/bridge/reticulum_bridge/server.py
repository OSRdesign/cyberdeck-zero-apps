# SPDX-License-Identifier: MIT
"""The bridge's run folder ($XDG_RUNTIME_DIR/reticulum) and its JSON-lines socket.

Files: bridge.lock (flock: one bridge per user), bridge.sock (mode 0600; a stale one is removed once the lock is held,
since no other bridge can be serving it), bridge.state (key=value, written atomically, removed at a clean exit).
One request per line: {"id": ..., "verb": "...", "params": {...}, "timeout": s}; one response line per request with the
same id; event lines carry "event" and no id. The loop sleeps in select() and wakes only for traffic, a reply, a
request deadline or the state tick. Requests run on two worker threads.
"""
import fcntl
import json
import os
import queue
import selectors
import socket
import time
from concurrent.futures import ThreadPoolExecutor

from . import BridgeError

MAX_LINE = 65536
MAX_CLIENTS = 8
MAX_OUT = 1 << 20
DEFAULT_TIMEOUT, MIN_TIMEOUT, MAX_TIMEOUT = 15.0, 0.5, 120.0
STOP_VERB = "service.stop"


class AlreadyRunning(Exception):
    pass


class RunDir:
    def __init__(self, path):
        self.path = path
        self.sock_path = os.path.join(path, "bridge.sock")
        self.state_path = os.path.join(path, "bridge.state")
        self.lock_path = os.path.join(path, "bridge.lock")
        self.lock_fd = None

    def claim(self):
        """Takes the per-user lock; removes a stale socket. Returns True when a stale socket was removed."""
        os.makedirs(self.path, mode=0o700, exist_ok=True)
        os.chmod(self.path, 0o700)
        fd = os.open(self.lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            raise AlreadyRunning(self.read_state().get("pid", "?"))
        self.lock_fd = fd
        stale = os.path.lexists(self.sock_path)
        if stale:
            os.unlink(self.sock_path)
        return stale

    def write_state(self, values):
        tmp = self.state_path + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            for k, v in values.items():
                f.write("%s=%s\n" % (k, str(v).replace("\n", " ")))
        os.replace(tmp, self.state_path)

    def read_state(self):
        try:
            with open(self.state_path) as f:
                return dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
        except OSError:
            return {}

    def cleanup(self):
        for p in (self.sock_path, self.state_path):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        if self.lock_fd is not None:
            os.close(self.lock_fd)                      # the lock file stays: unlinking it would race a new bridge
            self.lock_fd = None


class Client:
    def __init__(self, sock, number):
        self.sock, self.number = sock, number
        self.inbuf, self.outbuf = bytearray(), bytearray()
        self.subscribed = False                         # announces.subscribe: announce, heard
        self.msg_subscribed = False                     # messages.subscribe: message, message_state, unread, ...
        self.alive = True


class Server:
    def __init__(self, rundir, dispatch, log, tick=None, tick_interval=15.0):
        self.rundir, self.dispatch, self.log = rundir, dispatch, log
        self.tick, self.tick_interval = tick, tick_interval
        self.done = queue.SimpleQueue()
        self.pending = {}
        self.clients = {}
        self.stopping = False
        self.seq = 0
        self.tick_error = None
        self.executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="bridge")
        self.wake_r, self.wake_w = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        self.sel = selectors.DefaultSelector()
        self.lsock = None

    def listen(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM | socket.SOCK_CLOEXEC)
        old = os.umask(0o177)
        try:
            s.bind(self.rundir.sock_path)
        finally:
            os.umask(old)
        os.chmod(self.rundir.sock_path, 0o600)
        s.listen(MAX_CLIENTS)
        s.setblocking(False)
        self.lsock = s
        self.sel.register(s, selectors.EVENT_READ, "listen")
        self.sel.register(self.wake_r, selectors.EVENT_READ, "wake")

    def _wake(self):
        try:
            os.write(self.wake_w, b"x")
        except BlockingIOError:
            pass

    def request_stop(self):
        """Safe from a signal handler or any thread."""
        self.stopping = True
        self._wake()

    def post_event(self, name, data, topic="announce"):
        """From any thread: an event line to every client subscribed to the topic ("announce" or "messages")."""
        self.done.put(("event", {"event": name, "data": data}, topic))
        self._wake()

    # --- loop ---
    def serve(self):
        next_tick = time.monotonic() + self.tick_interval
        while not self.stopping:
            marks = [p[2] for p in self.pending.values()] + ([next_tick] if self.tick else [])
            timeout = max(0.0, min(marks) - time.monotonic()) if marks else None
            for key, mask in self.sel.select(timeout):
                if key.data == "listen":
                    self._accept()
                elif key.data == "wake":
                    try:
                        os.read(self.wake_r, 4096)
                    except BlockingIOError:
                        pass
                else:
                    if mask & selectors.EVENT_READ:
                        self._read(key.data)
                    if mask & selectors.EVENT_WRITE and key.data.alive:
                        self._flush(key.data)
            self._drain()
            now = time.monotonic()
            for k, (c, rid, deadline) in list(self.pending.items()):
                if deadline <= now:
                    del self.pending[k]
                    self._send(c, {"id": rid, "ok": False, "error": {
                        "code": "timeout", "message": "no answer within the request's timeout"}})
            if self.tick and now >= next_tick:
                next_tick = now + self.tick_interval
                self.executor.submit(self._guard, self.tick)
        self._shutdown()

    def _guard(self, fn, *args):
        try:
            fn(*args)
            self.tick_error = None
        except Exception as e:                           # a tick must never end the loop; log a new reason once
            if str(e) != self.tick_error:
                self.log("state tick failed: %s" % e)
            self.tick_error = str(e)

    def _accept(self):
        try:
            sock, _ = self.lsock.accept()
        except (BlockingIOError, InterruptedError):
            return
        if len(self.clients) >= MAX_CLIENTS:
            sock.close()
            return
        sock.setblocking(False)
        self.seq += 1
        c = Client(sock, self.seq)
        self.clients[sock.fileno()] = c
        self.sel.register(sock, selectors.EVENT_READ, c)

    def _drop(self, c):
        if not c.alive:
            return
        c.alive = False
        self.clients.pop(c.sock.fileno(), None)
        try:
            self.sel.unregister(c.sock)
        except (KeyError, ValueError):
            pass
        c.sock.close()
        for k in [k for k in self.pending if k[0] == c.number]:
            del self.pending[k]

    def _read(self, c):
        try:
            data = c.sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            data = b""
        if not data:
            self._drop(c)
            return
        c.inbuf += data
        while c.alive and b"\n" in c.inbuf:
            line, _, rest = bytes(c.inbuf).partition(b"\n")
            c.inbuf = bytearray(rest)
            if line.strip():
                self._request(c, line)
        if len(c.inbuf) > MAX_LINE:
            self._send(c, {"id": None, "ok": False, "error": {"code": "bad_request", "message": "line too long"}})
            self._drop(c)

    def _request(self, c, line):
        try:
            req = json.loads(line)
            if not isinstance(req, dict) or not isinstance(req.get("verb"), str):
                raise ValueError("an object with a verb expected")
            rid = req.get("id")
            if not isinstance(rid, (str, int, type(None))) or isinstance(rid, bool):
                raise ValueError("id must be a string, a number or null")
            params = req.get("params") or {}
            if not isinstance(params, dict):
                raise ValueError("params must be an object")
            timeout = float(req.get("timeout", DEFAULT_TIMEOUT))
            if timeout != timeout:
                raise ValueError("timeout must be a number")
        except (ValueError, TypeError) as e:
            self._send(c, {"id": None, "ok": False, "error": {"code": "bad_request", "message": str(e)}})
            return
        self.seq += 1
        key = (c.number, self.seq)
        deadline = time.monotonic() + min(max(timeout, MIN_TIMEOUT), MAX_TIMEOUT)
        self.pending[key] = (c, rid, deadline)
        self.executor.submit(self._work, key, rid, req["verb"], params, c)

    def _work(self, key, rid, verb, params, c):
        try:
            out = {"id": rid, "ok": True, "result": self.dispatch(verb, params, c)}
        except BridgeError as e:
            out = {"id": rid, "ok": False, "error": e.as_dict()}
        except Exception as e:
            self.log("%s failed: %s: %s" % (verb, type(e).__name__, e))
            out = {"id": rid, "ok": False, "error": {"code": "internal", "message": "%s: %s" % (type(e).__name__, e)}}
        self.done.put(("reply", key, out))
        if verb == STOP_VERB and out["ok"]:
            self.request_stop()
        else:
            self._wake()

    def _drain(self):
        while True:
            try:
                item = self.done.get_nowait()
            except queue.Empty:
                return
            if item[0] == "reply":
                p = self.pending.pop(item[1], None)
                if p and p[0].alive:
                    self._send(p[0], item[2])
            else:
                for c in list(self.clients.values()):
                    if c.msg_subscribed if item[2] == "messages" else c.subscribed:
                        self._send(c, item[1])

    def _send(self, c, obj):
        if not c.alive:
            return
        c.outbuf += json.dumps(obj, separators=(",", ":")).encode() + b"\n"
        if len(c.outbuf) > MAX_OUT:
            self._drop(c)                               # a client that does not read
            return
        self._flush(c)

    def _flush(self, c):
        try:
            n = c.sock.send(c.outbuf)
            del c.outbuf[:n]
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            self._drop(c)
            return
        self.sel.modify(c.sock, selectors.EVENT_READ | (selectors.EVENT_WRITE if c.outbuf else 0), c)

    def _shutdown(self):
        self._drain()
        for c in list(self.clients.values()):
            if c.subscribed or c.msg_subscribed:
                self._send(c, {"event": "stopping", "data": {}})
            if c.alive and c.outbuf:
                c.sock.setblocking(True)
                c.sock.settimeout(1.0)
                try:
                    c.sock.sendall(c.outbuf)
                except OSError:
                    pass
            self._drop(c)
        self.sel.unregister(self.lsock)
        self.lsock.close()
        self.executor.shutdown(wait=False, cancel_futures=True)
