#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measures the GPS chip (a dish and three reception bars) in the shots of the GPS scenes.

    python3 check_chip_shots.py <headless out dir>      (needs Pillow, numpy, scipy)

The chip has no text, so the scenes cannot assert it with `expect text`. The bars are filled with three exact colours
(red 0xFF4D4D one bar, orange 0xFF9A1F two bars, green 0x3FD166 three bars; atlas_chip.cpp); in a stale shot the bars are
drawn at 40 % opacity, so no exact colour is left, the "!" is amber (0xF0B400) and the bars of the fresh shot of the same
scene must be gone. Every size of every scene below is checked: the number of bars of the expected colour in the chip's
corner (the left 45 % of the picture, under the header), no other bar colour, and (stale) none. Exit status 1 on a miss."""
import glob, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

RED, ORANGE, GREEN = (0xFF, 0x4D, 0x4D), (0xFF, 0x9A, 0x1F), (0x3F, 0xD1, 0x66)
NAMES = {RED: "red", ORANGE: "orange", GREEN: "green"}

# scene -> shot -> (bars lit, colour or None)
EXPECT = {
    "o-gps-nodevice": {"no-device": (0, None)},
    "p-gps-searching": {"searching": (1, RED)},
    "q-gps-2d": {"fix-2d": (2, ORANGE)},
    "r-gps-3d": {"fix-3d": (3, GREEN), "fix-3d-z15": (3, GREEN)},
    "s-gps-moving": {"moving": (3, GREEN)},
    "t-gps-stale": {"fresh": (3, GREEN), "stale": (0, None)},
    "w-gps-other-port": {"no-receiver": (0, None), "map-port-chosen": (0, None)},
    "x1-gps-buttons": {"follow-off": (3, GREEN), "follow-on": (3, GREEN)},
    "y-gps-weak3d": {"weak-3d": (2, ORANGE)},
    "z-gps-sky": {"sky-good": (3, GREEN), "sky-stale": (0, None)},
}


def bars(path):
    im = np.array(Image.open(path).convert("RGB")).astype(int)
    h, w, _ = im.shape
    out = {}
    for col in (RED, ORANGE, GREEN):
        m = (im[:, :, 0] == col[0]) & (im[:, :, 1] == col[1]) & (im[:, :, 2] == col[2])
        m[:, int(w * 0.45):] = False
        m[: int(h * 0.08), :] = False
        found = 0
        for s in ndimage.find_objects(ndimage.label(m)[0]):
            hh, ww = s[0].stop - s[0].start, s[1].stop - s[1].start
            if ww >= 2 and hh >= 2 and hh <= 60:
                found += 1
        out[col] = found
    return out


def main(root):
    bad = 0
    n = 0
    for scene, shots in EXPECT.items():
        for shot, (want, col) in shots.items():
            for p in sorted(glob.glob(os.path.join(root, scene, "*", shot + ".png"))):
                n += 1
                name = "/".join(p.replace("\\", "/").split("/")[-3:-1]) + " " + shot
                got = bars(p)
                lit = {NAMES[c]: k for c, k in got.items() if k}
                ok = (not lit) if want == 0 else (got[col] == want and sum(got.values()) == want)
                print("PASS" if ok else "FAIL", name, "bars", lit or "none", "expected", want, NAMES.get(col, "-"))
                bad += not ok
    if n == 0:
        print("no shots under", root)
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
