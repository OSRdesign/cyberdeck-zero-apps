#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stages the files of the `reticulum` package that come from elsewhere (report 057, P1a part 2).

    python3 -I tools/vendor.py --cache DIR [--check]

* Fetches the two wheels pinned in bridge/requirements.txt into DIR (pip download --require-hashes --no-deps
  --only-binary=:all:; DIR is tried first with --no-index, so a second run needs no network), checks each file against
  its pinned sha256 again, and requires `py3-none-any` (pure Python: one tree serves arm64 and amd64, so there is no
  root.amd64/). No code of a wheel is run.
* Unpacks them unmodified into root/opt/reticulum/py/ (nothing left out; no symbolic links, no .data folder).
* Writes root/usr/share/doc/reticulum/LICENSE.rns and LICENSE.lxmf: the Reticulum License text, taken from the leading
  comment block of RNS/__init__.py and LXMF/Utilities/lxmd.py of the wheels themselves (the wheels carry no separate
  licence file), the comment marks removed, nothing retyped.
* Copies our bridge (bridge/reticulum_bridge/, without __pycache__) into root/opt/reticulum/bridge/reticulum_bridge/.

--check writes nothing: it stages the same tree in a temporary folder and exits 1 when the committed root/ differs
(a changed pin, a bridge edit not yet copied, a stray file such as a __pycache__).
"""
import argparse
import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REQ = os.path.join(APP, "bridge", "requirements.txt")
BRIDGE_SRC = os.path.join(APP, "bridge", "reticulum_bridge")
ROOT = os.path.join(APP, "root")
PY = "opt/reticulum/py"
BRIDGE = "opt/reticulum/bridge/reticulum_bridge"
DOC = "usr/share/doc/reticulum"
# the file of each wheel whose leading comment block is the licence text
LICENCE_SOURCE = {"rns": ("RNS/__init__.py", "LICENSE.rns"), "lxmf": ("LXMF/Utilities/lxmd.py", "LICENSE.lxmf")}
OWNED = (PY, BRIDGE, DOC + "/LICENSE.rns", DOC + "/LICENSE.lxmf")   # what this script writes; the rest of root/ is by hand


def norm(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def read_pins(path):
    text = re.sub(r"\\\n", " ", open(path, encoding="utf-8").read())
    pins = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"([A-Za-z0-9_.-]+)==(\S+)\s+(.*)$", line)
        hashes = set(re.findall(r"--hash=sha256:([0-9a-f]{64})", line))
        if not m or not hashes:
            sys.exit("%s: not an exact pin with hashes: %s" % (path, line))
        pins.append((norm(m.group(1)), m.group(2), hashes))
    return pins


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(cache):
    os.makedirs(cache, exist_ok=True)
    base = [sys.executable, "-I", "-m", "pip", "download", "--quiet", "--disable-pip-version-check", "--no-deps",
            "--only-binary=:all:", "--require-hashes", "-r", REQ, "-d", cache]
    if subprocess.call(base + ["--no-index", "--find-links", cache], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL) == 0:
        return "cache"
    subprocess.check_call(base)
    return "PyPI"


def pick_wheels(pins, cache):
    out = []
    for name, version, hashes in pins:
        found = []
        for f in sorted(os.listdir(cache)):
            if not f.endswith(".whl"):
                continue
            parts = f[:-4].split("-")
            if len(parts) >= 5 and norm(parts[0]) == name and parts[1] == version and sha256(os.path.join(cache, f)) in hashes:
                found.append(f)
        if len(found) != 1:
            sys.exit("%s: %d wheel(s) of %s==%s with a pinned hash" % (cache, len(found), name, version))
        if not found[0].endswith("-py3-none-any.whl"):
            sys.exit("%s is not a pure-Python wheel: the package would need an overlay per architecture" % found[0])
        out.append((name, os.path.join(cache, found[0])))
    return out


def unpack(wheel, dest):
    with zipfile.ZipFile(wheel) as z:
        for info in z.infolist():
            name = info.filename
            if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                sys.exit("%s: unsafe path %s" % (wheel, name))
            if stat.S_ISLNK(info.external_attr >> 16):
                sys.exit("%s: symbolic link %s" % (wheel, name))
            if name.split("/", 1)[0].endswith(".data"):
                sys.exit("%s: a .data folder (%s) is not handled" % (wheel, name))
            if name.endswith("/"):
                continue
            target = os.path.join(dest, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)


def licence_text(py_dir, rel):
    """The leading '#' comment block of a file (after a shebang and blank lines), the '# ' removed."""
    lines = open(os.path.join(py_dir, rel), encoding="utf-8").read().splitlines()
    i = 0
    while i < len(lines) and (lines[i].startswith("#!") or not lines[i].strip()):
        i += 1
    block = []
    while i < len(lines) and lines[i].startswith("#"):
        block.append(re.sub(r"^# ?", "", lines[i]))
        i += 1
    text = "\n".join(block).rstrip() + "\n"
    for must in ("Reticulum License", "Mark Qvist", "Permission is hereby granted", "harm to human beings",
                 "language model training", "THE SOFTWARE IS PROVIDED"):
        if must not in text:
            sys.exit("%s: the licence block lacks %r" % (rel, must))
    return text


def stage(dest, cache):
    pins = read_pins(REQ)
    where = fetch(cache)
    wheels = pick_wheels(pins, cache)
    py = os.path.join(dest, PY)
    os.makedirs(py)
    for _, w in wheels:
        unpack(w, py)
    doc = os.path.join(dest, DOC)
    os.makedirs(doc)
    for name, _ in wheels:
        rel, out = LICENCE_SOURCE[name]
        with open(os.path.join(doc, out), "w", encoding="utf-8", newline="\n") as f:
            f.write(licence_text(py, rel))
    shutil.copytree(BRIDGE_SRC, os.path.join(dest, BRIDGE), ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    return where, [os.path.basename(w) for _, w in wheels]


def tree(root):
    out = {}
    for base, _, names in os.walk(root):
        for n in names:
            p = os.path.join(base, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = p
    return out


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", required=True, help="wheel cache folder (outside the repository)")
    ap.add_argument("--check", action="store_true", help="compare, write nothing; exit 1 on a difference")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="reticulum-vendor-")
    try:
        staged = os.path.join(work, "root")
        where, names = stage(staged, a.cache)
        print("== %d wheels from %s, hashes checked: %s" % (len(names), where, " ".join(names)))
        new = tree(staged)
        old = {r: p for r, p in tree(ROOT).items() if any(r == o or r.startswith(o + "/") for o in OWNED)}
        if a.check:
            bad = sorted(r for r in set(new) | set(old) if r not in new or r not in old or read(new[r]) != read(old[r]))
            for r in bad:
                print("DIFFERS: " + r)
            print("== check: %s" % ("OK, %d files" % len(new) if not bad else "%d file(s) differ" % len(bad)))
            sys.exit(1 if bad else 0)
        for o in OWNED:
            p = os.path.join(ROOT, o)
            if os.path.isdir(p):
                shutil.rmtree(p)
            elif os.path.isfile(p):
                os.remove(p)
        for r in sorted(new):
            t = os.path.join(ROOT, r)
            os.makedirs(os.path.dirname(t), exist_ok=True)
            shutil.copyfile(new[r], t)
        print("== wrote %d files under %s" % (len(new), ROOT))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
