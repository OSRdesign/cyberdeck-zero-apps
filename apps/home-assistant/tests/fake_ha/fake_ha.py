#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A fake Home Assistant for the app's integration test (Python 3 standard library only, no network beyond 127.0.0.1).

Derived from the throwaway fakes of the spike (fake_ha_ws.py, fake_ha_auth.py, study 052 probe 2 and 3). It speaks the
subset the app uses, as Home Assistant 2026.10 does (core sources read for the spike: websocket_api/commands.py,
messages.py, auth/__init__.py, auth/indieauth.py):

  HTTP  GET  /auth/providers              200, no login
        GET  /auth/authorize?client_id&redirect_uri&state
                                          stands in for "the phone logged in": checks the client id like HA's indieauth
                                          (http/https, a local IP or a name, no fragment / user) and that the redirect
                                          has the same scheme and host:port, then 302 to redirect_uri?code=..&state=..
                                          (the Location also in the body, for the test's own "phone")
        POST /auth/token                  form: authorization_code (one-use code, same client id) and refresh_token
                                          (same client id as at the sign-in); 400 invalid_grant for an unknown token
        POST /auth/revoke                 form token=<refresh token>: removes it and its access tokens, closes their
                                          WebSocket connections; always 200
  WS    /api/websocket                    auth_required -> auth -> auth_ok / auth_invalid (then closed), and:
        supported_features (coalesce_messages: results and changes then go out in JSON arrays), get_config,
        auth/current_user, config/area_registry/list, config/device_registry/list,
        config/entity_registry/list_for_display, subscribe_entities (entity_ids; result, snapshot "a", then "c"
        changes), unsubscribe_events, call_service (light, switch, input_boolean: turn_on / turn_off / toggle;
        scene / script turn_on; lock lock / unlock; cover open_cover / close_cover; fan toggle / turn_on / turn_off;
        a list of entity ids as the target), frontend/get_icons (a few made-up rules, 0.2.0), ping -> pong; 0.3.0's
        stored dashboard: lovelace/dashboards/list (one map-only dashboard listed), lovelace/config (the default one:
        owner-shaped, study 053 section 13's counts, 9 entities + 2 sensor + 1 weather-forecast cards, 25 entity
        references, on the fake house's entities; by url_path; config_not_found otherwise), frontend/get_system_data
        (core: no default panel), weather/subscribe_forecast (daily: the result, then one event), history/
        history_during_period (a made-up curve every 10 minutes). Every message type received is counted: the test
        checks that no administrator command was sent.
  Test controls (GET /fake/...): calls (the service calls, JSON), stats (counters, JSON), wsping (a WebSocket ping
        frame to every client), rst (TCP reset of every WebSocket), expire (every access token refused from now on),
        revoke_all (every token removed, connections closed, as deleting the sign-in in the user's profile),
        huge (a 5 MB message on every subscription), state?entity=..&s=.. (a change made at the wall switch),
        types (the message types received, JSON). Any other /api/ path answers 404 and is counted (rest_calls): the
        app never calls the REST API.

Options: --port, --entities (2000), --areas (20), --frag BYTES (every message longer than that goes out in
continuation frames of that size; 0 = one frame), --expires S (access token lifetime, 1800), --secrets FILE (every token
issued is appended there so that the test can check none of them reached a log). It never prints a token.
"""
import argparse, base64, hashlib, json, secrets, socket, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from ipaddress import ip_address
from urllib.parse import parse_qs, urlencode, urlparse

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OPT = {}
LOCK = threading.RLock()
CODES = {}      # code -> client_id
REFRESH = {}    # refresh token -> client_id
ACCESS = {}     # access token -> (refresh token, expires at)
CLIENTS = []    # live WebSocket connections
CALLS = []
STATS = {"providers": 0, "authorize": 0, "exchanges": 0, "refreshes": 0, "refresh_refused": 0, "revokes": 0,
         "ws_connects": 0, "auth_ok": 0, "auth_invalid": 0, "pongs": 0, "subscribes": 0, "unsubscribes": 0,
         "messages_in": 0, "max_message_out": 0, "frames_out": 0, "lovelace_config": 0, "forecast_subs": 0,
         "history": 0, "rest_calls": 0}
TYPES = {}      # message type -> count (0.3.0: no administrator command)
DASH = {}       # "" (the default dashboard) or url_path -> its config
STATES = {}     # entity_id -> {"s":..., "a": {...}, "lc": float}
REG = {"areas": [], "devices": [], "entities": []}
EXPIRED_BEFORE = [0.0]


def log(msg):
    print("[fake-ha] " + msg, flush=True)


def secret(tok):
    if OPT.get("secrets"):
        with open(OPT["secrets"], "a") as f:
            f.write(tok + "\n")
    return tok


# ---------------------------------------------------------------- the house
def build_house(n_entities, n_areas):
    areas = [{"area_id": "area_%02d" % i, "name": "Room %02d" % i, "floor_id": None, "icon": None,
              "aliases": [], "labels": [], "picture": None} for i in range(n_areas)]
    devices = []
    for i in range(n_areas * 2):
        devices.append({"id": "dev_%03d" % i, "area_id": areas[i % n_areas]["area_id"], "name": "Device %d" % i,
                        "name_by_user": None, "manufacturer": "Fake", "model": "F1", "config_entries": ["x"],
                        "connections": [], "identifiers": [["fake", str(i)]], "sw_version": "1.0"})
    doms = ["light", "switch", "sensor", "binary_sensor", "input_boolean", "scene", "script", "cover", "lock",
            "climate", "media_player", "automation", "update", "person"]
    entities = []
    now = time.time()
    for i in range(n_entities):
        dom = doms[i % len(doms)]
        eid = "%s.fake_%04d" % (dom, i)
        e = {"ei": eid, "pl": "fake", "en": "Fake %s %d" % (dom.replace("_", " "), i)}
        if i % 3 == 0:
            e["di"] = devices[i % len(devices)]["id"]     # area through the device
        else:
            e["ai"] = areas[i % n_areas]["area_id"]
        if i % 50 == 7:
            e["hb"] = True
        if i % 40 == 9:
            e["ec"] = 1
        entities.append(e)
        attrs = {"friendly_name": e["en"].title()}
        s = "on" if i % 2 else "off"
        if dom == "light":
            attrs.update({"supported_color_modes": ["brightness"], "color_mode": "brightness"})
            if s == "on":
                attrs["brightness"] = 128
        elif dom == "sensor":
            s = "%.1f" % (20 + (i % 50) / 10.0)
            attrs.update({"unit_of_measurement": "°C", "device_class": "temperature", "state_class": "measurement"})
        elif dom == "binary_sensor":
            attrs["device_class"] = "door"
        elif dom == "cover":
            s = "closed"
            attrs["device_class"] = "garage" if i % 2 else "shade"
        elif dom == "lock":
            s = "locked"
        elif dom == "scene":
            s = "2026-10-31T19:00:00+00:00"
        elif dom == "climate":
            s = "heat"
            attrs.update({"current_temperature": 21.4, "temperature": 21, "hvac_action": "heating"})
        STATES[eid] = {"s": s, "a": attrs, "lc": now - i}
    REG.update({"areas": areas, "devices": devices, "entities": entities})
    # 0.3.0: a weather entity, and the owner-shaped stored dashboard on the house's entities
    STATES["weather.fake_home"] = {"s": "partlycloudy", "a": {"friendly_name": "Fake Home", "temperature": 14.2,
                                                              "temperature_unit": "°C"}, "lc": now}
    STATES["fan.fake_fan"] = {"s": "off", "a": {"friendly_name": "Fake Fan"}, "lc": now}
    def ids(dom, n, start=0):
        out = [e for e in sorted(STATES) if e.startswith(dom + ".fake_")]
        return out[start:start + n]
    lights, switches, sensors = ids("light", 6), ids("switch", 3), ids("sensor", 6)
    cards = [
        {"type": "entities", "title": "Lights", "entities": lights[:3]},
        {"type": "entities", "title": "More lights", "entities": [{"entity": lights[3], "name": "Renamed", "icon": "mdi:floor-lamp"}, lights[4]]},
        {"type": "entities", "title": "Plugs", "entities": switches[:2] + ["fan.fake_fan"]},
        {"type": "entities", "title": "Doors", "show_header_toggle": False, "entities": ids("lock", 2) + ids("cover", 1)},
        {"type": "sensor", "entity": sensors[0], "graph": "line"},
        {"type": "weather-forecast", "entity": "weather.fake_home"},
        {"type": "entities", "title": "Climate", "entities": ids("climate", 1) + [{"type": "section", "label": "Air"}] + sensors[1:3]},
        {"type": "entities", "entities": ids("binary_sensor", 2) + [{"type": "divider"}]},
        {"type": "entities", "title": "Scenes", "entities": ids("scene", 2)},
        {"type": "sensor", "entity": sensors[3], "graph": "line", "detail": 2},
        {"type": "entities", "title": "Misc", "entities": ids("input_boolean", 2) + [{"type": "weblink", "name": "Link", "url": "http://x"}]},
        {"type": "entities", "entities": ids("script", 1) + [switches[2]]},
    ]
    DASH[""] = {"title": "Fake", "views": [{"title": "Fake", "path": "fake", "icon": "mdi:home", "cards": cards}]}
    DASH["dashboard-map"] = {"views": [{"type": "panel", "title": "Map", "cards": [{"type": "map", "entities": []}]}]}


# ---------------------------------------------------------------- WebSocket framing
def enc_frame(op, payload, fin=True):
    b0 = (0x80 if fin else 0) | op
    n = len(payload)
    if n < 126:
        h = struct.pack("!BB", b0, n)
    elif n < 65536:
        h = struct.pack("!BBH", b0, 126, n)
    else:
        h = struct.pack("!BBQ", b0, 127, n)
    return h + payload


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            raise EOFError
        buf += c
    return buf


def read_frame(s):
    h = recv_exact(s, 2)
    op, fin, ln, masked = h[0] & 0x0F, bool(h[0] & 0x80), h[1] & 0x7F, h[1] & 0x80
    if ln == 126:
        ln = struct.unpack("!H", recv_exact(s, 2))[0]
    elif ln == 127:
        ln = struct.unpack("!Q", recv_exact(s, 8))[0]
    mk = recv_exact(s, 4) if masked else None
    data = recv_exact(s, ln) if ln else b""
    if mk:
        data = bytes(b ^ mk[i % 4] for i, b in enumerate(data))
    return op, fin, data


class Client:
    def __init__(self, sock):
        self.s = sock
        self.wlock = threading.Lock()
        self.token = None
        self.coalesce = False
        self.subs = {}      # subscription id -> set of entity ids
        self.alive = True

    def send_raw(self, frame):
        with self.wlock:
            self.s.sendall(frame)

    def send(self, obj):
        payload = json.dumps(obj, separators=(",", ":")).encode()
        STATS["max_message_out"] = max(STATS["max_message_out"], len(payload))
        frag = OPT["frag"]
        with self.wlock:
            if not frag or len(payload) <= frag:
                self.s.sendall(enc_frame(1, payload))
                STATS["frames_out"] += 1
                return
            off, first = 0, True
            while off < len(payload):
                chunk = payload[off:off + frag]
                off += len(chunk)
                self.s.sendall(enc_frame(1 if first else 0, chunk, fin=off >= len(payload)))
                STATS["frames_out"] += 1
                first = False

    def send_many(self, objs):
        if not objs:
            return
        if self.coalesce and len(objs) > 1:
            self.send(objs)          # one JSON array, as with coalesce_messages
        else:
            for o in objs:
                self.send(o)

    def kill(self, rst=True):
        """rst: a TCP reset (linger 0); else a plain close (FIN). The session's own thread is blocked in recv() on this
        socket: close() alone would not wake it (Linux keeps the socket while a call is in progress), shutdown() does."""
        self.alive = False
        try:
            if rst:
                self.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                self.s.shutdown(socket.SHUT_RD)
            else:
                self.s.shutdown(socket.SHUT_RDWR)
            self.s.close()
        except OSError:
            pass


def compressed(eid):
    st = STATES[eid]
    return {"s": st["s"], "a": st["a"], "c": "01FAKE" + eid[-4:], "lc": st["lc"]}


def push_change(eid, plus, minus=None):
    """a change to every subscription that holds the entity"""
    with LOCK:
        targets = [(c, sid) for c in CLIENTS if c.alive for sid, ids in c.subs.items() if eid in ids]
    for c, sid in targets:
        diff = {"+": plus}
        if minus:
            diff["-"] = {"a": minus}
        try:
            c.send({"id": sid, "type": "event", "event": {"c": {eid: diff}}})
        except OSError:
            pass


def result(mid, res=None, ok=True, err=None):
    m = {"id": mid, "type": "result", "success": ok}
    if ok:
        m["result"] = res
    else:
        m["error"] = err
    return m


def call_service(c, m):
    dom, svc = m.get("domain"), m.get("service")
    target = (m.get("target") or {}).get("entity_id") or (m.get("service_data") or {}).get("entity_id")
    data = m.get("service_data") or {}
    CALLS.append({"domain": dom, "service": svc, "entity_id": target, "data": data})
    if isinstance(target, list):  # a list target (0.3.0's header toggle): each entity, one result
        if not target or any(e not in STATES for e in target):
            return [result(m["id"], ok=False, err={"code": "not_found", "message": "Entity not found"})], None
        changes = []
        for e in target:
            r, ch = call_service(c, dict(m, target={"entity_id": e}))
            CALLS.pop()
            if ch:
                changes.append(ch)
        for ch in changes[:-1]:
            push_change(*ch)
        return r, (changes[-1] if changes else None)
    if target not in STATES:
        return [result(m["id"], ok=False, err={"code": "not_found", "message": "Entity not found"})], None
    st = STATES[target]
    plus, minus = {}, None
    if dom in ("light", "switch", "input_boolean", "fan") and svc in ("turn_on", "turn_off", "toggle"):
        on = {"turn_on": True, "turn_off": False, "toggle": st["s"] != "on"}[svc]
        st["s"] = "on" if on else "off"
        plus["s"] = st["s"]
        if dom == "light":
            if on:
                b = 255 if "brightness_pct" not in data else round(int(data["brightness_pct"]) * 255 / 100)
                st["a"]["brightness"] = b
                plus["a"] = {"brightness": b}
            elif "brightness" in st["a"]:
                del st["a"]["brightness"]
                minus = ["brightness"]
    elif dom in ("scene", "script") and svc == "turn_on":
        if dom == "scene":
            st["s"] = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())
            plus["s"] = st["s"]
    elif dom == "lock" and svc in ("lock", "unlock"):
        st["s"] = "locked" if svc == "lock" else "unlocked"
        plus["s"] = st["s"]
    elif dom == "cover" and svc in ("open_cover", "close_cover"):
        st["s"] = "open" if svc == "open_cover" else "closed"
        plus["s"] = st["s"]
    else:
        return [result(m["id"], ok=False, err={"code": "service_not_found", "message": "Service not found"})], None
    if plus:
        st["lc"] = time.time()
        plus["lc"] = st["lc"]
    return [result(m["id"], {"context": {"id": "01CTX", "parent_id": None, "user_id": "u1"}, "response": None})], \
        ((target, plus, minus) if plus else None)


def ws_session(c):
    s = c.s
    c.send({"type": "auth_required", "ha_version": "2026.10.4"})
    op, fin, d = read_frame(s)
    m = json.loads(d)
    tok = m.get("access_token", "")
    with LOCK:
        ent = ACCESS.get(tok)
        good = m.get("type") == "auth" and ent is not None and ent[1] > time.time() and ent[2] > EXPIRED_BEFORE[0]
    if not good:
        STATS["auth_invalid"] += 1
        c.send({"type": "auth_invalid", "message": "Invalid access token or password"})
        log("auth_invalid -> close")
        time.sleep(0.05)
        c.kill(rst=False)
        return
    c.token = tok
    STATS["auth_ok"] += 1
    c.send({"type": "auth_ok", "ha_version": "2026.10.4"})
    log("auth_ok")
    msg_parts = b""
    while c.alive:
        op, fin, d = read_frame(s)
        if op == 8:
            c.send_raw(enc_frame(8, d[:2]))
            break
        if op == 9:
            c.send_raw(enc_frame(10, d))
            continue
        if op == 10:
            STATS["pongs"] += 1
            log("pong received")
            continue
        msg_parts += d
        if not fin:
            continue
        m, msg_parts = json.loads(msg_parts), b""
        STATS["messages_in"] += 1
        t, mid = m.get("type"), m.get("id")
        with LOCK:
            TYPES[t] = TYPES.get(t, 0) + 1
        out, change = [], None
        if t == "supported_features":
            c.coalesce = bool((m.get("features") or {}).get("coalesce_messages"))
            out = [result(mid)]
        elif t == "ping":
            out = [{"id": mid, "type": "pong"}]
        elif t == "get_config":
            out = [result(mid, {"location_name": "Fake Home", "version": "2026.10.4",
                                "unit_system": {"temperature": "°C", "length": "km"}})]
        elif t == "auth/current_user":
            out = [result(mid, {"id": "u1", "name": "deck", "is_owner": False, "is_admin": False})]
        elif t == "config/area_registry/list":
            out = [result(mid, REG["areas"])]
        elif t == "config/device_registry/list":
            out = [result(mid, REG["devices"])]
        elif t == "config/entity_registry/list_for_display":
            out = [result(mid, {"entity_categories": {"0": "config", "1": "diagnostic"}, "entities": REG["entities"]})]
        elif t == "subscribe_entities":
            ids = set(m.get("entity_ids") or STATES.keys())
            with LOCK:
                c.subs[mid] = ids
            STATS["subscribes"] += 1
            c.send(result(mid))
            c.send({"id": mid, "type": "event", "event": {"a": {e: compressed(e) for e in ids if e in STATES}}})
            continue
        elif t == "unsubscribe_events":
            with LOCK:
                c.subs.pop(m.get("subscription"), None)
            STATS["unsubscribes"] += 1
            out = [result(mid)]
        elif t == "frontend/get_icons":
            # the fixture's own made-up rules, in the shape Home Assistant serves them (0.2.0's tiles)
            STATS["icons"] = STATS.get("icons", 0) + 1
            if m.get("category") == "entity_component":
                out = [result(mid, {"resources": {
                    "light": {"_": {"default": "mdi:lightbulb", "state": {"off": "mdi:lightbulb-off"}}},
                    "switch": {"_": {"default": "mdi:toggle-switch-variant"}}}})]
            else:
                out = [result(mid, {"resources": {}})]
        elif t == "lovelace/dashboards/list":
            out = [result(mid, [{"id": "d1", "url_path": "dashboard-map", "title": "Map", "icon": "mdi:map",
                                 "mode": "storage", "require_admin": False, "show_in_sidebar": True}])]
        elif t == "lovelace/config":
            STATS["lovelace_config"] += 1
            cfg = DASH.get(m.get("url_path") or "")
            out = [result(mid, cfg) if cfg is not None else
                   result(mid, ok=False, err={"code": "config_not_found", "message": "No config found."})]
        elif t == "frontend/get_system_data":
            out = [result(mid, {"value": None})]
        elif t == "weather/subscribe_forecast":
            STATS["forecast_subs"] += 1
            if m.get("entity_id") not in STATES:
                out = [result(mid, ok=False, err={"code": "invalid_entity_id", "message": "Weather entity not found"})]
            else:
                day0 = int(time.time()) // 86400 * 86400
                fc = [{"datetime": time.strftime("%Y-%m-%dT11:00:00+00:00", time.gmtime(day0 + i * 86400)),
                       "condition": ["sunny", "rainy", "cloudy", "fog", "snowy", "pouring"][i], "temperature": 10 + i,
                       "templow": 2 + i} for i in range(6)]
                c.send(result(mid))
                c.send({"id": mid, "type": "event", "event": {"type": "daily", "forecast": fc}})
                continue
        elif t == "history/history_during_period":
            STATS["history"] += 1
            def ts(v):
                from datetime import datetime
                return datetime.fromisoformat(v).timestamp()
            t0, t1 = ts(m["start_time"]), ts(m["end_time"])
            res = {}
            for e in m.get("entity_ids") or []:
                if e in STATES:
                    pts, tt = [], t0
                    while tt <= t1:
                        pts.append({"s": "%.2f" % (20 + 3 * ((tt - t0) / max(1, t1 - t0))), "lu": tt})
                        tt += 600
                    res[e] = pts
            out = [result(mid, res)]
        elif t == "call_service":
            out, change = call_service(c, m)
        else:
            out = [result(mid, ok=False, err={"code": "unknown_command", "message": "Unknown command."})]
        c.send_many(out)
        if change:
            push_change(*change)


# ---------------------------------------------------------------- HTTP
def verify_client_id(cid):
    p = urlparse(cid)
    if p.scheme not in ("http", "https") or p.fragment or p.username or p.password:
        return False
    if any(seg in (".", "..") for seg in (p.path or "/").split("/")):
        return False
    try:
        p.port
        addr = ip_address(p.hostname)
    except ValueError:
        return bool(p.hostname)
    return addr.is_loopback or addr.is_private or addr.is_link_local


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj).encode(), extra={"Cache-Control": "no-store"})

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/api/websocket":
            key = self.headers.get("Sec-WebSocket-Key", "").encode()
            acc = base64.b64encode(hashlib.sha1(key + GUID).digest())
            self.connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    b"Sec-WebSocket-Accept: " + acc + b"\r\n\r\n")
            STATS["ws_connects"] += 1
            c = Client(self.connection)
            with LOCK:
                CLIENTS.append(c)
            try:
                ws_session(c)
            except (EOFError, ConnectionError, OSError, ValueError) as e:
                log("connection ended: %s" % type(e).__name__)
            finally:
                c.alive = False
                with LOCK:
                    if c in CLIENTS:
                        CLIENTS.remove(c)
            self.close_connection = True
            return
        if u.path == "/auth/providers":
            STATS["providers"] += 1
            return self._json(200, {"providers": [{"name": "Home Assistant Local", "id": None, "type": "homeassistant"}],
                                    "preselect_remember_me": True})
        if u.path == "/auth/authorize":
            STATS["authorize"] += 1
            cid, ru = q.get("client_id", ""), q.get("redirect_uri", "")
            same = (urlparse(cid).scheme, urlparse(cid).netloc.lower()) == (urlparse(ru).scheme, urlparse(ru).netloc.lower())
            if not verify_client_id(cid) or not same:
                return self._send(400, b"Invalid redirect URI", "text/plain")
            code = secret("code_" + secrets.token_hex(12))
            with LOCK:
                CODES[code] = cid
            loc = ru + ("&" if "?" in ru else "?") + urlencode({"code": code, "state": q.get("state", "")})
            return self._send(302, loc.encode(), "text/plain", {"Location": loc})
        if u.path.startswith("/fake/"):
            return self.fake(u.path[6:], q)
        if u.path.startswith("/api/"):
            STATS["rest_calls"] += 1
        self._send(404, b"404: Not Found", "text/plain")

    def fake(self, what, q):
        with LOCK:
            clients = list(CLIENTS)
        if what == "calls":
            return self._json(200, CALLS)
        if what == "types":
            with LOCK:
                return self._json(200, dict(TYPES))
        if what == "stats":
            return self._json(200, dict(STATS, clients=len(clients), refresh_tokens=len(REFRESH)))
        if what == "wsping":
            for c in clients:
                try:
                    c.send_raw(enc_frame(9, b"hello"))
                except OSError:
                    pass
            return self._json(200, {"pinged": len(clients)})
        if what == "rst":
            for c in clients:
                c.kill(rst=True)
            log("reset %d connection(s)" % len(clients))
            return self._json(200, {"reset": len(clients)})
        if what == "expire":
            EXPIRED_BEFORE[0] = time.time()
            log("every access token issued so far is refused from now on")
            return self._json(200, {"ok": True})
        if what == "revoke_all":
            with LOCK:
                REFRESH.clear()
                ACCESS.clear()
            for c in clients:
                c.kill(rst=False)
            log("every sign-in removed")
            return self._json(200, {"ok": True})
        if what == "huge":
            blob = {"a": {"sensor.huge": {"s": "x", "a": {"pad": "y" * (5 * 1024 * 1024)}, "c": "c", "lc": 1}}}
            for c in clients:
                for sid in list(c.subs):
                    try:
                        c.send({"id": sid, "type": "event", "event": blob})
                    except OSError:
                        pass
            return self._json(200, {"ok": True})
        if what == "state":
            eid, s = q.get("entity", ""), q.get("s", "")
            if eid in STATES:
                STATES[eid]["s"] = s
                STATES[eid]["lc"] = time.time()
                push_change(eid, {"s": s, "lc": STATES[eid]["lc"]})
            return self._json(200, {"ok": eid in STATES})
        return self._send(404, b"no such control", "text/plain")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        f = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode()).items()}
        u = urlparse(self.path)
        if u.path == "/auth/revoke":
            STATS["revokes"] += 1
            rt = f.get("token", "")
            with LOCK:
                REFRESH.pop(rt, None)
                gone = [a for a, v in ACCESS.items() if v[0] == rt]
                for a in gone:
                    ACCESS.pop(a, None)
                clients = [c for c in CLIENTS if c.token in gone]
            for c in clients:
                c.kill(rst=False)
            return self._send(200)
        if u.path != "/auth/token":
            return self._send(404)
        gt = f.get("grant_type")
        life = OPT["expires"]
        if gt == "authorization_code":
            STATS["exchanges"] += 1
            cid = f.get("client_id", "")
            with LOCK:
                owner = CODES.pop(f.get("code", ""), None)
            if not cid or not verify_client_id(cid) or owner != cid:
                return self._json(400, {"error": "invalid_request", "error_description": "Invalid code"})
            rt = secret("ref_" + secrets.token_hex(32))
            at = secret("acc_" + secrets.token_hex(24))
            with LOCK:
                REFRESH[rt] = cid
                ACCESS[at] = (rt, time.time() + life, time.time())
            return self._json(200, {"access_token": at, "token_type": "Bearer", "refresh_token": rt,
                                    "expires_in": life, "ha_auth_provider": "homeassistant"})
        if gt == "refresh_token":
            STATS["refreshes"] += 1
            rt = f.get("refresh_token", "")
            with LOCK:
                owner = REFRESH.get(rt)
            if owner is None:
                STATS["refresh_refused"] += 1
                return self._json(400, {"error": "invalid_grant"})
            if owner != f.get("client_id"):
                STATS["refresh_refused"] += 1
                return self._json(400, {"error": "invalid_request"})
            at = secret("acc_" + secrets.token_hex(24))
            with LOCK:
                ACCESS[at] = (rt, time.time() + life, time.time())
            return self._json(200, {"access_token": at, "token_type": "Bearer", "expires_in": life})
        self._json(400, {"error": "unsupported_grant_type"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18123)
    ap.add_argument("--entities", type=int, default=2000)
    ap.add_argument("--areas", type=int, default=20)
    ap.add_argument("--frag", type=int, default=0)
    ap.add_argument("--expires", type=int, default=1800)
    ap.add_argument("--secrets", default="")
    a = ap.parse_args()
    OPT.update({"frag": a.frag, "expires": a.expires, "secrets": a.secrets})
    build_house(a.entities, a.areas)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    srv.daemon_threads = True
    log("listening on 127.0.0.1:%d, %d entities, %d areas, frames of %s, tokens %d s" %
        (a.port, a.entities, a.areas, a.frag or "any size", a.expires))
    srv.serve_forever()


if __name__ == "__main__":
    main()
