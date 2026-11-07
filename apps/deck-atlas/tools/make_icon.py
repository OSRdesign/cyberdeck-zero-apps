#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Deck Atlas tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/deck-atlas/tools/make_icon.py [--out DIR] [--sheet PNG]

Writes <out>/deck-atlas.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
--sheet writes a check picture: the icon at 100, 64 and 32 px and in greyscale next to its nearest neighbours (Net
Walk and Music-pink on the hue wheel, Serial Console and Markdown Notes for the paper-like silhouettes, Home Assistant
and Klondike for the light badges) and the colour options that were weighed.
Design (own): the badge is a folded map, three panels in a zigzag (each panel a parallelogram, a thin gap of the tile
background between them: the folds), in orchid #E8489E (hue about 328, the gap between Net Walk's magenta 304 and the
launcher's Music crimson pink 335 and Net Toolbox's rose 340: no 30-degree gap is left on the wheel, so the silhouette
and the symbol carry it, rule 4); one white symbol (rule 3: a mid-dark hue takes white): a map pin, a teardrop with its
hole, standing on the middle panel. Strokes and gaps are at least 9 units (the hole 10).
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from make_icons import WHITE, INK, Canvas  # noqa: E402

BADGE = "#E8489E"
OPTIONS = {"orchid (chosen)": ("#E8489E", WHITE), "navy": ("#24497A", WHITE), "atlas yellow": ("#F5D90F", INK)}


def draw(badge, ink):
    c = Canvas()
    top, bot, rise = 16, 86, 9   # the panels' top and bottom, and how far each fold rises or falls
    xs = [7, 36.5, 63.5, 93]     # the panel edges (the folds at the inner two)
    gap = 2.2                    # half the fold gap
    for i in range(3):
        x0, x1 = xs[i] + (gap if i else 0), xs[i + 1] - (gap if i < 2 else 0)
        up = i % 2 == 0          # panels 0 and 2 slope up to the right, panel 1 down
        y0a, y1a = (top + rise, top) if up else (top, top + rise)
        y0b, y1b = (bot, bot - rise) if up else (bot - rise, bot)
        c.polygon([(x0, y0a), (x1, y1a), (x1, y1b), (x0, y0b)], badge, round_r=1.5)
    # the pin: a disc and a point below it, with a hole of the badge colour
    cx, cy, r = 50, 42, 15
    c.polygon([(cx - r * 0.82, cy + r * 0.55), (cx + r * 0.82, cy + r * 0.55), (cx, cy + r + 19)], ink, round_r=1.0)
    c.circle(cx, cy, r, ink)
    c.circle(cx, cy, 5.5, badge)
    return c.finish()


def sheet(path, icon):
    from PIL import Image, ImageOps

    others = [os.path.join(REPO, "apps", a, "icon.png") for a in
              ("net-walk", "net-toolbox", "serial-console", "markdown-notes", "home-assistant", "klondike-solitaire",
               "mesh-hop")]
    ims = [icon] + [draw(b, s) for name, (b, s) in OPTIONS.items() if b != BADGE]
    ims += [Image.open(p).convert("RGBA") for p in others if os.path.exists(p)]
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
    ap.add_argument("--out", default=None, help="write only <out>/deck-atlas.png")
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
        im.save(os.path.join(a.out, "deck-atlas.png"))
        print("wrote", os.path.join(a.out, "deck-atlas.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "deck-atlas.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size)


if __name__ == "__main__":
    main()
