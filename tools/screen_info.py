#!/usr/bin/env python3
"""Screen requirements of an app, as the launcher computes them from its .desktop (shared by make_registry.py and
check_registry.py).

The launcher (projects/APPLaunch/main/ui/desktop_entry.cpp, parse_desktop_min_size) reads two optional keys of the
packaged .desktop, both "WxH" (x or X, decimal, each 1..65535, spaces around the separator allowed; anything else is
treated as absent):
  X-MinSize=WxH   the smallest LANDSCAPE logical screen the app supports. On a smaller screen (either dimension) the
                  tile is hidden. On a portrait launcher a fullscreen app is checked against the panel seen in landscape.
  X-Portrait=WxH  the smallest canvas the app supports on a PORTRAIT launcher (a size, not a flag). A fullscreen app that
                  declares it and fits gets the portrait canvas; otherwise it runs turned (landscape).
The registry carries the same values, normalised to "WxH" (lower-case x, no spaces, no leading zeros):
  "min_size": "WxH"           only when the app has an X-MinSize (else: no field, the app fits every screen)
  "portrait_min_size": "WxH"  only when the app has an X-Portrait
An app.json "min_size": "WxH" fills "min_size" when the .desktop has no X-MinSize (the .desktop wins when both exist).
"""
import os


def parse_size(value):
    """"WxH" as the launcher parses it -> "WxH" normalised, or None when it would be ignored (malformed)."""
    if not isinstance(value, str):
        return None
    pos = -1
    for index, char in enumerate(value):
        if char in "xX":
            pos = index
            break
    if pos < 0:
        return None
    dims = []
    for part in (value[:pos], value[pos + 1:]):
        part = part.strip(" \t")
        if not part or len(part) > 5 or not all(c in "0123456789" for c in part):
            return None
        number = int(part)
        if number < 1 or number > 65535:
            return None
        dims.append(number)
    return "%dx%d" % tuple(dims)


def parse_desktop(text):
    """The keys of the [Desktop Entry] group of a .desktop text (the last value of a repeated key wins)."""
    keys, inside = {}, False
    for line in text.splitlines():
        if not line or line[0] in "#;":
            continue
        if line[0] == "[":
            inside = line == "[Desktop Entry]"
            continue
        if inside and "=" in line:
            key, value = line.split("=", 1)
            keys[key.strip()] = value.strip()
    return keys


def desktop_files(app_dir, arch):
    """Packaged .desktop files of an app for an architecture: root/ with root.<arch>/ laid over it, by file name."""
    found = {}
    for base in (os.path.join(app_dir, "root"), os.path.join(app_dir, "root." + arch)):
        folder = os.path.join(base, "usr", "share", "APPLaunch", "applications")
        if os.path.isdir(folder):
            for name in sorted(os.listdir(folder)):
                if name.endswith(".desktop"):
                    found[name] = os.path.join(folder, name)
    return [found[name] for name in sorted(found)]


def desktop_values(app_dir, arch="arm64"):
    """(raw X-MinSize, raw X-Portrait) of the app's .desktop; None for an absent key."""
    raw_min = raw_portrait = None
    for path in desktop_files(app_dir, arch):
        with open(path, encoding="utf-8") as handle:
            keys = parse_desktop(handle.read())
        raw_min = raw_min if raw_min is not None else keys.get("X-MinSize")
        raw_portrait = raw_portrait if raw_portrait is not None else keys.get("X-Portrait")
    return raw_min, raw_portrait


def screen_fields(app_dir, meta, arch="arm64"):
    """The registry fields of an app: {} or {"min_size": ...} plus optionally "portrait_min_size"."""
    raw_min, raw_portrait = desktop_values(app_dir, arch)
    fields = {}
    min_size = parse_size(raw_min) or parse_size(meta.get("min_size"))
    if min_size:
        fields["min_size"] = min_size
    portrait = parse_size(raw_portrait)
    if portrait:
        fields["portrait_min_size"] = portrait
    return fields
