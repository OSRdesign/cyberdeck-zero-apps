#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Cassette tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/cassette/tools/make_icon.py [--out DIR]

Writes <out>/cassette.png (default: the app folder's icon.png, and the package copy under root/ when that folder
exists). Needs Pillow.
Design (own, nothing taken from a product): a compact cassette seen from the front. The badge is the cassette body in
sand #D9C99C with a thick ink label bar on top and an ink pill window in the lower half that holds the two hubs (sand
discs with an ink centre). No hue is free on the wheel (docs/ICON-PALETTE.md, "Choosing a new hue", rule 4), so the
app wins on silhouette (a wide rounded body with a pill window and two discs) and on a warm mid-light neutral between
Klondike's ivory and the dark tile; the only other colour is the ink. The badge sits inside the 5 px margin and in the
upper two thirds (y 12-68) so the two-line label under the tile never touches it.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from PIL import Image, ImageDraw  # noqa: E402
from make_icons import INK, N, SS, Canvas  # noqa: E402

SAND = "#D9C99C"


def rrect(x0, y0, x1, y1, r):
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=r * SS, fill=255)
    return m


def disc(cx, cy, r):
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=255)
    return m


def icon_cassette(sand, ink):
    c = Canvas()
    c.mask_fill(rrect(5, 12, 95, 68, 8), sand)              # the body
    c.mask_fill(rrect(16, 19, 84, 31, 4), ink)               # the label bar, 12 units thick
    c.mask_fill(rrect(21, 38, 79, 61, 11.5), ink)            # the pill window
    for cx in (37, 63):                                      # the two hubs
        c.mask_fill(disc(cx, 49.5, 8.5), sand)
        c.mask_fill(disc(cx, 49.5, 3.4), ink)
    return c.finish()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/cassette.png")
    a = ap.parse_args()
    im = icon_cassette(SAND, INK)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "cassette.png"))
        return
    im.save(os.path.join(APP, "icon.png"))
    img_dir = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images")
    if os.path.isdir(img_dir):
        im.save(os.path.join(img_dir, "cassette.png"))


if __name__ == "__main__":
    main()
