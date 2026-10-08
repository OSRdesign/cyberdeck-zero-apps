#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Simulated MeshCore companion radio for the Mesh Hop app (and MeshZero) (no board needed).

It creates a pseudo terminal and speaks the companion protocol over it, framed like the USB serial firmware
('<' + u16 length from the app, '>' + u16 length to the app). It answers: device query (with the exact DEVICE_INFO
frame of the user's Seeed XIAO nRF52840 running v1.15.0), app start, contacts, channels, message sync, send message
(+ ACK), send channel message, advert, battery, set time / name / radio / TX power, reset path, contact by key.
Clock cases for the app's clock policy: --gps / --no-custom-vars (what the board reports about GPS), --board-clock-offset
(board clock behind or ahead of this host), --refuse-set-time; SET_TIME in the past is refused like the firmware does.
It also pushes RX-log frames (0x88) and "messages waiting" (0x83) on its own, like a real radio on a busy mesh.
Phase 1 (0.1.1) cases: custom variables with the board GPS ("gps:0", switched by SET_CUSTOM_VAR; --no-gps-var: a board without
GPS; --old-firmware: no custom variables and no channel commands), what a channel send is answered with (--chan-reply ok | msgsent |
odd | none: "none" never answers, to exercise the 5 s fallback) and many contacts (--contacts 161) for the list speed.
Phase 2 (0.2.0) cases: REMOVE_CONTACT and ADD_UPDATE_CONTACT (--max-contacts N: TABLE_FULL beyond it; --slow-remove SEC), SET_OTHER_PARAMS and the
manual add mode (--manual-add; --old-other-params accepts only the 4 byte form), GET / SET_AUTOADD_CONFIG (--no-autoadd, --old-autoadd),
SEND_CONTROL_DATA with the zero-hop discover answered by four neighbours (--no-discover, --discover-reply ok|msgsent|none), adverts of strangers
as an ADVERT packet in the radio log (0x88) plus NEW_ADVERT (--new-nodes N, stdin "newnode"), GET_STATS (--no-stats, --old-stats),
GET_ALLOWED_REPEAT_FREQ and the repeat byte of SET_RADIO (--repeat-ranges), SET_PATH_HASH_MODE and the DEVICE_INFO of firmware level
--fw-level N (11: repeat and path hash mode bytes, like the user's board with mode 1 = 2 byte hashes), REBOOT and FACTORY_RESET (the port goes
away for 2 s and comes back; --no-factory-reset, --strict-reset-word needs the word "reset" behind the command byte).
0.2.2 cases: a SECOND BOARD (--identity B starts as board B: another public key, name, contacts, channels and queued messages; the stdin command
"swap" unplugs the cable, changes into the other board and plugs it back, like exchanging two boards on the same USB port). The factory reset
gives the board a NEW public key each time and an empty identity; --reset-format-delay SEC makes the board format for SEC seconds before it answers
(the ESP32 file system format is slow), --reset-no-ok restarts without ever sending the OK frame.

    python3 meshcore_sim.py                      # creates the port /tmp/ttyMC0 (a symlink to the pty)
    python3 meshcore_sim.py --link /tmp/ttyMC1 --chatter 20 --echo
    echo /tmp/ttyMC0 > ~/.local/share/mesh-hop/port     # tell the app to use it (or: MESHHOP_PORT=/tmp/ttyMC0)

Commands typed on the simulator's stdin (when it has one):
    msg <text>            a direct message from the first contact (Alice)
    chan <idx> <text>     a channel message "Zed: <text>"
    advert                a new advert from a neighbour
    unplug / plug         remove / recreate the port (unplug and replug the board)
    quit

On the deck (no root, no extra package):  python3 meshcore_sim.py &   then start the app: the pty stands in for the USB
cable. With socat instead (the app on one end, the simulator on the other):
    socat -d -d pty,raw,echo=0,link=/tmp/ttyMC0 pty,raw,echo=0,link=/tmp/ttyMC0sim &
    python3 meshcore_sim.py --device /tmp/ttyMC0sim
"""

import argparse
import errno
import os
import random
import select
import struct
import sys
import termios
import time
import tty

REAL_DEVICE_INFO = bytes.fromhex(
    "0d0baf280000000031392d4170722d32303236005365656564205869616f2d6e7266353200000000000000000000000000000000000000000000000076312e31352e302d6465653365323600000000000001"
)

CMD_APP_START, CMD_SEND_TXT, CMD_SEND_CHAN, CMD_GET_CONTACTS, CMD_GET_TIME, CMD_SET_TIME = 1, 2, 3, 4, 5, 6
CMD_ADVERT, CMD_SET_NAME, CMD_SYNC, CMD_SET_RADIO, CMD_SET_TXP, CMD_RESET_PATH = 7, 8, 10, 11, 12, 13
CMD_BATT, CMD_DEV_QUERY, CMD_CONTACT_BY_KEY, CMD_GET_CHANNEL, CMD_SET_CHANNEL = 20, 22, 30, 31, 32
CMD_GET_CUSTOM_VARS, CMD_SET_CUSTOM_VAR = 40, 41
# phase 2 (0.2.0)
CMD_ADD_UPDATE_CONTACT, CMD_REMOVE_CONTACT, CMD_REBOOT, CMD_SET_OTHER_PARAMS = 9, 15, 19, 38
CMD_FACTORY_RESET, CMD_SEND_CONTROL_DATA, CMD_GET_STATS, CMD_SET_AUTOADD, CMD_GET_AUTOADD = 51, 55, 56, 58, 59
CMD_GET_ALLOWED_REPEAT_FREQ, CMD_SET_PATH_HASH_MODE = 60, 61


def text_field(s, width):
    b = s.encode("utf-8")[:width]
    return b + b"\0" * (width - len(b))


class Contact:
    def __init__(self, seed, name, kind, path_len=0xFF, advert=None, lat=48.8566, lon=2.3522):
        self.key = bytes((seed + i) & 0xFF for i in range(32))
        self.name, self.kind, self.path_len = name, kind, path_len
        self.advert = advert or int(time.time()) - 600
        self.lat, self.lon = lat, lon

    def frame(self, code=3):
        return (bytes([code]) + self.key + bytes([self.kind, 0, self.path_len]) + b"\0" * 64 + text_field(self.name, 32)
                + struct.pack("<Iiii", self.advert, int(self.lat * 1e6), int(self.lon * 1e6), self.advert))


class Radio:
    def __init__(self, args):
        self.args = args
        self.name = "SimNode"
        self.freq_khz, self.bw_hz, self.sf, self.cr, self.txp, self.max_txp = 869525, 250000, 11, 5, 22, 22
        self.key = bytes(0xA0 + i for i in range(32))
        # positions around the node (48.8566, 2.3522) so that the distance column and the map have something to show;
        # the weather room shares none
        self.contacts = [Contact(0x10, "Alice", 1, path_len=1, lat=48.9020, lon=2.4010), Contact(0x50, "Bob", 1, lat=48.8010, lon=2.2890),
                         Contact(0x90, "Hilltop repeater", 2, path_len=3, lat=49.1200, lon=2.6100), Contact(0xC0, "Weather room", 3, lat=0.0, lon=0.0)]
        self.channels = {0: ("Public", bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")),
                         1: ("#test", bytes.fromhex("9cd8fcf22a47333b591d96a2b848b73f"))}
        self.queue = []          # frames for CMD_SYNC_NEXT
        self.timers = []         # (due, callable)
        self.ack_counter = 0
        self.battery_mv = 3960
        self.clock_offset = args.board_clock_offset     # seconds: the board clock is the sim host clock plus this
        self.gps = "1" if args.gps else "0"
        self.channel_count = 0
        now = int(time.time())
        # many contacts for the speed of the lists: every kind, every age, spread around the node
        used = {c.key[0] for c in self.contacts}
        seed = 0x20
        for i in range(args.contacts):
            while seed & 0xFF in used:
                seed += 1
            used.add(seed & 0xFF)
            kind = (1, 1, 1, 2, 3, 4)[i % 6]
            self.contacts.append(Contact(seed & 0xFF, "Node %03d %s" % ((i * 37) % 997, ("Alice", "Bob", "Carol", "Dave", "Eve")[i % 5]), kind,
                                         path_len=(0xFF if i % 4 else i % 3), advert=now - 60 - i * 411,
                                         lat=48.0 + (i % 17) * 0.07, lon=2.0 + (i % 23) * 0.06))
            seed += 5
        self.queue.append(self.dm(self.contacts[0], "Hello from the simulator", now - 120))
        self.queue.append(self.chan_msg(0, "Zed: anyone on the mesh?", now - 60))
        # phase 2 state
        self.port = None                                  # set by main(): reboot / factory reset unplug and replug it
        self.t0 = time.time()
        self.path_mode = 1                                # the user's real board reports mode 1 (2 byte hashes) in its DEVICE_INFO
        self.repeat = False
        self.manual_add = bool(args.manual_add)
        self.multi_acks, self.loc_policy, self.telemetry = 0, 0, 0
        self.autoadd = 0x02
        self.repeat_ranges = [] if args.repeat_ranges == "none" else [tuple(int(v) for v in r.split("-")) for r in args.repeat_ranges.split(",")]
        self.rx_packets, self.tx_packets = 120, 31
        # nodes that are not contacts: they advert, and answer a discover when they are in direct range
        self.nodes = [Contact(0xD0, "Newbie", 1, lat=48.8700, lon=2.3600), Contact(0xE0, "Ridge repeater", 2, lat=48.9000, lon=2.4500),
                      Contact(0xF0, "Dock sensor", 4, lat=0.0, lon=0.0)]
        self.node_count = 0
        self.neighbours = [(self.contacts[0], 5.5, -78), (self.contacts[2], -2.25, -104), (self.nodes[0], 8.0, -61), (self.nodes[1], -7.5, -112)]
        if args.new_nodes:
            for i in range(args.new_nodes):
                self.after(3.0 + 1.5 * i, lambda: self.new_node_advert(self.out))
        self.out = lambda payload: None                   # replaced by main()
        self.ident = "A"
        self.other = None                                 # the saved state of the board that is not plugged in
        self.reset_count = 0

    SWAP_ATTRS = ("name", "key", "contacts", "channels", "queue", "nodes", "neighbours", "node_count", "manual_add", "multi_acks", "loc_policy",
                  "telemetry", "autoadd", "path_mode", "repeat", "freq_khz", "bw_hz", "sf", "cr", "txp", "gps", "battery_mv")

    def make_board_b(self):
        """Another board: its own key, name, contacts, channels and waiting messages."""
        b = Radio(self.args)
        b.contacts = [Contact(0x70, "Zoe", 1, path_len=1, lat=48.9100, lon=2.3000), Contact(0x80, "Yan", 1, lat=48.8000, lon=2.4000),
                      Contact(0xB0, "Ridge B", 2, path_len=2, lat=49.0000, lon=2.5000)]
        b.name = "SimNodeB"
        b.key = bytes(0xB0 + i for i in range(32))
        b.channels = {0: ("Public", bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")), 1: ("#boardb", bytes.fromhex("11223344556677889900aabbccddeeff"))}
        now = int(time.time())
        b.queue = [b.dm(b.contacts[0], "Hello from board B", now - 90), b.chan_msg(1, "Quinn: B's own channel", now - 30)]
        b.nodes = [Contact(0xD8, "Newbie B", 1, lat=48.8700, lon=2.3600)]
        b.neighbours = [(b.contacts[0], 4.5, -80), (b.contacts[2], -1.0, -101)]
        b.battery_mv = 4010
        return b

    def swap_identity(self):
        """Exchange the board on the cable: the state of the plugged board is kept, the other board's state is loaded."""
        if self.other is None:
            mine = {k: getattr(self, k) for k in self.SWAP_ATTRS}
            b = self.make_board_b()
            for k in self.SWAP_ATTRS:
                setattr(self, k, getattr(b, k))
            self.other = mine
            self.ident = "B"
        else:
            cur = {k: getattr(self, k) for k in self.SWAP_ATTRS}
            for k, v in self.other.items():
                setattr(self, k, v)
            self.other = cur
            self.ident = "A" if self.ident == "B" else "B"
        print("meshcore_sim: board %s (%s) is on the cable, key %s" % (self.ident, self.name, self.key[:6].hex()), flush=True)

    # ---- frames the radio sends
    def device_info(self):
        base = bytearray(REAL_DEVICE_INFO[:80])
        base[1] = self.args.fw_level
        if self.args.fw_level >= 9:
            base.append(1 if self.repeat else 0)
        if self.args.fw_level >= 10:
            base.append(self.path_mode)
        return bytes(base)

    def advert_packet(self, c, hops=1, hash_size=2):
        """The on-air ADVERT packet of a node as it appears in the radio log: header, path, key, timestamp, signature, appdata."""
        flags = c.kind | 0x80 | (0x10 if (c.lat or c.lon) else 0)
        app = bytes([flags]) + (struct.pack("<ii", int(c.lat * 1e6), int(c.lon * 1e6)) if (c.lat or c.lon) else b"") + c.name.encode()[:31]
        path = bytes((0x30 + i * 7 + j) & 0xFF for i in range(hops) for j in range(hash_size))
        return (bytes([0x11, ((hash_size - 1) << 6) | hops]) + path + c.key + struct.pack("<I", int(time.time())) + os.urandom(64) + app)

    def log_frame(self, packet, snr=6.25, rssi=-91):
        return bytes([0x88, int(snr * 4) & 0xFF, rssi & 0xFF]) + packet

    def new_node_advert(self, out):
        """An advert from a node the board does not know: radio log + NEW_ADVERT (kept as a pending contact in manual mode, added otherwise)."""
        if self.node_count < len(self.nodes):
            c = self.nodes[self.node_count]
        else:
            seed = 0x60 + self.node_count * 3
            c = Contact(seed & 0xFF, "Roamer %d" % self.node_count, 1 + self.node_count % 4, lat=48.7 + self.node_count * 0.01, lon=2.3)
        self.node_count += 1
        out(self.log_frame(self.advert_packet(c), snr=3.5 + self.node_count, rssi=-90 - self.node_count))
        c.advert = int(time.time())
        if not self.manual_add and len(self.contacts) < self.args.max_contacts:
            self.contacts.append(c)
        out(c.frame(0x8A))

    def stats_frame(self, sub):
        up = int(time.time() - self.t0)
        self.rx_packets += 3
        if sub == 0:
            return struct.pack("<BBHIHB", 24, 0, self.battery_mv, up, 2, 0)
        if sub == 1:
            return struct.pack("<BBhbbII", 24, 1, -112, -95, 22, 5 + up // 7, 40 + up // 3)
        frame = struct.pack("<BBIIIIII", 24, 2, self.rx_packets, self.tx_packets, 4, 27, 90 + up // 5, 30)
        return frame + (struct.pack("<I", 3) if not self.args.old_stats else b"")

    def dm(self, c, text, ts=None):
        return (bytes([16, 0x16, 0, 0]) + c.key[:6] + bytes([0xFF, 0]) + struct.pack("<I", ts or int(time.time())) + text.encode())

    def chan_msg(self, idx, text, ts=None):
        return bytes([17, 0x0C, 0, 0, idx, 0x01, 0]) + struct.pack("<I", ts or int(time.time())) + text.encode()

    def self_info(self):
        name = self.name.encode()
        return (bytes([5, 1, self.txp & 0xFF, self.max_txp]) + self.key + struct.pack("<ii", 48856600, 2352200)
                + bytes([self.multi_acks, self.loc_policy, self.telemetry, 1 if self.manual_add else 0])
                + struct.pack("<II", self.freq_khz, self.bw_hz) + bytes([self.sf, self.cr]) + name)

    def board_time(self):
        return max(0, int(time.time() + self.clock_offset))

    def after(self, delay, fn):
        self.timers.append((time.time() + delay, fn))

    # ---- commands from the app; returns the list of reply frames
    def handle(self, c, out):
        code = c[0]
        a = self.args
        if code == CMD_DEV_QUERY:
            out(self.device_info())
        elif code == CMD_APP_START:
            out(self.self_info())
        elif code in (CMD_SET_TIME, CMD_RESET_PATH):
            out(b"\x00")
        elif code == CMD_ADVERT:
            out(b"\x00")
            self.after(1.5, lambda: out(bytes([0x80]) + random.choice(self.contacts).key))
        elif code == CMD_SET_NAME:
            self.name = c[1:].decode("utf-8", "ignore")
            out(b"\x00")
        elif code == CMD_SET_RADIO:
            f, b, sf, cr = struct.unpack("<IIBB", c[1:11])
            repeat = c[11] if len(c) > 11 else None
            if sf < 5 or sf > 12 or cr < 5 or cr > 8:
                out(bytes([1, 6]))
            elif repeat and not any(lo <= f <= hi for lo, hi in self.repeat_ranges):
                out(bytes([1, 6]))                      # repeat is only allowed on the listed frequencies
            else:
                self.freq_khz, self.bw_hz, self.sf, self.cr = f, b, sf, cr
                if repeat is not None:
                    self.repeat = bool(repeat)
                out(b"\x00")
        elif code == CMD_SET_PATH_HASH_MODE:
            if a.fw_level < 10 or len(c) < 3:
                out(bytes([1, 1]))
            elif c[2] > 2:
                out(bytes([1, 6]))
            else:
                self.path_mode = c[2]
                print("meshcore_sim: path hash mode set to %d" % c[2], flush=True)
                out(b"\x00")
        elif code == CMD_GET_ALLOWED_REPEAT_FREQ:
            if a.fw_level < 9:
                out(bytes([1, 1]))
            else:
                out(bytes([26]) + b"".join(struct.pack("<II", lo, hi) for lo, hi in self.repeat_ranges) + struct.pack("<II", 0, 0))
        elif code == CMD_REMOVE_CONTACT:
            key = bytes(c[1:33])
            for ct in self.contacts:
                if ct.key == key:
                    self.contacts.remove(ct)
                    print("meshcore_sim: contact removed %s" % ct.name, flush=True)
                    if a.slow_remove:
                        self.after(a.slow_remove, lambda: out(b"\x00"))
                    else:
                        out(b"\x00")
                    return
            out(bytes([1, 2]))
        elif code == CMD_ADD_UPDATE_CONTACT:
            if len(c) < 1 + 32 + 3 + 64 + 32 + 12:
                out(bytes([1, 6]))
                return
            key = bytes(c[1:33])
            existing = [ct for ct in self.contacts if ct.key == key]
            if not existing and len(self.contacts) >= a.max_contacts:
                out(bytes([1, 3]))                      # ERR_CODE_TABLE_FULL
                return
            kind, flags, plen = c[33], c[34], c[35]
            name = bytes(c[100:132]).split(b"\0")[0].decode("utf-8", "ignore")
            adv, lat, lon = struct.unpack("<Iii", c[132:144])
            ct = existing[0] if existing else Contact(key[0], name, kind)
            ct.key, ct.name, ct.kind, ct.path_len, ct.advert, ct.lat, ct.lon = key, name, kind, plen, adv, lat / 1e6, lon / 1e6
            if not existing:
                self.contacts.append(ct)
            self.nodes = [n for n in self.nodes if n.key != key]
            print("meshcore_sim: contact added/updated %s type %d" % (name, kind), flush=True)
            out(b"\x00")
        elif code == CMD_SET_OTHER_PARAMS:
            if a.old_other_params and len(c) != 4:
                out(bytes([1, 6]))
            elif len(c) < 4:
                out(bytes([1, 6]))
            else:
                self.manual_add, self.telemetry, self.loc_policy = bool(c[1]), c[2], c[3]
                if len(c) >= 5:
                    self.multi_acks = c[4]
                print("meshcore_sim: other params manual_add=%d telemetry=%d loc=%d multi_acks=%d (%d bytes)" %
                      (self.manual_add, self.telemetry, self.loc_policy, self.multi_acks, len(c)), flush=True)
                out(b"\x00")
        elif code == CMD_GET_AUTOADD:
            if a.no_autoadd:
                out(bytes([1, 1]))
            else:
                out(bytes([25, self.autoadd]) + (b"" if a.old_autoadd else bytes([0])))
        elif code == CMD_SET_AUTOADD:
            if a.no_autoadd or len(c) < 2:
                out(bytes([1, 1]))
            else:
                self.autoadd = c[1]
                print("meshcore_sim: autoadd config 0x%02x" % c[1], flush=True)
                out(b"\x00")
        elif code == CMD_SEND_CONTROL_DATA:
            if a.no_discover or len(c) < 7 or c[1] & 0xF0 != 0x80:
                out(bytes([1, 1]))
                return
            prefix_only, type_filter, tag = c[1] & 1, c[2], bytes(c[3:7])
            print("meshcore_sim: discover filter 0x%02x tag %s prefix_only=%d" % (type_filter, tag.hex(), prefix_only), flush=True)
            if a.discover_reply == "msgsent":
                out(bytes([6, 0, 1, 2, 3, 4]) + struct.pack("<I", 500))
            elif a.discover_reply == "ok":
                out(b"\x00")
            for i, (ct, snr, rssi) in enumerate(self.neighbours):
                if not (type_filter >> ct.kind) & 1:
                    continue
                key = ct.key[:8] if prefix_only else ct.key
                payload = bytes([0x90 | ct.kind, int(-2.0 * 4) & 0xFF]) + tag + key
                self.after(0.5 + 0.4 * i, lambda p=payload, s=snr, r=rssi: out(bytes([0x8E, int(s * 4) & 0xFF, r & 0xFF, 0]) + p))
        elif code == CMD_GET_STATS:
            if a.no_stats or len(c) < 2 or c[1] > 2:
                out(bytes([1, 1]))
            else:
                out(self.stats_frame(c[1]))
        elif code == CMD_REBOOT:
            if bytes(c[1:]) != b"reboot":
                out(bytes([1, 1]))
                return
            print("meshcore_sim: REBOOT", flush=True)
            self.restart()
        elif code == CMD_FACTORY_RESET:
            if a.no_factory_reset or (a.strict_reset_word and bytes(c[1:]) != b"reset"):
                out(bytes([1, 1]))
                return
            print("meshcore_sim: FACTORY RESET (frame %s)" % bytes(c).hex(), flush=True)
            self.contacts, self.channels = [], {}
            self.queue = []
            self.nodes, self.neighbours = [], []
            self.name = "MeshCore"
            self.reset_count += 1
            self.key = bytes((0x30 + self.reset_count * 7 + i * 5) & 0xFF for i in range(32))      # a new identity every time
            if a.reset_format_delay > 0:
                print("meshcore_sim: formatting the file system for %.1f s" % a.reset_format_delay, flush=True)
            def done():
                if not a.reset_no_ok:
                    out(b"\x00")
                self.restart(1.0 if not a.reset_no_ok else 0.0)
            if a.reset_format_delay > 0:
                self.after(a.reset_format_delay, done)
            else:
                done()
        elif code == CMD_SET_TXP:
            v = struct.unpack("<i", c[1:5])[0]
            if v > self.max_txp or v < -9:
                out(bytes([1, 6]))
            else:
                self.txp = v
                out(b"\x00")
        elif code == CMD_BATT:
            out(struct.pack("<BHII", 12, self.battery_mv, 120, 1024))
        elif code == CMD_GET_TIME:
            out(struct.pack("<BI", 9, self.board_time()))
        elif code == CMD_GET_CONTACTS:
            out(struct.pack("<BI", 2, len(self.contacts)))
            for i, ct in enumerate(self.contacts):
                out(ct.frame())
                if i == 1:
                    out(bytes([0x88, 0x30, 0xA0]) + os.urandom(40))     # a push inside the stream
            out(struct.pack("<BI", 4, int(time.time())))
        elif code == CMD_CONTACT_BY_KEY:
            for ct in self.contacts:
                if ct.key == bytes(c[1:33]):
                    out(ct.frame())
                    return
            out(bytes([1, 2]))
        elif code == CMD_GET_CUSTOM_VARS:
            if a.no_custom_vars or a.old_firmware:
                out(bytes([1, 1]))
            else:
                out(bytes([21]) + (b"radio.fem:1" if a.no_gps_var else ("gps:" + self.gps).encode()))
        elif code == CMD_SET_CUSTOM_VAR:
            kv = c[1:].decode("utf-8", "ignore")
            key, _, value = kv.partition(":")
            if a.no_custom_vars or a.old_firmware or key != "gps" or a.no_gps_var:
                out(bytes([1, 6]))
            else:
                self.gps = value
                out(b"\x00")
        elif code == CMD_GET_CHANNEL and a.old_firmware:
            out(bytes([1, 1]))
        elif code == CMD_GET_CHANNEL:
            idx = c[1]
            name, secret = self.channels.get(idx, ("", b"\0" * 16))
            out(bytes([18, idx]) + text_field(name, 32) + secret)
        elif code == CMD_SET_CHANNEL:
            idx = c[1]
            name = c[2:34].split(b"\0")[0].decode("utf-8", "ignore")
            self.channels[idx] = (name, bytes(c[34:50]))
            out(b"\x00")
        elif code == CMD_SYNC:
            out(self.queue.pop(0) if self.queue else bytes([10]))
        elif code == CMD_SEND_TXT:
            attempt = c[2]
            dst = bytes(c[7:13])
            text = c[13:].decode("utf-8", "ignore")
            if a.fail_send:
                out(bytes([1, 3]))
                return
            self.ack_counter += 1
            ack = struct.pack(">I", 0xA0B0C000 + self.ack_counter)
            out(bytes([6, 0]) + ack + struct.pack("<I", 2000))
            if not a.drop_acks:
                self.after(a.ack_delay, lambda: out(bytes([0x82]) + ack + struct.pack("<I", int(a.ack_delay * 1000))))
            if a.echo:
                for ct in self.contacts:
                    if ct.key[:6] == dst:
                        self.after(a.ack_delay + 1.0, lambda ct=ct, t=text: self.push_msg(out, self.dm(ct, "got: " + t)))
        elif code == CMD_SEND_CHAN:
            if a.chan_reply == "ok":
                out(b"\x00")
            elif a.chan_reply == "msgsent":
                out(bytes([6, 1, 1, 2, 3, 4]) + struct.pack("<I", 500))
            elif a.chan_reply == "odd":
                out(bytes([0x1F, 9, 9]))
            print("meshcore_sim: channel send %d bytes, replied %s" % (len(c), a.chan_reply), flush=True)
            if a.echo:
                text = c[7:].decode("utf-8", "ignore")
                self.after(1.0, lambda: self.push_msg(out, self.chan_msg(c[2], "Zed: echo " + text)))
        else:
            out(bytes([1, 1]))

    def push_msg(self, out, frame):
        self.queue.append(frame)
        out(bytes([0x83]))

    def restart(self, delay=0.3):
        """A reboot or a factory reset: the USB port goes away for a moment and comes back."""
        if self.port is None:
            return
        self.after(delay, self.port.unplug)
        self.after(delay + 2.2, self.port.plug)


class Port:
    """The pty and its symlink; plug() / unplug() simulate the USB cable."""

    def __init__(self, link, device=None):
        self.link, self.device, self.master, self.slave = link, device, None, None

    def plug(self):
        if self.device:           # an existing character device (one end of a socat pair): no pty, no symlink
            self.master = os.open(self.device, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(self.master)
            print(f"meshcore_sim: using {self.device}", flush=True)
            return
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        attrs = termios.tcgetattr(self.slave)
        attrs[3] &= ~termios.ECHO
        termios.tcsetattr(self.slave, termios.TCSANOW, attrs)
        name = os.ttyname(self.slave)
        try:
            os.unlink(self.link)
        except FileNotFoundError:
            pass
        os.symlink(name, self.link)
        print(f"meshcore_sim: port {self.link} -> {name}", flush=True)
        # no junk at plug time: the first bytes of a real board are pushes, sent from the main loop

    def unplug(self):
        for fd in (self.master, self.slave):
            if fd is not None:
                os.close(fd)
        self.master = self.slave = None
        if not self.device:
            try:
                os.unlink(self.link)
            except FileNotFoundError:
                pass
        print("meshcore_sim: unplugged", flush=True)

    def send(self, payload):
        if self.master is None:
            return
        os.write(self.master, b">" + struct.pack("<H", len(payload)) + payload)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--link", default="/tmp/ttyMC0", help="symlink to create for the pty (default /tmp/ttyMC0)")
    ap.add_argument("--device", help="use this existing character device instead of creating a pty (a socat pair end)")
    ap.add_argument("--chatter", type=float, default=0, metavar="SEC", help="every SEC seconds push a random message / log frame")
    ap.add_argument("--echo", action="store_true", help="contacts answer your messages a second after the ACK")
    ap.add_argument("--ack-delay", type=float, default=0.6, help="seconds before the ACK push (default 0.6)")
    ap.add_argument("--drop-acks", action="store_true", help="never send ACKs (exercises retries and 'no ack')")
    ap.add_argument("--fail-send", action="store_true", help="refuse every send with ERR_TABLE_FULL")
    ap.add_argument("--gps", action="store_true", help="the board reports custom variable gps:1 (a GPS receiver is enabled)")
    ap.add_argument("--no-custom-vars", action="store_true", help="GET_CUSTOM_VARS answers 'unsupported' (GPS state not reported)")
    ap.add_argument("--board-clock-offset", type=float, default=0.0, metavar="SEC",
                    help="the board clock is the host clock plus SEC (negative: behind, e.g. -13500000 = about 5 months; positive: ahead)")
    ap.add_argument("--refuse-set-time", action="store_true", help="refuse every SET_TIME with ERR_CODE_ILLEGAL_ARG (a time in the past is always refused)")
    ap.add_argument("--no-gps-var", action="store_true", help="the board lists custom variables but no gps (a board without a GPS receiver)")
    ap.add_argument("--old-firmware", action="store_true", help="no custom variables and no channel commands (a firmware too old for the new features)")
    ap.add_argument("--chan-reply", choices=["ok", "msgsent", "odd", "none"], default="ok",
                    help="the answer to a channel send: OK (default), MSG_SENT, an unexpected frame, or nothing at all")
    ap.add_argument("--contacts", type=int, default=0, metavar="N", help="N extra contacts of every kind and age (the list speed test)")
    ap.add_argument("--noise", action="store_true", help="print debug text on the port between frames (like some boards)")
    # phase 2 (0.2.0)
    ap.add_argument("--fw-level", type=int, default=11, help="the firmware level in DEVICE_INFO (default 11, like the user's board): 9+ reports repeat, 10+ the path hash mode")
    ap.add_argument("--manual-add", action="store_true", help="start in manual add mode: new nodes are pushed as pending (NEW_ADVERT) and not added")
    ap.add_argument("--max-contacts", type=int, default=350, help="the contact table size (ADD_UPDATE_CONTACT answers TABLE_FULL beyond it)")
    ap.add_argument("--new-nodes", type=int, default=0, metavar="N", help="N adverts from nodes the board does not know, one every 1.5 s from 3 s on")
    ap.add_argument("--repeat-ranges", default="433000-434000,863000-870000", help="allowed repeat frequencies in kHz (lo-hi,...), or none")
    ap.add_argument("--slow-remove", type=float, default=0, metavar="SEC", help="answer REMOVE_CONTACT only after SEC seconds (to watch the progress)")
    ap.add_argument("--old-other-params", action="store_true", help="SET_OTHER_PARAMS only in its 4 byte form (the app retries without multi_acks)")
    ap.add_argument("--no-autoadd", action="store_true", help="GET / SET_AUTOADD_CONFIG answer 'unsupported'")
    ap.add_argument("--old-autoadd", action="store_true", help="AUTOADD_CONFIG without the max_hops byte (before companion v1.14)")
    ap.add_argument("--no-discover", action="store_true", help="SEND_CONTROL_DATA answers 'unsupported'")
    ap.add_argument("--discover-reply", choices=["ok", "msgsent", "none"], default="ok", help="what the discover request itself is answered with")
    ap.add_argument("--no-stats", action="store_true", help="GET_STATS answers 'unsupported'")
    ap.add_argument("--old-stats", action="store_true", help="the packet statistics without the error counter (26 byte frame)")
    ap.add_argument("--no-factory-reset", action="store_true", help="FACTORY_RESET answers 'unsupported'")
    ap.add_argument("--identity", choices=["A", "B"], default="A", help="which board is on the cable at the start (B: another key, name, contacts and channels)")
    ap.add_argument("--reset-format-delay", type=float, default=0, metavar="SEC", help="the factory reset formats for SEC seconds before the OK frame (an ESP32 board is slow)")
    ap.add_argument("--reset-no-ok", action="store_true", help="the factory reset restarts the board without sending the OK frame")
    ap.add_argument("--strict-reset-word", action="store_true", help="FACTORY_RESET needs the word 'reset' behind the command byte (what I remember of the firmware)")
    args = ap.parse_args()
    if args.old_firmware:
        args.no_autoadd = args.no_stats = args.no_discover = True

    radio = Radio(args)
    if args.identity == "B":
        radio.swap_identity()
    port = Port(args.link, args.device)
    radio.port = port
    port.plug()
    buf = b""
    next_chatter = time.time() + args.chatter if args.chatter else None
    log_next = time.time() + 3
    stdin_ok = not sys.stdin.closed and (sys.stdin.isatty() or not os.isatty(0))
    stdin_open = True

    def out(payload):
        port.send(payload)

    radio.out = out

    # a real board pushes frames before the host says anything (an RX log of the traffic it hears)
    out(bytes([0x88, 0x28, 0xB0]) + os.urandom(40))

    while True:
        fds = []
        if port.master is not None:
            fds.append(port.master)
        if stdin_ok and stdin_open:
            fds.append(0)
        r, _, _ = select.select(fds, [], [], 0.1)
        now = time.time()
        if port.master is not None and port.master in r:
            try:
                data = os.read(port.master, 4096)
            except OSError as e:
                data = b""
                if e.errno not in (errno.EIO, errno.EAGAIN):
                    raise
            buf += data
            while True:
                i = buf.find(b"<")
                if i < 0:
                    buf = b""
                    break
                buf = buf[i:]
                if len(buf) < 3:
                    break
                n = struct.unpack("<H", buf[1:3])[0]
                if len(buf) < 3 + n:
                    break
                cmd, buf = buf[3:3 + n], buf[3 + n:]
                if cmd:
                    if args.noise:
                        os.write(port.master, b"[debug] cmd %d\r\n" % cmd[0])
                    radio.handle(cmd, out)
        if 0 in r:
            line = sys.stdin.readline()
            if not line:
                stdin_open = False
            else:
                parts = line.strip().split(" ", 2)
                if parts[0] == "msg" and len(parts) > 1:
                    radio.push_msg(out, radio.dm(radio.contacts[0], line.strip()[4:]))
                elif parts[0] == "chan" and len(parts) > 2:
                    radio.push_msg(out, radio.chan_msg(int(parts[1]), "Zed: " + parts[2]))
                elif parts[0] == "advert":
                    out(bytes([0x80]) + random.choice(radio.contacts).key)
                elif parts[0] == "newnode":
                    radio.new_node_advert(out)
                elif parts[0] == "reboot":
                    radio.restart()
                elif parts[0] == "swap":
                    radio.restart()
                    radio.after(0.4, radio.swap_identity)         # the other board is on the cable when the port comes back
                elif parts[0] == "unplug":
                    port.unplug()
                elif parts[0] == "plug":
                    port.plug()
                elif parts[0] == "quit":
                    break
        due = [t for t in radio.timers if t[0] <= now]
        radio.timers = [t for t in radio.timers if t[0] > now]
        for _, fn in due:
            fn()
        if now >= log_next:
            log_next = now + 3
            out(bytes([0x88, 0x28, 0xB0]) + os.urandom(random.randint(20, 60)))
        if next_chatter and now >= next_chatter:
            next_chatter = now + args.chatter
            if random.random() < 0.5:
                radio.push_msg(out, radio.chan_msg(0, "Zed: " + random.choice(["ping", "anyone here?", "weather is nice", "test 123"])))
            else:
                radio.push_msg(out, radio.dm(random.choice(radio.contacts[:2]), random.choice(["hi!", "are you there?", "lunch?"])))
    port.unplug()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
