#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the mmWave Radar tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/mmwave-radar/tools/make_icon.py [--out DIR] [--sheet PNG]

Writes <out>/mmwave-radar.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
--sheet writes a check picture: the icon at 100, 64 and 32 px and in greyscale next to its nearest neighbours on the
hue wheel (Mesh Hop, Net Walk) and the other radar-like badge (LAN Scan).
Design (own): the badge is a quarter disc, a radar's sweep from the lower left corner, in violet #C94CFF (hue about
282, the middle of the 22-degree gap between Mesh Hop's purple 260 and Net Walk's magenta 304; told apart from both by
the silhouette, a fan and not a hexagon or a monitor); one white symbol (rule 3: a dark hue takes white): two thick
range arcs and a target dot between them. Strokes are 9 units.
"""
import argparse
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from make_icons import WHITE, Canvas, polar  # noqa: E402

BADGE = "#C94CFF"
CX, CY, R = 9, 91, 84  # the sweep's corner and radius (100-unit coordinates)


def draw_sweep(badge, ink):
    """The brief's first idea: a quarter sweep with two arcs and a dot (kept for the sheet: it reads as RSS Reader's
    feed symbol, a dot and two quarter arcs from the lower left)."""
    c = Canvas()
    fan = [(CX, CY)] + [polar(CX, CY, R, a) for a in range(-90, 1, 3)]
    c.polygon(fan, badge, round_r=5)
    c.arc(CX, CY, 33, 9, -78, -12, ink)
    c.arc(CX, CY, 62, 9, -80, -10, ink)
    x, y = polar(CX, CY, 47.5, -45)
    c.circle(x, y, 7.5, ink)
    return c.finish()


def draw(badge, ink):
    """An arch (a radar screen's dome) with the app's own plot on it: the field-of-view wedge from the sensor at the
    bottom centre, two range arcs across it and a target dot between them."""
    c = Canvas()
    c.circle(50, 48, 41, badge)
    c.rrect(9, 48, 91, 92, 9, badge)
    ax, ay = 50, 82
    for a in (-121, -59):
        c.line([(ax, ay), polar(ax, ay, 58, a)], 9, ink)
    c.arc(ax, ay, 27, 9, -116, -64, ink)
    c.arc(ax, ay, 56, 9, -118, -62, ink)
    x, y = polar(ax, ay, 41.5, -78)
    c.circle(x, y, 7, ink)
    return c.finish()


def sheet(path, icon):
    from PIL import Image, ImageOps

    others = [os.path.join(REPO, "apps", a, "icon.png") for a in ("mesh-hop", "net-walk", "lanscan", "rss-reader", "home-assistant")]
    ims = [icon, draw_sweep(BADGE, WHITE)] + [Image.open(p).convert("RGBA") for p in others if os.path.exists(p)]
    W = 8 + len(ims) * 108
    out = Image.new("RGB", (W, 4 * 108 + 8), (0x1E, 0x1E, 0x1E))
    for i, im in enumerate(ims):
        x = 8 + i * 108
        out.paste(im, (x, 8), im)
        s64 = im.resize((64, 64), Image.LANCZOS)
        out.paste(s64, (x, 116), s64)
        s32 = im.resize((32, 32), Image.LANCZOS)
        out.paste(s32, (x, 224), s32)
        g = ImageOps.grayscale(im.convert("RGB")).convert("RGB")
        bg = Image.new("RGB", im.size, (0x1E, 0x1E, 0x1E))
        bg.paste(g, (0, 0), im)
        out.paste(bg, (x, 332))
    out.save(path)
    print("wrote", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/mmwave-radar.png")
    ap.add_argument("--sheet", default=None, help="also write the check picture to this PNG")
    a = ap.parse_args()
    im = draw(BADGE, WHITE)
    alpha = im.getchannel("A")
    cover = sum(1 for v in alpha.getdata() if v >= 128) / float(im.size[0] * im.size[1])
    print("ink coverage %.0f %%" % (cover * 100))
    if a.sheet:
        sheet(a.sheet, im)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "mmwave-radar.png"))
        print("wrote", os.path.join(a.out, "mmwave-radar.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "mmwave-radar.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size)


if __name__ == "__main__":
    main()
