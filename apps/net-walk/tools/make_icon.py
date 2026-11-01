#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Net Walk tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/net-walk/tools/make_icon.py [--out DIR]

Writes <out>/net-walk.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
Design (own, nothing taken from a game or its icon): a computer monitor, the badge in magenta #E23BD6 (hue about 303), with
one white #FFFFFF symbol on its screen (a dark hue takes a white symbol, rule 3 of docs/ICON-PALETTE.md): a server square in
the middle with three thick cables to three small computer squares (left, right, down), the network the game wires up. Strokes
are 9 units; the monitor's foot is a bar and a base.
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from make_icons import WHITE, Canvas  # noqa: E402

BADGE = "#E23BD6"


def draw(badge, ink):
    c = Canvas()
    # the monitor: a rounded screen, a neck and a base
    c.rrect(7, 8, 93, 75, 9, badge)
    c.rrect(40, 72, 60, 86, 2, badge)
    c.rrect(26, 84, 74, 93, 4, badge)
    # the network on the screen: cables first, then the squares
    c.line([(50, 41), (22, 41)], 9, ink)
    c.line([(50, 41), (78, 41)], 9, ink)
    c.line([(50, 41), (50, 62)], 9, ink)
    c.rrect(38, 29, 62, 53, 3, ink)       # the server
    c.rrect(13, 32, 31, 50, 3, ink)       # computers
    c.rrect(69, 32, 87, 50, 3, ink)
    c.rrect(41, 56, 59, 70, 3, ink)
    # a badge-coloured light on the server, so it reads as the one that powers the rest
    c.rrect(46, 37, 54, 45, 2, badge)
    return c.finish()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/net-walk.png")
    a = ap.parse_args()
    im = draw(BADGE, WHITE)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "net-walk.png"))
        print("wrote", os.path.join(a.out, "net-walk.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "net-walk.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size)


if __name__ == "__main__":
    main()
