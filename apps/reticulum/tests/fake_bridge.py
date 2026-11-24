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
  fresh       the first configuration: one Wi-Fi / LAN connection, off
  mixed       Wi-Fi / LAN off; a LoRa radio on and listen only (port, channel busy and noise reported), a server on the
              internet with a problem; two people heard
  radio       Wi-Fi / LAN off; a LoRa radio off with only its port set: every radio field "not set"
  busyport    a complete LoRa radio, off, whose port a program already uses (switching it on says so)

The numbers a scenario uses for a radio (SYNTHETIC, picked inside the ranges the bridge accepts, not a real band plan or a
recommendation) are in SYNTH_RADIO below.

--delay S      every answer waits S seconds (a slow bridge)
--appear-after S   the sockets do not exist for S seconds (a service that is still starting)
Stops on SIGTERM or when the file DIR/stop appears.
"""
import argparse
import json
import os
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
BUSY_PORT = "/dev/ttyBUSY0"


def new_iface(name, typ, enabled=False, outgoing=False, opts=None, online=False, stats=None, error=None):
    return {"name": name, "type": typ, "enabled": enabled, "outgoing": outgoing, "online": online, "stats": stats,
            "options": dict(opts or {}), "secrets_set": [], "subsections": [], "supported": True, "lock": None,
            "error": error}


class World:
    def __init__(self, scenario, log, name):
        self.scenario, self.log_file, self.name = scenario, log, name
        self.lock = threading.Lock()
        self.display_name = None
        self.stopped = False
        self.started = time.time()
        self.subscribers = []
        self.ifaces = [new_iface("Default Interface", "AutoInterface")]
        now = time.time()
        self.recent = []
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
        elif scenario in ("radio", "busyport"):
            if scenario == "busyport":
                opts = dict(port=BUSY_PORT, **SYNTH_RADIO)   # complete, so that switching on reaches the busy port
            else:
                opts = {"port": "/dev/ttyACM0"}
            self.ifaces.append(new_iface("LoRa radio", "RNodeInterface", opts=opts))
        for i in self.ifaces:
            self.refresh(i)

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
                "interfaces_up": [i["name"] for i in self.ifaces if i["online"]], "uptime": int(time.time() - self.started)}

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
        i["outgoing"] = p["outgoing"]
        self.changed()
        return dict(self.entry(i), reloaded=True if i["enabled"] else None)

    def v_subscribe(self, p):
        return {"subscribed": True, "recent": list(self.recent)}

    def v_paths(self, p):
        return {"count": 0, "paths": []}

    def v_announce(self, p):
        # the app must never ask for this: the checks fail the run when it shows up in the log
        raise BridgeError("no_transmit", "no interface that is up may transmit")

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

    def v_stop(self, p):
        self.stopped = True
        return {"stopping": True}

    def verbs(self):
        return {"hello": self.v_hello, "status": self.v_status, "ifaces": self.v_ifaces, "iface.types": self.v_types,
                "iface.add": self.v_add, "iface.edit": self.v_edit, "iface.remove": self.v_remove,
                "iface.enable": self.v_enable, "iface.outgoing": self.v_outgoing, "announces.subscribe": self.v_subscribe,
                "paths": self.v_paths, "announce": self.v_announce, "identity.info": self.v_identity,
                "identity.name": self.v_name, "service.stop": self.v_stop}

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
    ap.add_argument("--scenario", default="fresh", choices=["fresh", "mixed", "radio", "busyport"])
    ap.add_argument("--log", default=None)
    ap.add_argument("--delay", type=float, default=0.0)
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
