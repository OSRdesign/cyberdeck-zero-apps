# Images in RSS Reader

What the pinned launcher's LVGL offers an app for pictures, what RSS Reader uses, what it costs. Launcher pin `f35c2d6`
(`apps/_common/LAUNCHER_REF`), LVGL 9.5. Measured on the host harness (WSL, i5-12450H, the app's code built `-O2` as on the
board); the times on a Pi Zero 2W are an estimate (8 to 12 times slower), not a measurement.

## What the pin builds

The apps' LVGL configuration (`apps/_common/scons/cross_config_defaults.mk`, the launcher's own; on the PC the render
harness's `projects/APPLaunch/tests/render/config/lvgl_config.h`, the same choices):

| Decoder | In the pin's LVGL tree | Built | Notes |
|---|---|---|---|
| PNG, lodepng | `src/libs/lodepng` | **yes** (`LV_USE_LODEPNG`) | LVGL's copy decodes into an `lv_draw_buf_t` of ARGB8888 size |
| GIF, AnimatedGIF | `src/libs/gif` | **yes** (`LV_USE_GIF`, the `lv_gif` widget) | canvas at most 480 px wide (`MAX_WIDTH`) |
| JPEG, TJpgDec R0.03 | `src/libs/tjpgd` | no (`LV_USE_TJPGD` 0) | baseline only; `JD_USE_SCALE` 0 in its `tjpgdcnf.h` (no DCT scaling) |
| JPEG, libjpeg-turbo | wrapper only | no | needs the system library (the SDK sysroot has `libjpeg.a`) |
| PNG, libpng | wrapper only | no | system library |
| WebP, libwebp | wrapper only | no | system library |
| BMP | `src/libs/bmp` | no | |
| SVG | `src/libs/svg` | no | ThorVG is built (vector drawing), its SVG loader is not enabled |

Also in the configuration: `LV_COLOR_DEPTH 16` (the panels are RGB565), `LV_USE_CLIB_MALLOC` (`lv_malloc` is `malloc`:
safe from a worker thread), and `LV_CACHE_DEF_SIZE 0`: **no image cache**. An `lv_image` given a file path or an encoded
buffer decodes it again at every redraw, on the UI thread, at full size (a 4000 x 3000 photo is 48 MB of ARGB8888), and
`lv_image_decoder_get_info` also runs on the UI thread. So the app does not hand LVGL encoded pictures at all.

## What the app does

- **The header first** (`src/core/image_probe`): the app's own reader of the headers (PNG IHDR, the JPEG markers up to
  the frame header, GIF, WebP VP8 / VP8L / VP8X, BMP; AVIF and SVG recognised) gives the format, the declared size and
  whether a JPEG is baseline. A picture over its format's pixel limit, or that no decoder reads, is refused before any
  decoder allocates anything.
- **Decoding on a worker thread** (`src/ui/image_codecs.c`, `image_decode.cpp`), through the libraries' own C functions,
  never on the UI thread; **one decode at a time** in the whole app (a turn every decode waits for, cancellable): the
  reader's two download lanes, the full-screen view and nothing else decode.
- **Scaled while decoding**: the pixels go into an area average of the target size as they come (12 bytes per *target*
  pixel), the result is RGB565 (2 bytes a pixel), handed to LVGL as an `lv_image_dsc_t` it draws as it is. Thumbnails fit
  the text column and a third of the screen's height; the full-screen view fits the screen. Never enlarged by the
  decoder (the full-screen view lets LVGL fit a small picture to the screen).
- A transparent part is drawn over white (the page the picture was made for).
- After each decode `malloc_trim(0)`: glibc kept 20 MB of freed heap after an article with two 4 MP PNGs otherwise.

| Format | How | Limit | Memory |
|---|---|---|---|
| JPEG (baseline) | TJpgDec, **compiled into the app** from the pin's tree (the file unmodified, `LV_USE_TJPGD` set for that one translation unit; `-DRSS_NO_TJPGD` leaves it out). Block by block (MCU), BGR from LVGL's copy turned back to RGB; compiled with `wrapv` (its IDCT overflows `int` on broken data: UBSan) | 16 MP | 8 KB + the target: does not depend on the photo's size |
| JPEG progressive, arithmetic, lossless, 12-bit, CMYK | none: TJpgDec reads baseline only | | "Format not supported" |
| PNG | lodepng of the pin: `lodepng_inspect`, then `lodepng_decode32` (the whole picture), rows handed to the scaler, freed | 4.2 MP (2048 x 2048) | about 8 bytes a pixel at the peak: the inflated rows and the ARGB8888 buffer live together |
| GIF | AnimatedGIF of the pin is **off** (`-DRSS_USE_ANIMATEDGIF` turns it on: the first frame, line by line, 480 px wide at most). Its parser is not safe on a broken file: the mutation test of `tests/unit/img_tests.cpp` (a valid GIF with a few bytes changed) makes it `memmove` a negative size in `GIFParseInfo` (`gif.c:360`, ASan) | | "Format not supported" |
| WebP, BMP, SVG, AVIF | no decoder in the pin | | "Format not supported". The image requests no longer offer `image/webp` in `Accept`, so image servers that choose by that header send a JPEG |

The download (`fetch_image`): 2 MB at most, 15 s, at most six different pictures per article, into the image cache.

## Measured

`tests/unit/img_tests.cpp` built as `rss-reader-imgtest` (`--bench FILE W H`, `--article W H FILE...`), the pictures made by
`tools/make_img_fixtures.py`; RSS is VmHWM over VmRSS before the decode.

| Picture | Into | Host time | Peak added | Result kept |
|---|---|---|---|---|
| JPEG 3000 x 2000 (6 MP, 774 KB) | 608 x 160 box (640x480 thumbnail): 240 x 160 | 56 ms | +0.6 MB | 75 KB |
| the same | 640 x 480 (full screen, Zero 2W deck): 640 x 427 | 64 ms | +3.9 MB | 533 KB |
| the same | 1280 x 720 (full screen): 1080 x 720 | 69 ms | +10.7 MB | 1.5 MB |
| JPEG 4800 x 3300 (15.8 MP, 894 KB), the JPEG limit | thumbnail 233 x 160 | 108 ms | +0.6 MB | 72 KB |
| the same | 1280 x 720: 1047 x 720 | 112 ms | +10.2 MB | 1.4 MB |
| PNG 2048 x 2048 RGB (4.2 MP), the PNG limit | thumbnail 160 x 160 | 39 ms | **+32.2 MB** | 50 KB |
| PNG 2048 x 2048 RGBA | thumbnail 160 x 160 | 34 ms | +32.2 MB | 50 KB |
| the same | 720 x 720 (full screen, HackBerry) | 42 ms | +37.9 MB | 1 MB |
| PNG 3000 x 2000 (6 MP) | | refused by its header | 0 | |
| An article of six: JPEG 6 MP, JPEG 15.8 MP, PNG 4.2 MP, PNG 4.2 MP RGBA, JPEG 6 MP, JPEG 1600 x 1000 | 608 x 160 thumbnails | | +36 MB at the peak, +3 MB after | 402 KB |
| the same | 1232 x 240 thumbnails (1280x720) | | +36 MB at the peak, +3.4 MB after | 906 KB |
| An article of six JPEGs (6 MP, 15.8 MP, 6 MP, 640 x 360, 6 MP, 1600 x 1000) | 608 x 160 | | +4.7 MB at the peak, +3.1 MB after | 466 KB |

So the worst case is one PNG at its limit (about 32 MB, for the time of one decode), whatever else the article holds;
a JPEG of any accepted size costs its target only; what stays is the thumbnails (under 1 MB for six) and, while it is
open, one full-screen picture (under 2 MB at 1280x720). On a Zero 2W the decodes should take about 0.5 s (a 6 MP JPEG)
to 1.3 s (16 MP) by the estimate above: the board tests will tell.

## Binary size

Stripped arm64, launcher f35c2d6 (pinned): phase 2 3,609,112 bytes, phase 3 3,674,640 bytes (+64 KB: TJpgDec about 7 KB
of code plus its tables, the rest the app's images, full-screen view, cache and saved-articles code). lodepng was in the
binary already (LVGL registers its decoder at start). No new library: the same eight `NEEDED` entries.

## The image cache

`~/.cache/rss-reader/images` (`$RSSREADER_CACHE`, or `$XDG_CACHE_HOME/rss-reader/images`): one file per picture
(`<hash16 of the URL><.jpg|.png|...>`, the bytes as downloaded), 50 MB, the least recently used go first (a read makes a
file recent) down to 90 % of the cap after each download. Apart from the saved articles (their own copies, their own cap).
Settings > Clear image cache empties it.
