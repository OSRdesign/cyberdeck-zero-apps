# SPDX-License-Identifier: MIT
"""A small client of the bridge socket, for scripts, board tests and the PC tests (no RNS needed).

    python3 -m reticulum_bridge.client [--sock PATH] VERB ['{"json": "params"}'] [--events SECONDS]

Prints the response line (and, with --events, the event lines that arrive in that time). Exit 0 ok, 1 error reply,
3 no bridge.
"""
import json
import os
import socket
import sys
import time


def default_sock():
    return os.path.join(os.environ.get("XDG_RUNTIME_DIR", ""), "reticulum", "bridge.sock")


class Connection:
    def __init__(self, path=None, timeout=20.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        try:
            self.sock.connect(path or default_sock())
        except OSError:
            self.sock.close()
            raise
        self.buf = b""
        self.events = []
        self.next_id = 0

    def _line(self, deadline):
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("no line from the bridge")
            self.sock.settimeout(left)
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("the bridge closed the connection")
            self.buf += data
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line)

    def request(self, verb, params=None, timeout=15.0):
        """The whole response object; events read meanwhile go to self.events."""
        self.next_id += 1
        rid = self.next_id
        msg = {"id": rid, "verb": verb, "params": params or {}, "timeout": timeout}
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        deadline = time.monotonic() + timeout + 5
        while True:
            obj = self._line(deadline)
            if "event" in obj and "id" not in obj:
                self.events.append(obj)
            elif obj.get("id") == rid:
                return obj

    def call(self, verb, params=None, timeout=15.0):
        """The result; raises RuntimeError(code, message) on an error reply."""
        r = self.request(verb, params, timeout)
        if not r.get("ok"):
            raise RuntimeError(r["error"]["code"], r["error"]["message"])
        return r["result"]

    def wait_event(self, name, timeout, match=None):
        deadline = time.monotonic() + timeout
        while True:
            for e in self.events:
                if e["event"] == name and (match is None or match(e["data"])):
                    self.events.remove(e)
                    return e["data"]
            self.events.append(self._line(deadline))

    def close(self):
        self.sock.close()


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    sock = default_sock()
    events = 0.0
    if "--sock" in args:
        i = args.index("--sock")
        sock = args[i + 1]
        del args[i:i + 2]
    if "--events" in args:
        i = args.index("--events")
        events = float(args[i + 1])
        del args[i:i + 2]
    if not args:
        sys.stderr.write(__doc__)
        return 2
    try:
        c = Connection(sock)
    except OSError as e:
        sys.stderr.write("no bridge at %s: %s\n" % (sock, e.strerror))
        return 3
    r = c.request(args[0], json.loads(args[1]) if len(args) > 1 else {})
    print(json.dumps(r))
    end = time.monotonic() + events
    while time.monotonic() < end:
        try:
            print(json.dumps(c._line(end)))
        except TimeoutError:
            break
    return 0 if r.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
