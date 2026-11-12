#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Vinyl tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/vinyl/tools/make_icon.py [--out DIR] [--sheet]

Writes <out>/vinyl.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
Design (own): a record sliding out of its square sleeve, as the now-playing page draws it. The sleeve is a rounded square
in teal #26A69A with a thick white ring (the drafts' sleeve ring); the record is a teal disc behind it, cut from the
sleeve by a gap of the tile background, its white label half out. No hue is free on the wheel (ICON-PALETTE, "Choosing a
new hue", rule 4): the teal (hue 174) sits between Snake's teal (170, a launcher tile) and LAN Scan's cyan (186) and wins
on lightness (L* about 61, darker than LAN Scan's 78 and Snake's bright teal) and on silhouette (a square with a disc
behind it, not a lone circle). Two colours (teal and white), no stroke under 9 units, every pixel inside the 5 px margin
(x 5-94), in the upper part (y 11-70) so a label never touches it. --sheet also writes vinyl-check.png: 100, 32 px and greyscale.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from PIL import Image, ImageDraw  # noqa: E402
from make_icons import N, SS, WHITE, Canvas  # noqa: E402

TEAL = "#26A69A"


# The drawing in its first coordinates (sleeve 5-63, record to x 95), scaled by K about (50, 41) so that the reduced
# icon's edge pixels (LANCZOS spreads a shape about 2.5 px) stay inside the 5 px margin (design QA D-60, 2026-11-12).
K = 0.945


def _x(v):
    return 50 + (v - 50) * K


def _y(v):
    return 41 + (v - 41) * K


def icon_vinyl(teal, white):
    c = Canvas()
    # the record behind the sleeve
    c.circle(_x(68), _y(41), 27 * K, teal)
    # the gap of the tile background around the sleeve (cut out of the record)
    gap = Image.new("L", (N, N), 0)
    ImageDraw.Draw(gap).rounded_rectangle([0, _y(7) * SS, _x(68) * SS, _y(75) * SS], radius=10 * K * SS, fill=255)
    clear = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    c.img.paste(clear, (0, 0), gap)
    # the record's label, half out of the sleeve
    c.circle(_x(68), _y(41), 8 * K, white)
    c.img.paste(clear, (0, 0), gap)
    # the sleeve with its ring
    c.rrect(_x(5), _y(12), _x(63), _y(70), 6 * K, teal)
    c.stroke_ring(_x(34), _y(41), 14 * K, 9, white)  # the stroke stays 9 units (the palette's minimum)
    return c.finish()


def check_sheet(im):
    w = 100 + 8 + 32 + 8 + 100
    sheet = Image.new("RGB", (w + 16, 116), (0x1E, 0x1E, 0x1E))
    sheet.paste(im, (8, 8), im)
    small = im.resize((32, 32), Image.LANCZOS)
    sheet.paste(small, (116, 42), small)
    grey = im.convert("LA").convert("RGBA")
    sheet.paste(grey, (156, 8), grey)
    return sheet


def coverage(im):
    a = im.getchannel("A")
    return sum(1 for v in a.getdata() if v >= 128) / (im.width * im.height)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="write only <out>/vinyl.png")
    ap.add_argument("--sheet", action="store_true", help="also write the check picture vinyl-check.png")
    a = ap.parse_args()
    im = icon_vinyl(TEAL, WHITE)
    print(f"vinyl icon: {im.size[0]}x{im.size[1]}, ink coverage {coverage(im):.0%}")
    out = a.out or APP
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "vinyl.png"))
    else:
        im.save(os.path.join(APP, "icon.png"))
        img_dir = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images")
        if os.path.isdir(img_dir):
            im.save(os.path.join(img_dir, "vinyl.png"))
    if a.sheet:
        check_sheet(im).save(os.path.join(out, "vinyl-check.png"))


if __name__ == "__main__":
    main()
