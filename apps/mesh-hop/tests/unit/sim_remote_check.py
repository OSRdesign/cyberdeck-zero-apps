#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks of the simulator's remote repeaters and rooms (report 036, slice S1), without a port: tools/meshcore_sim.py's Radio is
driven directly (its handle() and its timers) and the frames it sends are read back.

    python3 sim_remote_check.py <path of meshcore_sim.py>       (run by run_tests.sh; exit status 0 = every check passed)
"""

import contextlib
import importlib.util
import io
import os
import struct
import sys

checks = 0
sim_log = io.StringIO()                           # what the simulator prints: shown only when a check fails


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        sys.__stdout__.write(sim_log.getvalue()[-2000:])
        sys.__stdout__.write("sim_remote_check: FAIL " + what + "\n")
        sys.__stdout__.flush()
        sys.exit(1)


def load(path):
    spec = importlib.util.spec_from_file_location("meshcore_sim_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Board:
    """A Radio and the frames it sent; flush() runs its timers at once (the remote delays)."""

    def __init__(self, sim, argv):
        self.sim = sim
        self.radio = sim.Radio(sim.build_parser().parse_args(argv))
        self.frames = []
        self.radio.out = self.frames.append

    def cmd(self, payload):
        self.radio.handle(bytes(payload), self.frames.append)

    def flush(self):
        while self.radio.timers:
            due, self.radio.timers = self.radio.timers, []
            for _, fn in due:
                fn()

    def take(self):
        f = list(self.frames)
        self.frames.clear()                       # the same list: the timers keep appending to it
        return f

    def contact(self, name, n=0):
        return [c for c in self.radio.contacts if c.name == name][n]

    def login(self, c, password):
        """SEND_LOGIN, the companion's SENT, then the remote verdict (or nothing)."""
        self.take()
        self.cmd(bytes([26]) + c.key + password.encode())
        sent = self.take()
        self.flush()
        return sent, self.take()

    def request(self, c, data, code=50):
        """SEND_BINARY_REQ (or 57: SEND_ANON_REQ) with the request data, SENT, then the pushes 0x8C (or nothing)."""
        self.take()
        self.cmd(bytes([code]) + c.key + bytes(data))
        sent = self.take()
        self.flush()
        return sent, [f for f in self.take() if f[0] == 0x8C]


def neighbours_req(count=255, offset=0, order=0, plen=4):
    return bytes([6, 0, count]) + struct.pack("<H", offset) + bytes([order, plen]) + b"\x11\x22\x33\x44"


def neighbours_page(push, plen=4):
    """(total, [(prefix, heard secs ago, snr)]) of a 0x8C NEIGHBOURS answer (the layout meshcore_py reader.py reads)."""
    d = push[6:]
    total, n = struct.unpack("<hh", d[:4])
    out = []
    for i in range(n):
        e = d[4 + i * (plen + 5): 4 + (i + 1) * (plen + 5)]
        out.append((e[:plen], struct.unpack("<I", e[plen:plen + 4])[0], struct.unpack("<b", e[plen + 4:plen + 5])[0] / 4))
    return total, out


def s2_checks(sim):
    """Report 036 slice S2: binary requests (50), anonymous requests (57), the push 0x8C with the tag of SENT."""
    b = Board(sim, ["--remote-cases"])
    hill, room = b.contact("Hilltop repeater"), b.contact("Weather room")
    # no login: SENT with a tag, then nothing (the node does not list this board)
    sent, pushes = b.request(hill, [1])
    check(len(sent) == 1 and sent[0][0] == 6 and len(sent[0]) == 10 and struct.unpack("<I", sent[0][6:10])[0] == 1600 and pushes == [],
          "STATUS without a login: SENT, no answer")
    tag0 = sent[0][2:6]
    check(tag0 != hill.key[:4], "a request's tag is the companion's clock, not the key prefix")
    # a guest: STATUS answered as 0x8C, 0, the tag, the 56 byte RepeaterStats padded to 16 byte blocks (4 + 56 -> 64)
    b.login(hill, "")
    sent, pushes = b.request(hill, [1])
    tag = sent[0][2:6]
    check(len(pushes) == 1 and pushes[0][1] == 0 and pushes[0][2:6] == tag and len(pushes[0]) == 2 + 64, "STATUS: the push carries the tag, padded")
    check(struct.unpack("<I", tag)[0] > struct.unpack("<I", tag0)[0], "tags grow (getCurrentTimeUnique)")
    st = pushes[0][6:]
    bat, txq, nf, rssi, rx, tx = struct.unpack("<HHhhII", st[:16])
    snr4, = struct.unpack("<h", st[42:44])
    rxerr, = struct.unpack("<I", st[52:56])
    check(bat == 4120 and nf == -112 and rssi == -89 and rx > 0 and tx > 0 and snr4 == 26 and rxerr == 7 and st[56:] == bytes(4),
          "RepeaterStats: battery, noise floor, RSSI, counters, SNR x 4, the error counter, then padding")
    # a room's ServerStats: 52 bytes (4 + 52 -> 64: 8 bytes of padding), posts at 48
    b.login(room, "")
    sent, pushes = b.request(room, [1])
    check(len(pushes) == 1 and len(pushes[0]) == 66 and struct.unpack("<HH", pushes[0][6 + 48:6 + 52]) == (12, 30) and pushes[0][6 + 52:] == bytes(8),
          "a room's status: ServerStats with n_posted / n_post_push")
    sent, pushes = b.request(room, neighbours_req())
    check(pushes == [], "a room has no neighbours")
    sent, pushes = b.request(room, [7])
    check(pushes == [], "a room has no owner info")
    # NEIGHBOURS, paged: 22 by default, 14 per page with 4 byte prefixes (the 130 byte buffer), the other repeater contacts among them
    sent, pushes = b.request(hill, neighbours_req())
    check(len(pushes) == 1 and pushes[0][2:6] == sent[0][2:6], "NEIGHBOURS answered with the tag")
    total, page = neighbours_page(pushes[0])
    check(total == 22 and len(page) == 14, "22 neighbours, 14 in the first page")
    others = [c for c in b.radio.contacts if c.kind == 2 and c.key != hill.key]
    prefixes = [p for p, _, _ in page]
    check(len(others) == 3 and all(c.key[:4] in prefixes for c in others), "the three other repeater contacts are among them (names to resolve)")
    check(all(page[i][1] <= page[i + 1][1] for i in range(len(page) - 1)), "newest first (heard seconds ago ascending)")
    sent, pushes = b.request(hill, neighbours_req(offset=14))
    total, page2 = neighbours_page(pushes[0])
    check(total == 22 and len(page2) == 8 and not set(p for p, _, _ in page2) & set(prefixes), "the second page: the 8 others")
    sent, pushes = b.request(hill, neighbours_req(count=5, order=2))
    total, strong = neighbours_page(pushes[0])
    check(len(strong) == 5 and all(strong[i][2] >= strong[i + 1][2] for i in range(4)), "count 5, strongest first")
    sent, pushes = b.request(hill, neighbours_req(plen=6))
    total, wide = neighbours_page(pushes[0], 6)
    check(len(wide) == 11 and len(wide[0][0]) == 6, "6 byte prefixes: 11 entries fit 130 bytes")
    sent, pushes = b.request(hill, bytes([6, 1, 255, 0, 0, 0, 4]))
    check(pushes == [], "a NEIGHBOURS version other than 0: no answer")
    # OWNER_INFO: "<firmware>\n<name>\n<owner info>", '|' of --owner-info as a new line
    sent, pushes = b.request(hill, [7])
    text = pushes[0][6:].rstrip(b"\0").decode() if pushes else ""
    check(text == "v1.18.0\nHilltop repeater\nSimulated owner\nAsk on #test", "OWNER_INFO text")
    # TELEMETRY: Cayenne LPP, a guest gets the base (voltage, temperature)
    sent, pushes = b.request(hill, [3, 0, 0, 0, 0])
    check(len(pushes) == 1 and pushes[0][6:14] == bytes([1, 0x74, 0x01, 0x9C, 1, 0x67, 0x00, 0xE7]) and pushes[0][14] == 0, "TELEMETRY base for a guest")
    b.login(hill, "password")
    sent, pushes = b.request(hill, [3, 0, 0, 0, 0])
    check(pushes[0][14:17] == bytes([2, 0x68, 101]), "an admin gets the other sensors too")
    sent, pushes = b.request(hill, [9])
    check(pushes == [], "an unknown request type: no answer")
    # one pending tag: a second request replaces it; the first answer still comes (an "unhandled response" with its own tag)
    b.take()
    b.cmd(bytes([50]) + hill.key + bytes([1]))
    b.cmd(bytes([50]) + hill.key + bytes([7]))
    sents = b.take()
    b.flush()
    pushes = [f for f in b.take() if f[0] == 0x8C]
    check(len(sents) == 2 and len(pushes) == 2 and pushes[0][2:6] == sents[0][2:6] and pushes[1][2:6] == sents[1][2:6] and
          b.radio.pending_req is None, "two requests: both answers come, each with its own tag")
    # a request clears the pending login (clearPendingReqs)
    b.take()
    b.cmd(bytes([26]) + hill.key + b"password")
    b.cmd(bytes([50]) + hill.key + bytes([1]))
    b.flush()
    check([f[0] for f in b.take() if f[0] in (0x85, 0x8C)] == [0x8C], "a request after a login: the login's answer is not matched any more")
    # --remote-drop: the next answer is lost on the air
    b.radio.remote_drop = 1
    sent, pushes = b.request(hill, [1])
    check(len(sent) == 1 and pushes == [] and b.radio.remote_drop == 0, "an answer lost on the air")
    # not a contact: ERR not found; a frame without the request type: unsupported
    b.take()
    b.cmd(bytes([50]) + bytes(32) + bytes([1]))
    check(b.take() == [bytes([1, 2])], "BINARY_REQ to a key that is not a contact: ERR not found")
    b.cmd(bytes([50]) + hill.key)
    check(b.take() == [bytes([1, 1])], "BINARY_REQ without a request type: unsupported")

    # ---- anonymous requests: no login, direct only, the limiter
    a = Board(sim, ["--remote-cases"])
    hill, mill, hexname = a.contact("Hilltop repeater"), a.contact("Old Mill  relay 2"), a.contact("909192")
    check(hill.path_len != 0xFF and hexname.path_len == 0xFF, "Hilltop has a route, 909192 floods")
    sent, pushes = a.request(hill, [2, 0x03, 0x30, 0x20, 0x10], code=57)
    body = pushes[0][6:] if pushes else b""
    check(len(sent) == 1 and sent[0][1] == 0 and len(pushes) == 1 and pushes[0][2:6] == sent[0][2:6] and len(body) >= 4 and
          body[4:].rstrip(b"\0") == b"Hilltop repeater\nSimulated owner\nAsk on #test", "anonymous OWNER: the clock, then name and owner info")
    check(abs(struct.unpack("<I", body[:4])[0] - int(sim.time.time())) < 5, "the node's clock in front")
    sent, pushes = a.request(hexname, [2, 0xFF], code=57)
    check(sent[0][1] == 1 and pushes == [], "anonymous by flood: ignored by the repeater")
    sent, pushes = a.request(mill, [3, 0x01, 0x40], code=57)
    check(len(pushes) == 1 and pushes[0][6 + 4] == 0x00, "anonymous BASIC: the clock and the feature byte (repeat on)")
    sent, pushes = a.request(mill, [2, 0xC1, 0x40], code=57)
    check(pushes == [], "a reply path with hash size 4: no answer")
    sent, pushes = a.request(a.contact("Club room"), [2, 0x00], code=57)
    check(pushes == [], "a room does not answer anonymous requests")
    for i in range(2):
        sent, pushes = a.request(hill, [3, 0x00], code=57)
        check(len(pushes) == 1, "anonymous request %d of 4 in the window" % (i + 2))
    sent, pushes = a.request(hill, [2, 0x00], code=57)
    check(len(pushes) == 1, "the 4th in the window")
    sent, pushes = a.request(hill, [2, 0x00], code=57)
    check(len(sent) == 1 and pushes == [], "the 5th within 180 s: over the limit, no answer")
    a.radio.remote_of(hill)["anon"][0] -= 181                # the window is over
    sent, pushes = a.request(hill, [2, 0x00], code=57)
    check(len(pushes) == 1, "a new window")
    a.take()
    a.cmd(bytes([57]) + hill.key)
    check(a.take() == [bytes([1, 1])], "ANON_REQ without data: unsupported")

    # ---- an old repeater: level 1, no OWNER_INFO, no anonymous answers, neighbours for admins only
    o = Board(sim, ["--old-repeater", "--remote-cases"])
    hill, room = o.contact("Hilltop repeater"), o.contact("Weather room")
    _, v = o.login(hill, "")
    check(len(v) == 1 and v[0][13] == 1, "an old repeater reports level 1")
    _, v = o.login(room, "")
    check(len(v) == 1 and v[0][13] == 1, "a room reports level 1 (simple_room_server)")
    sent, pushes = o.request(hill, [1])
    check(len(pushes) == 1 and len(pushes[0]) == 2 + 64 and pushes[0][6 + 52:] == bytes(8), "an old repeater's status: 52 bytes (no error counter)")
    sent, pushes = o.request(hill, [7])
    check(pushes == [], "no OWNER_INFO")
    sent, pushes = o.request(hill, neighbours_req())
    check(pushes == [], "neighbours: not for a guest")
    o.login(hill, "password")
    sent, pushes = o.request(hill, neighbours_req())
    check(len(pushes) == 1 and neighbours_page(pushes[0])[0] == 22, "neighbours for an admin")
    sent, pushes = o.request(hill, [2, 0x00], code=57)
    check(pushes == [], "no anonymous answer")
    # --neighbours, --owner-info, --remote-fw, --remote-battery, --remote-delay
    n = Board(sim, ["--neighbours", "3", "--owner-info", "Me|Here", "--remote-fw", "v9.9", "--remote-battery", "3700", "--remote-delay", "0.25"])
    hill = n.contact("Hilltop repeater")
    check(n.radio.args.remote_delay == 0.25, "--remote-delay")
    n.login(hill, "")
    n.take()
    n.cmd(bytes([50]) + hill.key + bytes([1]))
    n.take()
    check(len(n.radio.timers) == 1 and 0.2 < n.radio.timers[0][0] - sim.time.time() <= 0.25, "the answer is due --remote-delay later")
    n.flush()
    st = [f for f in n.take() if f[0] == 0x8C]
    check(len(st) == 1 and struct.unpack("<H", st[0][6:8])[0] == 3700, "--remote-battery")
    sent, pushes = n.request(hill, neighbours_req())
    check(neighbours_page(pushes[0])[0] == 3, "--neighbours 3")
    sent, pushes = n.request(hill, [7])
    check(pushes[0][6:].rstrip(b"\0") == b"v9.9\nHilltop repeater\nMe\nHere", "--owner-info, --remote-fw")


def main():
    sim = load(sys.argv[1])
    for k in ("MESHHOP_SIM_ADMIN_PASSWORD", "MESHHOP_SIM_GUEST_PASSWORD"):
        os.environ.pop(k, None)

    # the defaults: an empty guest password (the firmware's), admin "password", the four contacts of the other tests
    b = Board(sim, [])
    check(b.radio.args.guest_password == "" and b.radio.args.admin_password == "password", "default passwords")
    check(len(b.radio.contacts) == 4, "four contacts without --remote-cases")
    hill = b.contact("Hilltop repeater")
    sent, verdict = b.login(hill, "")
    check(len(sent) == 1 and sent[0][0] == 6 and sent[0][1] == 0 and sent[0][2:6] == hill.key[:4] and struct.unpack("<I", sent[0][6:10])[0] == 1600,
          "SENT: direct (Hilltop has a route), tag = the first 4 key bytes, est_timeout 1600 ms")
    check(len(verdict) == 1 and verdict[0][0] == 0x85 and verdict[0][1] == 0 and verdict[0][2:8] == hill.key[:6] and len(verdict[0]) == 14 and
          verdict[0][12] == 0 and verdict[0][13] == 2, "a blank login gives guest (14 byte push: ACL byte 0, repeater level 2)")
    sent, verdict = b.login(hill, "nope")
    check(len(sent) == 1 and verdict == [], "a wrong password: SENT, then nothing")
    sent, verdict = b.login(hill, "password")
    check(len(verdict) == 1 and verdict[0][1] == 1 and verdict[0][12] == 3, "admin")
    sent, verdict = b.login(hill, "")
    check(len(verdict) == 1 and verdict[0][12] == 3, "a blank password from a key in the access list gives the role back (admin, not guest)")
    room = b.contact("Weather room")
    sent, verdict = b.login(room, "")
    check(sent[0][1] == 1 and len(verdict) == 1 and verdict[0][2:8] == room.key[:6], "the room: sent by flood, guest by a blank password")

    # one pending login on the companion: a second SEND_LOGIN makes the first one's answer unmatchable
    b.take()
    b.cmd(bytes([26]) + hill.key + b"password")
    b.cmd(bytes([26]) + room.key + b"password")
    b.flush()
    pushes = [f for f in b.take() if f[0] == 0x85]
    check(len(pushes) == 1 and pushes[0][2:8] == room.key[:6], "only the last login is answered (clearPendingReqs)")
    # a chat node never answers a login
    sent, verdict = b.login(b.contact("Alice"), "password")
    check(len(sent) == 1 and verdict == [], "a chat node: no answer")
    # not a contact
    b.take()
    b.cmd(bytes([26]) + bytes(32))
    check(b.take() == [bytes([1, 2])], "not a contact: ERR not found")

    # answers lost on the air: the next N, then all, then none; the node still logs us in
    b.radio.remote_drop = 1
    sent, verdict = b.login(hill, "password")
    check(len(sent) == 1 and verdict == [] and b.radio.remote_drop == 0, "one answer lost")
    sent, verdict = b.login(hill, "password")
    check(len(verdict) == 1, "the next one comes")
    b.radio.remote_drop = sim.parse_drop("all")
    for _ in range(3):
        sent, verdict = b.login(hill, "password")
        check(verdict == [], "out of reach: no answer")
    b.radio.remote_drop = sim.parse_drop("0")
    check(sim.parse_drop("2") == 2 and sim.parse_drop("ALL") == -1 and sim.parse_drop("") == 0, "drop values")
    check(Board(sim, ["--remote-drop", "all"]).radio.remote_drop == -1 and Board(sim, ["--remote-drop", "3"]).radio.remote_drop == 3, "--remote-drop")
    check(Board(sim, ["--remote-delay", "0.1"]).radio.args.remote_delay == 0.1 and Board(sim, ["--login-delay", "0.2"]).radio.args.remote_delay == 0.2,
          "--remote-delay and its alias --login-delay")

    # a node that restarts forgets its guests (not saved to flash) and keeps its admins; "all" forgets everyone
    g = Board(sim, ["--guest-password", "hello"])
    hill, room = g.contact("Hilltop repeater"), g.contact("Weather room")
    _, v = g.login(hill, "password")
    _, w = g.login(room, "hello")
    check(len(v) == 1 and v[0][12] == 3 and len(w) == 1 and w[0][12] == 0, "admin on the repeater, guest in the room")
    _, v = g.login(room, "")
    check(len(v) == 1 and v[0][12] == 0, "blank: back as guest while in the list")
    g.radio.remote_reboot()
    _, v = g.login(room, "")
    check(v == [], "after a restart the guest is forgotten: a blank login gets no answer (guest password set)")
    _, v = g.login(hill, "")
    check(len(v) == 1 and v[0][12] == 3, "the admin stays after a restart")
    g.radio.remote_reboot(None, True)
    _, v = g.login(hill, "")
    check(v == [], "remote-reboot all: the admin is forgotten too")
    # the remote CLI "reboot" restarts that node only
    _, v = g.login(room, "hello")
    _, v = g.login(hill, "password")
    g.take()
    g.cmd(bytes([2, 1, 0]) + struct.pack("<I", 1790000000) + hill.key[:6] + b"reboot")
    g.flush()
    check(g.radio.remote[hill.key]["perm"] == 3 and g.radio.remote[room.key]["perm"] == 0, "CLI reboot of the repeater: its admin stays, the room untouched")

    # logout: OK, local to the companion; the node keeps us; the bookkeeping counts it
    g.take()
    g.cmd(bytes([29]) + hill.key)
    check(g.take() == [b"\x00"], "LOGOUT answered OK")
    r = g.radio.remote[hill.key]
    check(r["logouts"] == 1 and r["perm"] == 3 and r["logins"] >= 2, "logout bookkeeping: the node still lists us as admin")
    g.cmd(bytes([29]) + bytes(32))
    check(g.take() == [b"\x00"], "LOGOUT of an unknown key: OK all the same (stopConnection)")

    # LOGIN_FAIL instead of silence (--login-fail-push), still only for the pending login
    f = Board(sim, ["--login-fail-push"])
    _, v = f.login(f.contact("Hilltop repeater"), "nope")
    check(len(v) == 1 and v[0][0] == 0x86 and v[0][2:8] == f.contact("Hilltop repeater").key[:6], "LOGIN_FAIL push")

    # passwords from the environment
    os.environ["MESHHOP_SIM_ADMIN_PASSWORD"], os.environ["MESHHOP_SIM_GUEST_PASSWORD"] = "adm", "gst"
    e = Board(sim, [])
    check(e.radio.args.admin_password == "adm" and e.radio.args.guest_password == "gst", "passwords from the environment")
    _, v = e.login(e.contact("Hilltop repeater"), "")
    check(v == [], "a guest password set: a blank login from a stranger gets no answer")
    _, v = e.login(e.contact("Hilltop repeater"), "gst")
    check(len(v) == 1 and v[0][12] == 0, "the guest password")
    check(Board(sim, ["--guest-password", ""]).radio.args.guest_password == "", "the argument wins over the environment")
    del os.environ["MESHHOP_SIM_ADMIN_PASSWORD"], os.environ["MESHHOP_SIM_GUEST_PASSWORD"]

    # --remote-cases: the same name twice, a hex-looking name that is the start of another key, spaces
    rc = Board(sim, ["--remote-cases"])
    names = [c.name for c in rc.radio.contacts]
    check(len(names) == 8 and names.count("Hilltop repeater") == 2, "two repeaters with the same name")
    h1, h2 = rc.contact("Hilltop repeater", 0), rc.contact("Hilltop repeater", 1)
    check(h1.key != h2.key and h1.kind == h2.kind == 2, "the same name, two keys")
    hexname = rc.contact("909192")
    check(h1.key.hex().startswith(hexname.name) and hexname.key != h1.key and hexname.kind == 2, "a hex-looking name that is the start of the first Hilltop's key")
    check("Old Mill  relay 2" in names and "Club room" in names and rc.contact("Club room").kind == 3, "spaces in names, a second room")
    check(len({c.key[0] for c in rc.radio.contacts}) == 8, "distinct keys")
    _, v = rc.login(h2, "password")
    check(len(v) == 1 and v[0][2:8] == h2.key[:6], "a login reaches the repeater of that KEY, not the first of that name")
    many = Board(sim, ["--remote-cases", "--contacts", "30"])
    check(len(many.radio.contacts) == 38 and len({c.key[0] for c in many.radio.contacts}) == 38, "--contacts keeps away from their keys")

    # 0.7.2: RESET_PATH (13): the contact's route is forgotten (flood), its record says so; an unknown key: ERR not found
    rp = Board(sim, ["--remote-cases"])
    h1 = rp.contact("Hilltop repeater")
    check(h1.path_len != 0xFF, "the first Hilltop has a route")
    rp.take()
    rp.cmd(bytes([13]) + h1.key)
    check(rp.take() == [b"\x00"] and h1.path_len == 0xFF, "RESET_PATH: OK, the route is flood")
    sent, _ = rp.login(h1, "")
    check(sent[0][1] == 1, "after the reset a login goes by flood")
    rp.cmd(bytes([13]) + bytes(32))
    check(rp.take() == [bytes([1, 2])], "RESET_PATH of an unknown key: ERR not found")

    s2_checks(sim)

    # 0.8.0 --learn-path: an answer to a login sent by flood brings a route back: the contact is 1 hop away and PATH_UPDATED (0x81)
    # follows the answer; a contact with a route, or a login without an answer, changes nothing; without the option nothing moves
    lp = Board(sim, ["--remote-cases", "--learn-path"])
    flood = lp.contact("909192")
    check(flood.path_len == 0xFF, "909192 is a flood contact")
    lp.take()
    lp.cmd(bytes([26]) + flood.key)
    sent = lp.take()
    lp.flush()
    after = lp.take()
    check(sent[0][1] == 1 and [f[0] for f in after] == [0x85, 0x81] and after[1][1:] == flood.key and flood.path_len == 1,
          "--learn-path: the login answer by flood, then PATH_UPDATED with the key; 1 hop now")
    sent, after = lp.login(flood, "")
    check(sent[0][1] == 0 and [f[0] for f in after] == [0x85], "the next login goes direct, no new PATH_UPDATED")
    club = lp.contact("Club room")
    _, after = lp.login(club, "wrong")
    check(after == [] and club.path_len == 0xFF, "no answer (a wrong password): no route learnt")
    plain = Board(sim, ["--remote-cases"])
    _, after = plain.login(plain.contact("909192"), "")
    check([f[0] for f in after] == [0x85] and plain.contact("909192").path_len == 0xFF, "without --learn-path the route stays flood")


if __name__ == "__main__":
    with contextlib.redirect_stdout(sim_log):
        main()
    print("sim_remote_check: %d checks passed" % checks, flush=True)
