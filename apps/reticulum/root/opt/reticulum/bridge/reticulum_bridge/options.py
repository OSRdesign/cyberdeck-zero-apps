# SPDX-License-Identifier: MIT
"""Interface types and their options as data, and the validation of what a client sends (report 057, 4.1, 4.2).

One generic mechanism for every medium (D19): a client builds its add/edit form from iface.types, the bridge checks the
values here. Each option has a name, a kind (int, float, bool, str, list) and, where the RNS manual or report 057 4.2
states them, min/max, choices and the upstream default (the value RNS uses when the option is absent; None = none).
Radio fields (frequency, bandwidth, txpower, spreadingfactor, codingrate) have no default and no example value: they
start empty and the user fills them (D10). required = RNS cannot bring the interface up without it: the bridge refuses
iface.enable until it is set. enabled and outgoing are not options: only iface.enable and iface.outgoing change them.
"""
import math
import re

from . import BridgeError


def _o(name, kind, **kw):
    d = {"name": name, "kind": kind, "default": None}
    d.update(kw)
    return d


def _port(name, **kw):
    return _o(name, "int", min=1, max=65535, **kw)


_ID = [_o("id_callsign", "str", max_bytes=32), _o("id_interval", "int", min=1, unit="s")]
_SERIAL = [_o("port", "str", required=True, device=True), _o("speed", "int", min=1, unit="baud"),
           _o("databits", "int", choices=[5, 6, 7, 8]), _o("parity", "str", choices=["none", "even", "odd"]),
           _o("stopbits", "int", choices=[1, 2])]
_KISS = [_o("preamble", "int", min=0, unit="ms"), _o("txtail", "int", min=0, unit="ms"),
         _o("persistence", "int", min=0, max=255), _o("slottime", "int", min=0, unit="ms"),
         _o("flow_control", "bool", default=False)]
_RADIO = [_o("frequency", "int", min=137000000, max=3000000000, unit="Hz", required=True),
          _o("bandwidth", "int", min=7800, max=1625000, unit="Hz", required=True),
          _o("txpower", "int", min=0, max=37, unit="dBm", required=True),
          _o("spreadingfactor", "int", min=5, max=12, required=True),
          _o("codingrate", "int", min=5, max=8, required=True),
          _o("airtime_limit_short", "float", min=0, max=100, unit="%"),
          _o("airtime_limit_long", "float", min=0, max=100, unit="%")]

# Every type also takes these (manual, "Common Interface Options"; the discovery options are not for the deck).
COMMON = [_o("mode", "str", choices=["full", "gateway", "access_point", "roaming", "boundary", "internal"],
             default="full"),
          _o("network_name", "str"), _o("passphrase", "str", secret=True),
          _o("ifac_size", "int", min=8, max=512, unit="bits"),
          _o("announce_cap", "float", min=1, max=100, unit="%", default=2),
          _o("bitrate", "int", min=1, unit="bit/s"), _o("gravity", "int", default=0), _o("bootstrap_only", "bool")]

# The documented types of RNS 1.6 (manual interfaces.rst; report 4.1). The loader's undocumented WeaveInterface is left
# out; custom interface modules are listed by ifaces but cannot be added or edited here.
TYPES = {
    "AutoInterface": [_o("group_id", "str"),
                      _o("multicast_address_type", "str", choices=["temporary", "permanent"], default="temporary"),
                      _o("devices", "list"), _o("ignored_devices", "list"),
                      _o("discovery_scope", "str", choices=["link", "admin", "site", "organisation", "global"]),
                      _port("discovery_port"), _port("data_port")],
    "BackboneInterface": [_o("listen_on", "str"), _port("port"), _o("device", "str"), _o("prefer_ipv6", "bool"),
                          _o("remote", "str"), _o("target_host", "str"), _port("target_port"),
                          _o("block_fast_flapping", "bool", default=True),
                          _o("fast_flapping_block_time", "int", min=0, unit="min", default=720),
                          _o("fast_flapping_threshold", "int", min=0, unit="s", default=20),
                          _o("fast_flapping_grace", "int", min=0, default=5)],
    "TCPServerInterface": [_o("listen_ip", "str"), _port("listen_port"), _o("device", "str"),
                           _o("prefer_ipv6", "bool"), _port("port"), _o("i2p_tunneled", "bool")],
    "TCPClientInterface": [_o("target_host", "str", required=True), _port("target_port", required=True),
                           _o("kiss_framing", "bool"), _o("fixed_mtu", "int", min=1), _o("i2p_tunneled", "bool")],
    "UDPInterface": [_o("listen_ip", "str"), _port("listen_port"), _o("forward_ip", "str"), _port("forward_port"),
                     _o("device", "str"), _port("port")],
    "I2PInterface": [_o("connectable", "bool"), _o("peers", "list")],
    "RNodeInterface": [_o("port", "str", required=True, device=True)] + _RADIO
                      + [_o("flow_control", "bool", default=False)] + _ID,
    "RNodeMultiInterface": [],
    "SerialInterface": _SERIAL,
    "PipeInterface": [_o("command", "str", required=True), _o("respawn_delay", "int", min=0, unit="s")],
    "KISSInterface": _SERIAL + _KISS + _ID,
    "AX25KISSInterface": [_o("callsign", "str"), _o("ssid", "int", min=0, max=15)] + _SERIAL + _KISS,
}
UNSUPPORTED = {"RNodeMultiInterface": "its sub-interfaces are nested sections: edit the configuration by hand"}
# Types whose port option can be a serial device: the bridge takes the port lock before RNS opens it (report 4.4).
PORT_LOCK_TYPES = {"RNodeInterface", "RNodeMultiInterface", "SerialInterface", "KISSInterface", "AX25KISSInterface"}
SWITCHES = ("enabled", "outgoing")         # upstream defaults: enabled no, outgoing True; new interfaces: no / False

# What `outgoing = False` really does for each type, read from the bundled RNS 1.6.1 source (docs/dev/reticulum-bridge.md
# section 4, "RNS audit"). RNS only checks `outgoing` when it broadcasts a packet (Transport.outbound: `interface.OUT`);
# anything an interface sends by itself ignores it.
#   "silent"    : listen only sends nothing at all (the radio and serial kinds, UDP, a pipe).
#   "handshake" : listen only still sends the connection's own set-up when it comes up (a TCP or Backbone or I2P link: a
#                 tunnel hello of about 240 bytes, then keep-alives), no message, name or announcement.
#   "none"      : there is no listen only: the kind keeps sending discovery packets whatever `outgoing` says (AutoInterface
#                 multicasts a discovery token about every 1.6 s). Owner decision 2026-11-25: such a connection is on
#                 (sending allowed) or off, nothing else; the bridge refuses outgoing = false for it and turns a stored
#                 "on but listen only" one off.
LISTEN_ONLY = {"AutoInterface": "none",
               "BackboneInterface": "handshake", "TCPServerInterface": "handshake",
               "TCPClientInterface": "handshake", "I2PInterface": "handshake",
               "UDPInterface": "silent", "RNodeInterface": "silent", "RNodeMultiInterface": "silent",
               "SerialInterface": "silent", "PipeInterface": "silent", "KISSInterface": "silent",
               "AX25KISSInterface": "silent"}

NAME_RE = re.compile(r"[^\W_][\w .()+-]{0,47}")
_PLAIN_RE = re.compile(r"[\w./:@+-]+")


def listen_only_of(type_name):
    """"silent", "handshake" or "none" (see LISTEN_ONLY). A type the bridge does not know counts as "none": nothing is
    promised for it, so it has no listen-only state."""
    return LISTEN_ONLY.get(type_name, "none")


def type_list():
    """The iface.types result: every documented type with its options, then the common options."""
    out = []
    for name, opts in TYPES.items():
        t = {"type": name, "supported": name not in UNSUPPORTED, "port_lock": name in PORT_LOCK_TYPES,
             "listen_only": listen_only_of(name), "options": opts}
        if name in UNSUPPORTED:
            t["reason"] = UNSUPPORTED[name]
        out.append(t)
    return {"types": out, "common": COMMON, "switches": {"enabled": False, "outgoing": False}}


def options_of(type_name):
    if type_name not in TYPES:
        raise BridgeError("not_supported", "unknown interface type %r" % type_name)
    if type_name in UNSUPPORTED:
        raise BridgeError("not_supported", "%s: %s" % (type_name, UNSUPPORTED[type_name]))
    return {o["name"]: o for o in TYPES[type_name] + COMMON}


def check_name(name):
    if not isinstance(name, str) or not NAME_RE.fullmatch(name) or name.endswith(" "):
        raise BridgeError("invalid", "name: 1-48 letters, digits, spaces and . ( ) + - _, starting with a letter or "
                          "digit", field="name")
    return name


def _bad(o, why):
    raise BridgeError("invalid", "%s: %s" % (o["name"], why), field=o["name"])


def _text(o, v):
    if not isinstance(v, str):
        _bad(o, "text expected")
    if any(ord(c) < 32 or ord(c) == 127 for c in v) or v != v.strip():
        _bad(o, "no control characters, no leading or trailing spaces")
    if len(v.encode()) > min(o.get("max_bytes", 256), 256):
        _bad(o, "at most %d bytes" % min(o.get("max_bytes", 256), 256))
    if '"' in v and "'" in v:
        _bad(o, "cannot hold both kinds of quotes")
    return v


def quote(v):
    """A value as ConfigObj reads it back unchanged (quoted unless plain)."""
    if _PLAIN_RE.fullmatch(v):
        return v
    return '"%s"' % v if '"' not in v else "'%s'" % v


def to_config(o, v):
    """One client value -> its config text, or None to remove the option (None or "" = empty field)."""
    if v is None or v == "":
        return None
    k = o["kind"]
    if k == "bool":
        if isinstance(v, str) and v.lower() in ("yes", "true", "no", "false"):
            v = v.lower() in ("yes", "true")
        if not isinstance(v, bool):
            _bad(o, "yes or no expected")
        return "yes" if v else "no"
    if k in ("int", "float"):
        if isinstance(v, bool):
            _bad(o, "number expected")
        if isinstance(v, str):
            try:
                v = int(v.strip()) if k == "int" else float(v.strip())
            except ValueError:
                _bad(o, "number expected")
        if k == "int" and isinstance(v, float) and v.is_integer():
            v = int(v)
        if not isinstance(v, (int, float)) or (k == "int" and not isinstance(v, int)) or not math.isfinite(v):
            _bad(o, "whole number expected" if k == "int" else "number expected")
        if o.get("min") is not None and v < o["min"]:
            _bad(o, "at least %s" % o["min"])
        if o.get("max") is not None and v > o["max"]:
            _bad(o, "at most %s" % o["max"])
        if o.get("choices") and v not in o["choices"]:
            _bad(o, "one of %s" % ", ".join(map(str, o["choices"])))
        return str(v)
    if k == "list":
        items = v.split(",") if isinstance(v, str) else v
        if not isinstance(items, list) or not items:
            _bad(o, "list of names expected")
        items = [_text(o, i.strip() if isinstance(i, str) else i) for i in items]
        if any(not i or not _PLAIN_RE.fullmatch(i) for i in items):
            _bad(o, "names of letters, digits and . / : @ + - _ only")
        return ", ".join(items)
    v = _text(o, v)
    if o.get("choices") and v not in o["choices"]:
        _bad(o, "one of %s" % ", ".join(o["choices"]))
    return quote(v)


def validate(type_name, values):
    """Client values {option: value} -> {option: config text or None}; raises BridgeError("invalid", ...)."""
    known = options_of(type_name)
    if not isinstance(values, dict):
        raise BridgeError("invalid", "options must be an object", field="options")
    out = {}
    for key, v in values.items():
        if key in SWITCHES:
            raise BridgeError("invalid", "%s is changed only by iface.%s" % (
                key, "enable" if key == "enabled" else "outgoing"), field=key)
        if key not in known:
            raise BridgeError("invalid", "%s has no option %r" % (type_name, key), field=key)
        out[key] = to_config(known[key], v)
    return out


def missing_required(type_name, current):
    """Names of the required options not set in a section (current: {option: config value})."""
    if type_name not in TYPES or type_name in UNSUPPORTED:
        return []
    return [o["name"] for o in TYPES[type_name] if o.get("required") and current.get(o["name"]) in (None, "")]


def check_pairs(type_name, current):
    """Rules that span two options (report 4.2: the ID beacon needs both id_callsign and id_interval)."""
    if "id_callsign" in options_of(type_name) and (current.get("id_callsign") is None) != (
            current.get("id_interval") is None):
        raise BridgeError("invalid", "id_callsign and id_interval are used together (RNS ignores one alone)",
                          field="id_callsign")
