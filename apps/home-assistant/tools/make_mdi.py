#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Material Design Icons for the Home Assistant app: the font, the name table and the licence text.

    python3 apps/home-assistant/tools/make_mdi.py [--download | --font-tgz F --svg-tgz S] [--apache PATH] [--check]

Takes the npm packages @mdi/font and @mdi/svg at version 7.4.47 (the version Home Assistant's frontend uses, Apache-2.0,
by Pictogrammers), checks them against the npm registry's sha512 integrity pinned below, and writes into the app:

    root/usr/share/APPLaunch/share/home-assistant/materialdesignicons-webfont.ttf   the font, unchanged
    root/usr/share/APPLaunch/share/home-assistant/mdi-names.bin                     name -> codepoint (below)
    root/usr/share/doc/home-assistant/LICENSE.mdi                                   Pictogrammers' notice + Apache-2.0

--download fetches both tarballs from registry.npmjs.org (nothing else is contacted); --font-tgz / --svg-tgz use local
copies. --apache is the Apache-2.0 text (default /usr/share/common-licenses/Apache-2.0, Debian's copy). --check builds
everything in memory and compares it with the files in the app (exit 1 when one differs or is missing): the files are
committed so that a package builds offline, and this proves they are what the pinned packages give.

mdi-names.bin (little-endian), every icon name and every alias of meta.json, without the "mdi:" prefix (an icon's own
name wins over another icon's alias; an alias shared by several icons goes to the first of them in meta.json):
    0   "MDN1"
    4   u32 count
    8   16 bytes: the MDI version, NUL padded
    24  count x u32: offset of each entry in the blob, entries sorted by name (bytes)
    ..  blob: per entry the name, a NUL, the codepoint as 3 bytes (u24)
The app (src/core/mdi.cpp) maps the file and looks names up by binary search. No icon table of Home Assistant's
frontend is involved: the names are the font's own.
"""
import argparse
import base64
import hashlib
import io
import json
import os
import struct
import sys
import tarfile
import urllib.request

VERSION = "7.4.47"
PACKAGES = {
    "font": ("https://registry.npmjs.org/@mdi/font/-/font-%s.tgz" % VERSION,
             "sha512-43MtGpd585SNzHZPcYowu/84Vz2a2g31TvPMTm9uTiCSWzaheQySUcSyUH/46fPnuPQWof2yd0pGBtzee/IQWw=="),
    "svg": ("https://registry.npmjs.org/@mdi/svg/-/svg-%s.tgz" % VERSION,
            "sha512-WQ2gDll12T9WD34fdRFgQVgO8bag3gavrAgJ0frN4phlwdJARpE6gO1YvLEMJR0KKgoc+/Ea/A0Pp11I00xBvw=="),
}
HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
SHARE = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "home-assistant")
DOC = os.path.join(APP, "root", "usr", "share", "doc", "home-assistant")
OUT = {
    "ttf": os.path.join(SHARE, "materialdesignicons-webfont.ttf"),
    "bin": os.path.join(SHARE, "mdi-names.bin"),
    "licence": os.path.join(DOC, "LICENSE.mdi"),
}


def integrity_ok(data, expected):
    return "sha512-" + base64.b64encode(hashlib.sha512(data).digest()).decode() == expected


def fetch(which, path):
    url, expected = PACKAGES[which]
    if path:
        with open(path, "rb") as f:
            data = f.read()
    else:
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
    if not integrity_ok(data, expected):
        sys.exit("@mdi/%s %s: integrity mismatch (expected %s)" % (which, VERSION, expected))
    return data


def member(tgz, name):
    with tarfile.open(fileobj=io.BytesIO(tgz), mode="r:gz") as t:
        f = t.extractfile("package/" + name)
        if f is None:
            sys.exit("package/%s missing" % name)
        return f.read()


def names_bin(meta):
    # An icon's own name always wins; an alias that is also another icon's name, or that several icons share (meta.json
    # has about 580 such tags: "invite", "box"), goes to the first icon that has it, in meta.json's order (by name).
    table = {}
    for icon in meta:
        if icon["name"] in table:
            sys.exit("icon name %s twice" % icon["name"])
        table[icon["name"]] = int(icon["codepoint"], 16)
    for icon in meta:
        for n in icon.get("aliases") or []:
            table.setdefault(n, int(icon["codepoint"], 16))
    entries = sorted((n.encode("ascii"), cp) for n, cp in table.items())
    blob = bytearray()
    offsets = []
    for name, cp in entries:
        if b"\0" in name or cp >= 1 << 24:
            sys.exit("bad entry %r" % name)
        offsets.append(len(blob))
        blob += name + b"\0" + struct.pack("<I", cp)[:3]
    head = b"MDN1" + struct.pack("<I", len(entries)) + VERSION.encode().ljust(16, b"\0")
    return head + b"".join(struct.pack("<I", o) for o in offsets) + bytes(blob), len(entries)


def licence(pictogrammers, apache):
    # the package's LICENSE has CRLF line ends: the text file is written with LF only
    pictogrammers = pictogrammers.replace(b"\r\n", b"\n")
    apache = apache.replace(b"\r\n", b"\n")
    return (b"Material Design Icons " + VERSION.encode() + b" (the font materialdesignicons-webfont.ttf and the icon\n"
            b"names in mdi-names.bin), by Pictogrammers (https://pictogrammers.com), npm packages @mdi/font and\n"
            b"@mdi/svg, licensed under the Apache License, Version 2.0. The package's own notice, then the licence\n"
            b"text, follow. The font is shipped unchanged; mdi-names.bin is generated from @mdi/svg's meta.json by\n"
            b"apps/home-assistant/tools/make_mdi.py.\n\n" + pictogrammers.rstrip(b"\n") + b"\n\n" + b"-" * 78 +
            b"\n\n" + apache)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--download", action="store_true", help="fetch the pinned tarballs from registry.npmjs.org")
    p.add_argument("--font-tgz")
    p.add_argument("--svg-tgz")
    p.add_argument("--apache", default="/usr/share/common-licenses/Apache-2.0")
    p.add_argument("--check", action="store_true", help="compare with the files in the app instead of writing")
    a = p.parse_args()
    if not a.download and not (a.font_tgz and a.svg_tgz):
        p.error("--download, or both --font-tgz and --svg-tgz")
    font = fetch("font", None if a.download else a.font_tgz)
    svg = fetch("svg", None if a.download else a.svg_tgz)
    ttf = member(font, "fonts/materialdesignicons-webfont.ttf")
    meta = json.loads(member(svg, "meta.json").decode("utf-8"))
    binary, count = names_bin(meta)
    with open(a.apache, "rb") as f:
        apache = f.read()
    if b"Apache License" not in apache or b"Version 2.0" not in apache:
        sys.exit("%s is not the Apache-2.0 text" % a.apache)
    files = {"ttf": ttf, "bin": binary, "licence": licence(member(font, "LICENSE"), apache)}
    if a.check:
        bad = 0
        for key, data in files.items():
            try:
                with open(OUT[key], "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            print("%s %s" % ("same" if same else "DIFFERS", OUT[key]))
            bad += not same
        sys.exit(1 if bad else 0)
    for key, data in files.items():
        os.makedirs(os.path.dirname(OUT[key]), exist_ok=True)
        with open(OUT[key], "wb") as f:
            f.write(data)
        print("%s: %d bytes" % (OUT[key], len(data)))
    print("%d icons, %d names with aliases" % (len(meta), count))


if __name__ == "__main__":
    main()
