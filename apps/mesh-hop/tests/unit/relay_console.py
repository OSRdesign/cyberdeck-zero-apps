#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A stand-in for meshcore-cli on Mesh Hop's CLI relay (report 033, phase P3), for the headless flow test_relay_sim.cpp.

It puts on the wire what meshcore-cli and its library meshcore_py send (the framing of meshcore_py tcp_cx.py; the payloads of
send_appstart, send_device_query, set_flood_scope, get_contacts, get_msg, send_login, send_cmd, send_logout) and checks what the
simulated node (tools/meshcore_sim.py) answers through Mesh Hop's relay. Nothing of upstream is run here: phase P4 bundles the real,
pinned meshcore-cli and adds its own smoke flow. Exit status 0 = every check passed.

    python3 relay_console.py --port <relay port> [--expect-dm <text>]
"""

import argparse
import re
import select
import socket
import struct
import sys
import time

checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        print("relay_console: FAIL " + what, flush=True)
        sys.exit(1)


class Console:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)       # asyncio does the same
        self.buf = b""
        self.waiting = False                                               # MESSAGES_WAITING seen, not fetched yet
        self.logins = []                                                   # LOGIN_SUCCESS / LOGIN_FAIL pushes
        self.msgs = []                                                     # (sender prefix, text type, text)

    def send(self, payload):
        self.s.sendall(b"<" + struct.pack("<H", len(payload)) + payload)

    def take(self):
        while True:
            i = self.buf.find(b">")
            if i < 0:
                self.buf = b""
                return None
            self.buf = self.buf[i:]
            if len(self.buf) < 3:
                return None
            n = struct.unpack("<H", self.buf[1:3])[0]
            if n == 0 or n > 300:
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < 3 + n:
                return None
            f, self.buf = self.buf[3:3 + n], self.buf[3 + n:]
            return f

    def next_frame(self, timeout):
        end = time.time() + timeout
        while True:
            f = self.take()
            if f is not None:
                return f
            left = end - time.time()
            if left <= 0:
                return None
            r, _, _ = select.select([self.s], [], [], left)
            if not r:
                return None
            data = self.s.recv(4096)
            if not data:
                print("relay_console: FAIL the relay closed the connection", flush=True)
                sys.exit(1)
            self.buf += data

    def on_push(self, f):
        if f[0] == 0x83:
            self.waiting = True
        elif f[0] in (0x85, 0x86):
            self.logins.append(f)

    def request(self, payload, codes, timeout=2.0):
        """meshcore_py CommandHandler.send: the first answer with one of the codes, or an ERROR; pushes go on the side."""
        self.send(payload)
        end = time.time() + timeout
        while True:
            f = self.next_frame(max(0.0, end - time.time()))
            if f is None:
                check(False, "an answer to command %d" % payload[0])
            if f[0] >= 0x80:
                self.on_push(f)
            elif f[0] in codes or f[0] == 1:
                return f

    def fetch(self):
        """get_msg until NO_MORE_MSGS (meshcore_py sync / auto-fetch on MESSAGES_WAITING)."""
        self.waiting = False
        while True:
            f = self.request(b"\x0a", {7, 8, 16, 17, 27, 10})
            if f[0] == 10:
                return
            if f[0] in (7, 16):
                o = 4 if f[0] == 16 else 1
                prefix, txt_type = f[o:o + 6], f[o + 7]
                text = f[o + 12 + (4 if txt_type == 2 else 0):].decode("utf-8", "ignore")
                self.msgs.append((prefix, txt_type, text))
            else:
                self.msgs.append((b"", 0, f[11 if f[0] == 17 else 8:].decode("utf-8", "ignore")))

    def pump_until(self, done, timeout):
        end = time.time() + timeout
        while not done():
            left = end - time.time()
            if left <= 0:
                return False
            f = self.next_frame(left)
            if f is None:
                return done()
            if f[0] >= 0x80:
                self.on_push(f)
            if self.waiting:
                self.fetch()
        return True

    def login(self, key, password):
        """send_login_sync: MSG_SENT, then the verdict within suggested_timeout / 800 s."""
        self.logins.clear()
        f = self.request(b"\x1a" + key + password.encode(), {6})
        check(f[0] == 6, "login accepted by the companion (MSG_SENT)")
        timeout = struct.unpack("<I", f[6:10])[0] / 800
        self.pump_until(lambda: bool(self.logins), timeout)
        return self.logins[0] if self.logins else None

    def cli(self, key, command, silent=False):
        """send_cmd (text type 1, first and only attempt), then the reply as a contact message of text type 1."""
        f = self.request(b"\x02\x01\x00" + struct.pack("<I", int(time.time())) + key[:6] + command.encode(), {6})
        check(f[0] == 6 and f[2:6] == b"\0\0\0\0", "command %r sent, no ACK expected" % command)
        before = len(self.msgs)

        def reply():
            return next((t for p, ty, t in self.msgs[before:] if p == key[:6] and ty == 1), None)

        self.pump_until(lambda: reply() is not None, 2.0 if silent else 4.0)
        return reply()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--expect-dm", default="", help="a direct message that waits on the node when the console starts")
    a = ap.parse_args()
    c = Console(a.port)

    # what meshcore-cli does at connect: MeshCore.connect (app start, 2 s), main (device query), interactive_loop (scope reset,
    # contacts, channels, sync_msgs, then auto-fetch)
    f = c.request(b"\x01\x03      mccli", {5}, timeout=2.0)
    check(f[0] == 5 and f[58:].decode() == "SimNode", "APP_START answered by the node with its SELF_INFO")
    f = c.request(b"\x16\x03", {13})
    check(f[0] == 13 and f[1] == 11, "DEVICE_QUERY answered (firmware level 11)")
    f = c.request(b"\x36\x00" + b"\0" * 16, {0})
    check(f[0] == 0, "SET_FLOOD_SCOPE (reset) answered OK")
    c.send(b"\x04")
    contacts = {}
    while True:
        f = c.next_frame(3.0)
        if f is None:
            check(False, "the contact list")
        if f[0] >= 0x80:
            c.on_push(f)
        elif f[0] == 3:
            contacts[f[100:132].split(b"\0")[0].decode()] = f[1:33]
        elif f[0] == 4:
            break
    check("Hilltop repeater" in contacts and "Weather room" in contacts, "the repeater and the room are contacts")
    for i in range(8):
        f = c.request(bytes([0x1f, i]), {18})
        check(f[0] == 18, "channel %d" % i)
    c.fetch()
    if a.expect_dm:
        check(any(t == a.expect_dm and ty == 0 for _, ty, t in c.msgs), "the waiting direct message reached the console")
    rep, room = contacts["Hilltop repeater"], contacts["Weather room"]

    # logins: a wrong password gets no answer (the firmware is silent), a guest cannot run commands, an admin can
    check(c.login(rep, "nope") is None, "a wrong password: no answer")
    check(c.cli(rep, "ver", silent=True) is None, "a command before any login: ignored")
    p = c.login(rep, "hello")
    check(p is not None and p[0] == 0x85 and p[1] == 0 and p[2:8] == rep[:6] and p[12] == 0, "guest login")
    check(c.cli(rep, "ver", silent=True) is None, "a command as a guest: ignored")
    p = c.login(rep, "password")
    check(p is not None and p[0] == 0x85 and p[1] == 1 and p[12] == 3, "admin login")

    # the remote CLI (CommonCLI replies)
    check((c.cli(rep, "ver") or "").startswith("v1.15.0-sim (Build: "), "ver")
    check(c.cli(rep, "get name") == "> Hilltop repeater", "get name")
    check(c.cli(rep, "set name Hill2") == "OK", "set name")
    check(c.cli(rep, "get name") == "> Hill2", "get name after set name")
    check(re.fullmatch(r"\d\d:\d\d - \d+/\d+/\d+ UTC", c.cli(rep, "clock") or "") is not None, "clock")
    check(c.cli(rep, "frobnicate") == "Unknown command", "an unknown command")
    check(c.cli(rep, "get prv.key") == "??: prv.key", "get prv.key is refused over the mesh")
    check(c.cli(rep, "reboot", silent=True) is None, "reboot answers nothing")

    # the room: admin, then a blank password from a key already in its access list; logout
    p = c.login(room, "password")
    check(p is not None and p[1] == 1, "room admin login")
    p = c.login(room, "")
    check(p is not None and p[0] == 0x85, "blank password accepted from a known key")
    f = c.request(b"\x1d" + room, {0})
    check(f[0] == 0, "logout")
    c.s.close()
    print("relay_console: %d checks passed" % checks, flush=True)


if __name__ == "__main__":
    main()
