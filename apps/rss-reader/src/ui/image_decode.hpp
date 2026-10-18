/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: image bytes to pixels the reader can show, off the UI thread. What the pinned launcher's LVGL offers and
 * what it costs is in docs/IMAGES.md; in short: one decoder, stb_image (third_party/stb, via image_codecs.h), for
 *
 *   JPEG  baseline and progressive (8-bit Huffman); arithmetic, lossless, 12-bit -> Unsupported
 *   PNG   all colour types, interlaced or not
 *   GIF   the first frame only
 *   BMP   the common variants
 *
 * stb_image decodes a picture whole, so the memory is a few bytes a pixel (the pixel limits below keep it under
 * kDecodeMemCeiling, which the decoder's allocator enforces); WebP, SVG, AVIF -> Unsupported.
 *
 * The header is read first (core/image_probe.hpp): a picture over its format's pixel limit is refused before anything is
 * allocated. The result is scaled down (an area average, never up) to fit a box and kept as RGB565 (the panels' 16-bit
 * format: 2 bytes a pixel, drawn by LVGL as it is). Decodes take turns (one at a time in the whole process, whichever
 * thread asks): a decode waiting for its turn can be cancelled.
 */
#pragma once

#include "image_probe.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rssui {

/* The pixel limits (width x height from the header), measured on the host harness: docs/IMAGES.md. */
constexpr std::uint64_t kMaxJpegPixels = 16u * 1000 * 1000;             /* baseline: 4.4 bytes a pixel at the peak (4:2:0), 5.9 (4:4:4): 70 to 94 MB at the limit */
constexpr std::uint64_t kMaxProgressiveJpegPixels = 6u * 1000 * 1000;   /* progressive keeps every DCT coefficient: 7.3 bytes a pixel (4:2:0), 11.7 (4:4:4): 44 to 70 MB at the limit */
constexpr std::uint64_t kMaxPngPixels = 4200u * 1000;                   /* about 8 bytes a pixel: 34 MB at the limit */
constexpr std::uint64_t kMaxGifPixels = 4200u * 1000;                   /* 8.8 bytes a pixel (canvas, background, history): 37 MB at the limit */
constexpr std::uint64_t kMaxBmpPixels = 4200u * 1000;                   /* 4 bytes a pixel: 17 MB at the limit */
constexpr std::size_t kDecodeMemCeiling = 96u * 1024 * 1024;            /* the decoder's allocator refuses past this: TooLarge (a 16 MP 4:4:4 JPEG is 94 MB: just under) */

enum class DecodeStatus { Ok, Unsupported, TooLarge, Broken, Cancelled };
const char *decode_status_name(DecodeStatus s);               /* "ok", "unsupported", "too large", "broken", "cancelled" */

struct DecodedImage {
    int width = 0, height = 0;                 /* of the pixels */
    int src_width = 0, src_height = 0;         /* of the picture */
    std::vector<std::uint8_t> rgb565;          /* width * height * 2 bytes, LVGL's RGB565 (little endian) */
};

/* Whether this build can decode an image with that header within the limits (Ok), without decoding it. */
DecodeStatus decode_check(const rssreader::ImageProbe &p);

/* Decodes and scales to fit max_w x max_h (no enlargement), the transparent parts drawn over bg (0xRRGGBB). Never throws;
 * Cancelled when *cancel turns true (checked while waiting for the turn and while decoding). */
DecodeStatus decode_image(const std::string &bytes, int max_w, int max_h, std::uint32_t bg, DecodedImage &out,
                          const std::atomic<bool> *cancel = nullptr);

/* The size a sw x sh picture gets in a max_w x max_h box: proportions kept, never larger than the picture, at least 1 x 1. */
void fit_size(int sw, int sh, int max_w, int max_h, int &w, int &h);

/* The number of decodes that ran so far, and the largest number that ever ran at the same time (1: they took turns). */
int decodes_done();
int decodes_peak_parallel();

} // namespace rssui
