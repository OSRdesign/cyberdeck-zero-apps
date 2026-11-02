#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the Home Assistant app's tile icon with the shared icon helpers (tools/icons/make_icons.py, docs/ICON-PALETTE.md).

    python apps/home-assistant/tools/make_icon.py [--variant NAME] [--out DIR]
    python apps/home-assistant/tools/make_icon.py --sheet <png>      the options sheet (every variant next to its neighbours)
    python apps/home-assistant/tools/make_icon.py --hues             the hue table, the gaps, each variant's distances

Writes <out>/home-assistant.png (default: the app folder's icon.png and the package copy under root/). Needs Pillow.
Design (own, generic: no logo or mark of the Home Assistant project): a house (roof, walls, chimney) with one symbol
inside, a signal (a dot and two thick arcs opening upwards). Strokes are 9 units. The variants differ only in their two
colours (rule 3: a badge colour and a symbol colour):

  light   a very light cool neutral #E6EBF0, ink symbol: THE ICON (owner's choice, 2026-11-01). No hue is 30 degrees
          from every listed one any more; rule 4 of the palette: it wins on silhouette (the only house) and on tint
          (cool) against Klondike's warm ivory, the one badge as light (L* 92.8 against 94.7)
  orchid  #BE3DF5, white symbol   the first draft's icon (hue 282, 22 degrees from Mesh Hop and Net Walk)
  lime    the best free hue by numbers (the widest gap of the hue wheel, computed below), ink symbol
  slate   a very dark slate #34465B, white symbol (rule 4, the other end)
"""
import argparse
import colorsys
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
sys.path.insert(0, os.path.join(REPO, "tools", "icons"))

from make_icons import INK, WHITE, Canvas  # noqa: E402

# Badge colours of the apps in docs/ICON-PALETTE.md (hue from the colour) and the launcher's built-in tiles (the doc's
# approximate hues). Neutral badges (Klondike's ivory, the template's grey) have no hue to keep away from.
APP_BADGES = {
    "LAN Scan": "#19D3E6", "Wi-Fi Survey": "#FF7A1A", "Mesh Hop": "#8E5CF5", "MeshZero": "#E23BD6",
    "Serial Console": "#FFC21A", "System Monitor": "#F23B3B", "viz1090": "#5BB8FF", "Net Toolbox": "#FF5C93",
    "RSS Reader": "#FFA11C", "Mine Field": "#5B7CFF", "Net Walk": "#E23BD6", "Markdown Notes": "#CBF22C",
}
NEUTRAL_BADGES = {"Klondike Solitaire": "#F3F0E3", "App template": "#8A9099"}
LAUNCHER_HUES = {
    "CLI, SSH (green, reserved)": 120, "Python (teal-green)": 160, "Python (blue)": 195, "Snake": 170,
    "Settings": 245, "IP Panel": 18, "Files": 215, "LoRa": 210, "Music": 335, "Claw": 10,
}

COMPARE = ["net-walk", "mesh-hop", "net-toolbox", "markdown-notes", "klondike-solitaire", "serial-console", "mine-field",
           "viz1090"]


def hue_of(hexcol):
    r, g, b = (int(hexcol.lstrip("#")[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    return h * 360.0, s, v


def lstar(hexcol):
    """CIE L* (0 black .. 100 white) of an sRGB colour"""
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (int(hexcol.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
    y = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
    return 116 * y ** (1 / 3.0) - 16 if y > 0.008856 else 903.3 * y


def listed_hues():
    hues = [(round(hue_of(c)[0]), name) for name, c in APP_BADGES.items()]
    hues += [(h, "launcher: " + name) for name, h in LAUNCHER_HUES.items()]
    return sorted(hues)


def angle(a, b):
    d = abs(a - b) % 360
    return min(d, 360 - d)


def min_distance(h):
    best = None
    for hh, name in listed_hues():
        d = angle(h, hh)
        if best is None or d < best[0]:
            best = (d, name)
    return best


def best_free_hue():
    """the hue whose nearest listed hue is the farthest (scanned in 1 degree steps)"""
    return max(range(360), key=lambda h: (min_distance(h)[0], -abs(h - 96)))


def hsv_hex(h, s, v):
    r, g, b = colorsys.hsv_to_rgb(h / 360.0, s, v)
    return "#%02X%02X%02X" % (round(r * 255), round(g * 255), round(b * 255))


def variants():
    h = best_free_hue()
    return {
        "orchid": ("#BE3DF5", WHITE, "the 0.1.0 draft's orchid"),
        "lime": (hsv_hex(h, 0.78, 0.92), INK, "the best free hue by numbers (%d degrees)" % h),
        "light": ("#E6EBF0", INK, "a very light cool neutral"),
        "slate": ("#34465B", WHITE, "a very dark slate"),
    }


def draw(badge, ink):
    c = Canvas()
    # the house: chimney, roof and walls as one rounded silhouette
    c.rrect(66, 12, 80, 40, 2, badge)
    c.polygon([(50, 6), (95, 47), (84, 47), (84, 94), (16, 94), (16, 47), (5, 47)], badge, round_r=2.5)
    # the signal: a dot and two arcs opening upwards (angles clockwise from east, screen coordinates)
    c.circle(50, 82, 6.5, ink)
    c.arc(50, 82, 16, 9, 225, 315, ink)
    c.arc(50, 82, 30, 9, 228, 312, ink)
    return c.finish()


def describe(name):
    badge, ink, what = variants()[name]
    h, s, v = hue_of(badge)
    if s < 0.15:
        dist = "n/a (neutral, saturation %.2f)" % s
    else:
        d, near = min_distance(h)
        dist = "%d degrees (nearest: %s)" % (round(d), near)
    neighbours = dict(APP_BADGES, **NEUTRAL_BADGES)
    close = min(neighbours.items(), key=lambda kv: abs(lstar(kv[1]) - lstar(badge)))
    return ("%-6s %s badge %s, symbol %s, hue %3d, S %.2f, V %.2f, L* %4.1f; min hue distance %s; closest lightness: "
            "%s (L* %.1f)" % (name, what, badge, ink, round(h), s, v, lstar(badge), dist, close[0], lstar(close[1])))


def print_hues():
    print("Listed hues (docs/ICON-PALETTE.md apps and launcher tiles):")
    for h, name in listed_hues():
        print("  %3d  %s" % (h, name))
    hs = sorted(set(h for h, _ in listed_hues()))
    gaps = sorted(((hs[(i + 1) % len(hs)] - hs[i]) % 360, hs[i], hs[(i + 1) % len(hs)]) for i in range(len(hs)))[::-1]
    print("Widest gaps (width, from, to; the middle is width/2 from both):")
    for w, a, b in gaps[:5]:
        print("  %3d  %3d .. %3d  (middle %d, %d degrees from each side)" % (w, a, b, (a + w // 2) % 360, w // 2))
    print("Variants:")
    for name in variants():
        print("  " + describe(name))


def sheet(path):
    from PIL import Image, ImageDraw
    icons = {}
    for name, (badge, ink, _) in variants().items():
        icons[name] = draw(badge, ink)
    others = []
    for app in COMPARE:
        p = os.path.join(REPO, "apps", app, "icon.png")
        if os.path.exists(p):
            others.append((app, Image.open(p).convert("RGBA").resize((100, 100), Image.LANCZOS)))
    sizes = [100, 64, 32]
    cell = 116
    label_w = 150
    names = ["lime", "light", "slate", "orchid"]
    block_h = 26 + 2 * (sum(s + 12 for s in sizes)) + 30
    W = label_w + cell * (1 + len(others)) + 20
    H = 70 + block_h * len(names)
    out = Image.new("RGB", (W, H), (0x2A, 0x2A, 0x2A))
    d = ImageDraw.Draw(out)
    d.text((12, 10), "Home Assistant icon options: each candidate (first column) next to its neighbours, at 100 / 64 / 32 px, "
           "colour then greyscale, on the tile background #1E1E1E", fill=(230, 230, 230))
    d.text((12, 28), "Pick one with: python apps/home-assistant/tools/make_icon.py --variant <name>  (lime: (a), light: (b), "
           "slate: (c), orchid: the first draft; the icon is light)", fill=(200, 200, 200))
    x0 = label_w + cell
    for i, (app, _) in enumerate(others):
        d.text((x0 + i * cell + 4, 50), app[:16], fill=(170, 170, 170))
    y = 70
    tags = {"lime": "(a)", "light": "(b) chosen", "slate": "(c)", "orchid": "draft"}
    for name in names:
        badge, ink, what = variants()[name]
        d.text((12, y + 4), "%s %s  %s" % (tags[name], name, badge), fill=(240, 240, 240))
        info = describe(name).split("; ")
        d.text((12, y + 18), info[1][:60] if len(info) > 1 else "", fill=(180, 180, 180))
        yy = y + 40
        for grey in (False, True):
            for s in sizes:
                row = [icons[name]] + [im for _, im in others]
                for i, im in enumerate(row):
                    x = label_w + i * cell + (cell - s) // 2
                    tile = Image.new("RGB", (s + 8, s + 8), (0x1E, 0x1E, 0x1E))
                    ic = im.resize((s, s), Image.LANCZOS)
                    tile.paste(ic, (4, 4), ic)
                    if grey:
                        tile = tile.convert("L").convert("RGB")
                    out.paste(tile, (x - 4, yy - 4))
                d.text((12, yy + s // 2 - 6), "%s %d px" % ("grey" if grey else "colour", s), fill=(150, 150, 150))
                yy += s + 12
        y += block_h
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    out.save(path)
    print("wrote", path, out.size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="light", choices=sorted(variants()), help="the colours (default light)")
    ap.add_argument("--out", default=None, help="write only <out>/home-assistant.png")
    ap.add_argument("--sheet", default=None, help="write the options sheet to this PNG and stop")
    ap.add_argument("--hues", action="store_true", help="print the hue table, the gaps and the variants, and stop")
    a = ap.parse_args()
    if a.hues:
        print_hues()
        return
    if a.sheet:
        sheet(a.sheet)
        return
    badge, ink, _ = variants()[a.variant]
    im = draw(badge, ink)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        im.save(os.path.join(a.out, "home-assistant.png"))
        print("wrote", os.path.join(a.out, "home-assistant.png"), im.size)
        return
    icon = os.path.join(APP, "icon.png")
    im.save(icon)
    tile = os.path.join(APP, "root", "usr", "share", "APPLaunch", "share", "images", "home-assistant.png")
    os.makedirs(os.path.dirname(tile), exist_ok=True)
    shutil.copyfile(icon, tile)
    print("wrote", icon, "and", tile, im.size, "variant", a.variant)


if __name__ == "__main__":
    main()
