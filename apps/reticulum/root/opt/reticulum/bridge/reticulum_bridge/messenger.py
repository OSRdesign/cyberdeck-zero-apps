# SPDX-License-Identifier: MIT
"""Contacts, the heard list and direct messages (phase P2) on top of the bridge's LXMF router.

Rules (owner decisions for P2):
* direct messages only: LXMF's DIRECT method (a link to the contact), no propagation node, stamps off;
* states of an outgoing message: sending, sent (only when a relay took it; direct delivery never passes through it),
  delivered, failed. After the library's bounded automatic tries a message ends failed; there is no automatic resend,
  ever (not when the contact is heard again, not at restart: what was sending then is marked failed); only the retry
  verb sends it again;
* nothing transmits unless the user acts AND a connection is allowed to send: a send, a retry and an announce are
  refused by the bridge itself (code `no_transmit`) when no interface is enabled, online and outgoing, and when the
  path to the contact is known to run over an interface that may not transmit;
* contacts and messages are local only: no delete, rename or read mark is ever sent to the other side;
* the message text is never logged.
"""
import collections
import json
import os
import re
import threading
import time

from . import BridgeError, store as store_mod
from .alerts import Alerter

ADDRESS_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_MAX = 64
TEXT_MAX_BYTES = 8192            # sanity cap of the bridge (a request line is at most 64 KiB); not the 287-byte rule
PREVIEW = 80
HISTORY_DEFAULT, HISTORY_MAX = 50, 200
POLL = 2.0                       # seconds between looks at the state of messages that are on their way
SETTINGS_DEFAULTS = {"alerts": False}


def iface_name(iface):
    """The configured interface behind an RNS interface object (a TCP server's connections name their parent)."""
    if iface is None:
        return None
    return getattr(getattr(iface, "parent_interface", None) or iface, "name", None)


def norm_address(value, field="address"):
    """Strict: a string of exactly 32 hex digits (surrounding blanks are dropped, case is normalised to lower)."""
    if not isinstance(value, str) or not ADDRESS_RE.match(value.strip().lower()):
        raise BridgeError("invalid", "%s: 32 hexadecimal digits (0-9, a-f) expected" % field, field=field)
    return value.strip().lower()


def check_name(value, field="name"):
    """None for no name; else 1-64 characters without control characters."""
    if value is None or value == "":
        return None
    if not isinstance(value, str) or not value.strip() or len(value.strip()) > NAME_MAX or any(
            ord(ch) < 32 or ord(ch) == 127 for ch in value):
        raise BridgeError("invalid", "%s: 1-%d characters, no control characters" % (field, NAME_MAX), field=field)
    return value.strip()


def check_text(value):
    if not isinstance(value, str) or not value.strip():
        raise BridgeError("invalid", "text: a message cannot be empty", field="text")
    value = value.replace("\r\n", "\n").replace("\r", "\n")
    if any((ord(ch) < 32 and ch not in "\n\t") or ord(ch) == 127 for ch in value):
        raise BridgeError("invalid", "text: control characters are not allowed", field="text")
    try:
        n = len(value.encode("utf-8"))
    except UnicodeEncodeError:
        raise BridgeError("invalid", "text: not valid Unicode text", field="text")
    if n > TEXT_MAX_BYTES:
        raise BridgeError("invalid", "text: at most %d bytes (this one has %d)" % (TEXT_MAX_BYTES, n), field="text",
                          limit=TEXT_MAX_BYTES, bytes=n)
    return value


def route_ok(senders, iface_name, iface_type):
    """May a packet to a destination whose next hop is this interface leave? (report 057 8.1: `outgoing = False`
    silences announces and path requests, but a packet to a known path still goes out, so the bridge checks.)
    When this program is a client of someone else's shared instance the path leads through the local socket and the
    owner of the interfaces decides: nothing to check here."""
    if iface_name is None or iface_type == "LocalClientInterface":
        return True
    return iface_name in senders


class Settings:
    """A small JSON file in the data folder (0600, atomic rewrite)."""

    def __init__(self, path, log):
        self.path, self.log = path, log
        self.values = dict(SETTINGS_DEFAULTS)
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            for k in SETTINGS_DEFAULTS:
                if isinstance(data.get(k), bool):
                    self.values[k] = data[k]
        except FileNotFoundError:
            pass
        except (OSError, ValueError, AttributeError) as e:
            log("settings file unreadable (%s): defaults used" % e)

    def get(self):
        return dict(self.values)

    def update(self, changes):
        for k, v in changes.items():
            if k not in SETTINGS_DEFAULTS:
                raise BridgeError("invalid", "unknown setting %r (known: %s)" % (k, ", ".join(SETTINGS_DEFAULTS)),
                                  field=k)
            if not isinstance(v, bool):
                raise BridgeError("invalid", "%s must be true or false" % k, field=k)
        new = dict(self.values, **changes)
        tmp = self.path + ".tmp"
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(new, f)
                f.write("\n")
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self.path)
        except OSError as e:
            raise BridgeError("config_error", "cannot write %s: %s" % (self.path, e.strerror))
        self.values = new
        return self.get()


class Messenger:
    def __init__(self, bridge, db, settings, alerter=None):
        self.b = bridge
        self.RNS, self.LXMF = bridge.RNS, bridge.LXMF
        self.db, self.settings = db, settings
        self.alerter = alerter or Alerter(bridge_log(bridge))
        self.own = bridge.dest.hash.hex()
        self.heard = collections.OrderedDict()               # address -> entry, newest last
        self.inflight = {}                                   # message id -> (address, LXMessage) on its way
        self.waiting = {}                                    # message id -> lookup of a contact not heard yet
        self.cond = threading.Condition()
        self.closing = False
        self.thread = None
        for e in db.load_heard():
            self.heard[e["address"]] = e
        n = db.fail_interrupted()
        if n:
            bridge_log(bridge)("%d message(s) were still sending when the last run ended: marked failed" % n)
        bridge.router.register_delivery_callback(self.on_delivery)

    # --- helpers ---
    def post(self, name, data):
        if self.b.server:
            self.b.server.post_event(name, data, topic="messages")

    def name_of(self, address):
        c = self.db.contact(address)
        if c and c["name"]:
            return c["name"]
        h = self.heard.get(address)
        return h["name"] if h and h.get("name") else None

    def connections_changed(self):
        """After an interface verb: when no interface is left that may transmit, what is still on its way is
        cancelled (the user took the permission back) and fails with detail `no_transmit`."""
        try:
            self.b.senders(need_online=False)
            return
        except BridgeError:
            pass
        with self.cond:
            items = list(self.inflight.items())
            self.inflight.clear()
            waiting = list(self.waiting)
            self.waiting.clear()
        for rid, (addr, lxm) in items:
            try:
                self.b.router.cancel_outbound(lxm.message_id)
            except Exception as e:
                self.b.log("cancel_outbound: %s" % e)
            self._set_state(rid, "failed", "no_transmit")
        for rid in waiting:
            self._set_state(rid, "failed", "no_transmit")
        if items or waiting:
            self.b.log("%d message(s) stopped: no connection may send any more" % (len(items) + len(waiting)))

    def state_changed(self):
        try:
            self.b.refresh_state()
        except Exception as e:
            self.b.log("state file: %s" % e)

    def close(self):
        with self.cond:
            self.closing = True
            self.cond.notify_all()
        self.db.close()

    def tick(self):
        self.db.flush()

    # --- heard ---
    def on_heard(self, ev):
        """From the bridge's announce handler (an RNS thread): only LXMF delivery announces, never our own."""
        addr = ev["destination"]
        if ev.get("kind") != "lxmf" or addr == self.own:
            return
        old = self.heard.pop(addr, None)
        name = ev.get("name") or (old or {}).get("name")
        e = {"address": addr, "name": name, "time": ev["time"], "hops": ev.get("hops"), "interface": ev.get("interface")}
        self.heard[addr] = e
        while len(self.heard) > self.db.heard_max:
            self.heard.popitem(last=False)
        self.db.put_heard(e)
        if self.waiting:
            with self.cond:
                self.cond.notify_all()                         # a message may have been waiting for this key
        if self.b.server:                                      # on the announces.subscribe stream, like `announce`
            self.b.server.post_event("heard", dict(e, contact=self.db.contact(addr) is not None))

    def v_heard_list(self, p, c):
        limit = p.get("limit", 200)
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= store_mod.HEARD_MAX:
            raise BridgeError("invalid", "limit: 1-%d" % store_mod.HEARD_MAX, field="limit")
        contacts = {x["address"] for x in self.db.contacts()}
        rows = sorted(self.heard.values(), key=lambda e: -e["time"])[:limit]
        return {"count": len(self.heard),
                "heard": [dict(e, contact=e["address"] in contacts) for e in rows]}

    # --- contacts ---
    def v_contacts_list(self, p, c):
        out = []
        unread = {x["address"]: x["unread"] for x in self.db.conversations(1000)}
        for x in self.db.contacts():
            h = self.heard.get(x["address"])
            out.append(dict(x, unread=unread.get(x["address"], 0), last_heard=h["time"] if h else None,
                            heard_name=h["name"] if h else None))
        return {"contacts": out}

    def v_contacts_add(self, p, c):
        addr = norm_address(p.get("address"))
        if addr == self.own:
            raise BridgeError("invalid", "address: that is your own address", field="address")
        name = check_name(p.get("name"))
        if self.db.contact(addr):
            raise BridgeError("exists", "this address is already a contact")
        if name is None:                                      # added from the Heard list: take its name when it has one
            h = self.heard.get(addr)
            name = h["name"] if h else None
        self.db.add_contact(addr, name)
        self.b.log("contact added: %s" % addr)
        self.post("contacts_changed", {"address": addr, "action": "added"})
        return dict(self.db.contact(addr), unread=self.db.unread_of(addr))

    def v_contacts_rename(self, p, c):
        addr = norm_address(p.get("address"))
        if not self.db.contact(addr):
            raise BridgeError("not_found", "no such contact")
        self.db.rename_contact(addr, check_name(p.get("name")))
        self.post("contacts_changed", {"address": addr, "action": "renamed"})
        return self.db.contact(addr)

    def v_contacts_remove(self, p, c):
        addr = norm_address(p.get("address"))
        keep = p.get("keep_conversation", True)
        if not isinstance(keep, bool):
            raise BridgeError("invalid", "keep_conversation must be true or false", field="keep_conversation")
        if not self.db.contact(addr):
            raise BridgeError("not_found", "no such contact")
        self.db.remove_contact(addr)
        deleted = 0
        if not keep:
            deleted = self._delete_conversation(addr)
        self.b.log("contact removed: %s" % addr)
        self.post("contacts_changed", {"address": addr, "action": "removed"})
        return {"removed": addr, "conversation_kept": keep, "messages_deleted": deleted}

    # --- messages ---
    def _conv_row(self, g):
        m = g["last"]
        preview = " ".join(m["text"].split())
        if len(preview) > PREVIEW:
            preview = preview[:PREVIEW - 1] + "…"
        c = self.db.contact(g["address"])
        return {"address": g["address"], "name": self.name_of(g["address"]), "contact": c is not None,
                "count": g["count"], "unread": g["unread"], "preview": preview, "last_time": m["time"],
                "last_dir": m["dir"], "last_state": m["state"], "last_id": m["id"]}

    def v_conversations(self, p, c):
        rows = [self._conv_row(g) for g in self.db.conversations(200)]
        return {"conversations": rows, "unread_total": self.db.unread_total()}

    def v_history(self, p, c):
        addr = norm_address(p.get("address"))
        limit = p.get("limit", HISTORY_DEFAULT)
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= HISTORY_MAX:
            raise BridgeError("invalid", "limit: 1-%d" % HISTORY_MAX, field="limit")
        before = p.get("before")
        if before is not None and (not isinstance(before, int) or isinstance(before, bool) or before < 1):
            raise BridgeError("invalid", "before: a message id", field="before")
        rows, more = self.db.history(addr, limit, before)
        return {"address": addr, "name": self.name_of(addr), "contact": self.db.contact(addr) is not None,
                "messages": rows, "has_more": more}

    def check_may_send(self, addr):
        senders = self.b.senders()                            # raises no_transmit
        iface = self.RNS.Transport.next_hop_interface(bytes.fromhex(addr))
        name = iface_name(iface)
        if not route_ok(senders, name, type(iface).__name__ if iface is not None else None):
            raise BridgeError("no_transmit", "the way to this contact leads over %r, which may not transmit; allow it "
                              "to transmit first (iface.outgoing)" % name, interface=name)

    def v_send(self, p, c):
        addr = norm_address(p.get("address"))
        text = check_text(p.get("text"))
        self.check_may_send(addr)
        row = self.db.add_message(addr, "out", text, "sending")
        self.b.log("message %d to %s queued (%d bytes)" % (row["id"], addr, row["bytes"]))
        self.post("message", dict(row, name=self.name_of(addr), contact=self.db.contact(addr) is not None,
                                  unread_total=self.db.unread_total()))
        self._submit(row["id"], addr, text)
        return self.db.message(row["id"])

    def v_retry(self, p, c):
        rid = p.get("id")
        if not isinstance(rid, int) or isinstance(rid, bool):
            raise BridgeError("invalid", "id: a message id", field="id")
        m = self.db.message(rid)
        if not m:
            raise BridgeError("not_found", "no such message")
        if m["dir"] != "out" or m["state"] != "failed":
            raise BridgeError("invalid", "only a failed message that you sent can be sent again (this one is %s)" %
                              m["state"], field="id")
        self.check_may_send(m["address"])
        self.db.reset_for_retry(rid)
        self._submit(rid, m["address"], m["text"])
        row = self.db.message(rid)
        self.post("message_state", {"id": rid, "address": m["address"], "state": "sending", "detail": None})
        return row

    def v_delete_conversation(self, p, c):
        addr = norm_address(p.get("address"))
        n = self._delete_conversation(addr)
        return {"address": addr, "messages_deleted": n}

    def _delete_conversation(self, addr):
        with self.cond:
            for rid in [r for r, w in self.waiting.items() if w["address"] == addr]:
                self.waiting.pop(rid)
            for rid, (a, lxm) in list(self.inflight.items()):
                if a == addr:
                    self.inflight.pop(rid)
                    try:
                        self.b.router.cancel_outbound(lxm.message_id)       # local only: stops our own tries
                    except Exception as e:
                        self.b.log("cancel_outbound: %s" % e)
        n = self.db.delete_conversation(addr)
        self.post("conversation_deleted", {"address": addr, "unread_total": self.db.unread_total()})
        self.state_changed()
        return n

    def v_mark_read(self, p, c):
        addr = norm_address(p.get("address"))
        n = self.db.mark_read(addr)
        total = self.db.unread_total()
        if n:
            self.post("unread", {"address": addr, "unread": 0, "unread_total": total})
            self.state_changed()
        return {"address": addr, "marked": n, "unread_total": total}

    def v_limits(self, p, c):
        M = self.LXMF.LXMessage
        return {"single_packet_bytes": M.ENCRYPTED_PACKET_MAX_CONTENT, "link_packet_bytes": M.LINK_PACKET_MAX_CONTENT,
                "max_text_bytes": TEXT_MAX_BYTES, "overhead_bytes": M.LXMF_OVERHEAD,
                "per_conversation": self.db.per_conv, "total": self.db.cap_total,
                "delivery_attempts": self.LXMF.LXMRouter.MAX_DELIVERY_ATTEMPTS,
                "note": "up to single_packet_bytes of text travel in one packet; longer text is sent as a link "
                        "transfer (slower on radio), never blocked below max_text_bytes"}

    def v_settings_get(self, p, c):
        return self.settings.get()

    def v_settings_set(self, p, c):
        return self.settings.update(p)

    # --- sending ---
    def _submit(self, rid, addr, text):
        """Hands the message to LXMF; when the contact's key is not known yet (typed address, never heard) the bridge
        first asks the network for the path, every DELIVERY_RETRY_WAIT seconds up to MAX_DELIVERY_ATTEMPTS times (the
        router's own bounds), and then the message fails with detail `no_path`."""
        try:
            if not self._hand_over(rid, addr, text):
                with self.cond:
                    self.waiting[rid] = {"address": addr, "text": text, "tries": 0, "next": 0.0}
                self.b.log("message %d: %s is not known yet, asking for a path" % (rid, addr))
        except Exception as e:
            with self.cond:
                self.inflight.pop(rid, None)
                self.waiting.pop(rid, None)
            self.b.log("message %d could not be handed to LXMF: %s: %s" % (rid, type(e).__name__, e))
            self._set_state(rid, "failed", "internal")
            raise BridgeError("internal", "the message could not be handed to LXMF (see the bridge log)")
        self._start_poller()

    def _hand_over(self, rid, addr, text):
        """True when LXMF has the message; False when the contact's key is unknown (nothing was sent)."""
        RNS, LXMF = self.RNS, self.LXMF
        h = bytes.fromhex(addr)
        ident = RNS.Identity.recall(h)
        if ident is None:
            return False
        iface = RNS.Transport.next_hop_interface(h)
        if iface is not None and getattr(iface, "online", True) is False:
            # The known way runs over an interface that is gone (a TCP client of ours reconnected: its connection is a
            # new interface). LXMF would keep using the dead path and fail after its tries, so ask for a new one.
            try:
                self.b.rns.drop_path(h)                      # gone at once (expire_path only marks it)
            except Exception:
                RNS.Transport.expire_path(h)
            self.b.log("path to %s ran over an interface that is gone: dropped, asking again" % addr)
            return False
        dest = RNS.Destination(ident, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
        lxm = LXMF.LXMessage(dest, self.b.dest, text, desired_method=LXMF.LXMessage.DIRECT)
        lxm.register_delivery_callback(lambda m, rid=rid: self._finished(rid, "delivered"))
        lxm.register_failed_callback(lambda m, rid=rid: self._finished(rid, "failed"))
        with self.cond:
            self.inflight[rid] = (addr, lxm)
        self.b.router.handle_outbound(lxm)
        return True

    def _look_for_waiting(self):
        R = self.LXMF.LXMRouter
        now = time.monotonic()
        with self.cond:
            todo = list(self.waiting.items())
        for rid, w in todo:
            try:
                if self._hand_over(rid, w["address"], w["text"]):
                    with self.cond:
                        self.waiting.pop(rid, None)
                    continue
                if now < w["next"]:
                    continue
                if w["tries"] >= R.MAX_DELIVERY_ATTEMPTS:
                    with self.cond:
                        self.waiting.pop(rid, None)
                    self._set_state(rid, "failed", "no_path")
                    continue
                w["tries"] += 1
                w["next"] = now + R.DELIVERY_RETRY_WAIT
                self.b.senders()                             # still allowed to send? (raises no_transmit)
                self.RNS.Transport.request_path(bytes.fromhex(w["address"]))   # allowed: checked when it was queued
            except BridgeError as e:
                with self.cond:
                    self.waiting.pop(rid, None)
                self._set_state(rid, "failed", e.code)
            except Exception as e:
                self.b.log("message %d lookup: %s: %s" % (rid, type(e).__name__, e))
                with self.cond:
                    self.waiting.pop(rid, None)
                self._set_state(rid, "failed", "internal")

    def _detail(self, lxm):
        M = self.LXMF.LXMessage
        return {M.REJECTED: "rejected", M.CANCELLED: "cancelled"}.get(lxm.state, "undelivered")

    def _finished(self, rid, state):
        with self.cond:
            entry = self.inflight.pop(rid, None)
        if entry is None:
            return                                            # deleted meanwhile
        detail = None if state == "delivered" else self._detail(entry[1])
        self._set_state(rid, state, detail)

    def _set_state(self, rid, state, detail=None):
        if self.closing:
            return
        if not self.db.set_state(rid, state, detail):
            return
        m = self.db.message(rid)
        if m:
            self.post("message_state", {"id": rid, "address": m["address"], "state": state, "detail": detail})

    def _start_poller(self):
        with self.cond:
            if self.thread is None or not self.thread.is_alive():
                self.thread = threading.Thread(target=self._poll, name="msg-state", daemon=True)
                self.thread.start()
            self.cond.notify_all()

    def _poll(self):
        """Looks at the messages on their way every 2 s (and sleeps when there is none): LXMF's callbacks cover
        delivered and failed, the state of the message object covers `sent` and a message it gave up on silently."""
        M = self.LXMF.LXMessage
        while True:
            with self.cond:
                while not self.inflight and not self.waiting and not self.closing:
                    self.cond.wait()
                if self.closing:
                    return
                items = list(self.inflight.items())
            if self.waiting:
                self._look_for_waiting()
            for rid, (addr, lxm) in items:
                st = lxm.state
                if st == M.SENT:
                    self._set_state(rid, "sent")
                elif st == M.DELIVERED:
                    self._finished(rid, "delivered")
                elif st in (M.FAILED, M.REJECTED, M.CANCELLED):
                    self._finished(rid, "failed")
            with self.cond:
                self.cond.wait(POLL)
                if self.closing:
                    return

    # --- receiving ---
    def on_delivery(self, lxm):
        """LXMF delivery callback (a router or RNS thread)."""
        try:
            addr = lxm.source_hash.hex()
            if addr == self.own:
                return
            text = lxm.content_as_string() or ""
            if not text.strip():
                if not lxm.fields:
                    return
                text = "[attachment not shown]"
            if len(text.encode("utf-8")) > TEXT_MAX_BYTES:
                text = text.encode("utf-8")[:TEXT_MAX_BYTES].decode("utf-8", "ignore") + "…"
            text = "".join(ch for ch in text if ch in "\n\t" or (ord(ch) >= 32 and ord(ch) != 127))
            row = self.db.add_message(addr, "in", text, "received", unread=True)
        except Exception as e:
            self.b.log("incoming message dropped: %s: %s" % (type(e).__name__, e))
            return
        name = self.name_of(addr)
        total = self.db.unread_total()
        self.b.log("message %d from %s (%d bytes)" % (row["id"], addr, row["bytes"]))
        self.post("message", dict(row, name=name, contact=self.db.contact(addr) is not None, unread_total=total))
        self.state_changed()
        if self.settings.values.get("alerts"):
            self.alerter.alert(name or "Anonymous " + addr[:8])


def bridge_log(bridge):
    return bridge.log
