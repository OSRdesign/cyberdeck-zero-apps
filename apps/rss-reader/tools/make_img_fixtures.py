"""The image fixtures of RSS Reader (tests/data/ui/img) and the big pictures of the measurements of docs/IMAGES.md.

    python tools/make_img_fixtures.py tests/data/ui/img <a folder for the big pictures, outside the repository>

Needs Python 3 with Pillow. Not part of the tests: the fixtures are committed; run it again to change them."""
import io, math, os, random, struct, sys, zlib
from PIL import Image, ImageDraw, ImageFont

repo = sys.argv[1]          # tests/data/ui/img
big = sys.argv[2]           # the measurement pictures (several MB): not for the repository
os.makedirs(repo, exist_ok=True)
os.makedirs(big, exist_ok=True)
random.seed(7)


def graph(w, h):
    """A latency graph: dark panel, grid, two curves (before / after)."""
    im = Image.new("RGB", (w, h), (24, 30, 42))
    d = ImageDraw.Draw(im)
    for x in range(0, w, max(8, w // 12)):
        d.line([(x, 0), (x, h)], fill=(44, 52, 66))
    for y in range(0, h, max(8, h // 8)):
        d.line([(0, y), (w, y)], fill=(44, 52, 66))
    pts_a = [(x, int(h * 0.25 + h * 0.18 * math.sin(x / (w / 9.0)) + random.randint(-6, 6))) for x in range(0, w, 4)]
    pts_b = [(x, int(h * 0.78 + h * 0.05 * math.sin(x / (w / 13.0)) + random.randint(-3, 3))) for x in range(0, w, 4)]
    d.line(pts_a, fill=(240, 120, 60), width=max(2, w // 200))
    d.line(pts_b, fill=(90, 200, 120), width=max(2, w // 200))
    d.rectangle([w * 0.06, h * 0.06, w * 0.34, h * 0.16], fill=(240, 120, 60))
    d.rectangle([w * 0.06, h * 0.86, w * 0.34, h * 0.94], fill=(90, 200, 120))
    return im


def photo(w, h, seed=1):
    """A landscape-like picture: sky gradient, sun, hills (smooth: compresses like a photo)."""
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for x in range(w):
            px[x, y] = (int(70 + 120 * t), int(120 + 80 * t), int(200 - 60 * t))
    d = ImageDraw.Draw(im)
    d.ellipse([w * 0.65, h * 0.12, w * 0.8, h * 0.12 + w * 0.15], fill=(255, 220, 120))
    for k, col in enumerate([(60, 110, 70), (40, 90, 55), (30, 70, 45)]):
        base = h * (0.55 + 0.12 * k)
        pts = [(0, h)] + [(x, base + h * 0.06 * math.sin(x / (w / (3 + k)) + rnd.random())) for x in range(0, w + 8, 8)] + [(w, h)]
        d.polygon(pts, fill=col)
    return im


def noise_photo(w, h, seed=3):
    """Busy texture: makes a big JPEG weigh like a real one."""
    im = photo(w, h, seed)
    px = im.load()
    rnd = random.Random(seed)
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            n = rnd.randint(-18, 18)
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)), max(0, min(255, b + n)))
    return im


def save(im, path, **kw):
    im.save(path, **kw)
    print(f"{os.path.basename(path)}: {im.size[0]}x{im.size[1]} {os.path.getsize(path)} bytes")


# ---- the fixtures of the tests (small)
save(graph(640, 360), os.path.join(repo, "graph.jpg"), quality=82, optimize=True)
save(photo(1600, 1000), os.path.join(repo, "wide.jpg"), quality=70, optimize=True)
save(photo(480, 320, 5), os.path.join(repo, "progressive.jpg"), quality=75, progressive=True)
save(photo(400, 300, 9).convert("CMYK"), os.path.join(repo, "cmyk.jpg"), quality=75)
logo = Image.new("RGBA", (360, 160), (0, 0, 0, 0))
d = ImageDraw.Draw(logo)
d.rounded_rectangle([10, 20, 350, 140], radius=30, fill=(30, 110, 200, 255))
d.ellipse([40, 45, 110, 115], fill=(255, 255, 255, 255))
d.rectangle([130, 60, 320, 100], fill=(255, 255, 255, 200))
save(logo, os.path.join(repo, "logo.png"))
frames = []
for k in range(3):
    f = Image.new("P", (240, 140), 0)
    f.putpalette([20, 20, 30, 250, 200, 60, 60, 160, 240, 230, 80, 80] + [0] * (256 * 3 - 12))
    dd = ImageDraw.Draw(f)
    dd.ellipse([20 + 60 * k, 30, 100 + 60 * k, 110], fill=1 + k)
    frames.append(f)
frames[0].save(os.path.join(repo, "anim.gif"), save_all=True, append_images=frames[1:], duration=200, loop=0)
print("anim.gif", os.path.getsize(os.path.join(repo, "anim.gif")))
Image.new("P", (600, 120), 1).save(os.path.join(repo, "wide.gif"))
print("wide.gif", os.path.getsize(os.path.join(repo, "wide.gif")))
save(photo(300, 200, 11), os.path.join(repo, "pic.webp"), quality=70)

# a PNG whose header says 30000 x 30000 (900 MP): the bytes after IHDR are a short, valid-looking IDAT start
def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
ihdr = struct.pack(">IIBBBBB", 30000, 30000, 8, 2, 0, 0, 0)
with open(os.path.join(repo, "huge.png"), "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\0" * 4000)) + chunk(b"IEND", b""))
print("huge.png", os.path.getsize(os.path.join(repo, "huge.png")))
# a JPEG whose frame header says 20000 x 20000: a real small JPEG with its SOF0 size rewritten
buf = io.BytesIO()
photo(64, 64).save(buf, "JPEG", quality=70)
jb = bytearray(buf.getvalue())
i = jb.find(b"\xff\xc0")
struct.pack_into(">HH", jb, i + 5, 20000, 20000)
open(os.path.join(repo, "huge.jpg"), "wb").write(jb)
print("huge.jpg", len(jb))
# a broken JPEG: the start of graph.jpg, then noise
g = open(os.path.join(repo, "graph.jpg"), "rb").read()
open(os.path.join(repo, "corrupt.jpg"), "wb").write(g[:1200] + bytes(random.randrange(256) for _ in range(3000)))
# a JPEG cut in the middle (a transfer that stopped)
open(os.path.join(repo, "truncated.jpg"), "wb").write(g[: len(g) // 2])
# an HTML page (what a server answers for a missing picture, labelled image/jpeg)
open(os.path.join(repo, "notimage.jpg"), "wb").write(b"<!DOCTYPE html><html><head><title>404</title></head><body><h1>Not found</h1></body></html>\n")

# ---- the pictures of the measurements (docs/IMAGES.md), outside the repository
save(noise_photo(3000, 2000), os.path.join(big, "jpeg-6mp.jpg"), quality=80)
save(noise_photo(4800, 3300), os.path.join(big, "jpeg-16mp.jpg"), quality=55)
save(photo(2048, 2048), os.path.join(big, "png-4mp.png"))
rgba = photo(2048, 2048).convert("RGBA")
save(rgba, os.path.join(big, "png-4mp-rgba.png"))
save(photo(3000, 2000), os.path.join(big, "png-6mp.png"))
gif = photo(480, 8300).convert("P", palette=Image.ADAPTIVE)
save(gif, os.path.join(big, "gif-4mp.gif"))
