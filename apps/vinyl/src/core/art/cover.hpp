/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl art: the pictures of the now-playing page, as pixel buffers the UI hands to LVGL (pure, no LVGL; tests/test_art):
 *
 *   decode_cover   an embedded cover (JPEG or PNG, stb_image) scaled to the sleeve's size. stb_image cannot scale while
 *                  it decodes, so the memory of a decode is the whole picture (a 3000 px cover is 27 MB in RGB, report 059
 *                  section 1.5, and the owner skipped the measuring probe): a picture whose long side is over
 *                  kMaxCoverSide (1200 px, 4.3 MB) is NOT decoded (TooLarge: the plain sleeve shows instead), the
 *                  decoder's allocator refuses more than kCoverMemCeiling, and the full picture is freed as soon as it is
 *                  scaled. A cover that is not square is cut to its centre square, as a sleeve shows it.
 *   render_sleeve  the plain sleeve of the drafts (diagonal red-brown-teal gradient with a cream ring) for a file without
 *                  a cover;
 *   render_disc    the record: striped grooves (#141414 / #1f1f1f, 2 design px each), transparent outside;
 *   render_label   the teal centre label with its spin marks (a cream dot near the rim, a cream ring round the dark
 *                  spindle hole), the only part that turns
 *                  (the grooves look the same at any angle), so the turning is visible.
 * Buffers are ARGB8888 (LVGL's LV_COLOR_FORMAT_ARGB8888, little endian B G R A) or RGB565 (covers, opaque).
 */
#pragma once

#include "../tags/tag_reader.hpp"

#include <cstdint>
#include <vector>

namespace vinyl {

constexpr int kMaxCoverSide = 1200;
constexpr size_t kCoverMemCeiling = 24u << 20;

enum class CoverStatus { Ok, None, TooLarge, Broken };
const char *cover_status_name(CoverStatus s);

struct PixelImage {
    int w = 0, h = 0;
    bool rgb565 = false;            /* else ARGB8888 */
    std::vector<uint8_t> px;        /* w * h * (2 or 4) bytes */
};

/* The size of a picture from its header (no decode): false when it is not a JPEG or PNG stb_image reads. */
bool cover_size(const Picture &pic, int &w, int &h);
/* Decodes and scales to side x side (RGB565). */
CoverStatus decode_cover(const Picture &pic, int side, PixelImage &out, int max_side = kMaxCoverSide);

/* Peak bytes the cover decoder's allocator held during the last decode (for the tests and the log). */
size_t cover_peak_bytes();

PixelImage render_sleeve(int side);
PixelImage render_disc(int diameter, double design_scale);
/* label_d: the label's diameter, hole_d the spindle hole's (the drafts: 50/6 on a 148 disc, 64/10 on a 196 disc). */
PixelImage render_label(int label_d, int hole_d, double design_scale);

}  // namespace vinyl
