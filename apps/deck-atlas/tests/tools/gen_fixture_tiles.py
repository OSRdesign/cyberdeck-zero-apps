#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deck Atlas: writes the SYNTHETIC test tile folder (z/x/y.png, 256 px palette PNGs, pure standard library).

Nothing here is derived from real map data: a tile is a grid, its own "z/x/y" as text, a few invented roads and an
invented coastline / lake, all defined in one coordinate system so neighbouring tiles and their parents line up.
Same tile names the scenes use (zoom 15 block 16590-16591 x 11268-11269 and its parents at zoom 14 and 13).

    python3 gen_fixture_tiles.py <out folder>        (the build script passes tests/fixtures/tiles; deterministic)
"""
import math
import os
import struct
import sys
import zlib

TILES = [(13, 4147, 2817), (14, 8295, 5634), (15, 16590, 11268), (15, 16590, 11269), (15, 16591, 11268), (15, 16591, 11269)]

PALETTE = [(236, 232, 220),   # 0 land
           (150, 190, 225),   # 1 water
           (200, 196, 184),   # 2 grid
           (255, 255, 255),   # 3 minor road
           (245, 205, 90),    # 4 main road
           (140, 135, 120),   # 5 road casing
           (40, 40, 40),      # 6 text
           (190, 220, 175)]   # 7 park (the polygon)

# 3x5 digit glyphs, rows top to bottom, 3 bits each
FONT = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
        "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
        "8": "111101111101111", "9": "111101111001111", "/": "001001010100100"}

# everything below in zoom-15 tile units (x 16590..16592, y 11268..11270 is the block)
MAIN = [((16589.5, 11268.1), (16592.5, 11269.4))]
MINOR = [((16590.5, 11267.5), (16590.5, 11270.5)), ((16589.5, 11268.6), (16592.5, 11268.6)),
         ((16591.1, 11267.5), (16591.7, 11270.5)), ((16590.0, 11269.8), (16592.2, 11269.7))]
PARK = [(16590.15, 11268.15), (16590.55, 11268.1), (16590.78, 11268.4), (16590.4, 11268.75)]
BASE = (16590.0, 11268.0)


def coast(u):          # the sea lies south of this line (v larger)
    return 11269.35 + 0.30 * math.sin(2.3 * (u - 16590.0)) + 0.1 * math.sin(7.0 * (u - 16590.0))


def seg_dist(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def inside(px, py, poly):
    c = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def draw_text(img, text, x0, y0, scale):
    for ch in text:
        g = FONT[ch]
        for r in range(5):
            for c in range(3):
                if g[r * 3 + c] == "1":
                    for yy in range(scale):
                        for xx in range(scale):
                            img[(y0 + r * scale + yy) * 256 + x0 + c * scale + xx] = 6
        x0 += 4 * scale


def render(z, x, y):
    k = 2.0 ** (15 - z)           # zoom-15 tile units per tile of this zoom
    upp = k / 256.0               # units per pixel
    img = bytearray(256 * 256)
    for py in range(256):
        v = (y + (py + 0.5) / 256.0) * k
        for px in range(256):
            u = (x + (px + 0.5) / 256.0) * k
            c = 0
            if v > coast(u):
                c = 1
            elif inside(u, v, PARK):
                c = 7
            for a, b in MINOR:
                if seg_dist(u, v, a, b) < 2.0 * upp:
                    c = 3
                elif seg_dist(u, v, a, b) < 3.0 * upp and c != 3:
                    c = 5 if c != 1 else c
            for a, b in MAIN:
                d = seg_dist(u, v, a, b)
                if d < 3.0 * upp:
                    c = 4
                elif d < 4.2 * upp and c != 4:
                    c = 5
            if px == 0 or py == 0 or px == 255 or py == 255 or (px % 64 == 0 and c in (0, 1)) or (py % 64 == 0 and c in (0, 1)):
                if c in (0, 1, 7):
                    c = 2
            img[py * 256 + px] = c
    draw_text(img, "%d/%d/%d" % (z, x, y), 8, 8, 3)
    draw_text(img, "%d" % z, 108, 100, 8)
    return img


def png(img):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(img[r * 256:(r + 1) * 256]) for r in range(256))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 256, 8, 3, 0, 0, 0)) +
            chunk(b"PLTE", b"".join(bytes(c) for c in PALETTE)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    for z, x, y in TILES:
        d = os.path.join(out, str(z), str(x))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "%d.png" % y), "wb") as f:
            f.write(png(render(z, x, y)))
    print("wrote %d synthetic tiles to %s" % (len(TILES), out))


if __name__ == "__main__":
    main()
