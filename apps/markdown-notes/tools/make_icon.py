#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Markdown Notes tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/markdown-notes/tools/make_icon.py [--out DIR]

Writes <out>/markdown-notes.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
The function below is meant to move into tools/icons/make_icons.py (ICONS) with its palette row when the app is
published: lemon #CBF22C (hue about 72, the free yellow-lime gap), ink symbol, badge an open book (two pages, the
spine dipping at the top and the bottom), symbol a bold hash (Markdown's heading mark).
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from make_icons import INK, Canvas  # noqa: E402

HUE = "#CBF22C"


def icon_markdown_notes(hue, ink):
    """An open book (two pages, the spine lower than the outer corners) with a bold hash on it.

    0.1.1: inside the 5 px margin (x 5-95) and in the upper two thirds (y 6-66): on the home grid the tile's label sits
    under the icon box, and a label wrapped onto two lines (0.1.0's "Markdown Notes") rises into the box's lower part
    (on the deck from 94 % of it, on the 480x320 preset from about 70 %); with the lower third free the label never
    touches the badge, whatever the name and the screen."""
    c = Canvas()
    c.polygon([(5, 6), (50, 13), (95, 6), (95, 59), (50, 66), (5, 59)], hue, round_r=5)
    c.line([(43, 18), (39, 54)], 9, ink)       # the hash: two slanted bars...
    c.line([(61, 18), (57, 54)], 9, ink)
    c.line([(29, 29), (72, 29)], 9, ink)       # ...and two cross bars
    c.line([(27, 43), (70, 43)], 9, ink)
    return c.finish()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/markdown-notes.png")
    a = ap.parse_args()
    im = icon_markdown_notes(HUE, INK)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "markdown-notes.png"))
        print("wrote", os.path.join(a.out, "markdown-notes.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "markdown-notes.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size)


if __name__ == "__main__":
    main()
