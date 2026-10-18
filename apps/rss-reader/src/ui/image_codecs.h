/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the image decoder behind one small C interface: stb_image (public domain / MIT, third_party/stb, one header,
 * compiled into image_codecs.c only). See docs/IMAGES.md.
 *
 *   JPEG  baseline and progressive, 8-bit, Huffman (grey, YCbCr, CMYK/YCCK); arithmetic, lossless and 12-bit are refused
 *   PNG   every colour type and bit depth, interlaced or not
 *   GIF   the first frame only (transparency kept as alpha 0)
 *   BMP   the common variants (stb_image's reader)
 *   Everything else stb_image reads (PSD, TGA, HDR, PIC, PNM) is compiled out (STBI_ONLY_*).
 *
 * The picture is decoded whole (stb_image has no streaming API), then handed out in bands of rows. Every allocation of
 * the decoder goes through a counter with a hard ceiling (`mem_limit`): a picture that would need more is refused
 * (RSS_IMG_TOO_LARGE) instead of exhausting a small board. Not thread safe: the app decodes one picture at a time.
 *
 * `begin` is called with the picture's size from its header (return 0 to stop there: too large, or cancelled), then
 * `rows` with rectangles of pixels, 3 bytes (R G B) or 4 (R G B A) each, `stride` bytes from one row to the next
 * (return 0 to stop).
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rss_img_sink {
    void *user;
    int (*begin)(void *user, unsigned width, unsigned height);
    int (*rows)(void *user, unsigned left, unsigned top, unsigned right, unsigned bottom, const unsigned char *px, int bytes_per_pixel,
                size_t stride);
} rss_img_sink;

enum {
    RSS_IMG_OK = 0,
    RSS_IMG_STOPPED = 1,         /* a callback returned 0 */
    RSS_IMG_UNSUPPORTED = 2,     /* a variant the decoder does not read */
    RSS_IMG_BROKEN = 3,          /* not that format, or broken */
    RSS_IMG_NO_DECODER = 4,      /* this build has no decoder */
    RSS_IMG_TOO_LARGE = 5,       /* over the decoder's dimension limit or the memory ceiling */
};

/* Decodes a JPEG, PNG, GIF (first frame) or BMP. `bytes_per_pixel`: 3 (R G B, for JPEG) or 4 (R G B A). `mem_limit`: the
 * ceiling in bytes for all the decoder's allocations together (0: none). */
int rss_image_decode(const unsigned char *data, size_t size, int bytes_per_pixel, size_t mem_limit, const rss_img_sink *sink);

/* The size the header declares (stb_image's own header reader); 1 when it reads it, else 0 and 0 x 0. */
int rss_image_info(const unsigned char *data, size_t size, unsigned *width, unsigned *height);

/* The most the decoder had allocated at one time during the last rss_image_decode (bytes): for the measurements. */
size_t rss_image_last_peak(void);

/* The longest side stb_image accepts (STBI_MAX_DIMENSIONS). */
unsigned rss_image_max_dimension(void);

#ifdef __cplusplus
}
#endif
