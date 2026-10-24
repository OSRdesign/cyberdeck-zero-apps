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



if __name__ == "__main__":
    with contextlib.redirect_stdout(sim_log):
        main()
    print("sim_remote_check: %d checks passed" % checks, flush=True)
