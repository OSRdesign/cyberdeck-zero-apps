#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Klondike Solitaire tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/klondike-solitaire/tools/make_icon.py [--out DIR]

Writes <out>/klondike-solitaire.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
Design (own, nothing taken from a card set or a game): two playing cards fanned, the back one tilted left, the front one
right with a bold spade in ink on it. The badge colour is ivory #F3F0E3, a very light neutral: every free hue of the wheel
is within about 30 degrees of a neighbour (and green is reserved for "OK"), so the app wins on lightness and silhouette
(docs/ICON-PALETTE.md, "Choosing a new hue", rule 4); the only other colour is the ink. A thin gap (the tile's own dark
background) separates the front card from the back one, so one colour is enough. The badge sits inside the 5 px margin
(x 5-95) and in the upper two thirds (y 6-68), like Markdown Notes: the two-line label under the tile never touches it.
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from PIL import Image, ImageDraw  # noqa: E402
from make_icons import INK, N, SS, Canvas  # noqa: E402

IVORY = "#F3F0E3"


def rrect_mask(c, cx, cy, w, h, r, angle):
    """A rounded rectangle centred on (cx, cy), turned by `angle` degrees (clockwise on the screen), as an 8x mask."""
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).rounded_rectangle([(cx - w / 2) * SS, (cy - h / 2) * SS, (cx + w / 2) * SS, (cy + h / 2) * SS],
                                        radius=r * SS, fill=255)
    return m.rotate(-angle, resample=Image.BICUBIC, center=(cx * SS, cy * SS))


def spade_mask(c, cx, cy, size, angle):
    """A bold spade (a pointed body over two lobes, a short flared stem) about `size` units high, turned with the card."""
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)
    s = size

    def P(x, y):
        return ((cx + x * s) * SS, (cy + y * s) * SS)

    r = 0.36 * s
    for sx in (-1, 1):  # the lobes
        x, y = cx + sx * 0.34 * s, cy + 0.10 * s
        d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS], fill=255)
    d.polygon([P(0, -0.95), P(-0.69, 0.18), P(0.69, 0.18)], fill=255)          # the body, its point up
    d.polygon([P(-0.10, 0.25), P(0.10, 0.25), P(0.30, 0.85), P(-0.30, 0.85)], fill=255)  # the stem
    m = c.round_mask(m, 0.7)  # soften the point and the joins a little
    return m.rotate(-angle, resample=Image.BICUBIC, center=(cx * SS, cy * SS))


def icon_klondike(ivory, ink):
    c = Canvas()
    w, h, r = 40, 56, 5.5
    # the back card, tilted left
    back = rrect_mask(c, 38, 38, w, h, r, -16)
    c.mask_fill(back, ivory)
    # the gap, then the front card, tilted right, with its spade
    gap = rrect_mask(c, 60, 37, w + 7, h + 7, r + 3, 11)
    c.img.paste((0, 0, 0, 0), (0, 0), gap)
    front = rrect_mask(c, 60, 37, w, h, r, 11)
    c.mask_fill(front, ivory)
    c.mask_fill(spade_mask(c, 60, 40, 22, 11), ink)
    return c.finish()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/klondike-solitaire.png")
    a = ap.parse_args()
    im = icon_klondike(IVORY, INK)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "klondike-solitaire.png"))
        print("wrote", os.path.join(a.out, "klondike-solitaire.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "klondike-solitaire.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size)


if __name__ == "__main__":
    main()
