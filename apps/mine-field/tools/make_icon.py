#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Mine Field tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/mine-field/tools/make_icon.py [--out DIR]

Writes <out>/mine-field.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
Design (own, nothing taken from a game or its icon): a square badge cut into four tiles by a gap of the tile's own dark
background, the top right corner snipped off. The badge is violet-blue #5B7CFF (hue about 228, kept off the green/yellow
tiles of the board); the only other colour is the white symbol (white on dark/mid hues, docs/ICON-PALETTE.md). A bold flag
(pole, pennant, base) sits on the top left tile and a bold figure "1", drawn as a polygon, on the bottom right one: the
two things you read on the board. Strokes are 9 units or more except the flag's pole (7).
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

BADGE = "#5B7CFF"


def draw(badge, ink):
    c = Canvas()
    x0, y0, x1, y1, gap, r = 7.0, 7.0, 93.0, 93.0, 8.0, 7.0
    mid = (x0 + x1) / 2
    a0, a1 = mid - gap / 2, mid + gap / 2
    # four tiles: top left, top right (corner snipped), bottom left, bottom right
    c.rrect(x0, y0, a0, a0, r, badge)
    c.polygon([(a1, y0), (x1 - 14, y0), (x1, y0 + 14), (x1, a0), (a1, a0)], badge, round_r=3)
    c.rrect(x0, a1, a0, y1, r, badge)
    c.rrect(a1, a1, x1, y1, r, badge)
    # the flag on the top left tile: a pole, a pennant to the right, a base
    c.rrect(18, 13, 25, 39, 1.5, ink)
    c.polygon([(24, 13), (40, 20.5), (24, 28)], ink)
    c.rrect(13, 37, 32, 42, 2, ink)
    # a bold "1" on the bottom right tile: a stem with a flag-shaped top, and a foot
    c.polygon([(66, 62), (77, 58), (77, 83), (66, 83)], ink)
    c.rrect(62, 82, 82, 88, 2.5, ink)
    return c.finish()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/mine-field.png")
    a = ap.parse_args()
    im = draw(BADGE, WHITE)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "mine-field.png"))
        print("wrote", os.path.join(a.out, "mine-field.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "mine-field.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size)


if __name__ == "__main__":
    main()
