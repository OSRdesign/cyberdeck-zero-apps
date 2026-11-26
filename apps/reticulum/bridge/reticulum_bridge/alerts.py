# SPDX-License-Identifier: MIT
"""New-message alerts: a toast and a beep, asked of the launcher through launcher.sock (docs/APP-CONTRACT.md section 14).

This is the only place that talks to the launcher. The requests used are `toast <text>` and `beep notify` (one request
line, one reply line, at most 254 bytes plus the newline, one connection per request; the launcher draws the toast when
it is in front, the beep plays at the volume in force). The socket is `$XDG_RUNTIME_DIR/applaunch/launcher.sock`
(no /tmp fallback; `RETICULUM_BRIDGE_LAUNCHER_SOCK` replaces the path: used by the tests). No socket, no reply, `err ...`:
the alert is dropped silently (an older launcher or none: contract section 7), and a failure is remembered for 10 s so
a burst does not retry per message. At most one alert per 2 s. The message text never goes into a request: the toast
names the sender only. Requests run on a short-lived thread of their own, never on an RNS or request thread.
"""
import os
import socket
import threading
import time

MIN_GAP = 2.0
FAIL_BACKOFF = 10.0
TIMEOUT = 0.5
MAX_LINE = 254


def socket_path():
    p = os.environ.get("RETICULUM_BRIDGE_LAUNCHER_SOCK")
    if p:
        return p
    run = os.environ.get("XDG_RUNTIME_DIR")
    return os.path.join(run, "applaunch", "launcher.sock") if run else None


def clean(text, limit=60):
    """Printable, one line, short: it ends up on the launcher's toast."""
    text = "".join(ch if ch.isprintable() else " " for ch in text).strip()
    return text if len(text) <= limit else text[:limit - 1] + "…"


def request(line, path=None, timeout=TIMEOUT):
    """One request to the launcher. Returns the reply line, or None when there is no service."""
    path = path or socket_path()
    data = line.encode("utf-8")
    if not path or len(data) > MAX_LINE:
        return None
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        s.sendall(data + b"\n")
        buf = b""
        while b"\n" not in buf and len(buf) < 512:
            chunk = s.recv(512)
            if not chunk:
                break
            buf += chunk
        return buf.split(b"\n", 1)[0].decode("utf-8", "replace") or None
    except OSError:
        return None
    finally:
        s.close()


class Alerter:
    """alert(sender_name) -> True when a toast and a beep were handed to a worker thread."""

    def __init__(self, log=lambda m: None, path=None):
        self.log, self.path = log, path
        self.lock = threading.Lock()
        self.last = 0.0
        self.down_until = 0.0
        self.sent = 0                                       # alerts handed on (for the tests)

    def alert(self, sender):
        now = time.monotonic()
        with self.lock:
            if now < self.down_until or now - self.last < MIN_GAP:
                return False
            self.last = now
            self.sent += 1
        threading.Thread(target=self._run, args=(sender,), name="alert", daemon=True).start()
        return True

    def _run(self, sender):
        who = clean(sender) if sender else "someone"
        reply = request("toast New message from " + who, self.path)
        if reply is None:
            with self.lock:
                self.down_until = time.monotonic() + FAIL_BACKOFF
            self.log("alert: the launcher did not answer (alerts are dropped for %d s)" % FAIL_BACKOFF)
            return
        request("beep notify", self.path)
