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

    # ---- frames the radio sends
    def dm(self, c, text, ts=None):
        return (bytes([16, 0x16, 0, 0]) + c.key[:6] + bytes([0xFF, 0]) + struct.pack("<I", ts or int(time.time())) + text.encode())

    def chan_msg(self, idx, text, ts=None):
        return bytes([17, 0x0C, 0, 0, idx, 0x01, 0]) + struct.pack("<I", ts or int(time.time())) + text.encode()

    def self_info(self):
        name = self.name.encode()
        return (bytes([5, 1, self.txp & 0xFF, self.max_txp]) + self.key + struct.pack("<ii", 48856600, 2352200) + bytes([0, 0, 0, 0])
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
            out(REAL_DEVICE_INFO)
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
            if sf < 5 or sf > 12 or cr < 5 or cr > 8:
                out(bytes([1, 6]))
            else:
                self.freq_khz, self.bw_hz, self.sf, self.cr = f, b, sf, cr
                out(b"\x00")
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
    args = ap.parse_args()

    radio = Radio(args)
    port = Port(args.link, args.device)
    port.plug()
    buf = b""
    next_chatter = time.time() + args.chatter if args.chatter else None
    log_next = time.time() + 3
    stdin_ok = not sys.stdin.closed and (sys.stdin.isatty() or not os.isatty(0))
    stdin_open = True

    def out(payload):
        port.send(payload)

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
