# SPDX-License-Identifier: MIT
"""The bridge process: RNS shared instance + LXMF router + the verbs of protocol 1 (docs/dev/reticulum-bridge.md).

Transmit rules (D8): interfaces the bridge writes start with enabled = no and outgoing = False; iface.enable and
iface.outgoing are the only switches; the only verb that sends is announce, and it refuses while no interface that is
up may transmit. The LXMF router is started with the deck's identity but never announces by itself (PC test: no
announce or path request bytes); lxmf.send and the message store are phase P2.
"""
import base64
import collections
import os
import signal
import sys
import threading
import time

from . import PROTOCOL, BridgeError, __version__, options, portlock, rnsconfig
from .server import AlreadyRunning, RunDir, Server

RECENT = 64
PATHS_MAX = 256
STAT_KEYS = {"status", "rxb", "txb", "arxb", "atxb", "prxb", "ptxb", "bitrate", "clients", "rssi", "snr",
             "signal_quality", "held_announces", "txdrp"}
STAT_PREFIXES = ("noise", "interference", "airtime", "channel_load", "battery", "cpu_", "mem_")
LOCAL_TYPES = ("LocalServerInterface", "LocalClientInterface")
NAME_MAX = 64


def log(msg):
    sys.stderr.write("[reticulum] %s\n" % msg)
    sys.stderr.flush()


def as_bool(v, default):
    if v is None:
        return default
    return v if isinstance(v, bool) else str(v).strip().lower() in ("yes", "true", "on", "1", "y")


def param_bool(p, key, default=None):
    v = p.get(key, default)
    if not isinstance(v, bool):
        raise BridgeError("invalid", "%s must be true or false" % key, field=key)
    return v


def _scalar(v):
    if isinstance(v, bytes):
        return v.hex()
    return v if isinstance(v, (int, float, str, bool, type(None))) else str(v)


def set_process_name(name=b"reticulum"):
    """comm shows in other apps' "used by" messages (Mesh Hop reads /proc/<pid>/comm)."""
    try:
        import ctypes
        ctypes.CDLL(None).prctl(15, name, 0, 0, 0)         # PR_SET_NAME
    except Exception:
        pass


class _Announces:
    aspect_filter = None                                  # every announce RNS accepts

    def __init__(self, bridge):
        self.bridge = bridge

    def received_announce(self, destination_hash, announced_identity, app_data):
        self.bridge.heard(destination_hash, announced_identity, app_data)


class Bridge:
    def __init__(self, config_dir, data_dir, run_dir, use_lxmf=True):
        self.config_dir = config_dir
        self.config_path = os.path.join(config_dir, "config")
        self.data_dir = data_dir
        self.run_dir = RunDir(run_dir)
        self.use_lxmf = use_lxmf
        self.locks = portlock.PortLocks()
        self.errors = {}                                  # interface -> why it is not up (busy port, failed attach)
        self.mutex = threading.Lock()                     # one configuration change at a time
        self.state_lock = threading.Lock()
        self.recent = collections.deque(maxlen=RECENT)
        self.RNS = self.LXMF = self.rns = self.router = self.dest = self.identity = None
        self.mode, self.started, self.server, self.last_state = None, time.time(), None, None
        self.verbs = {"hello": self.v_hello, "status": self.v_status, "ifaces": self.v_ifaces,
                      "iface.types": self.v_types, "iface.add": self.v_add, "iface.edit": self.v_edit,
                      "iface.remove": self.v_remove, "iface.enable": self.v_enable,
                      "iface.outgoing": self.v_outgoing, "announces.subscribe": self.v_subscribe,
                      "paths": self.v_paths, "announce": self.v_announce, "identity.info": self.v_identity,
                      "identity.export": self.v_export, "service.stop": self.v_stop}

    # --- life cycle ---
    def run(self):
        try:
            self.start()
        except AlreadyRunning as e:
            log("already running (pid %s)" % e)
            return 3
        except BridgeError as e:
            log("cannot start: %s" % e.message)
            self.run_dir.cleanup()
            return 1
        try:
            self.server.serve()
        except Exception:
            import traceback
            log("server loop failed:\n" + traceback.format_exc())
        finally:
            self.shutdown()
        return 0

    def start(self):
        os.umask(0o077)
        set_process_name()
        if self.run_dir.claim():
            log("removed a stale socket")
        if rnsconfig.ensure_first_run(self.config_dir):
            log("no configuration found: wrote a first one to %s (every interface off)" % self.config_path)
        busy = {}
        for name, sec in rnsconfig.interfaces(rnsconfig.read(self.config_path)).items():
            if self._enabled(sec) and self._lockable(sec):
                try:
                    self.locks.acquire(name, sec["port"])      # before RNS opens the port (report 4.4)
                except BridgeError as e:
                    busy[name] = e.message
        import RNS                                         # lazy: only the daemon pays for it
        self.RNS = RNS
        self.rns = RNS.Reticulum(configdir=self.config_dir)
        self.mode = "client" if getattr(self.rns, "is_connected_to_shared_instance", False) else "master"
        if self.mode == "client":                          # another program owns the interfaces and their ports
            self.locks.release_all()
            busy = {}
        for name, why in busy.items():
            self.errors[name] = why
            self._call("detach_interface", name)
            log("%s left down: %s" % (name, why))
        if self.use_lxmf:
            try:
                self._start_lxmf()
            except BridgeError as e:
                log("LXMF not started: %s" % e.message)
        RNS.Transport.register_announce_handler(_Announces(self))
        self.server = Server(self.run_dir, self.dispatch, log, tick=self.refresh_state)
        self.server.listen()
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):   # after RNS, which sets its own handlers
            signal.signal(sig, lambda *_: self.server.request_stop())
        self.refresh_state()
        log("ready: %s instance, config %s, RNS %s, LXMF %s" % (
            self.mode, self.config_dir, RNS.__version__, self.LXMF.__version__ if self.router else "off"))

    def _start_lxmf(self):
        import LXMF
        self.LXMF = LXMF
        os.makedirs(self.data_dir, mode=0o700, exist_ok=True)
        path = os.path.join(self.data_dir, "identity")
        if os.path.exists(path):
            self.identity = self.RNS.Identity.from_file(path)
            if self.identity is None:
                raise BridgeError("identity", "%s is not a valid identity file (left as it is)" % path)
        else:
            self.identity = self.RNS.Identity()
            tmp = path + ".tmp"
            self.identity.to_file(tmp)
            os.chmod(tmp, 0o600)
            os.replace(tmp, path)
            log("created the deck identity in %s" % path)
        self.router = LXMF.LXMRouter(identity=self.identity, storagepath=os.path.join(self.data_dir, "lxmf"))
        self.dest = self.router.register_delivery_identity(self.identity, display_name=self._display_name())

    def shutdown(self):
        log("stopping")
        if self.router:
            try:
                self.router.exit_handler()
            except Exception as e:
                log("LXMF exit: %s" % e)
        self.locks.release_all()
        self.run_dir.cleanup()
        if self.rns:
            self.RNS.Reticulum.exit_handler()              # persists RNS's tables; ends the process (status 0)

    def dispatch(self, verb, params, client):
        fn = self.verbs.get(verb)
        if not fn:
            raise BridgeError("unknown_verb", "unknown verb %r" % verb)
        return fn(params, client)

    # --- helpers ---
    def _enabled(self, sec):
        return as_bool(sec.get("enabled", sec.get("interface_enabled")), False)

    def _lockable(self, sec):
        return sec.get("type") in options.PORT_LOCK_TYPES and portlock.is_device(sec.get("port"))

    def _call(self, method, name):
        try:
            return bool(getattr(self.rns, method)(name))
        except Exception as e:
            log("%s(%s): %s" % (method, name, e))
            return False

    def _display_name(self):
        try:
            with open(os.path.join(self.data_dir, "display_name"), encoding="utf-8") as f:
                return f.read().strip() or None
        except OSError:
            return None

    def _section(self, name):
        text = rnsconfig.read(self.config_path)
        sec = rnsconfig.interfaces(text).get(name)
        if sec is None:
            raise BridgeError("not_found", "no interface named %r" % name)
        return text, sec

    def _write(self, text):
        try:
            rnsconfig.write_atomic(self.config_path, text)
        except OSError as e:
            raise BridgeError("config_error", "cannot write %s: %s" % (self.config_path, e.strerror))

    def _stats(self):
        try:
            raw = self.rns.get_interface_stats() or {}
        except Exception as e:
            log("interface statistics: %s" % e)
            return {}
        out = {}
        for i in raw.get("interfaces", []):
            if i.get("parent_interface_name") or i.get("type") in LOCAL_TYPES:
                continue
            out[i.get("short_name") or i.get("name")] = {
                k: v for k, v in i.items()
                if (k in STAT_KEYS or k.startswith(STAT_PREFIXES)) and isinstance(v, (int, float, str, bool, type(None)))}
        return out

    def _ifaces(self):
        stats = self._stats()
        secret = {o["name"] for o in options.COMMON if o.get("secret")}
        out = []
        for name, sec in rnsconfig.interfaces(rnsconfig.read(self.config_path)).items():
            t = sec.get("type")
            st = stats.get(name)
            out.append({
                "name": name, "type": t, "enabled": self._enabled(sec), "outgoing": as_bool(sec.get("outgoing"), True),
                "online": bool(st and st.get("status")), "stats": st,
                "options": {k: v for k, v in sec.items() if k not in secret and k not in options.SWITCHES
                            and k not in ("type", "interface_enabled") and not isinstance(v, dict)},
                "secrets_set": sorted(k for k in secret if k in sec),
                "subsections": [k for k, v in sec.items() if isinstance(v, dict)],
                "supported": t in options.TYPES and t not in options.UNSUPPORTED,
                "missing": options.missing_required(t, sec),
                "lock": ("held" if name in self.locks.held else "not held") if self._lockable(sec) else None,
                "error": self.errors.get(name)})
        return out

    def _entry(self, name):
        return next((i for i in self._ifaces() if i["name"] == name), None)

    def _changed(self):
        self.refresh_state()
        if self.server:
            self.server.post_event("ifaces_changed", {})

    def refresh_state(self):
        """bridge.state beside the socket; rewritten only when a value changed (tick every 15 s and after changes)."""
        with self.state_lock:
            up = [i["name"] for i in self._ifaces() if i["online"]]
            state = {"pid": os.getpid(), "state": "up", "bridge": __version__, "protocol": PROTOCOL,
                     "rns": self.RNS.__version__, "lxmf": self.LXMF.__version__ if self.router else "off",
                     "mode": self.mode, "config_dir": self.config_dir,
                     "identity": self.identity.hash.hex() if self.identity else "",
                     "lxmf_address": self.dest.hash.hex() if self.dest else "",
                     "ifaces_up": len(up), "ifaces_up_names": ";".join(n.replace(";", ",") for n in up),
                     "started": int(self.started)}
            if state != self.last_state:
                self.run_dir.write_state(state)
                self.last_state = state

    def heard(self, destination_hash, identity, app_data):
        """Announce handler (an RNS thread): kept in a bounded list and sent to subscribed clients."""
        RNS = self.RNS
        kind, name = "other", None
        try:
            if identity is not None:
                for k, aspects in (("lxmf", ("lxmf", "delivery")), ("lxmf_propagation", ("lxmf", "propagation")),
                                   ("nomadnet_node", ("nomadnetwork", "node"))):
                    if RNS.Destination.hash(identity, *aspects) == destination_hash:
                        kind = k
                        break
            if kind == "lxmf" and self.LXMF and app_data:
                name = self.LXMF.display_name_from_app_data(app_data)
            elif kind == "nomadnet_node" and app_data:
                name = app_data.decode("utf-8")
        except Exception:
            name = None
        hops = RNS.Transport.hops_to(destination_hash)
        iface = RNS.Transport.next_hop_interface(destination_hash)
        event = {"time": round(time.time(), 3), "destination": destination_hash.hex(),
                 "identity": identity.hash.hex() if identity is not None else None, "kind": kind,
                 "name": name if isinstance(name, str) else None,
                 "hops": None if hops == RNS.Transport.PATHFINDER_M else hops,
                 "interface": getattr(iface, "name", None) if iface is not None else None}
        self.recent.append(event)
        if self.server:
            self.server.post_event("announce", event)

    # --- verbs ---
    def v_hello(self, p, c):
        return {"bridge": __version__, "protocol": PROTOCOL, "rns": self.RNS.__version__,
                "lxmf": self.LXMF.__version__ if self.router else None, "verbs": sorted(self.verbs)}

    def v_status(self, p, c):
        ifs = self._ifaces()
        return {"pid": os.getpid(), "state": "up", "bridge": __version__, "protocol": PROTOCOL,
                "rns": self.RNS.__version__, "lxmf": self.LXMF.__version__ if self.router else None,
                "mode": self.mode, "transport": bool(self.RNS.Reticulum.transport_enabled()),
                "config_dir": self.config_dir, "identity_hash": self.identity.hash.hex() if self.identity else None,
                "lxmf_address": self.dest.hash.hex() if self.dest else None, "display_name": self._display_name(),
                "interfaces_total": len(ifs), "interfaces_up": [i["name"] for i in ifs if i["online"]],
                "uptime": int(time.time() - self.started)}

    def v_ifaces(self, p, c):
        return {"interfaces": self._ifaces()}

    def v_types(self, p, c):
        return options.type_list()

    def v_add(self, p, c):
        name, t = options.check_name(p.get("name")), p.get("type")
        values = options.validate(t, p.get("options") or {})
        options.check_pairs(t, values)
        items = [("type", t), ("enabled", "no"), ("outgoing", "False")]       # D8: written off, never on
        items += [(k, v) for k, v in values.items() if v is not None]
        with self.mutex:
            self._write(rnsconfig.add_section(rnsconfig.read(self.config_path), name, items))
        log("added interface %r (%s), off" % (name, t))
        self._changed()
        return self._entry(name)

    def v_edit(self, p, c):
        name = p.get("name")
        with self.mutex:
            text, sec = self._section(name)
            t = sec.get("type")
            changes = options.validate(t, p.get("options") or {})
            merged = dict(sec, **changes)
            options.check_pairs(t, merged)
            on = self._enabled(sec)
            if on and options.missing_required(t, merged):
                raise BridgeError("incomplete", "switch the interface off before emptying %s" % ", ".join(
                    options.missing_required(t, merged)))
            new = rnsconfig.edit_section(text, name, changes)
            after = rnsconfig.interfaces(new)[name]
            if on and self.mode == "master" and self._lockable(after):
                self.locks.acquire(name, after["port"])              # a new port is locked before the write
            elif not self._lockable(after):
                self.locks.release(name)
            self._write(new)
            reloaded = self._call("reload_interface", name) if on else None
        log("edited interface %r" % name)
        self._changed()
        return dict(self._entry(name), reloaded=reloaded)

    def v_remove(self, p, c):
        name = p.get("name")
        with self.mutex:
            text, sec = self._section(name)
            self._write(rnsconfig.remove_section(text, name))
            if self._enabled(sec):
                self._call("detach_interface", name)
            self.locks.release(name)
            self.errors.pop(name, None)
        log("removed interface %r" % name)
        self._changed()
        return {"removed": name}

    def v_enable(self, p, c):
        name, on = p.get("name"), param_bool(p, "enabled", True)
        with self.mutex:
            text, sec = self._section(name)
            t = sec.get("type")
            if on:
                missing = options.missing_required(t, sec)
                if missing:
                    raise BridgeError("incomplete", "fill in first: %s" % ", ".join(missing), missing=missing)
                if self._enabled(sec) and name in self._stats():
                    return self._entry(name)
                if self.mode == "master" and self._lockable(sec):
                    self.locks.acquire(name, sec["port"])          # raises busy: nothing written
                self._write(rnsconfig.edit_section(text, name, {"enabled": "yes"}))   # the section first, then attach
                if not self._call("attach_interface", name):
                    self._write(rnsconfig.edit_section(rnsconfig.read(self.config_path), name, {"enabled": "no"}))
                    self.locks.release(name)
                    raise BridgeError("attach_failed", "RNS did not bring %r up (see the bridge log)" % name)
            else:
                self._write(rnsconfig.edit_section(text, name, {"enabled": "no"}))
                self._call("detach_interface", name)
                self.locks.release(name)
            self.errors.pop(name, None)
        log("interface %r switched %s" % (name, "on" if on else "off"))
        self._changed()
        return self._entry(name)

    def v_outgoing(self, p, c):
        name, on = p.get("name"), param_bool(p, "outgoing")
        with self.mutex:
            text, sec = self._section(name)
            self._write(rnsconfig.edit_section(text, name, {"outgoing": "True" if on else "False"}))
            reloaded = self._call("reload_interface", name) if self._enabled(sec) else None
        log("interface %r may %stransmit" % (name, "" if on else "not "))
        self._changed()
        return dict(self._entry(name), reloaded=reloaded)

    def v_subscribe(self, p, c):
        c.subscribed = param_bool(p, "on", True)
        return {"subscribed": c.subscribed, "recent": list(self.recent)}

    def v_paths(self, p, c):
        table = self.rns.get_path_table(max_hops=p.get("max_hops")) or []
        return {"count": len(table), "paths": [{k: _scalar(v) for k, v in e.items()} for e in table[:PATHS_MAX]]}

    def v_announce(self, p, c):
        if not self.router:
            raise BridgeError("lxmf_off", "the LXMF router is not running (see the bridge log)")
        name = p.get("display_name")
        if name is not None:
            if not isinstance(name, str) or not name.strip() or len(name) > NAME_MAX or any(
                    ord(ch) < 32 for ch in name):
                raise BridgeError("invalid", "display_name: 1-%d characters, no control characters" % NAME_MAX,
                                  field="display_name")
            name = name.strip()
        name = name or self._display_name()
        if not name:
            raise BridgeError("no_display_name", "set a display name first: announces carry it (D12)")
        senders = [i["name"] for i in self._ifaces() if i["enabled"] and i["outgoing"] and i["online"]]
        if not senders:
            raise BridgeError("no_transmit", "no interface that is up may transmit: switch one on and allow it to "
                              "transmit first (iface.outgoing)")
        if name != self._display_name():
            path = os.path.join(self.data_dir, "display_name")
            with open(path + ".tmp", "w", encoding="utf-8") as f:
                f.write(name + "\n")
            os.replace(path + ".tmp", path)
        self.dest.display_name = name
        self.router.announce(self.dest.hash)
        log("announced %s on request via %s" % (self.dest.hash.hex(), ", ".join(senders)))
        return {"announced": True, "lxmf_address": self.dest.hash.hex(), "display_name": name, "via": senders}

    def v_identity(self, p, c):
        if not self.identity:
            raise BridgeError("lxmf_off", "no identity loaded (LXMF is off)")
        return {"identity_hash": self.identity.hash.hex(), "lxmf_address": self.dest.hash.hex(),
                "display_name": self._display_name(), "identity_file": os.path.join(self.data_dir, "identity")}

    def v_export(self, p, c):
        if not self.identity:
            raise BridgeError("lxmf_off", "no identity loaded (LXMF is off)")
        if p.get("confirm") is not True:
            raise BridgeError("confirm", "the key gives full control of this identity: send confirm: true")
        log("identity exported on request (the key is not logged)")
        return {"identity_hash": self.identity.hash.hex(), "format": "base32",
                "key": base64.b32encode(self.identity.get_private_key()).decode()}

    def v_stop(self, p, c):
        log("stop requested")
        return {"stopping": True}
