#!/usr/bin/env python3
"""Finishes the images of USER-GUIDE.md after make-doc-images.sh copied the raw scene PNGs into docs/img:

  1. draws the numbered markers on every image whose name starts with "annot-" (the positions are in MARKERS below,
     in pixels of the rendered image; when the layout changes, look at the picture and move the numbers),
  2. makes layouts.png, the strip that puts the screen sizes side by side,
  3. shrinks any PNG over 100 KB to a 128-colour palette (the scene renders are 10 to 50 KB, so usually nothing).

Usage:  python finish-images.py docs/doc-images.tsv docs/img
Needs Pillow (pip install pillow). Changes only files in the image folder.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# name -> [(number, x, y)] centre of each marker, in pixels of the image (640x480 deck recording)
MARKERS = {
    "annot-recorder-deck.png": [
        (1, 34, 92),    # the cassette window
        (2, 356, 100),  # the counter
        (3, 544, 92),   # the Recordings and Settings buttons
        (4, 12, 279),   # the info strip
        (5, 606, 229),  # the level meter
        (6, 118, 307),  # CARD and SOFT
        (7, 32, 388),   # the five keys
    ],
}

# the strip of screen sizes: (label, file)
STRIP = [
    ("320x480", "size-320x480.png"),
    ("480x320", "recorder-landscape.png"),
    ("640x480", "size-640x480.png"),
    ("800x480", "size-800x480.png"),
    ("480x480", "size-480x480.png"),
    ("720x720", "size-720x720.png"),
]
STRIP_HEIGHT = 220


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf", "Arial Bold.ttf", "LiberationSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def annotate(path, marks):
    im = Image.open(path).convert("RGB")
    d = ImageDraw.Draw(im)
    f = font(15)
    r = 11
    for n, x, y in marks:
        d.ellipse((x - r, y - r, x + r, y + r), fill=(0, 112, 200), outline=(255, 255, 255), width=2)
        d.text((x, y), str(n), font=f, fill=(255, 255, 255), anchor="mm")
    im.save(path, optimize=True)


def make_strip(img_dir):
    parts = []
    for label, name in STRIP:
        p = os.path.join(img_dir, name)
        if not os.path.exists(p):
            print("strip: missing", name)
            return
        im = Image.open(p).convert("RGB")
        w = round(im.width * STRIP_HEIGHT / im.height)
        parts.append((label, im.resize((w, STRIP_HEIGHT), Image.LANCZOS)))
    gap, label_h = 14, 26
    width = sum(i.width for _, i in parts) + gap * (len(parts) + 1)
    out = Image.new("RGB", (width, STRIP_HEIGHT + label_h + gap * 2), (245, 245, 245))
    d = ImageDraw.Draw(out)
    f = font(16)
    x = gap
    for label, im in parts:
        out.paste(im, (x, gap))
        d.text((x + im.width // 2, gap + STRIP_HEIGHT + label_h // 2 + 2), label, font=f, fill=(40, 40, 40), anchor="mm")
        x += im.width + gap
    out = out.quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    out.save(os.path.join(img_dir, "layouts.png"), optimize=True)


def shrink(path):
    if os.path.getsize(path) <= 100 * 1024:
        return
    im = Image.open(path).convert("RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    im.save(path, optimize=True)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    tsv, img_dir = sys.argv[1], sys.argv[2]
    names = []
    for line in open(tsv, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        names.append(line.split("\t")[0])
    for name in names:
        path = os.path.join(img_dir, name)
        if not os.path.exists(path):
            print("missing", name)
            return 1
        if name in MARKERS:
            annotate(path, MARKERS[name])
        elif name.startswith("annot-"):
            print("no markers for", name)
    make_strip(img_dir)
    for name in names + ["layouts.png"]:
        path = os.path.join(img_dir, name)
        if os.path.exists(path):
            shrink(path)
    over = [(n, os.path.getsize(os.path.join(img_dir, n))) for n in names + ["layouts.png"]
            if os.path.exists(os.path.join(img_dir, n)) and os.path.getsize(os.path.join(img_dir, n)) > 100 * 1024]
    for n, s in over:
        print("note: %s is %d KB" % (n, s // 1024))
    print("finished %d images" % (len(names) + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
