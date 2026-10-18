/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the image decoders of the pinned launcher's LVGL tree, behind one small C interface (their headers are C and
 * do not mix with C++: image_codecs.c is the only file that includes them). See docs/IMAGES.md.
 *
 *   JPEG  TJpgDec (ChaN), lvgl/src/libs/tjpgd: in the pin's source tree but left out by its configuration (LV_USE_TJPGD 0);
 *         image_codecs.c compiles that file, unmodified, for this translation unit (-DRSS_NO_TJPGD leaves it out: every
 *         JPEG is then "not supported"). Baseline only (SOF0, 8-bit, grey or YCbCr); streamed block by block (an MCU,
 *         8x8 to 16x16 pixels) with 8 KB of work memory whatever the size of the picture.
 *   PNG   lodepng, lvgl/src/libs/lodepng (LV_USE_LODEPNG 1 in the pin): the whole picture is inflated, then converted
 *         into an ARGB8888-sized buffer (LVGL's modification), handed out row by row, then freed.
 *   GIF   AnimatedGIF, lvgl/src/libs/gif (LV_USE_GIF 1 in the pin): only with -DRSS_USE_ANIMATEDGIF (it is not safe on a
 *         broken file: image_codecs.c); then the first frame, line by line (8-bit indices through the palette, transparent
 *         pixels with alpha 0), canvases wider than 480 pixels refused (its MAX_WIDTH).
 *
 * The decoders call `begin` with the picture's size (return 0 to stop there: too large, or cancelled), then `rows` with
 * rectangles of pixels, 3 bytes (R G B) or 4 (R G B A) each, `stride` bytes from one row to the next (return 0 to stop).
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
    RSS_IMG_UNSUPPORTED = 2,     /* a variant this decoder does not read (progressive JPEG, a GIF over 480 wide ...) */
    RSS_IMG_BROKEN = 3,          /* not that format, or broken */
    RSS_IMG_NO_DECODER = 4,      /* this build has no decoder for the format */
};

int rss_jpeg_decode(const unsigned char *data, size_t size, const rss_img_sink *sink);
int rss_png_decode(const unsigned char *data, size_t size, const rss_img_sink *sink);
int rss_gif_decode(const unsigned char *data, size_t size, const rss_img_sink *sink);
/* The width and height of a PNG from lodepng's own header check (0 on failure). */
int rss_png_inspect(const unsigned char *data, size_t size, unsigned *width, unsigned *height);

int rss_jpeg_available(void);
int rss_png_available(void);
int rss_gif_available(void);
/* AnimatedGIF's widest canvas (480 in the pin). */
int rss_gif_max_width(void);

#ifdef __cplusplus
}
#endif
