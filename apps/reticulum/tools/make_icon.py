#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draws the Reticulum tile icon: 100x100, transparent, drawn at 8x and reduced (docs/ICON-PALETTE.md).

A badge in slate teal (#4FA3A5), a triangle with soft corners (no other app has one), and one bold ink symbol: three thick
lines crossing at a hub, a net seen from above. Two colours, strokes of 9 px at 100 px, no text.

    python tools/make_icon.py [--out apps/reticulum/icon.png]    (also writes root/usr/share/APPLaunch/share/images/reticulum.png)
"""
import argparse
import math
import os

from PIL import Image, ImageDraw

BADGE = (0x4F, 0xA3, 0xA5, 255)
INK = (0x14, 0x18, 0x1C, 255)
S = 8            # supersampling
SIZE = 100


def rounded_triangle(draw, cx, cy, r, radius, fill):
    """A triangle (point up) with corners rounded by `radius`, built as the union of its inset triangle and discs."""
    pts = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in (-90, 30, 150)]
    # inset triangle: move every edge inward by `radius`, then add a disc of that radius on each inset corner
    inr = r - 2 * radius
    inner = [(cx + inr * math.cos(math.radians(a)), cy + inr * math.sin(math.radians(a))) for a in (-90, 30, 150)]
    draw.polygon(inner, fill=fill)
    for x, y in inner:
        draw.ellipse([x - radius, y - radius, x + radius, y + radius], fill=fill)
    # the three rectangles that close the sides
    for i in range(3):
        (x1, y1), (x2, y2) = inner[i], inner[(i + 1) % 3]
        dx, dy = y2 - y1, -(x2 - x1)
        n = math.hypot(dx, dy)
        dx, dy = dx / n * radius, dy / n * radius
        draw.polygon([(x1, y1), (x2, y2), (x2 + dx, y2 + dy), (x1 + dx, y1 + dy)], fill=fill)
        draw.polygon([(x1, y1), (x2, y2), (x2 - dx, y2 - dy), (x1 - dx, y1 - dy)], fill=fill)
    return pts


def draw():
    img = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = 50 * S, 56 * S
    rounded_triangle(d, cx, cy, 50 * S, 7 * S, BADGE)
    # the symbol: three thick lines through a hub, and a ring-less filled hub
    arm = 15 * S
    width = 9 * S
    for a in (0, 60, 120):
        dx, dy = arm * math.cos(math.radians(a)), arm * math.sin(math.radians(a))
        d.line([(cx - dx, cy + 4 * S - dy), (cx + dx, cy + 4 * S + dy)], fill=INK, width=width)
        for sx in (-1, 1):
            x, y = cx + sx * dx, cy + 4 * S + sx * dy
            d.ellipse([x - width // 2, y - width // 2, x + width // 2, y + width // 2], fill=INK)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    app = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(app, "icon.png"))
    args = ap.parse_args()
    img = draw()
    img.save(args.out)
    tile = os.path.join(app, "root", "usr", "share", "APPLaunch", "share", "images", "reticulum.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    img.save(tile)
    print("wrote", args.out, "and", tile)


if __name__ == "__main__":
    main()
