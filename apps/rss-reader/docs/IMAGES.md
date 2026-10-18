# Images in RSS Reader

What decodes the pictures, what it reads, what it costs. The decoder is **stb_image v2.30** (`third_party/stb`, public
domain / MIT), compiled into the app: the pinned launcher's LVGL (pin `f35c2d6`, `apps/_common/LAUNCHER_REF`, LVGL 9.5) has
a baseline-only JPEG decoder that its configuration leaves off, lodepng (PNG) and an unsafe GIF decoder, which is not
enough for what feeds link to (most real-board photos are progressive JPEG). Measured on the host harness (WSL, i5-12450H,
the app's code built `-O2` as on the board); the times on a Pi Zero 2W are an estimate (8 to 12 times slower), not a
measurement. Memory numbers are exact on the host: they come from `rss-reader-imgtest --bench` (VmHWM minus VmRSS before the
decode) and from the decoder's own allocation counter.

## What the app does

- **The header first** (`src/core/image_probe`, plus `stbi_info_from_memory` in `image_codecs.c`): the format, the declared
  size and, for a JPEG, the frame type (baseline, progressive, arithmetic, lossless), precision and components. A picture over
  its format's pixel limit, over 16384 pixels on a side (`STBI_MAX_DIMENSIONS`), or that no decoder reads is refused before the
  decoder allocates anything ("Too large to show", "Format not supported").
- **Decoding on a worker thread** (`src/ui/image_codecs.c`, `image_decode.cpp`), never on the UI thread; **one decode at a
  time** in the whole app (a turn every decode waits for, cancellable while waiting; stb_image cannot be interrupted once it
  runs: the decode is under 2 s even on a Zero 2W, by the estimate): the reader's two download lanes, the full-screen view
  and nothing else decode.
- **A hard memory ceiling**: every stb_image allocation goes through a counter (`STBI_MALLOC` and friends, a 16-byte header
  on each block). Past `kDecodeMemCeiling` (96 MiB) the allocation fails, stb_image reports "outofmem" and the picture is
  "Too large to show", whatever its header said. Every accepted size fits under it by construction (table below).
- **Scaled after decoding**: stb_image has no streaming or scaled API, so the whole picture is decoded (3 bytes a pixel for
  JPEG, 4 for the others) and handed to the resampler in bands of 16 rows: an area average into the target size (12 bytes per
  *target* pixel), RGB565 as before (2 bytes a pixel), given to LVGL as an `lv_image_dsc_t`. Thumbnails fit the text column and a
  third of the screen's height; the full-screen view fits the screen. Never enlarged by the decoder.
- A transparent part (PNG alpha, GIF transparent colour) is drawn over white.
- After each decode `malloc_trim(0)`: glibc kept 20 MB of freed heap after an article with two big pictures otherwise.
- `STBI_NO_SIMD`: the code that the tests run on the PC is the code that runs on the board. stb_image has NEON paths for the
  JPEG IDCT and colour conversion (`STBI_NEON`, automatic on aarch64 without `STBI_NO_SIMD`): worth trying once the board
  tests show the decode times, and then to be run through the same tests on an arm64 host.

## Formats

| Format | Read | Not read ("Format not supported") | Pixel limit | Peak memory (bytes a pixel) |
|---|---|---|---|---|
| JPEG baseline | SOF0 / SOF1, 8-bit, Huffman; grey, YCbCr, CMYK / YCCK (stb converts) | arithmetic (SOF9+), lossless (SOF3), 12-bit | **16 MP** | 4.4 (4:2:0, the camera default), 5.9 (4:4:4) |
| JPEG progressive | SOF2, 8-bit, Huffman | the same | **6 MP** | 7.3 (4:2:0), 11.7 (4:4:4): every DCT coefficient is kept |
| PNG | every colour type and depth, interlaced or not (16-bit reduced to 8) | | **4.2 MP** | 6.8 (RGB) to 7.8 (RGBA) |
| GIF | the **first frame only** (a static picture; transparency kept) | the next frames | **4.2 MP** | 8.8 (canvas, background, history) |
| BMP | uncompressed and RLE, 1 to 32 bit | BMP with JPEG / PNG compression | **4.2 MP** | 4 |
| WebP, AVIF, SVG | | all | | |
| EXIF orientation | | a photo turned by its EXIF tag is shown **as stored** (not turned) | | |

Compiled out (`STBI_ONLY_*`): PSD, TGA, HDR, PIC, PNM, linked/other formats; the library never sees them.

The limits are on `width x height` from the header. JPEG changed from the phase 3 numbers (16 MP streamed by TJpgDec at
0.6 MB) because stb_image decodes whole: **16 MP baseline now costs 70 MB (4:2:0) or 93 MB (4:4:4)** for the time of the
decode, **progressive 6 MP costs 44 to 70 MB** (it was "not supported"). PNG is as before (4.2 MP, about 30 MB). GIF went from
"off" to 4.2 MP (37 MB) and BMP is new (17 MB). A progressive JPEG over 6 MP (a 12 MP camera original, 53 to 105 MB) is
refused by its header with "Too large to show"; to accept them the ceiling would have to go over 100 MB, which is more than
we want on a 512 MB board that also runs the launcher.

The download (`fetch_image`): 2 MB at most, 15 s, at most six different pictures per article, into the image cache. The
request's `Accept` header offers `image/jpeg,image/png,image/gif`, not WebP, so image servers that choose by that header send
a JPEG. A **truncated** JPEG now shows the part that arrived (stb_image fills the rest grey) instead of an error; a truncated PNG
or GIF is an error.

## Measured

`tests/unit/img_tests.cpp` built as `rss-reader-imgtest` (`--bench FILE W H`, `--article W H FILE...`), the pictures made by
`tools/make_img_fixtures.py` (noise over a landscape, so a JPEG weighs like a real one). "Peak added" is VmHWM minus VmRSS
before the decode (the 2 MB encoded file and the result included); "decoder" is the allocator's own count of stb_image's
blocks. The thumbnail is 608 x 160 (the 640 x 480 deck), the full-screen target 1280 x 720.

| Picture | Host time | Peak added (thumbnail) | Peak added (full screen) | Decoder | Result |
|---|---|---|---|---|---|
| JPEG baseline 4:2:0, 3 MP (2000 x 1500) | 34 ms | +13.6 MB | +21.4 MB | 13.2 MB | 66 KB / 1.3 MB |
| JPEG baseline 4:2:0, 6 MP (3000 x 2000) | 51 ms | +26.8 MB | +35.6 MB | 26.4 MB | 75 KB / 1.5 MB |
| JPEG baseline 4:2:0, 12 MP (4000 x 3000) | 100 ms | +53.2 MB | +61.0 MB | 52.8 MB | 66 KB / 1.3 MB |
| JPEG baseline 4:4:4, 12 MP | 111 ms | +70.8 MB | +78.5 MB | 70.3 MB | |
| JPEG baseline 4:2:0, 15.8 MP (4800 x 3300), at the limit | 109 ms | +70.1 MB | +78.7 MB | 69.7 MB | 72 KB / 1.4 MB |
| JPEG baseline 4:4:4, 15.8 MP | 154 ms | **+93.1 MB** | +101.8 MB | 92.9 MB | |
| JPEG progressive 4:2:0, 3 MP (2000 x 1500) | 40 ms | +22.4 MB | +30.1 MB | 22.0 MB | |
| JPEG progressive 4:4:4, 3 MP | 49 ms | +35.8 MB | +43.5 MB | 35.3 MB | |
| JPEG progressive 4:2:0, 6 MP (3000 x 2000), at the limit | 102 ms | +44.4 MB | +53.1 MB | 44.0 MB | 75 KB / 1.5 MB |
| JPEG progressive 4:4:4, 6 MP, at the limit | 95 ms | +70.6 MB | +79.5 MB | 70.3 MB | |
| JPEG progressive 4:2:0, 8 MP (3264 x 2448) | 95 ms | +59.0 MB | +66.8 MB | 58.5 MB | refused now (limit 6 MP; measured before the limit was lowered) |
| JPEG progressive 4:4:4, 8 MP | 127 ms | +94.1 MB | +102 MB | 93.7 MB | refused (limit 6 MP) |
| JPEG progressive 4:2:0 and 4:4:4, 12 MP (4000 x 3000) | | **refused by its header, +0.5 MB** (the file), "Too large to show" | | 0 | |
| PNG 2048 x 2048 RGB (4.2 MP), at the limit | 21 ms | +30.6 MB | | 28.7 MB | 50 KB |
| PNG 2048 x 2048 RGBA | 20 ms | **+33.2 MB** | | 32.8 MB | 50 KB |
| PNG 3000 x 2000 (6 MP) | | refused by its header, +0 | | | |
| GIF 2048 x 2048 (4.2 MP), at the limit | 32 ms | +37.3 MB | +43.0 MB | 36.9 MB | 50 KB / 1 MB |
| BMP 2048 x 2048 (4.2 MP, 12.6 MB file) | 23 ms | +16.5 MB (the file is not counted: 17.4 MB RSS before) | | 16.4 MB | 50 KB |

So the worst case is one baseline 16 MP 4:4:4 JPEG at 93 MB, a progressive 6 MP 4:4:4 at 70 MB (an unusual encoding: Pillow's
`subsampling=0`; photos are 4:2:0: 70 and 44 MB), or a PNG / GIF at 33 to 37 MB, for the time of one decode, whatever else the
article holds; what stays is the thumbnails (under 1 MB for six) and, while it is open, one full-screen picture (under 2 MB at
1280 x 720). The 96 MiB ceiling is above all of these (the 93 MB case is the tightest).

**On a 512 MB Pi Zero 2W** (assuming the launcher and the OS use 150 to 200 MB, which was not measured here): the 32 MB PNG
case (4.2 MP RGBA, as before this phase) leaves well over 200 MB free; a 4:2:0 16 MP photo (70 MB) fits; the absolute worst
case, 93 MB at the ceiling, still leaves over 100 MB, and `malloc_trim` gives it back after the decode. Nothing decodes in
parallel, and a picture whose allocation fails (a smaller board, a fragmented heap) is "Too large to show", not a crash.
Not measured on a board (no boards touched in this phase): the board tests (`tests/BOARD-TESTS.md`, section 5) ask for the
real decode times and the free memory.

## What decodes nothing, and what is left out

- **WebP, AVIF, SVG**: "Format not supported" (the tile still shows the description). See the WebP assessment below.
- **Animated GIF**: only the first frame shows (a still picture). The frames after it are never decoded.
- **EXIF orientation**: ignored. A phone photo taken in portrait that carries the tag shows turned on its side. Honouring it
  needs the tag parsed out of the APP1 segment (the app's own parser, the Exif header, the IFD0 entry 0x0112, byte order) and
  the resampler to write transposed; not trivial to do safely in this phase, so left out.
- **Arithmetic, lossless and 12-bit JPEG**: not read by stb_image. Rare on the web.
- **ICC profiles**: ignored (as every stb_image user).

## Tests

`tests/unit/img_tests.cpp` (1829 checks, no LVGL needed; part of `tests/unit/run_tests.sh`, `build.sh test | asan | host`):
decoding of every fixture (`tests/data/ui/img`: baseline, progressive, 4:4:4, CMYK JPEG; PNG with alpha; animated and
static GIF with a transparent colour; BMP), pixel checks (the sky colour, the transparent corner over white and over black, the
first frame of a GIF), the header limits (`decode_check`), the lying headers (20000 x 20000 JPEG / progressive / PNG / GIF /
BMP: "Too large"), cut and broken files of every format, arithmetic / lossless / 12-bit frame types ("not supported"), the memory
ceiling (a picture refused when the allocator is limited, exact at its own peak), cancel, turns (8 threads, one decode at
a time) and a **mutation fuzz**: 7 fixtures (baseline JPEG, progressive JPEG, 4:4:4 JPEG, PNG, animated GIF, static GIF,
BMP) x 600 mutated files (1 to 12 random bytes changed, a third of them in the first 64 bytes where the header fields are, a
fifth cut short).

Findings of the fuzz under ASan + UBSan (`-fno-sanitize-recover`), 600 files a format (4200) and then 6000 a format (42000):
**one**, signed integer overflow in stb_image's JPEG IDCT on broken coefficients (`stb_image.h:2502`, `-273887 * -8034`), the
same class as TJpgDec's in phase 3. It is undefined behaviour in the C standard but wraps on every target the app builds for;
contained by compiling the one header with `#pragma GCC optimize("wrapv")` in `image_codecs.c` (the header stays
unmodified). No other finding: no out-of-bounds read or write, no leak, no crash, no hang in 46,200 files (about 40 % decode
to a picture, 55 % are refused as broken, 3 % by size or memory). The 42,000-file run is `RSS_FUZZ_N=6000`.

stb_image has a record of fuzzer-found bugs fixed over the years; v2.30 is the newest release. A fuzz of 46,000 small
mutations is not a proof: the decode runs on a worker thread of an app that has
nothing secret in memory, with a memory ceiling and the pixel limits above, and a crash would be a crash of the reader, not
of the launcher.

## Binary size

Stripped arm64, launcher f35c2d6 (pinned): phase 3 with TJpgDec and lodepng 3,674,640 bytes; with stb_image
3,740,216 bytes (+64 KB: the JPEG, PNG, GIF and BMP decoders of stb_image, less TJpgDec and the unused AnimatedGIF call
paths). lodepng is in the binary anyway (LVGL registers its decoder at start). No new library: the same eight `NEEDED` entries.

## The image cache

`~/.cache/rss-reader/images` (`$RSSREADER_CACHE`, or `$XDG_CACHE_HOME/rss-reader/images`): one file per picture
(`<hash16 of the URL><.jpg|.png|.gif|.bmp|.webp>`, the bytes as downloaded), 50 MB, the least recently used go first (a read
makes a file recent) down to 90 % of the cap after each download. Apart from the saved articles (their own copies, their own
cap). Settings > Clear image cache empties it.

## What the pin builds (for reference)

The apps' LVGL configuration (`apps/_common/scons/cross_config_defaults.mk`): `LV_COLOR_DEPTH 16`, `LV_USE_CLIB_MALLOC`,
`LV_CACHE_DEF_SIZE 0` (no image cache: an `lv_image` given a file or an encoded buffer decodes it again at every redraw, on
the UI thread, at full size), lodepng and the `lv_gif` widget on, TJpgDec, libjpeg, libpng, libwebp, BMP and SVG off. So the
app never hands LVGL encoded pictures: it decodes itself, scales, and gives LVGL RGB565 pixels. TJpgDec and AnimatedGIF are no
longer used by the app (`-DRSS_NO_TJPGD` and `-DRSS_USE_ANIMATEDGIF` are gone).

## WebP: an assessment (not implemented)

For deciding later. Everything here except the source sizes is an estimate, nothing was built.

- **What it brings.** WebP is the one format feeds still send that the app cannot show; many image servers (CDNs, WordPress
  with Jetpack / Photon, Cloudflare Polish) pick WebP by `Accept` when it is offered. The app does not offer it, so servers
  send JPEG or PNG today and WebP appears only for files that exist only as WebP. The gain is therefore real but small; the
  alternative that costs nothing is to keep not offering `image/webp`.
- **Library and licence.** libwebp (https://chromium.googlesource.com/webm/libwebp, mirror https://github.com/webmproject/libwebp),
  BSD 3-Clause plus the WebM "additional IP rights" patent grant (a `COPYING` and a `PATENTS` file to ship in
  `usr/share/doc/rss-reader/copyright`). v1.5.0 (December 2024) is the version measured below; take the newest tagged release
  at vendoring time. C99, no dependencies.
- **Vendoring the decoder only** (what `dwebp` and `libwebpdecoder` use): `src/dec/*.c` (10 files, 281 KB of source: `alpha`,
  `buffer`, `frame`, `idec`, `io`, `quant`, `tree`, `vp8`, `vp8l`, `webp` `_dec.c`), `src/dsp/` the generic C files (`alpha_processing`,
  `cpu`, `dec`, `dec_clip_tables`, `filters`, `lossless`, `rescaler`, `upsampling`, `yuv`: about 150 KB) and, for speed on the
  Zero 2W, the NEON ones (`dec_neon`, `lossless_neon`, `upsampling_neon`, `yuv_neon`, `rescaler_neon`, `alpha_processing_neon`,
  `filters_neon`), `src/utils/` (`bit_reader`, `color_cache`, `huffman`, `quant_levels_dec`, `random`, `rescaler`, `utils`,
  `thread`: about 60 KB) and `src/webp/*.h`. No encoder, no `mux`, no `demux`, no `sharpyuv`. About 31 `.c` files, 480 KB of
  source (a third of it the generic dsp). The x86 SIMD files (`*_sse2.c`, `*_sse41.c`, `*_avx2.c`) and MIPS ones are dropped
  (`WEBP_HAVE_*` / `WEBP_USE_*` not defined; the build system's `dsp/cpu.c` picks NEON on aarch64 by `__ARM_NEON`). Needs
  `WEBP_USE_THREAD` off (`thread_utils.c` compiles to stubs without `pthread`), and one more translation-unit set in the
  cross and host builds (`apps/_common/build_app.sh` already copies `third_party/`; a `.c` tree would be copied the same way and
  the scons glob would have to include it).
- **Binary growth.** `libwebpdecoder` is about 200 to 300 KB stripped on x86-64 with all its SIMD variants (Debian's
  `libwebp7` with the encoder is 494 KB); with the generic C and NEON paths only, for aarch64, an estimate of **+120 to +200 KB**
  (3 to 5 % of the 3.74 MB binary), compared with +64 KB for the whole of stb_image.
- **Memory.** Lossy WebP (VP8, the usual photo case) decodes in macroblock rows: with `WebPDecoderConfig` `use_scaling`
  the output is the target size plus a few row buffers, so **the memory is about the size of the result, not of the photo**
  (like TJpgDec before this phase: a 16 MP lossy WebP in under 2 MB), at the price of a custom call sequence (`WebPDecode` with
  `options.use_scaling`, `scaled_width`, `scaled_height`, or the incremental `WebPIDecoder`). Lossless WebP (VP8L) holds the whole
  ARGB picture (4 bytes a pixel) plus its transform data (3 to 5 more), like PNG: **about 8 bytes a pixel**, so the same 4.2 MP
  limit and 34 MB. Alpha adds a plane. Animated WebP (`VP8X` with the animation flag) is not decodable by `WebPDecode` alone:
  it needs `demux` (50 KB of source) for the first frame, or it stays "not supported". The header limit check already exists
  (`probe_webp` reads VP8, VP8L and VP8X sizes).
- **Risk.** libwebp is among the most fuzzed libraries (OSS-Fuzz, a long CVE list, including the 2023 `BuildHuffmanTable` heap
  overflow, CVE-2023-4863, exploited in the wild; fixed in 1.3.2). A vendored copy has to be kept current, unlike stb_image
  which is a single file. The same memory ceiling, header limits and mutation fuzz as here would apply (libwebp allocates
  through `WebPSafeMalloc` in `utils.c`, which has a compile-time size cap, `WEBP_MAX_ALLOCABLE_MEMORY`, but no hook, so a
  counting allocator like the one here would need a small source change or a link-time `-Wl,--wrap=malloc`).
- **Recommendation.** Do not add it now: the app does not ask servers for WebP, the cost is 120 to 200 KB, a second decoder
  family to keep patched, and the risk list above. Add it if the board tests (section 5 of `BOARD-TESTS.md`) show real feeds whose
  only picture is WebP. The change would be: vendor `third_party/libwebp` (decoder subset), one `rss_webp_decode` next to
  `rss_image_decode` with `use_scaling`, WebP added to `Accept`, and the probe/limits as for PNG.
