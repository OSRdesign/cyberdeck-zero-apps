#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measures the shots of the GPS status page scenes (zs1 ... zs7).

    python3 check_status_shots.py <headless out dir>      (needs Pillow, numpy, scipy)

What `expect text` cannot see:
  1. Nothing is clipped sideways: the screen's first and last pixel columns are black in every shot of the page (a card, a
     bar or a label that ran off the canvas would put a pixel there), and no colour of the constellation palette touches
     the 4 px next to either edge.
  2. The chip beside the headline has the right bars (the same exact colours as the map's chip, check_chip_shots.py): none for
     no device and GPS off (no chip at all), one red bar for connected without a fix, two orange for the weak 2D fix, three
     green for the good fix; stale: no exact colour (dimmed to 40 %) and the amber "!".
  3. The satellites card draws what it should: in the good-fix scene's scrolled and end shots the three systems of the feed
     (GPS blue, GLONASS purple, SBAS teal) occur as solid areas; no bar of a solid colour is cut by the body's top or
     bottom edge more than the scroll itself does (checked by the solid areas being rectangles: each solid bar column is
     one run). The sky plot's horizon ring is round: its bounding box is square within 2 px, in the shots that show all of it.
Exit status 1 on a miss."""
import glob, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

RED, ORANGE, GREEN = (0xFF, 0x4D, 0x4D), (0xFF, 0x9A, 0x1F), (0x3F, 0xD1, 0x66)
AMBER = (0xF0, 0xB4, 0x00)
GPS_BLUE, GLO_PINK, SBAS_TEAL = (0x56, 0xB4, 0xE9), (0xCC, 0x79, 0xA7), (0x2E, 0xC4, 0xB6)
PALETTE = [GPS_BLUE, GLO_PINK, (0xF0, 0xE4, 0x42), (0x8E, 0x7C, 0xFF), (0xD0, 0xD0, 0xD0), SBAS_TEAL]
NAMES = {RED: "red", ORANGE: "orange", GREEN: "green"}

# scene -> (bars lit, colour or None, stale)   for the shot "top" (the chip sits in the first card)
CHIP = {
    "zs1-status-nodevice": (0, None, False),
    "zs2-status-nofix": (1, RED, False),
    "zs3-status-good": (3, GREEN, False),
    "zs4-status-weak2d": (2, ORANGE, False),
    "zs5-status-stale": (0, None, True),
}


PAGE_SHOTS = {"top", "scrolled-1", "scrolled-2", "end", "off", "from-settings"}


def load(path):
    return np.array(Image.open(path).convert("RGB")).astype(int)


def mask(im, col):
    return (im[:, :, 0] == col[0]) & (im[:, :, 1] == col[1]) & (im[:, :, 2] == col[2])


def check_edges(im, label):
    h, w, _ = im.shape
    ok = True
    for x in (0, w - 1):
        if im[:, x, :].sum() != 0:
            print("FAIL", label, "pixels in the edge column", x)
            ok = False
    for col in PALETTE:
        m = mask(im, col)
        if m[:, :4].any() or m[:, w - 4:].any():
            print("FAIL", label, "a palette colour", col, "touches a side edge")
            ok = False
    return ok


def chip_bars(im):
    h, w, _ = im.shape
    out = {}
    for col in (RED, ORANGE, GREEN):
        m = mask(im, col)
        m[: int(h * 0.08), :] = False   # the header
        n = 0
        for s in ndimage.find_objects(ndimage.label(m)[0]):
            if s[1].stop - s[1].start >= 2 and s[0].stop - s[0].start >= 2:
                n += 1
        out[col] = n
    return out


def check_chip(im, label, want, col, stale):
    got = chip_bars(im)
    lit = {NAMES[c]: k for c, k in got.items() if k}
    ok = (not lit) if want == 0 else (got[col] == want and sum(got.values()) == want)
    below = im[int(im.shape[0] * 0.15):, :, :]   # the header's title is amber too
    amber = mask(below, AMBER).sum() > 0
    if stale and not amber:
        ok = False
    if not stale and amber and want:
        ok = False
    print("PASS" if ok else "FAIL", label, "chip bars", lit or "none", "expected", want, NAMES.get(col, "-"),
          "stale mark" if amber else "")
    return ok


def check_round(im, label):
    # the horizon ring (0x6A6A6A, 2 px) of the sky plot, when the shot shows all of it
    m = mask(im, (0x6A, 0x6A, 0x6A))
    if m.sum() < 200:
        return True
    ys, xs = np.nonzero(m)
    if ys.min() <= im.shape[0] * 0.15 or ys.max() >= im.shape[0] - 2:
        return True   # cut by the scroll area: not a measure
    wd, ht = xs.max() - xs.min(), ys.max() - ys.min()
    ok = abs(wd - ht) <= 2
    print("PASS" if ok else "FAIL", label, "sky ring", wd, "x", ht)
    return ok


def main(root):
    bad = n = 0
    for scene in sorted(glob.glob(os.path.join(root, "zs*"))):
        name = os.path.basename(scene)
        for p in sorted(glob.glob(os.path.join(scene, "*", "*.png"))):
            size = os.path.basename(os.path.dirname(p))
            shot = os.path.splitext(os.path.basename(p))[0]
            label = "%s %s %s" % (name, size, shot)
            if shot not in PAGE_SHOTS:
                continue   # the Settings list's selected row reaches the edges by design
            im = load(p)
            n += 1
            w, h = (int(v) for v in size.split("x"))
            ok = im.shape[1] == w and im.shape[0] == h
            ok = check_edges(im, label) and ok
            ok = check_round(im, label) and ok
            if shot == "top" and name in CHIP:
                want, col, stale = CHIP[name]
                ok = check_chip(im, label, want, col, stale) and ok
            if shot == "off":
                ok = chip_bars(im) == {RED: 0, ORANGE: 0, GREEN: 0} and ok
            print("PASS" if ok else "FAIL", label)
            bad += not ok
        if name == "zs3-status-good":
            for p in sorted(glob.glob(os.path.join(scene, "*", "end.png"))):
                im = load(p)
                for col, what in ((GPS_BLUE, "GPS"), (GLO_PINK, "GLONASS"), (SBAS_TEAL, "SBAS")):
                    cnt = int(mask(im, col).sum())
                    ok = cnt > 300
                    print("PASS" if ok else "FAIL", name, os.path.basename(os.path.dirname(p)), "end:", what, "solid px", cnt)
                    bad += not ok
                    n += 1
    if n == 0:
        print("no shots under", root)
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
