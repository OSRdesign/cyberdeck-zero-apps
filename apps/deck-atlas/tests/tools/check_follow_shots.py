#!/usr/bin/env python3
"""Measures the follow button against the engine's zoom buttons in the shots of the x*-gps-buttons scenes.

    python3 check_follow_shots.py <headless out dir>      (needs Pillow, numpy, scipy)

In every follow-off shot the three plates (dark rounded squares in the right 45 % of the screen) must have the same size;
the follow plate is in the zoom buttons' column below them, or in the zoom-out button's row on its left (no room below),
at the same gap as between + and -. Exit status 1 on any difference."""
import glob, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage


def plates(path):
    im = np.array(Image.open(path).convert("RGB")).astype(int)
    h, w, _ = im.shape
    lum = im.sum(2) / 3
    m = np.zeros(lum.shape, bool)
    x0 = int(w * 0.55)
    m[:, x0:] = lum[:, x0:] <= 95
    m = ndimage.binary_fill_holes(ndimage.binary_closing(m, iterations=2))
    out = []
    for s in ndimage.find_objects(ndimage.label(m)[0]):
        hh, ww = s[0].stop - s[0].start, s[1].stop - s[1].start
        if 20 <= ww <= 120 and abs(hh - ww) <= 3:
            out.append((s[1].start, s[0].start, ww, hh))
    return sorted(out)


def main(root):
    bad = 0
    shots = sorted(glob.glob(os.path.join(root, "x*-gps-buttons", "*", "follow-off.png")))
    for p in shots:
        b = plates(p)
        name = "/".join(p.replace("\\", "/").split("/")[-3:-1])
        if len(b) != 3 or len({(w, h) for _, _, w, h in b}) != 1:
            print("FAIL", name, "plates", b)
            bad += 1
            continue
        if len({q[0] for q in b}) == 1:
            zi, zo, f = sorted(b, key=lambda q: q[1])
        else:
            zoom = sorted([q for q in b if q[0] == max(x for x, _, _, _ in b)], key=lambda q: q[1])
            zi, zo = zoom
            f = [q for q in b if q not in zoom][0]
        size = zi[2]
        pitch = zo[1] - zi[1]
        if f[0] == zo[0]:  # below
            ok = f[1] - zo[1] == pitch
        else:  # left of the zoom-out button, same row, the same gap
            ok = f[1] == zo[1] and zo[0] - f[0] - size == pitch - size
        print("PASS" if ok else "FAIL", name, "plate", size, "pitch", pitch, "follow", f[:2], "zoom", zi[:2], zo[:2])
        bad += not ok
    if not shots:
        print("no shots under", root)
        bad = 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
