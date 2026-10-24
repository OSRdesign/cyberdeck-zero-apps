#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Vendors Mesh Hop's meshcore-cli bundle (report 033, packaging K1) into the package trees.

    python3 -I cli/vendor.py --cache DIR [--smoke DIR]

For each package architecture (arm64, amd64) it fetches the wheels pinned in cli/requirements.txt into DIR/<arch>
(pip download --require-hashes --no-deps --only-binary=:all:, for CPython 3.10 and later; the cache is tried first
with --no-index, so a second run needs no network), checks every wheel against its pinned sha256 again, unpacks them
(no code from them is run; EXCLUDE below is left out) and writes:
  root/opt/mesh-hop/cli/py/                 the arm64 bundle (pure Python, plus pycryptodome's aarch64 .so files)
  root.amd64/opt/mesh-hop/cli/py/           only what differs on amd64 (pycryptodome's x86-64 .so files, its RECORD
                                            and WHEEL), laid over root/ by tools/build_deb.py
  root/usr/share/doc/mesh-hop/LICENSE.<distribution>   each bundled distribution's licence files
--smoke DIR: the same bundle for this machine's architecture, with cli/mccli and cli/mccli.py, as it is installed
(DIR/opt/mesh-hop/cli), for the smoke flow of build.sh cli. Both py/ folders are emptied first (no stale files).
"""
import argparse
import hashlib
import io
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

ARCHES = {"arm64": "manylinux2014_aarch64", "amd64": "manylinux2014_x86_64"}
HOST_ARCH = {"x86_64": "amd64", "aarch64": "arm64"}
PYTHON_FLOOR = "3.10"                # meshcore-cli and prompt_toolkit need 3.10; app.json depends: python3 (>= 3.10)
BUNDLE = "opt/mesh-hop/cli"          # in the package
LICENCE_NAMES = ("LICENSE", "LICENCE", "COPYING", "NOTICE", "AUTHORS")
# Left out of the unpacked wheels: pycryptodome's own test suite (1.5 MB, 108 files), never imported at run time; its
# test vectors also trip tools/branding/check-branding.sh. Everything else of every wheel is kept as is.
EXCLUDE = ("Crypto/SelfTest/",)


def norm(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def read_pins(path):
    """[(normalized name, version, {sha256, ...})] from a requirements file in hash-checking form."""
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


def fetch(req, cache, plat):
    os.makedirs(cache, exist_ok=True)
    base = [sys.executable, "-I", "-m", "pip", "download", "--quiet", "--disable-pip-version-check", "--no-deps",
            "--only-binary=:all:", "--require-hashes", "--platform", plat, "--python-version", PYTHON_FLOOR,
            "--implementation", "cp", "-r", req, "-d", cache]
    if subprocess.call(base + ["--no-index", "--find-links", cache], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL) == 0:
        return "cache"
    subprocess.check_call(base)
    return "PyPI"


def pick_wheels(pins, cache):
    """The one wheel of the cache for each pin, its hash checked again."""
    files = [f for f in sorted(os.listdir(cache)) if f.endswith(".whl")]
    out = []
    for name, version, hashes in pins:
        found = []
        for f in files:
            parts = f[:-4].split("-")
            if len(parts) >= 5 and norm(parts[0]) == name and parts[1] == version:
                path = os.path.join(cache, f)
                if sha256(path) in hashes:
                    found.append(path)
        if len(found) != 1:
            sys.exit("%s: %d wheel(s) of %s==%s with a pinned hash" % (cache, len(found), name, version))
        out.append(found[0])
    return out


def unpack(wheel, dest):
    with zipfile.ZipFile(wheel) as z:
        for info in z.infolist():
            name = info.filename
            if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                sys.exit("%s: unsafe path %s" % (wheel, name))
            if stat.S_ISLNK(info.external_attr >> 16):
                sys.exit("%s: symbolic link %s" % (wheel, name))
            top = name.split("/", 1)[0]
            if top.endswith(".data"):
                sys.exit("%s: a .data folder (%s) is not handled" % (wheel, name))
            if name.endswith("/") or name.startswith(EXCLUDE):
                continue
            target = os.path.join(dest, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)


def tree(root):
    out = {}
    for base, _, names in os.walk(root):
        for n in names:
            p = os.path.join(base, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = p
    return out


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def same(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        return fa.read() == fb.read()


def reset(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)


def copy_files(files, src_root, dst_root):
    for rel in files:
        target = os.path.join(dst_root, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(os.path.join(src_root, rel), target)


def licences(staged):
    """{distribution name: text} from each dist-info: licenses/ (PEP 639) and LICENSE*, COPYING*... files."""
    out = {}
    for d in sorted(os.listdir(staged)):
        if not d.endswith(".dist-info"):
            continue
        dist = d[:-len(".dist-info")].rsplit("-", 1)[0]
        parts = []
        for rel, path in sorted(tree(os.path.join(staged, d)).items()):
            base = rel.rsplit("/", 1)[-1].upper()
            if rel.startswith("licenses/") or base.startswith(LICENCE_NAMES):
                parts.append((rel.rsplit("/", 1)[-1], open(path, encoding="utf-8", errors="replace").read()))
        if not parts:
            sys.exit("%s: no licence file" % d)
        text = "".join(("" if len(parts) == 1 else "==> %s <==\n" % n) + t.rstrip() + "\n\n" for n, t in parts)
        out[norm(dist)] = text.rstrip() + "\n"
    return out


def size(root):
    return sum(os.path.getsize(p) for p in tree(root).values()) if os.path.isdir(root) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", required=True, help="wheel cache (one folder per architecture)")
    ap.add_argument("--smoke", help="also install the bundle for this machine's architecture here (DIR/opt/...)")
    a = ap.parse_args()
    cli = os.path.dirname(os.path.abspath(__file__))
    app = os.path.dirname(cli)
    req = os.path.join(cli, "requirements.txt")
    pins = read_pins(req)

    work = tempfile.mkdtemp(prefix="mesh-hop-vendor-")
    try:
        staged = {}
        for arch, plat in ARCHES.items():
            cache = os.path.join(a.cache, arch)
            where = fetch(req, cache, plat)
            wheels = pick_wheels(pins, cache)
            staged[arch] = os.path.join(work, arch)
            for w in wheels:
                unpack(w, staged[arch])
            print("== cli bundle %s: %d wheels from %s, hashes checked: %s" % (
                arch, len(wheels), where, " ".join(os.path.basename(w) for w in wheels)))

        arm, amd = tree(staged["arm64"]), tree(staged["amd64"])
        stray = sorted(r for r in set(arm) - set(amd) if is_elf(arm[r]))
        if stray:
            sys.exit("aarch64 files with no amd64 replacement (the overlay cannot remove them): " + ", ".join(stray))
        overlay = sorted(r for r in amd if r not in arm or not same(arm[r], amd[r]))
        if any(not is_elf(amd[r]) and not r.split("/", 1)[0].endswith(".dist-info") for r in overlay):
            sys.exit("amd64 differs in non-binary files: " + ", ".join(overlay))

        py_arm = os.path.join(app, "root", BUNDLE, "py")
        py_amd = os.path.join(app, "root.amd64", BUNDLE, "py")
        reset(py_arm)
        copy_files(sorted(arm), staged["arm64"], py_arm)
        reset(py_amd)
        copy_files(overlay, staged["amd64"], py_amd)

        doc = os.path.join(app, "root", "usr", "share", "doc", "mesh-hop")
        for f in os.listdir(doc):
            if f.startswith("LICENSE."):
                os.remove(os.path.join(doc, f))
        lic = licences(staged["arm64"])
        for dist, text in lic.items():
            with open(os.path.join(doc, "LICENSE." + dist), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        print("== %s: %d files, %.2f MB (arm64)" % (py_arm, len(arm), size(py_arm) / 1e6))
        print("== %s: %d files, %.2f MB (laid over it in the amd64 package)" % (py_amd, len(overlay), size(py_amd) / 1e6))
        print("== %s: LICENSE.%s" % (doc, " LICENSE.".join(sorted(lic))))

        if a.smoke:
            host = HOST_ARCH.get(platform.machine())
            if not host:
                sys.exit("--smoke: no bundle for " + platform.machine())
            dest = os.path.join(a.smoke, BUNDLE)
            reset(dest)
            copy_files(sorted(tree(staged[host])), staged[host], os.path.join(dest, "py"))
            for f in ("mccli", "mccli.py"):
                shutil.copyfile(os.path.join(app, "root", BUNDLE, f), os.path.join(dest, f))
            os.chmod(os.path.join(dest, "mccli"), 0o755)
            # left writable on purpose: the smoke flow checks that no __pycache__ appears in it (a read-only folder
            # would hide a write, Python skips a failed .pyc write silently)
            print("== smoke bundle (%s): %s" % (host, dest))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
