# SPDX-License-Identifier: MIT
"""The Reticulum configuration file: where it is, the first-run file, and surgical edits of [interfaces] sections.

Rules (report 057 5.3): the standard location (RNS's search order, ~/.reticulum when none exists); a first configuration
only when none exists, never over an existing one; every change re-reads the file (hand edits made while the bridge
runs are kept), touches only the lines of one [[section]], is checked by parsing the result with the same ConfigObj
parser RNS uses, and is written atomically after a backup copy (config.bak).
"""
import os
import re
import shutil

from . import BridgeError

SEARCH = ("/etc/reticulum", "~/.config/reticulum", "~/.reticulum")    # RNS's own order (manual, "Using Reticulum")

# The first configuration (owner's rules, D7-D9): shared instance, no transport, log level 3 (notices), and the only
# interface RNS would otherwise create by itself, present but switched off. Other interfaces are added by the user.
FIRST_RUN = """\
# Reticulum configuration, first written by the CyberdeckZero reticulum bridge because none existed.
# Nothing transmits until an interface is switched on (enabled = yes) and allowed to send (outgoing = True).
# Hand edits are kept; the bridge changes only the [[sections]] you change through it. Reference: rnsd --exampleconfig

[reticulum]
  enable_transport = No
  share_instance = Yes

[logging]
  loglevel = 3

[interfaces]
  [[Default Interface]]
    type = AutoInterface
    enabled = no
    outgoing = False
"""

_HDR = re.compile(r"^\s*(\[+)\s*(.*?)\s*(\]+)\s*(#.*)?$")
_KEY = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=")


def resolve_dir(explicit=None):
    """The config folder: an explicit one, else the first of RNS's search order that holds a config, else
    ~/.reticulum."""
    if explicit:
        return os.path.abspath(os.path.expanduser(explicit))
    for d in SEARCH:
        d = os.path.expanduser(d)
        if os.path.isfile(os.path.join(d, "config")):
            return d
    return os.path.expanduser("~/.reticulum")


def ensure_first_run(config_dir):
    """Writes FIRST_RUN when the folder has no config; never touches an existing one. True when it wrote."""
    os.makedirs(config_dir, mode=0o700, exist_ok=True)
    path = os.path.join(config_dir, "config")
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)     # O_EXCL: an existing file always wins
    except FileExistsError:
        return False
    with os.fdopen(fd, "w") as f:
        f.write(FIRST_RUN)
        f.flush()
        os.fsync(f.fileno())
    return True


def parse(text):
    """The text as RNS reads it: RNS's vendored ConfigObj (falls back to a system configobj)."""
    try:
        from RNS.vendor.configobj import ConfigObj
    except ImportError:                                  # pragma: no cover
        from configobj import ConfigObj
    try:
        return ConfigObj(text.splitlines())
    except Exception as e:
        raise BridgeError("config_error", "the configuration does not parse: %s" % e)


def _header(line):
    m = _HDR.match(line)
    if not m or len(m.group(1)) != len(m.group(3)):
        return None
    name = m.group(2)
    if len(name) > 1 and name[0] == name[-1] and name[0] in "'\"":
        name = name[1:-1]
    return len(m.group(1)), name


def _interfaces_span(lines):
    """(first line after [interfaces], end) or None when the file has no [interfaces] section."""
    start = None
    for i, line in enumerate(lines):
        h = _header(line)
        if h and h[0] == 1:
            if start is not None:
                return start, i
            if h[1] == "interfaces":
                start = i + 1
    return (start, len(lines)) if start is not None else None


def _section_span(lines, name):
    """(header index, end index) of [[name]] inside [interfaces]; end is after its last key or sub-section line."""
    span = _interfaces_span(lines)
    if span:
        for i in range(*span):
            h = _header(lines[i])
            if h and h[0] == 2 and h[1] == name:
                end = last = i + 1
                while end < span[1]:
                    h2 = _header(lines[end])
                    if h2 and h2[0] <= 2:
                        break
                    if lines[end].strip() and not lines[end].lstrip().startswith("#"):
                        last = end + 1
                    end += 1
                return i, last
    raise BridgeError("not_found", "no interface named %r" % name)


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        raise BridgeError("config_error", "no configuration at %s" % path)


def interfaces(text):
    """{name: {option: str or list, ...}} of the [interfaces] section; sub-sections reported by name only."""
    out = {}
    for name, sec in (parse(text).get("interfaces") or {}).items():
        if hasattr(sec, "items"):
            out[name] = {k: (dict(v) if hasattr(v, "items") else v) for k, v in sec.items()}
    return out


def _indent(lines, i, default):
    m = re.match(r"^(\s*)", lines[i]) if i < len(lines) else None
    return m.group(1) if m and m.group(1) else default


def add_section(text, name, items):
    """Appends [[name]] with items [(key, config text)] at the end of [interfaces] (creating it if absent)."""
    lines = text.splitlines()
    if name in interfaces(text):
        raise BridgeError("exists", "an interface named %r exists" % name)
    span = _interfaces_span(lines)
    if span is None:
        lines += ["", "[interfaces]"]
        span = (len(lines), len(lines))
    at = span[1]
    while at > span[0] and not lines[at - 1].strip():
        at -= 1
    new = ["", "  [[%s]]" % name] + ["    %s = %s" % kv for kv in items]
    lines[at:at] = new
    return _finish(lines, name, dict(items))


def edit_section(text, name, changes):
    """Changes {key: config text or None (remove)} inside [[name]] only; other lines stay as they are."""
    lines = text.splitlines()
    start, end = _section_span(lines, name)
    block = lines[start:end]
    if any("'''" in ln or '"""' in ln for ln in block):
        raise BridgeError("not_supported", "%r has multi-line values: edit the configuration by hand" % name)
    sub = next((j for j in range(1, len(block)) if (_header(block[j]) or (0,))[0] >= 3), len(block))
    indent = _indent(lines, start + 1, "    ") if end > start + 1 else "    "
    for key, val in changes.items():
        hit = [j for j in range(1, sub) if _KEY.match(block[j]) and _KEY.match(block[j]).group(1) == key]
        if val is None:
            for j in reversed(hit):
                del block[j]
                sub -= 1
        elif hit:
            block[hit[0]] = "%s%s = %s" % (re.match(r"^(\s*)", block[hit[0]]).group(1), key, val)
            for j in reversed(hit[1:]):
                del block[j]
                sub -= 1
        else:
            block.insert(sub, "%s%s = %s" % (indent, key, val))
            sub += 1
    lines[start:end] = block
    return _finish(lines, name, changes)


def remove_section(text, name):
    lines = text.splitlines()
    start, end = _section_span(lines, name)
    while end < len(lines) and not lines[end].strip():
        end += 1
    del lines[start:end]
    return _finish(lines, name, None)


def _flat(v):
    return ", ".join(v) if isinstance(v, list) else v


def _finish(lines, name, expect):
    """Joins the lines and checks the result as RNS will read it: the intended values, or the section gone."""
    text = "\n".join(lines) + "\n"
    got = interfaces(text)
    if expect is None:
        if name in got:
            raise BridgeError("config_error", "%r is still present after removal" % name)
        return text
    sec = got.get(name) or {}
    for key, val in expect.items():
        want = None if val is None else _flat(parse("x = " + val).get("x"))
        if _flat(sec.get(key)) != want:
            raise BridgeError("config_error", "%r: %s would read back as %r, not %r" % (
                name, key, _flat(sec.get(key)), want))
    return text


def write_atomic(path, text):
    """Backup copy (path.bak), then temporary file + fsync + rename; keeps the file's mode (0600 for a new one)."""
    folder = os.path.dirname(path)
    mode = 0o600
    if os.path.exists(path):
        mode = os.stat(path).st_mode & 0o777
        shutil.copy2(path, path + ".bak")
    tmp = os.path.join(folder, ".config.%d.tmp" % os.getpid())
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    dfd = os.open(folder, os.O_RDONLY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)
