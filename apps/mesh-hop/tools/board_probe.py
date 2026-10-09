#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-mostly serial probe of a MeshCore companion board (USB serial framing), for the Mesh Hop phase 3 board checks.

Python 3 standard library only (termios / os / select). Run it on the deck with Mesh Hop CLOSED:

    python3 board_probe.py channel-send [--index 0] [--yes]
    python3 board_probe.py log-echo [--seconds 120] [--send] [--index 0] [--yes]
    python3 board_probe.py flood-scope [--region '#test']
    python3 board_probe.py stats
    python3 board_probe.py factory-reset-support
    (global option: --port /dev/ttyACM0)

Framing (as in core/protocol.hpp): app -> board '<' + u16 LE length + payload; board -> app '>' + u16 LE length + payload.
Every frame sent and received is printed as hex with the decoded code name. The end of each subcommand prints PASS / NOTE lines.

Writes to the board: channel-send / log-echo --send send ONE text to the chosen channel (it goes on the air); flood-scope sets the
flood scope and ALWAYS restores it to null in a finally block. Nothing else is written. FACTORY_RESET is never sent. The queue of
waiting messages is never read (no SYNC_NEXT), so the probe does not take messages away from the app.
"""

import argparse
import hashlib
import os
import select
import struct
import sys
import termios
import time

CMD_APP_START, CMD_SEND_CHAN, CMD_DEVICE_QUERY, CMD_GET_CHANNEL = 1, 3, 22, 31
CMD_FACTORY_RESET, CMD_SET_FLOOD_SCOPE, CMD_GET_STATS = 51, 54, 56      # CMD_SET_FLOOD_SCOPE = 54: from memory of the firmware, see the protocol doc

CMD_NAMES = {1: "APP_START", 2: "SEND_TXT_MSG", 3: "SEND_CHANNEL_TXT_MSG", 22: "DEVICE_QUERY", 31: "GET_CHANNEL", 51: "FACTORY_RESET",
             54: "SET_FLOOD_SCOPE", 56: "GET_STATS"}
RESP_NAMES = {0: "RESP_OK", 1: "RESP_ERR", 2: "CONTACT_START", 3: "CONTACT", 4: "CONTACT_END", 5: "SELF_INFO", 6: "MSG_SENT",
              7: "CONTACT_MSG", 8: "CHANNEL_MSG", 9: "CURRENT_TIME", 10: "NO_MORE_MSGS", 12: "BATTERY", 13: "DEVICE_INFO",
              16: "CONTACT_MSG_V3", 17: "CHANNEL_MSG_V3", 18: "CHANNEL_INFO", 21: "CUSTOM_VARS", 24: "STATS", 25: "AUTOADD_CONFIG",
              26: "ALLOWED_REPEAT_FREQ", 27: "CHANNEL_DATA",
              0x80: "PUSH_ADVERT", 0x81: "PUSH_PATH_UPDATE", 0x82: "PUSH_SEND_CONFIRMED(ACK)", 0x83: "PUSH_MSG_WAITING",
              0x88: "PUSH_LOG_RX_DATA", 0x8A: "PUSH_NEW_ADVERT", 0x8E: "PUSH_CONTROL_DATA", 0x8F: "PUSH_CONTACT_DELETED",
              0x90: "PUSH_CONTACTS_FULL"}
ERR_NAMES = {1: "UNSUPPORTED_CMD", 2: "NOT_FOUND", 3: "TABLE_FULL", 4: "BAD_STATE", 5: "FILE_IO_ERROR", 6: "ILLEGAL_ARG"}
PAYLOAD_TYPES = {0: "REQ", 1: "RESPONSE", 2: "TXT_MSG", 3: "ACK", 4: "ADVERT", 5: "GRP_TXT", 6: "GRP_DATA", 7: "ANON_REQ", 8: "PATH",
                 9: "TRACE", 10: "MULTIPART", 11: "CONTROL", 15: "RAW_CUSTOM"}
ROUTE_TYPES = {0: "TRANSPORT_FLOOD", 1: "FLOOD", 2: "DIRECT", 3: "TRANSPORT_DIRECT"}

PUBLIC_SECRET = bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")
T0 = time.time()


def stamp():
    return "%7.2f" % (time.time() - T0)


def say(msg):
    print(msg, flush=True)


def verdict(kind, msg):
    say("%s: %s" % (kind, msg))


# ------------------------------------------------------------------ port

def port_users(path):
    """pids (other than ours) that have the port open, best effort from /proc."""
    real = os.path.realpath(path)
    found = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            for fd in os.listdir("/proc/%s/fd" % pid):
                try:
                    if os.path.realpath("/proc/%s/fd/%s" % (pid, fd)) == real:
                        with open("/proc/%s/cmdline" % pid, "rb") as f:
                            found.append((int(pid), f.read().replace(b"\0", b" ").decode("utf-8", "replace").strip()))
                        break
                except OSError:
                    continue
        except OSError:
            continue
    return found


class Link:
    def __init__(self, path, check_busy=True):
        self.path = path
        users = port_users(path) if check_busy else []
        if users:
            for pid, cmd in users:
                say("ERROR: %s is in use by pid %d (%s). Quit Mesh Hop (hold Esc 3 s) and try again." % (path, pid, cmd))
            sys.exit(2)
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            say("ERROR: cannot open %s: %s" % (path, e))
            sys.exit(2)
        try:
            import fcntl
            fcntl.ioctl(self.fd, termios.TIOCEXCL)          # a second opener (the app) now fails instead of sharing the stream
        except Exception:
            pass
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = b""
        self.noise = 0
        self.pushes = []

    def close(self):
        try:
            import fcntl
            fcntl.ioctl(self.fd, termios.TIOCNXCL)
        except Exception:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass

    def send(self, payload, note=""):
        frame = b"<" + struct.pack("<H", len(payload)) + payload
        code = payload[0]
        say("[%s] TX %-24s %s%s" % (stamp(), CMD_NAMES.get(code, "cmd %d" % code), payload.hex(" "), ("   " + note) if note else ""))
        os.write(self.fd, frame)

    def _pump(self, wait):
        r, _, _ = select.select([self.fd], [], [], max(0.0, wait))
        if r:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                data = b""
            self.buf += data

    def _next_frame(self):
        while True:
            i = self.buf.find(b">")
            if i < 0:
                self.noise += len(self.buf)
                self.buf = b""
                return None
            if i:
                self.noise += i
                self.buf = self.buf[i:]
            if len(self.buf) < 3:
                return None
            n = struct.unpack("<H", self.buf[1:3])[0]
            if n == 0 or n > 600:                         # not a plausible frame: skip this '>' as noise
                self.noise += 1
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < 3 + n:
                return None
            frame, self.buf = self.buf[3:3 + n], self.buf[3 + n:]
            return frame

    def frames(self, seconds):
        """Yield every frame that arrives within `seconds` (printing each one)."""
        end = time.time() + seconds
        while True:
            f = self._next_frame()
            if f is not None:
                show_rx(f)
                yield f
                continue
            left = end - time.time()
            if left <= 0:
                return
            self._pump(min(left, 0.2))

    def request(self, payload, timeout=5.0, note=""):
        """Send a command and return (answer frame or None, pushes seen meanwhile). The answer is the first frame with code < 0x80."""
        self.send(payload, note)
        pushes = []
        for f in self.frames(timeout):
            if f[0] >= 0x80:
                pushes.append(f)
                self.pushes.append(f)
                continue
            return f, pushes
        return None, pushes


def decode_hint(f):
    code = f[0]
    try:
        if code == 1 and len(f) >= 2:
            return "error %d (%s)" % (f[1], ERR_NAMES.get(f[1], "?"))
        if code == 6 and len(f) >= 10:
            return "type %d (1=flood 0=direct), expected_ack %s, suggested_timeout %d ms" % (f[1], f[2:6].hex(), struct.unpack("<I", f[6:10])[0])
        if code == 13 and len(f) >= 80:
            return "fw_level %d, max_contacts %d, max_channels %d, build %r, model %r, version %r" % (
                f[1], f[2] * 2, f[3], cstr(f[8:20]), cstr(f[20:60]), cstr(f[60:80]))
        if code == 18 and len(f) >= 50:
            return "channel %d name %r secret_hash(first byte of sha256) %02x" % (f[1], cstr(f[2:34]), hashlib.sha256(f[34:50]).digest()[0])
        if code == 5 and len(f) >= 58:
            return "name %r" % f[58:].split(b"\0")[0].decode("utf-8", "replace")
        if code == 0x88:
            return decode_log(f)
        if code == 0x82 and len(f) >= 5:
            return "ack code %s" % f[1:5].hex()
        if code == 24 and len(f) >= 2:
            return decode_stats(f)
    except Exception as e:                                # a decode hint never breaks the probe
        return "(decode failed: %s)" % e
    return ""


def cstr(b):
    return bytes(b).split(b"\0")[0].decode("utf-8", "replace")


def parse_log(f):
    """PUSH_LOG_RX_DATA: snr*4 (i8), rssi (i8), then the raw packet. Returns a dict or None."""
    if len(f) < 4:
        return None
    snr = struct.unpack("b", f[1:2])[0] / 4.0
    rssi = struct.unpack("b", f[2:3])[0]
    raw = f[3:]
    if len(raw) < 2:
        return {"snr": snr, "rssi": rssi, "raw": raw, "ok": False}
    hdr = raw[0]
    route, ptype = hdr & 3, (hdr >> 2) & 0xF
    pos = 1 + (4 if route in (0, 3) else 0)
    if len(raw) <= pos:
        return {"snr": snr, "rssi": rssi, "raw": raw, "ok": False}
    plb = raw[pos]
    hops, hsz = plb & 63, (plb >> 6) + 1
    start = pos + 1 + hops * hsz
    if start > len(raw) or hsz > 3:
        return {"snr": snr, "rssi": rssi, "raw": raw, "ok": False, "route": route, "ptype": ptype}
    return {"snr": snr, "rssi": rssi, "raw": raw, "ok": True, "route": route, "ptype": ptype, "hops": hops, "hsz": hsz,
            "path": raw[pos + 1:start], "payload": raw[start:]}


def decode_log(f):
    p = parse_log(f)
    if p is None:
        return "(too short)"
    s = "snr %.2f rssi %d" % (p["snr"], p["rssi"])
    if not p["ok"]:
        return s + ", raw packet not readable"
    s += ", %s %s, hops %d (hash %d byte), payload %d bytes" % (ROUTE_TYPES.get(p["route"], "?"), PAYLOAD_TYPES.get(p["ptype"], "type %d" % p["ptype"]),
                                                              p["hops"], p["hsz"], len(p["payload"]))
    if p["ptype"] == 5 and p["payload"]:
        s += ", channel hash %02x" % p["payload"][0]
    return s


def decode_stats(f):
    sub = f[1]
    if sub == 0 and len(f) >= 11:
        _, _, mv, up, errs, q = struct.unpack("<BBHIHB", f[:11])
        return "core: battery %d mV, uptime %d s, errors %d, queue %d" % (mv, up, errs, q)
    if sub == 1 and len(f) >= 14:
        _, _, noise, rssi, snr4, tx, rx = struct.unpack("<BBhbbII", f[:14])
        return "radio: noise floor %d dBm, last rssi %d, last snr %.2f, tx air %d s, rx air %d s" % (noise, rssi, snr4 / 4.0, tx, rx)
    if sub == 2 and len(f) >= 26:
        v = struct.unpack("<IIIIII", f[2:26])
        s = "packets: recv %d sent %d flood_tx %d direct_tx %d flood_rx %d direct_rx %d" % v
        if len(f) >= 30:
            s += " recv_errors %d" % struct.unpack("<I", f[26:30])[0]
        return s
    return "stats sub type %d, %d bytes (shorter or different than expected)" % (sub, len(f))


def show_rx(f):
    code = f[0]
    hint = decode_hint(f)
    say("[%s] RX %-24s %s%s" % (stamp(), RESP_NAMES.get(code, "code 0x%02x (unknown)" % code), f.hex(" "), ("   -> " + hint) if hint else ""))


# ------------------------------------------------------------------ shared steps

def handshake(link):
    """DEVICE_QUERY + APP_START (the board needs APP_START before most commands). Returns the DEVICE_INFO frame or None."""
    info, _ = link.request(bytes([CMD_DEVICE_QUERY, 3]))
    if info is None or info[0] != 13:
        verdict("NOTE", "no DEVICE_INFO answer to DEVICE_QUERY (is this a companion board on the right port, at 115200?)")
    me, _ = link.request(bytes([CMD_APP_START, 3]) + b"      " + b"board-probe")
    if me is None or me[0] != 5:
        verdict("NOTE", "no SELF_INFO answer to APP_START")
    return info


def channel_info(link, idx):
    f, _ = link.request(bytes([CMD_GET_CHANNEL, idx]))
    if f is not None and f[0] == 18 and len(f) >= 50:
        return cstr(f[2:34]), bytes(f[34:50])
    return None, None


def do_channel_send(link, idx, text):
    """Returns (answer frame, pushes during the wait, send time)."""
    payload = bytes([CMD_SEND_CHAN, 0, idx]) + struct.pack("<I", int(time.time())) + text.encode("utf-8")
    t_send = time.time()
    ans, pushes = link.request(payload, timeout=5.0, note="(text %r)" % text)
    return ans, pushes, t_send


def classify_send(ans):
    if ans is None:
        verdict("NOTE", "no answer within 5 s to SEND_CHANNEL_TXT_MSG")
    elif ans[0] == 0:
        verdict("PASS", "the board answered RESP_OK (00) to the channel send: channel sends are confirmed by a bare OK, no MSG_SENT / ack code")
    elif ans[0] == 6:
        verdict("PASS", "the board answered MSG_SENT (06): %s" % decode_hint(ans))
    elif ans[0] == 1:
        verdict("NOTE", "the board REFUSED the channel send: %s" % decode_hint(ans))
    else:
        verdict("NOTE", "UNEXPECTED answer to the channel send, code 0x%02x: %s" % (ans[0], ans.hex(" ")))


def confirm_send(args, idx, text):
    say("")
    say("This will transmit ONE text on channel %d over the air:  %r" % (idx, text))
    if args.yes:
        return True
    try:
        return input("Type y to send, anything else to abort: ").strip().lower() == "y"
    except EOFError:
        return False


def test_text():
    return "mesh-hop board probe %s (test, please ignore)" % time.strftime("%H:%M:%S")


# ------------------------------------------------------------------ subcommands

def cmd_channel_send(link, args):
    handshake(link)
    name, secret = channel_info(link, args.index)
    if name is None:
        verdict("NOTE", "GET_CHANNEL %d gave no CHANNEL_INFO (empty slot or old firmware); sending anyway" % args.index)
    text = test_text()
    if not confirm_send(args, args.index, text):
        verdict("NOTE", "aborted, nothing was sent")
        return
    ans, pushes, _ = do_channel_send(link, args.index, text)
    classify_send(ans)
    say("--- listening %d s for pushes after the send ---" % args.wait)
    n = 0
    for f in link.frames(args.wait):
        n += 1
    say("pushes / frames in the %d s after the answer: %d" % (args.wait, n + len(pushes)))
    verdict("NOTE", "frames that arrived before the answer (pushes): %d" % len(pushes))


def cmd_log_echo(link, args):
    handshake(link)
    secret = None
    t_send = None
    if args.send:
        name, secret = channel_info(link, args.index)
        text = test_text()
        if not confirm_send(args, args.index, text):
            verdict("NOTE", "aborted, nothing was sent")
            return
        ans, pre_pushes, t_send = do_channel_send(link, args.index, text)
        classify_send(ans)
    else:
        if args.index is not None:
            name, secret = channel_info(link, args.index)
    chash = hashlib.sha256(secret).digest()[0] if secret else None
    if chash is not None:
        say("channel %d hash byte (sha256 of the secret, first byte): %02x - GRP_TXT packets with this hash belong to that channel" % (args.index, chash))
    say("--- listening %d s (Ctrl+C stops early) ---" % args.seconds)
    logs, others = [], []
    try:
        for f in link.frames(args.seconds):
            if f[0] == 0x88:
                p = parse_log(f)
                logs.append((time.time(), p))
            else:
                others.append(f)
    except KeyboardInterrupt:
        say("(stopped early)")
    grp = [(t, p) for t, p in logs if p and p.get("ok") and p["ptype"] == 5 and (chash is None or p["payload"][:1] == bytes([chash]))]
    say("")
    say("summary: %d LOG_RX_DATA frames, %d other pushes, %d GRP_TXT%s" % (
        len(logs), len(others), len(grp), (" on channel hash %02x" % chash) if chash is not None else ""))
    if logs:
        verdict("PASS", "the board pushes PUSH_LOG_RX_DATA (0x88) frames, with a readable raw packet in %d of %d" % (
            sum(1 for _, p in logs if p and p.get("ok")), len(logs)))
    else:
        verdict("NOTE", "no 0x88 frame at all in %d s (a quiet band, or this firmware does not push the RX log to the app)" % args.seconds)
    if args.send and t_send is not None:
        by_payload = {}
        for t, p in grp:
            by_payload.setdefault(bytes(p["payload"]), []).append((t - t_send, p))
        echoes = [(k, v) for k, v in by_payload.items() if v[0][0] >= -0.5]
        for k, v in echoes:
            say("GRP_TXT payload %s...: heard %d time(s) after the send, at %s s, hops %s" % (
                k[:8].hex(), len(v), ", ".join("%.1f" % x[0] for x in v), ", ".join(str(x[1]["hops"]) for x in v)))
        if echoes:
            verdict("PASS", "%d distinct GRP_TXT payload(s) on the channel were logged after our send. Our own message is among them if the "
                            "payload was heard with hops >= 1 within a few seconds; other people's messages look the same, so check the "
                            "list above on a quiet channel (cannot decrypt here, no crypto in the standard library)" % len(echoes))
        else:
            verdict("NOTE", "NO channel GRP_TXT packet was logged after the send: either no repeater is in range, or the board does not "
                            "log echoes of its own message")
    else:
        say("(no --send: run again with --send to correlate echoes with our own message)")


def scope_key(region):
    if not region:
        return b"\0" * 16
    return hashlib.sha256(region.encode("utf-8")).digest()[:16]


def set_scope(link, key, label):
    f, _ = link.request(bytes([CMD_SET_FLOOD_SCOPE, 0]) + key, note="(%s, key %s)" % (label, key.hex()))
    return f


def cmd_flood_scope(link, args):
    handshake(link)
    results = []
    try:
        f = set_scope(link, b"\0" * 16, "null key")
        results.append(("null key", f))
        key = scope_key(args.region)
        f = set_scope(link, key, "region %r = sha256(name)[:16]" % args.region)
        results.append(("region key", f))
    finally:
        say("--- restoring the flood scope to null (always) ---")
        f = set_scope(link, b"\0" * 16, "restore null key")
        results.append(("restore", f))
    say("")
    for label, f in results:
        if f is None:
            verdict("NOTE", "%s: no answer within 5 s" % label)
        elif f[0] == 0:
            verdict("PASS", "%s: RESP_OK" % label)
        elif f[0] == 1:
            verdict("NOTE", "%s: refused, %s" % (label, decode_hint(f)))
        else:
            verdict("NOTE", "%s: unexpected code 0x%02x" % (label, f[0]))
    if results[-1][1] is None or results[-1][1][0] != 0:
        verdict("NOTE", "the RESTORE was not confirmed with OK: power-cycle the board (the scope is held in RAM on the firmware as far as known) and tell us")
    verdict("NOTE", "this probe cannot see whether the scope changes what goes on the air; it only shows that the board accepts the command")


def cmd_stats(link, args):
    handshake(link)
    ok = 0
    for sub in (0, 1, 2):
        f, _ = link.request(bytes([CMD_GET_STATS, sub]))
        if f is None:
            verdict("NOTE", "stats sub type %d: no answer within 5 s" % sub)
        elif f[0] == 24:
            ok += 1
            verdict("PASS", "stats sub type %d: %s" % (sub, decode_stats(f)))
        elif f[0] == 1:
            verdict("NOTE", "stats sub type %d: %s" % (sub, decode_hint(f)))
        else:
            verdict("NOTE", "stats sub type %d: unexpected code 0x%02x" % (sub, f[0]))
    verdict("PASS" if ok == 3 else "NOTE", "GET_STATS answered %d of 3 sub types" % ok)


def cmd_factory_reset_support(link, args):
    info = handshake(link)
    say("")
    if info is not None and info[0] == 13 and len(info) >= 80:
        verdict("PASS", "device info: %s" % decode_hint(info))
    say("FACTORY_RESET is NOT sent by this probe. The only way to know whether the board accepts it is to send it (the board then wipes")
    say("its keys, contacts and channels), and the reference client and the firmware disagree on the frame (bare 33 versus 33 'reset').")
    verdict("NOTE", "from the device info alone we cannot tell if FACTORY_RESET is supported; the real reset test is a separate step, on a board "
                    "whose identity can be thrown away (see the protocol document)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial port (default /dev/ttyACM0)")
    ap.add_argument("--no-busy-check", action="store_true", help="skip the check that no other process has the port (only for the simulator, which holds its own pty open)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("channel-send", help="send one test text to a channel and show the answer and the pushes")
    p.add_argument("--index", type=int, default=0, help="channel index (default 0 = public)")
    p.add_argument("--wait", type=int, default=20, help="seconds to listen after the send (default 20)")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation before sending")
    p = sub.add_parser("log-echo", help="listen for LOG_RX_DATA (0x88) and other pushes; --send also sends one channel text")
    p.add_argument("--seconds", type=int, default=120, help="listening time (default 120)")
    p.add_argument("--send", action="store_true", help="send one test text first, then listen, and look for echoes")
    p.add_argument("--index", type=int, default=0, help="channel index for --send (default 0 = public)")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation before sending")
    p = sub.add_parser("flood-scope", help="SET_FLOOD_SCOPE null, then a region key, then null again (always)")
    p.add_argument("--region", default="#test", help="region name the key is derived from (default '#test')")
    sub.add_parser("stats", help="GET_STATS sub types 0, 1, 2")
    sub.add_parser("factory-reset-support", help="what the device info says; never sends FACTORY_RESET")
    args = ap.parse_args()

    link = Link(args.port, not args.no_busy_check)
    say("port %s opened at 115200 8N1 (raw); no other process has it open" % args.port)
    try:
        {"channel-send": cmd_channel_send, "log-echo": cmd_log_echo, "flood-scope": cmd_flood_scope, "stats": cmd_stats,
         "factory-reset-support": cmd_factory_reset_support}[args.cmd](link, args)
    except KeyboardInterrupt:
        say("(interrupted)")
    finally:
        if link.noise:
            say("(%d bytes of non-frame text were skipped on the port)" % link.noise)
        link.close()


if __name__ == "__main__":
    main()
