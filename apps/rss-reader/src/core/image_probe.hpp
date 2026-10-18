/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: what an image is, from its first bytes, before anything decodes it. The format (PNG, JPEG, GIF, WebP,
 * BMP, SVG, AVIF), the size its header declares and, for a JPEG, whether it is baseline (the decoder the app has) or
 * progressive / arithmetic / lossless (it has none). Reads the headers only (a JPEG's markers up to its frame header),
 * never more than the bytes given, never throws: the app refuses an image here before a decoder allocates anything
 * (docs/IMAGES.md: the pixel limits).
 */
#pragma once

#include <cstdint>
#include <string>

namespace rssreader {

enum class ImageFormat { Unknown, Png, Jpeg, Gif, Webp, Bmp, Svg, Avif };

struct ImageProbe {
    ImageFormat format = ImageFormat::Unknown;
    int width = 0, height = 0;        /* as the header declares them; 0 when it does not say (or the bytes stop first) */
    bool baseline = false;            /* JPEG: a baseline frame (SOF0), what the app's decoder reads */
    bool progressive = false;         /* JPEG: SOF2 (or another non-baseline frame type: see jpeg_sof) */
    int jpeg_sof = -1;                /* JPEG: the frame marker (0xC0..0xCF), -1 when none was found */
    int components = 0;               /* JPEG: 1 grey, 3 colour, 4 CMYK */
    bool interlaced = false;          /* PNG Adam7, GIF interlaced first frame */
    bool alpha = false;               /* PNG: a colour type with alpha (or a tRNS chunk is not looked at) */
    std::uint64_t pixels() const { return static_cast<std::uint64_t>(width > 0 ? width : 0) * static_cast<std::uint64_t>(height > 0 ? height : 0); }
};

ImageProbe probe_image(const std::string &bytes);
/* "PNG", "JPEG", "GIF", "WebP", "BMP", "SVG", "AVIF", "unknown". */
const char *image_format_name(ImageFormat f);

} // namespace rssreader
