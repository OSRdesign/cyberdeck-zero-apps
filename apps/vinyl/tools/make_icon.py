#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Vinyl tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/vinyl/tools/make_icon.py [--out DIR] [--sheet]

Writes <out>/vinyl.png (default: the app folder's icon.png and the package copy under root/, the same file). Needs
Pillow. Design (the coordinator's, 2026-11-12, design QA D-60): a square cream cover sleeve (#DCD4BD, the app's panel)
with a black record (#14181C) sliding out of it to the right, the record's plain teal label (#1F7A72, the app's accent)
half out of the sleeve. The sleeve keeps the earlier icon's ring, now teal; two plain grey groove rings (#4A5056) make
the black record show on the dark tile (#1E1E1E), where black alone disappears. Large, simple shapes: the sleeve 64
units, the record 44. Measured as the design QA does (the reduced 100x100 PNG): every pixel with any alpha inside x 5-94
and y 5-94, ink coverage (alpha >= 128) at least 45 %; main() prints both and stops if either fails. --sheet also writes
vinyl-check.png: 100, 32 px and greyscale on the tile colour.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from PIL import Image  # noqa: E402
from make_icons import Canvas  # noqa: E402

CREAM = "#DCD4BD"   # the sleeve (the app's cream panel)
BLACK = "#14181C"   # the record (the palette's ink)
TEAL = "#1F7A72"    # the label and the sleeve's ring (the app's accent)
GROOVE = "#4A5056"  # the record's two groove rings

# 100-unit coordinates. LANCZOS spreads an edge about 2.5 px when the 8x drawing is reduced, so the shapes stay within
# x 7.5-92.5 and y 8.5-92 to keep every pixel inside the 5 px margin.
SLEEVE_X, SLEEVE_Y, SLEEVE = 7.5, 8.5, 64.0
RECORD_R = 22.0
RECORD_RIGHT = 92.5
LABEL_R = 8.0


def icon_vinyl():
    c = Canvas()
    cy = SLEEVE_Y + SLEEVE / 2
    cx = RECORD_RIGHT - RECORD_R  # the label's centre on the sleeve's right edge: half of it shows
    # the record behind the sleeve, its grooves and its label
    c.circle(cx, cy, RECORD_R, BLACK)
    for f in (0.62, 0.86):
        c.stroke_ring(cx, cy, f * RECORD_R, 2.2, GROOVE)
    c.circle(cx, cy, LABEL_R, TEAL)
    # the sleeve and its ring
    c.rrect(SLEEVE_X, SLEEVE_Y, SLEEVE_X + SLEEVE, SLEEVE_Y + SLEEVE, 4, CREAM)
    c.stroke_ring(SLEEVE_X + SLEEVE / 2, cy, 0.24 * SLEEVE, 9, TEAL)
    return c.finish()


def margin_box(im):
    """The box of every pixel with any alpha (the design QA's measurement), as getbbox: (x0, y0, x1, y1), x1 exclusive."""
    return im.getchannel("A").point(lambda v: 255 if v >= 1 else 0).getbbox()


def check_sheet(im):
    w = 100 + 8 + 32 + 8 + 100
    sheet = Image.new("RGB", (w + 16, 116), (0x1E, 0x1E, 0x1E))  # the tile colour
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
    im = icon_vinyl()
    box, cov = margin_box(im), coverage(im)
    print(f"vinyl icon: {im.size[0]}x{im.size[1]}, pixels in x {box[0]}-{box[2] - 1} y {box[1]}-{box[3] - 1}, "
          f"ink coverage {cov:.1%}")
    if box[0] < 5 or box[1] < 5 or box[2] > 95 or box[3] > 95 or cov < 0.45:
        sys.exit("vinyl icon: outside the 5 px margin or under 45 % coverage (docs/ICON-PALETTE.md)")
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
