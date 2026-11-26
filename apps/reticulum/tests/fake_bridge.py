#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A fake of the bridge for the app's scenes and walk-throughs: no RNS, no radio, no network.

It speaks protocol 1 (docs/dev/reticulum-bridge.md): JSON lines on a Unix socket, the same verbs, the same error codes.
The connection types and the checks of option values are the real bridge's own (reticulum_bridge.options), so an
out-of-range value is refused with the real words. Everything else is canned data kept in memory, one independent world
per socket, so every size of a scene runs in its own.

    python3 fake_bridge.py --base DIR --names 640x480,480x320 [--scenario NAME] [--log FILE] [--delay S] [--appear-after S]

serves DIR/<name>/bridge.sock for every name. Every request is appended to FILE as one JSON line
({"name", "verb", "params"}); the run_scenes.sh checks read it (nothing may ask to transmit unless a scene says so).

Scenarios (the connections the world starts with):
  chat        phase P2: a LoRa radio on and ALLOWED to send, three contacts (Anna with 2 unread, Marek, Camp base that is
              offline: its messages fail and a retry delivers), four people heard, conversations with every message state
  chatblocked the same world with the radio on but listen only: sending, retrying and announcing are refused (no_transmit)
  chatempty   phase P2 with nothing yet: no contacts, no messages, nobody heard (the radio is on and may send)
  fresh       the first configuration: one Wi-Fi / LAN connection, off
  mixed       Wi-Fi / LAN off; a LoRa radio on and listen only (port, channel busy and noise reported), a server on the
              internet with a problem; two people heard
  listenonly  Wi-Fi / LAN off; four connections that open a link, all on and listen only: Internet client "Home server",
              Internet server, Fast internet link, I2P (each shows the listen-only note under its page)
  radio       Wi-Fi / LAN off; a LoRa radio off with only its port set: every radio field "not set"
  busyport    a complete LoRa radio, off, whose port a program already uses (switching it on says so)

The numbers a scenario uses for a radio (SYNTHETIC, picked inside the ranges the bridge accepts, not a real band plan or a
recommendation) are in SYNTH_RADIO below.

--delay S      every answer waits S seconds (a slow bridge)
--deliver-after S   a message you send changes from sending to delivered (or failed) after S seconds (default 0.7)
--heard-after S   (scenario chat) S seconds after a client subscribes to announces, Camp base is heard: the bridge's `heard`
                  event with contact: true (the app's copy of contacts.list says "not heard yet" until then)
--appear-after S   the sockets do not exist for S seconds (a service that is still starting)
Stops on SIGTERM or when the file DIR/stop appears.
"""
import argparse
import calendar
import datetime
import json
import os
import re
import signal
import socketserver
import sys
import threading
import time

sys.dont_write_bytecode = True    # nothing is written into the source tree
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bridge"))
from reticulum_bridge import BridgeError, options  # noqa: E402

# SYNTHETIC placeholders for the "mixed" radio: not a band plan, not advice. Chosen only to be inside the bridge's ranges.
SYNTH_RADIO = {"frequency": "200000000", "bandwidth": "100000", "txpower": "10", "spreadingfactor": "7", "codingrate": "5"}
ADDRESS = "329a5b7e0d1f4c8a9b2e6d3071c2c109"
ANNA = "8f694d83c5e1b2a09f3d7c6e1a2b3c4d"
MAREK = "b2c4e6f8a0a1b3c5d7e9f0a2b4c6d8e0"
CAMP = "c0ffee11223344556677889900aabbcc"
TOM = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
ANON = "7be1d2c3b4a5968778695a4b3c2d04aa"
KIM = "aabbccdd00112233aabbccdd00112233"
SINGLE_PACKET = 287
MAX_TEXT = 8192
BUSY_PORT = "/dev/ttyBUSY0"


def new_iface(name, typ, enabled=False, outgoing=False, opts=None, online=False, stats=None, error=None):
    return {"name": name, "type": typ, "enabled": enabled, "outgoing": outgoing, "online": online, "stats": stats,
            "listen_only": options.listen_only_of(typ), "options": dict(opts or {}), "secrets_set": [], "subsections": [], "supported": True, "lock": None,
            "error": error}


class World:
    def __init__(self, scenario, log, name):
        self.scenario, self.log_file, self.name = scenario, log, name
        self.lock = threading.Lock()
        self.display_name = None
        self.stopped = False
        self.started = time.time()
        self.subscribers = []
        self.msg_subs = []
        self.deliver_after = 0.7
        self.heard_after, self.heard_sent = 0.0, False
        self.contacts, self.messages, self.heard = [], [], []
        self.next_id = 1
        self.settings = {"alerts": False}
        self.ifaces = [new_iface("Default Interface", "AutoInterface")]
        self.notices = []
        now = time.time()
        self.midnight = calendar.timegm(datetime.datetime.now(datetime.timezone.utc).date().timetuple())
        if scenario == "notice":     # an older version left Wi-Fi LAN on and listen only: the service switched it off at start
            self.notices = [{"kind": "listen_only_removed", "name": "Default Interface", "type": "AutoInterface", "time": now}]
        self.recent = []
        if scenario in ("chat", "chatblocked", "chatempty"):
            self.ifaces.append(new_iface(
                "LoRa radio", "RNodeInterface", enabled=True, online=True, outgoing=(scenario != "chatblocked"),
                opts=dict(port="/dev/ttyACM0", **SYNTH_RADIO),
                stats={"status": True, "channel_load_short": 3.0, "noise_floor": -112, "rssi": -90, "snr": 7}))
            self.deck_chat(scenario, now)
            self.recent = [{"time": h["time"], "destination": h["address"], "identity": None, "kind": "lxmf", "name": h["name"],
                            "hops": h["hops"], "interface": h["interface"]} for h in self.heard]
        if scenario == "mixed":
            self.ifaces.append(new_iface(
                "LoRa radio", "RNodeInterface", enabled=True, online=True,
                opts=dict(port="/dev/ttyACM0", **SYNTH_RADIO),
                stats={"status": True, "channel_load_short": 3.0, "noise_floor": -112, "rssi": -90, "snr": 7}))
            self.ifaces.append(new_iface("Home server", "TCPClientInterface", enabled=True, outgoing=True,
                                         opts={"target_host": "server.example.invalid", "target_port": "4242"},
                                         error="could not connect to server.example.invalid:4242"))
            self.recent = [
                {"time": now - 125, "destination": "aabbccdd00112233aabbccdd00112233", "identity": None,
                 "kind": "lxmf", "name": "Kim", "hops": 1, "interface": "LoRa radio"},
                {"time": now - 7500, "destination": "11223344556677881122334455667788", "identity": None,
                 "kind": "nomadnet_node", "name": None, "hops": 3, "interface": "LoRa radio"}]
        elif scenario == "listenonly":
            for nm, typ, o in (("Home server", "TCPClientInterface", {"target_host": "server.example.invalid", "target_port": "4242"}),
                               ("Internet server", "TCPServerInterface", {"listen_ip": "0.0.0.0", "listen_port": "4242"}),
                               ("Fast link", "BackboneInterface", {"target_host": "server.example.invalid", "target_port": "4243"}),
                               ("I2P", "I2PInterface", {})):
                self.ifaces.append(new_iface(nm, typ, enabled=True, online=True, outgoing=False, opts=o))
        elif scenario in ("radio", "busyport"):
            if scenario == "busyport":
                opts = dict(port=BUSY_PORT, **SYNTH_RADIO)   # complete, so that switching on reaches the busy port
            else:
                opts = {"port": "/dev/ttyACM0"}
            self.ifaces.append(new_iface("LoRa radio", "RNodeInterface", opts=opts))
        for i in self.ifaces:
            self.refresh(i)

    # ---- the chat world (phase P2)
    def deck_chat(self, scenario, now):
        # midnight UTC: the headless harness runs the app with TZ=UTC, so the clock times of a scene do not depend on this PC
        midnight = calendar.timegm(datetime.datetime.now(datetime.timezone.utc).date().timetuple())
        if scenario == "chatempty":
            return
        self.display_name = "Alex"
        self.contacts = [
            {"address": ANNA, "name": "Anna", "added": now - 86400 * 9, "last_heard": now - 600, "heard_name": "Anna"},
            {"address": MAREK, "name": "Marek", "added": now - 86400 * 8, "last_heard": now - 300, "heard_name": "Marek"},
            {"address": CAMP, "name": "Camp base", "added": now - 86400 * 7, "last_heard": 0, "heard_name": None}]
        self.heard = [
            {"address": ANNA, "name": "Anna", "time": now - 600, "hops": 1, "interface": "LoRa radio", "contact": True},
            {"address": TOM, "name": "Radio Tom", "time": now - 125, "hops": 1, "interface": "LoRa radio", "contact": False},
            {"address": MAREK, "name": "Marek", "time": now - 300, "hops": 2, "interface": "LoRa radio", "contact": True},
            {"address": ANON, "name": None, "time": now - 840, "hops": 2, "interface": "Default Interface", "contact": False},
            {"address": KIM, "name": "Kim", "time": now - 3000, "hops": 3, "interface": "LoRa radio", "contact": False}]
        def add(addr, dirn, t, text, state, unread=False, detail=None):
            self.messages.append({"id": self.next_id, "address": addr, "dir": dirn, "time": t, "text": text, "state": state,
                                  "detail": detail, "unread": unread, "bytes": len(text.encode()), "delivered_at": None})
            self.next_id += 1
        add(ANNA, "in", midnight + 12 * 3600 + 30 * 60, "Are you at the camp yet?", "received")
        add(ANNA, "out", midnight + 12 * 3600 + 34 * 60, "Yes, I arrived 10 minutes ago.", "delivered")
        add(ANNA, "in", midnight + 12 * 3600 + 40 * 60, "Good. Bring the antenna, please.", "received", unread=True)
        add(ANNA, "in", midnight + 12 * 3600 + 41 * 60, "And the long cable.", "received", unread=True)
        add(CAMP, "out", midnight + 11 * 3600 + 2 * 60, "Arrived, all fine.", "delivered")
        add(MAREK, "in", midnight - 2 * 86400 + 10 * 3600, "Can you check the battery?", "received")
        add(MAREK, "out", midnight - 2 * 86400 + 10 * 3600 + 900, "Battery is low, back soon.", "failed",
            detail="no_transmit" if scenario == "chatblocked" else "undelivered")   # the blocked world: it was cancelled for that

    def may_send(self):
        return any(i["enabled"] and i["online"] and i["outgoing"] and i["error"] is None for i in self.ifaces)

    def need_send(self):
        if not self.may_send():
            raise BridgeError("no_transmit", "no interface that is up may transmit: switch one on and allow it to transmit first "
                              "(iface.outgoing)")

    def schedule_heard(self):
        """--heard-after: Camp base (never heard) is heard once, as the bridge's `heard` event on the announce stream."""
        if self.heard_after <= 0 or self.heard_sent or not self.contact(CAMP):
            return
        self.heard_sent = True

        def run():
            with self.lock:
                t = time.time()
                self.heard.append({"address": CAMP, "name": "Camp base", "time": t, "hops": 2, "interface": "LoRa radio",
                                   "contact": True})
                self.contact(CAMP).update(last_heard=t, heard_name="Camp base")
                for c in list(self.subscribers):
                    c.send({"event": "heard", "data": {"address": CAMP, "name": "Camp base", "time": t, "hops": 2,
                                                       "interface": "LoRa radio", "contact": True}})
        threading.Timer(self.heard_after, run).start()

    def push(self, event, data):
        for c in list(self.msg_subs):
            c.send({"event": event, "data": data})

    def unread_total(self):
        return sum(1 for m in self.messages if m["unread"])

    def contact(self, address):
        for c in self.contacts:
            if c["address"] == address:
                return c
        return None

    def heard_of(self, address):
        for h in self.heard:
            if h["address"] == address:
                return h
        return None

    def name_of(self, address):
        c = self.contact(address)
        if c:
            return c["name"] or c["heard_name"]
        h = self.heard_of(address)
        return h["name"] if h else None

    def check_address(self, a):
        if not isinstance(a, str):
            raise BridgeError("invalid", "address: 32 hexadecimal digits (0-9, a-f) expected", field="address")
        a = a.strip().lower()
        if not re.fullmatch(r"[0-9a-f]{32}", a):
            raise BridgeError("invalid", "address: 32 hexadecimal digits (0-9, a-f) expected", field="address")
        if a == ADDRESS:
            raise BridgeError("invalid", "address: that is your own address", field="address")
        return a

    def contact_out(self, c):
        return {"address": c["address"], "name": c["name"], "added": c["added"],
                "unread": sum(1 for m in self.messages if m["address"] == c["address"] and m["unread"]),
                "last_heard": c["last_heard"], "heard_name": c["heard_name"]}

    def v_contacts_list(self, p):
        return {"contacts": [self.contact_out(c) for c in sorted(self.contacts, key=lambda c: (c["name"] is None, (c["name"] or "").lower()))]}

    def v_contacts_add(self, p):
        a = self.check_address(p.get("address"))
        name = p.get("name")
        if name is not None:
            if not isinstance(name, str) or not name.strip() or len(name.strip()) > 64 or any(ord(ch) < 32 for ch in name):
                raise BridgeError("invalid", "name: 1-64 characters, no control characters", field="name")
            name = name.strip()
        if self.contact(a):
            raise BridgeError("exists", "that address is already a contact", field="address")
        h = self.heard_of(a)
        c = {"address": a, "name": name or (h["name"] if h else None), "added": time.time(),
             "last_heard": h["time"] if h else 0, "heard_name": h["name"] if h else None}
        self.contacts.append(c)
        if h:
            h["contact"] = True
        self.push("contacts_changed", {"address": a, "action": "added"})
        return self.contact_out(c)

    def v_contacts_rename(self, p):
        c = self.contact(self.check_address(p.get("address")))
        if not c:
            raise BridgeError("not_found", "no such contact")
        n = p.get("name")
        if n in (None, ""):
            c["name"] = None
        else:
            if not isinstance(n, str) or not n.strip() or len(n.strip()) > 64 or any(ord(ch) < 32 for ch in n):
                raise BridgeError("invalid", "name: 1-64 characters, no control characters", field="name")
            c["name"] = n.strip()
        self.push("contacts_changed", {"address": c["address"], "action": "renamed"})
        return self.contact_out(c)

    def v_contacts_remove(self, p):
        a = self.check_address(p.get("address"))
        c = self.contact(a)
        if not c:
            raise BridgeError("not_found", "no such contact")
        keep = p.get("keep_conversation", True)
        self.contacts.remove(c)
        gone = 0
        if not keep:
            before = len(self.messages)
            self.messages = [m for m in self.messages if m["address"] != a]
            gone = before - len(self.messages)
        h = self.heard_of(a)
        if h:
            h["contact"] = False
        self.push("contacts_changed", {"address": a, "action": "removed"})
        return {"removed": a, "conversation_kept": bool(keep), "messages_deleted": gone}

    def v_heard_list(self, p):
        lim = int(p.get("limit", 200))
        rows = sorted(self.heard, key=lambda h: -h["time"])
        return {"count": len(rows), "heard": rows[:lim]}

    def v_conversations(self, p):
        out = []
        for a in {m["address"] for m in self.messages}:
            ms = [m for m in self.messages if m["address"] == a]
            last = ms[-1]
            c = self.contact(a)
            out.append({"address": a, "name": self.name_of(a), "contact": c is not None, "count": len(ms),
                        "unread": sum(1 for m in ms if m["unread"]), "preview": last["text"][:80], "last_time": last["time"],
                        "last_dir": last["dir"], "last_state": last["state"], "last_detail": last["detail"],
                        "last_id": last["id"]})
        out.sort(key=lambda c: -c["last_time"])
        return {"conversations": out[:200], "unread_total": self.unread_total()}

    def v_history(self, p):
        a = self.check_address(p.get("address"))
        lim = max(1, min(200, int(p.get("limit", 50))))
        before = p.get("before")
        ms = [m for m in self.messages if m["address"] == a and (before is None or m["id"] < before)]
        more = len(ms) > lim
        return {"address": a, "name": self.name_of(a), "contact": self.contact(a) is not None, "messages": ms[-lim:], "has_more": more}

    def v_send(self, p):
        a = self.check_address(p.get("address"))
        text = p.get("text")
        if not isinstance(text, str) or not text.strip() or any(ord(ch) < 32 for ch in text):
            raise BridgeError("invalid", "text: a message needs text and no control characters", field="text")
        if len(text.encode()) > MAX_TEXT:
            raise BridgeError("invalid", "text: too long", field="text", limit=MAX_TEXT)
        self.need_send()
        # A fixed clock, 12:35 UTC plus one second per message: the seeded conversations are dated 12:30-12:41 of today, so a
        # message sent by a scene must not jump ahead of them with the PC's real time of day (the scenes open "the first
        # conversation" and found a different one after 12:41 UTC) and the shots read the same at any hour.
        m = {"id": self.next_id, "address": a, "dir": "out", "time": self.midnight + 12 * 3600 + 35 * 60 + self.next_id,
             "text": text, "state": "sending",
             "detail": None, "unread": False, "bytes": len(text.encode()), "delivered_at": None}
        self.next_id += 1
        self.messages.append(m)
        self.push("message", dict(m, name=self.name_of(a), contact=self.contact(a) is not None, unread_total=self.unread_total()))
        self.settle(m)
        return dict(m)

    def settle(self, m):
        """After deliver_after seconds the message is delivered, or fails for the offline contact (a retry delivers)."""
        def run():
            with self.lock:
                if m not in self.messages or m["state"] != "sending":
                    return
                offline = m["address"] == CAMP and not m.get("retried")
                if offline:
                    m["state"], m["detail"] = "failed", "undelivered"
                else:
                    m["state"], m["detail"] = "delivered", None
                    # a fixed clock like the send time: delivered 20 s after the send, or 13 minutes after it for a retry (the
                    # contact came back), so "Delivered 12:48" proves the line shows the delivery, not the send (12:35)
                    m["delivered_at"] = m["time"] + (13 * 60 if m.get("retried") else 20)
                self.push("message_state", {"id": m["id"], "address": m["address"], "state": m["state"], "detail": m["detail"],
                                            "delivered_at": m["delivered_at"]})
        threading.Timer(self.deliver_after, run).start()

    def v_retry(self, p):
        mid = p.get("id")
        m = next((x for x in self.messages if x["id"] == mid), None)
        if m is None:
            raise BridgeError("not_found", "no such message")
        if m["dir"] != "out" or m["state"] != "failed":
            raise BridgeError("invalid", "id: only a failed message you sent can be sent again", field="id")
        self.need_send()
        m["state"], m["detail"], m["retried"], m["delivered_at"] = "sending", None, True, None
        self.push("message_state", {"id": m["id"], "address": m["address"], "state": "sending", "detail": None,
                                    "delivered_at": None})
        self.settle(m)
        return {k: v for k, v in m.items() if k != "retried"}

    def v_delete_conv(self, p):
        a = self.check_address(p.get("address"))
        before = len(self.messages)
        self.messages = [m for m in self.messages if m["address"] != a]
        self.push("conversation_deleted", {"address": a, "unread_total": self.unread_total()})
        return {"address": a, "messages_deleted": before - len(self.messages)}

    def v_mark_read(self, p):
        a = self.check_address(p.get("address"))
        n = 0
        for m in self.messages:
            if m["address"] == a and m["unread"]:
                m["unread"] = False
                n += 1
        if n:
            self.push("unread", {"address": a, "unread": 0, "unread_total": self.unread_total()})
        return {"address": a, "marked": n, "unread_total": self.unread_total()}

    def v_limits(self, p):
        return {"single_packet_bytes": SINGLE_PACKET, "link_packet_bytes": 319, "max_text_bytes": MAX_TEXT, "overhead_bytes": 112,
                "per_conversation": 2000, "total": 20000, "delivery_attempts": 5, "note": "fake"}

    def v_msg_subscribe(self, p):
        return {"subscribed": True, "unread_total": self.unread_total()}

    def v_settings_get(self, p):
        return dict(self.settings)

    def v_settings_set(self, p):
        for k, v in p.items():
            if k != "alerts" or not isinstance(v, bool):
                raise BridgeError("invalid", "settings: only alerts (true or false) can be set", field=k)
        self.settings.update(p)
        return dict(self.settings)

    # ---- helpers
    def refresh(self, i):
        t = i["type"]
        i["supported"] = t in options.TYPES and t not in options.UNSUPPORTED
        i["missing"] = options.missing_required(t, i["options"])
        if i["enabled"] and not i.get("forced_offline") and i["error"] is None:
            i["online"] = True
        if not i["enabled"]:
            i["online"] = False

    def find(self, name):
        for i in self.ifaces:
            if i["name"] == name:
                return i
        raise BridgeError("not_found", "no interface named %r" % name)

    def entry(self, i):
        d = {k: v for k, v in i.items() if k != "forced_offline"}
        if i["stats"] is None and i["online"]:
            d["stats"] = {"status": True}
        if not i["online"]:
            d["stats"] = None
        return d

    # ---- verbs
    def v_hello(self, p):
        return {"bridge": "fake", "protocol": 1, "rns": "fake", "lxmf": "fake", "verbs": sorted(self.verbs())}

    def v_status(self, p):
        return {"pid": os.getpid(), "state": "up", "bridge": "fake", "protocol": 1, "rns": "fake", "lxmf": "fake",
                "mode": "master", "transport": False, "config_dir": "/fake", "identity_hash": "00" * 16,
                "lxmf_address": ADDRESS, "display_name": self.display_name, "interfaces_total": len(self.ifaces),
                "interfaces_up": [i["name"] for i in self.ifaces if i["online"]], "uptime": int(time.time() - self.started),
                "unread": self.unread_total(), "alerts": self.settings["alerts"]}

    def v_ifaces(self, p):
        return {"interfaces": [self.entry(i) for i in self.ifaces]}

    def v_types(self, p):
        return options.type_list()

    def v_add(self, p):
        name, typ = options.check_name(p.get("name")), p.get("type")
        values = options.validate(typ, p.get("options") or {})
        options.check_pairs(typ, values)
        if any(i["name"] == name for i in self.ifaces):
            raise BridgeError("exists", "an interface named %r exists" % name)
        # D8: written off, never on, listen only
        i = new_iface(name, typ, enabled=False, outgoing=False, opts={k: v for k, v in values.items() if v is not None})
        self.ifaces.append(i)
        self.refresh(i)
        self.changed()
        return self.entry(i)

    def v_edit(self, p):
        i = self.find(p.get("name"))
        values = options.validate(i["type"], p.get("options") or {})
        merged = dict(i["options"])
        for k, v in values.items():
            if v is None:
                merged.pop(k, None)
            else:
                merged[k] = v
        options.check_pairs(i["type"], merged)
        i["options"] = merged
        self.refresh(i)
        self.changed()
        return dict(self.entry(i), reloaded=True if i["enabled"] else None)

    def v_remove(self, p):
        i = self.find(p.get("name"))
        self.ifaces.remove(i)
        self.changed()
        return {"removed": i["name"]}

    def v_enable(self, p):
        i = self.find(p.get("name"))
        want = p.get("enabled", True) is not False
        if want:
            if i["missing"]:
                raise BridgeError("incomplete", "set these options first: %s" % ", ".join(i["missing"]), missing=i["missing"])
            if i["options"].get("port") == BUSY_PORT:
                raise BridgeError("busy", "%s is used by mesh-hop. Close that program first." % BUSY_PORT, port=BUSY_PORT,
                                  holders=[{"pid": 4242, "name": "mesh-hop"}])
            if i["listen_only"] == "none":      # the bridge's rule: no listen-only state, so the caller must say the user agreed
                if p.get("allow_sending") is not True:
                    raise BridgeError("confirm", "this kind of connection sends as soon as it is on: send allow_sending: true",
                                      field="allow_sending")
                i["outgoing"] = True
            i["enabled"] = True
            i["error"] = None
        else:
            i["enabled"] = False
        self.refresh(i)
        self.changed()
        return self.entry(i)

    def v_outgoing(self, p):
        i = self.find(p.get("name"))
        if not isinstance(p.get("outgoing"), bool):
            raise BridgeError("invalid", "outgoing: true or false expected", field="outgoing")
        if not p["outgoing"] and i["listen_only"] == "none":
            raise BridgeError("invalid", "%s has no listen-only state: switch it off instead" % i["type"], field="outgoing")
        i["outgoing"] = p["outgoing"]
        self.changed()
        return dict(self.entry(i), reloaded=True if i["enabled"] else None)

    def v_subscribe(self, p):
        return {"subscribed": True, "recent": list(self.recent)}

    def v_paths(self, p):
        return {"count": 0, "paths": []}

    def v_announce(self, p):
        # only the user's confirmation asks for this: the checks fail the run unless the scene says how many to expect
        if not self.display_name:
            raise BridgeError("no_display_name", "set a display name first")
        self.need_send()
        return {"announced": True, "lxmf_address": ADDRESS, "display_name": self.display_name,
                "via": [i["name"] for i in self.ifaces if i["enabled"] and i["online"] and i["outgoing"]]}

    def v_identity(self, p):
        return {"identity_hash": "00" * 16, "lxmf_address": ADDRESS, "display_name": self.display_name,
                "identity_file": "/fake/identity"}

    def v_name(self, p):
        if "name" in p:
            n = p["name"]
            if n in (None, ""):
                self.display_name = None
            else:
                if not isinstance(n, str) or not n.strip() or len(n.strip()) > 64 or any(ord(c) < 32 for c in n):
                    raise BridgeError("invalid", "name: 1-64 characters, no control characters", field="name")
                self.display_name = n.strip()
        return {"display_name": self.display_name}

    def v_notices(self, p):
        out, self.notices = self.notices, []
        return {"notices": out}

    def v_stop(self, p):
        self.stopped = True
        return {"stopping": True}

    def verbs(self):
        return {"hello": self.v_hello, "status": self.v_status, "ifaces": self.v_ifaces, "iface.types": self.v_types,
                "iface.add": self.v_add, "iface.edit": self.v_edit, "iface.remove": self.v_remove,
                "iface.enable": self.v_enable, "iface.outgoing": self.v_outgoing, "announces.subscribe": self.v_subscribe,
                "paths": self.v_paths, "announce": self.v_announce, "identity.info": self.v_identity,
                "identity.name": self.v_name, "service.stop": self.v_stop, "notices.take": self.v_notices,
                "contacts.list": self.v_contacts_list, "contacts.add": self.v_contacts_add, "contacts.rename": self.v_contacts_rename,
                "contacts.remove": self.v_contacts_remove, "heard.list": self.v_heard_list,
                "messages.conversations": self.v_conversations, "messages.history": self.v_history, "messages.send": self.v_send,
                "messages.retry": self.v_retry, "messages.delete_conversation": self.v_delete_conv,
                "messages.mark_read": self.v_mark_read, "messages.limits": self.v_limits, "messages.subscribe": self.v_msg_subscribe,
                "settings.get": self.v_settings_get, "settings.set": self.v_settings_set}

    def changed(self):
        for c in list(self.subscribers):
            c.send({"event": "ifaces_changed", "data": {}})


class Handler(socketserver.StreamRequestHandler):
    def send(self, obj):
        with self.wlock:
            try:
                self.wfile.write((json.dumps(obj) + "\n").encode())
                self.wfile.flush()
            except OSError:
                pass

    def handle(self):
        self.wlock = threading.Lock()
        world = self.server.world
        while True:
            try:
                line = self.rfile.readline()
            except OSError:
                return
            if not line:
                return
            try:
                req = json.loads(line)
                verb, params, rid = req["verb"], req.get("params") or {}, req.get("id")
            except (ValueError, KeyError, TypeError):
                self.send({"id": None, "ok": False, "error": {"code": "bad_request", "message": "not a request"}})
                continue
            with world.lock:
                with open(world.log_file, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"name": world.name, "verb": verb, "params": params}) + "\n")
            if self.server.delay:
                time.sleep(self.server.delay)
            try:
                fn = world.verbs().get(verb)
                if fn is None:
                    raise BridgeError("unknown_verb", "no verb %r" % verb)
                with world.lock:
                    result = fn(params)
                if verb == "announces.subscribe":
                    world.subscribers.append(self)
                    world.schedule_heard()
                if verb == "messages.subscribe":
                    world.msg_subs.append(self)
                self.send({"id": rid, "ok": True, "result": result})
            except BridgeError as e:
                self.send({"id": rid, "ok": False, "error": e.as_dict()})
            if verb == "service.stop":
                return


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True)
    ap.add_argument("--names", required=True)
    ap.add_argument("--scenario", default="fresh",
                    choices=["fresh", "mixed", "radio", "busyport", "chat", "chatblocked", "chatempty", "notice", "listenonly"])
    ap.add_argument("--deliver-after", type=float, default=0.7)
    ap.add_argument("--log", default=None)
    ap.add_argument("--delay", type=float, default=0.0)
    ap.add_argument("--heard-after", type=float, default=0.0)
    ap.add_argument("--appear-after", type=float, default=0.0)
    args = ap.parse_args()
    log = args.log or os.path.join(args.base, "requests.log")
    open(log, "a").close()
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *a: stop.set())
    signal.signal(signal.SIGINT, lambda *a: stop.set())
    stop_file = os.path.join(args.base, "stop")
    servers = []
    deadline = time.time() + args.appear_after
    while time.time() < deadline and not stop.is_set() and not os.path.exists(stop_file):
        time.sleep(0.05)
    for name in [n for n in args.names.split(",") if n]:
        d = os.path.join(args.base, name)
        os.makedirs(d, mode=0o700, exist_ok=True)
        path = os.path.join(d, "bridge.sock")
        if os.path.exists(path):
            os.unlink(path)
        s = Server(path, Handler)
        s.world, s.delay = World(args.scenario, log, name), args.delay
        s.world.deliver_after = args.deliver_after
        s.world.heard_after = args.heard_after
        threading.Thread(target=s.serve_forever, daemon=True).start()
        servers.append(s)
    print("fake bridge ready: %d socket(s) under %s" % (len(servers), args.base), flush=True)
    while not stop.is_set() and not os.path.exists(stop_file):
        time.sleep(0.1)
    for s in servers:
        s.shutdown()
        s.server_close()


if __name__ == "__main__":
    main()
