#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the app tile icons and the review sheet.

    python tools/icons/make_icons.py [--launcher DIR] [--out DIR]

Needs Pillow only. Every icon is drawn at 8x (800x800) and reduced to the canonical 100x100 with LANCZOS, so edges
are clean. Rules: docs/ICON-PALETTE.md. One function per app; ICONS lists them with their two colours.

  out/<id>.png   the 100x100 transparent icons (canonical size, see the palette doc)
  out/sheet.png  review sheet: 64 px, 32 px, greyscale, next to the launcher's built-in tiles
                 (--launcher points at the launcher checkout; without it the built-in row is skipped)
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

SIZE = 100          # canonical icon size (px), square: the launcher stretches an icon to a square
SS = 8              # supersampling factor
N = SIZE * SS
TILE_BG = (0x1E, 0x1E, 0x1E)
TILE_BORDER = (0x3A, 0x3A, 0x3A)
INK = "#14181C"     # the dark second colour
WHITE = "#FFFFFF"


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


class Canvas:
    """A transparent 8x canvas with helpers in 100-unit coordinates."""

    def __init__(self):
        self.img = Image.new("RGBA", (N, N), (0, 0, 0, 0))

    @staticmethod
    def s(v):
        return v * SS

    def pts(self, p):
        return [(x * SS, y * SS) for x, y in p]

    def mask_fill(self, mask, colour):
        layer = Image.new("RGBA", (N, N), hexrgb(colour) + (255,))
        self.img.paste(layer, (0, 0), mask)

    def round_mask(self, mask, radius):
        """Round the convex corners of a mask: blur then threshold (radius in 100-unit coords)."""
        m = mask.filter(ImageFilter.GaussianBlur(radius * SS))
        return m.point(lambda v: 255 if v >= 128 else 0)

    def shape_mask(self, draw_fn):
        m = Image.new("L", (N, N), 0)
        draw_fn(ImageDraw.Draw(m))
        return m

    def polygon(self, pts, colour, round_r=0):
        m = self.shape_mask(lambda d: d.polygon(self.pts(pts), fill=255))
        if round_r:
            m = self.round_mask(m, round_r)   # blur + threshold keeps straight edges, rounds convex corners
        self.mask_fill(m, colour)

    def circle(self, cx, cy, r, colour):
        d = ImageDraw.Draw(self.img)
        d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=hexrgb(colour) + (255,))

    def stroke_ring(self, cx, cy, r, w, colour):
        m = self.shape_mask(lambda d: (
            d.ellipse([(cx - r - w / 2) * SS, (cy - r - w / 2) * SS, (cx + r + w / 2) * SS, (cy + r + w / 2) * SS], fill=255),
            d.ellipse([(cx - r + w / 2) * SS, (cy - r + w / 2) * SS, (cx + r - w / 2) * SS, (cy + r - w / 2) * SS], fill=0)))
        self.mask_fill(m, colour)

    def arc(self, cx, cy, r, w, a0, a1, colour):
        """Round-capped arc, angles in degrees, 0 = east, clockwise (screen coordinates)."""
        m = self.shape_mask(lambda d: self._arc_draw(d, cx, cy, r, w, a0, a1))
        self.mask_fill(m, colour)

    def _arc_draw(self, d, cx, cy, r, w, a0, a1):
        d.arc([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], a0, a1, fill=255, width=int(w * SS))
        for a in (a0, a1):
            x = cx + r * math.cos(math.radians(a))
            y = cy + r * math.sin(math.radians(a))
            d.ellipse([(x - w / 2) * SS, (y - w / 2) * SS, (x + w / 2) * SS, (y + w / 2) * SS], fill=255)

    def line(self, pts, w, colour):
        """Polyline with round caps and joins."""
        m = self.shape_mask(lambda d: self._line_draw(d, pts, w))
        self.mask_fill(m, colour)

    def _line_draw(self, d, pts, w):
        P = self.pts(pts)
        d.line(P, fill=255, width=int(w * SS))
        for x, y in P:
            d.ellipse([x - w * SS / 2, y - w * SS / 2, x + w * SS / 2, y + w * SS / 2], fill=255)

    def rrect(self, x0, y0, x1, y1, r, colour):
        d = ImageDraw.Draw(self.img)
        d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=r * SS, fill=hexrgb(colour) + (255,))

    def finish(self):
        return self.img.resize((SIZE, SIZE), Image.LANCZOS)


def polar(cx, cy, r, deg):
    return (cx + r * math.cos(math.radians(deg)), cy + r * math.sin(math.radians(deg)))


# ---------------------------------------------------------------------------------------------- the icons

def icon_lanscan(hue, ink):
    """Radar scope: cyan disc, two dark range rings, a sweep and filled blips sitting on the rings."""
    c = Canvas()
    c.circle(50, 50, 45, hue)
    c.stroke_ring(50, 50, 37, 8, ink)
    c.stroke_ring(50, 50, 14, 8, ink)
    c.line([(50, 50), polar(50, 50, 37, -45)], 8, ink)    # sweep
    for ang, r in ((155, 25.5), (75, 25.5)):                    # blips: dots in the gap, bigger than the strokes
        x, y = polar(50, 50, r, ang)
        c.circle(x, y, 5.8, ink)
    return c.finish()


def icon_wifi(hue, ink):
    c = Canvas()
    # diamond-ish: a rotated square with soft corners
    cx = cy = 50
    h = 49
    c.polygon([(cx, cy - h), (cx + h, cy), (cx, cy + h), (cx - h, cy)], hue, round_r=7)
    ox, oy = 50, 63
    c.circle(ox, oy, 6.5, ink)
    c.arc(ox, oy, 19, 9, 225, 315, ink)
    c.arc(ox, oy, 33, 9, 225, 315, ink)
    return c.finish()


def icon_meshhop(hue, ink):
    c = Canvas()
    R = 48
    hexa = [polar(50, 50, R, 90 + 60 * i) for i in range(6)]
    c.polygon(hexa, hue, round_r=7)
    a, b, d = (50, 27), (28, 66), (72, 66)
    c.line([a, b, d, a], 9, ink)
    for p in (a, b, d):
        c.circle(p[0], p[1], 10.5, ink)
    return c.finish()


def icon_serial(hue, ink):
    """A whole DB9 plug: the mounting flange with its two screw holes, the D-shell and the nine pins."""
    c = Canvas()
    c.rrect(4, 19, 96, 81, 8, hue)                       # flange
    for x in (12, 88):                                   # screw holes
        c.circle(x, 50, 5, ink)
    c.polygon([(22, 28), (78, 28), (71, 72), (29, 72)], ink, round_r=4)   # D-shell
    for x in range(5):
        c.circle(30 + 10 * x, 41, 4, hue)              # top row of five pins
    for x in range(4):
        c.circle(35 + 10 * x, 59, 4, hue)              # bottom row of four pins
    return c.finish()


def icon_sysmon(hue, ink):
    c = Canvas()
    # shield
    pts = [(8, 12), (50, 4), (92, 12), (92, 52), (50, 97), (8, 52)]
    c.polygon(pts, hue, round_r=8)
    c.line([(20, 46), (36, 46), (44, 28), (55, 66), (63, 46), (80, 46)], 8.5, ink)
    return c.finish()


def icon_viz(hue, ink):
    c = Canvas()
    c.rrect(9, 9, 91, 91, 28, hue)                      # squircle
    # plane, seen from above, nose up: fuselage + swept wings + tail
    c.line([(50, 17), (50, 80)], 12, ink)
    c.polygon([(50, 40), (86, 62), (86, 70), (50, 58), (14, 70), (14, 62)], ink)
    c.polygon([(50, 70), (68, 84), (68, 89), (50, 82), (32, 89), (32, 84)], ink)
    return c.finish()


def icon_template(hue, ink):
    c = Canvas()
    oct_ = [polar(50, 50, 48, 22.5 + 45 * i) for i in range(8)]
    c.polygon(oct_, hue, round_r=5)
    c.line([(50, 28), (50, 72)], 11, ink)
    c.line([(28, 50), (72, 50)], 11, ink)
    return c.finish()


def icon_nettoolbox(hue, ink):
    """Pentagon (point up, soft corners) with two thick horizontal arrows: traffic both ways, upper right, lower left."""
    c = Canvas()
    pent = [polar(50, 53, 49, -90 + 72 * i) for i in range(5)]
    c.polygon(pent, hue, round_r=6)

    def arrow(y, x0, x1):
        d = 1 if x1 > x0 else -1
        c.line([(x0, y), (x1 - d * 12, y)], 10, ink)                                  # shaft
        c.polygon([(x1, y), (x1 - d * 20, y - 10.5), (x1 - d * 20, y + 10.5)], ink, round_r=1.5)  # head

    arrow(46, 23, 77)    # out
    arrow(70, 77, 23)    # back
    return c.finish()


def icon_rssreader(hue, ink):
    """A page with its top right corner turned down, the feed symbol on it: a dot and two thick quarter arcs."""
    c = Canvas()
    c.polygon([(15, 5), (63, 5), (85, 27), (85, 95), (15, 95)], hue, round_r=6)     # the page, its corner cut
    c.polygon([(63, 5), (63, 27), (85, 27)], ink, round_r=1.5)                       # the corner turned down
    ox, oy = 33, 77
    c.circle(ox, oy, 7.5, ink)
    c.arc(ox, oy, 22, 9.5, 270, 360, ink)
    c.arc(ox, oy, 37, 9.5, 270, 360, ink)
    return c.finish()


# id, function, badge hue, symbol colour, badge name
ICONS = [
    ("lanscan",              icon_lanscan,  "#19D3E6", INK,   "circle"),
    ("wifi-survey",          icon_wifi,     "#FF7A1A", INK,   "diamond"),
    ("mesh-hop",             icon_meshhop,  "#8E5CF5", WHITE, "hexagon"),
    ("serial-console",       icon_serial,   "#FFC21A", INK,   "DB9 plug"),
    ("system-monitor",       icon_sysmon,   "#F23B3B", WHITE, "shield"),
    ("viz1090",              icon_viz,      "#5BB8FF", INK,   "squircle"),
    ("net-toolbox",          icon_nettoolbox, "#FF5C93", INK, "pentagon"),
    ("rss-reader",           icon_rssreader, "#FFA11C", INK, "page, corner turned"),
    ("cyberdeck-app-template", icon_template, "#8A9099", INK, "octagon"),
]

BUILTIN = [("setting", "Settings"), ("store", "Store"), ("cli", "CLI"), ("python", "Python"),
           ("ssh", "SSH"), ("ip_panel", "IP Panel"), ("math", "Calc"), ("game", "Snake")]


# ---------------------------------------------------------------------------------------------- the sheet

def font(sz):
    for f in ("arial.ttf", "DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(f, sz)
        except OSError:
            pass
    return ImageFont.load_default()


def tile(icon, px, grey=False):
    """The icon on a launcher-like tile (tile colour and border), icon scaled to `px` square."""
    pad = max(4, px // 6)
    t = Image.new("RGBA", (px + 2 * pad, px + 2 * pad), (0, 0, 0, 0))
    d = ImageDraw.Draw(t)
    d.rounded_rectangle([0, 0, t.width - 1, t.height - 1], radius=pad * 2, fill=TILE_BG + (255,), outline=TILE_BORDER, width=2)
    ic = icon.resize((px, px), Image.LANCZOS)
    t.alpha_composite(ic, (pad, pad))
    if grey:
        a = t.getchannel("A")
        t = t.convert("L").convert("RGBA")
        t.putalpha(a)
    return t


def make_sheet(icons, launcher_dir, path):
    names = [i[0] for i in icons]
    built = []
    if launcher_dir:
        for k, label in BUILTIN:
            p = os.path.join(launcher_dir, "projects", "APPLaunch", "APPLaunch", "share", "images", k + "_100.png")
            if os.path.exists(p):
                built.append((label, Image.open(p).convert("RGBA")))
    cols = max(len(icons), len(built))
    cell = 100
    lab_w = 150
    rows = [("apps at 64 px (tile colour)", 64, False, False),
            ("apps at 32 px", 32, False, False),
            ("apps greyscale 64", 64, True, False),
            ("apps greyscale 32", 32, True, False)]
    if built:
        rows += [("launcher built-ins 64", 64, False, True), ("launcher built-ins greyscale 64", 64, True, True)]
    W = lab_w + cols * cell + 20
    H = 50 + len(rows) * 116 + 120
    sheet = Image.new("RGB", (W, H), (12, 12, 12))
    d = ImageDraw.Draw(sheet)
    d.text((10, 10), "Icon review sheet: canonical 100x100, shown on the launcher tile colour #1E1E1E", fill=(220, 220, 220), font=font(15))
    y = 40
    for label, px, grey, builtin in rows:
        d.text((10, y + 30), label, fill=(190, 190, 190), font=font(12))
        src = built if builtin else list(zip(names, [ic[1] for ic in icons]))
        for i, (nm, im) in enumerate(src):
            t = tile(im, px, grey)
            sheet.paste(t, (lab_w + i * cell + (cell - t.width) // 2, y + (96 - t.height) // 2 + 4), t)
            if px == 64 and not grey:
                d.text((lab_w + i * cell + 4, y + 98), nm[:14], fill=(150, 150, 150), font=font(10))
        y += 116
    # mixed strip: an approximation of a home-grid row (apps and built-ins together) at 64 px
    d.text((10, y + 4), "mixed grid", fill=(190, 190, 190), font=font(12))
    mix = []
    if built:
        order = [built[2][1], icons[0][1], built[5][1], icons[1][1], built[0][1], icons[2][1], built[4][1], icons[3][1],
                 built[3][1], icons[4][1], built[6][1], icons[5][1], built[7][1], icons[6][1]]
    else:
        order = [i[1] for i in icons]
    x = 10
    for im in order:
        t = tile(im, 64)
        if x + t.width > W:
            break
        sheet.paste(t, (x, y + 24), t)
        x += t.width + 6
    sheet.save(path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(here, "out"))
    ap.add_argument("--launcher", default=os.path.normpath(os.path.join(here, "..", "..", "..", "launcher")))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    icons = []
    for ident, fn, hue, ink, _shape in ICONS:
        im = fn(hue, ink)
        im.save(os.path.join(a.out, ident + ".png"))
        icons.append((ident, im))
        print("wrote", ident + ".png", im.size)
    ld = a.launcher if os.path.isdir(a.launcher) else None
    if not ld:
        print("launcher dir not found: built-in row skipped", file=sys.stderr)
    make_sheet(icons, ld, os.path.join(a.out, "sheet.png"))
    print("wrote sheet.png")


if __name__ == "__main__":
    main()
