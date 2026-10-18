/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the decoders of the pinned LVGL tree behind image_codecs.h (see there and docs/IMAGES.md). Nothing here
 * touches LVGL's objects or display: the decoders run on a worker thread (lodepng allocates through lv_malloc, which is
 * the C library's malloc in the pin: LV_USE_CLIB_MALLOC).
 */
#include "image_codecs.h"

#include "lvgl/lvgl.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================== JPEG: TJpgDec */

#ifndef RSS_NO_TJPGD

#if LV_USE_TJPGD          /* a launcher whose LVGL has it on already: its copy, no second one */
#include "lvgl/src/libs/tjpgd/tjpgd.h"
#else
#undef LV_USE_TJPGD
#define LV_USE_TJPGD 1
/* TJpgDec's IDCT multiplies in int and overflows on a broken stream (UBSan sees it in the mutation fuzz): wrapping is
 * what it relies on, so make it the defined behaviour for its functions */
#pragma GCC push_options
#pragma GCC optimize("wrapv")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "lvgl/src/libs/tjpgd/tjpgd.c"
#pragma GCC diagnostic pop
#pragma GCC pop_options
#endif

/* The decoder's work memory: its 512-byte stream buffer, the Huffman and quantisation tables and one MCU (ChaN gives
 * 3.1 KB for JD_FASTDECODE 1): 8 KB leaves room. */
#define RSS_JPEG_POOL 8192

typedef struct {
    const unsigned char *data;
    size_t size, pos;
    const rss_img_sink *sink;
} rss_jpeg_io;

static size_t rss_jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    rss_jpeg_io *io = (rss_jpeg_io *)jd->device;
    size_t left = io->size - io->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, io->data + io->pos, n);
    io->pos += n;
    return n;
}

static int rss_jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    rss_jpeg_io *io = (rss_jpeg_io *)jd->device;
    const size_t stride = (size_t)(rect->right - rect->left + 1) * 3;
    /* LVGL's copy writes B G R (LVGL's RGB888 order): to R G B, in the decoder's own work buffer */
    unsigned char *p = (unsigned char *)bitmap;
    const size_t n = stride / 3 * (size_t)(rect->bottom - rect->top + 1);
    for (size_t i = 0; i < n; ++i, p += 3) {
        const unsigned char t = p[0];
        p[0] = p[2];
        p[2] = t;
    }
    return io->sink->rows(io->sink->user, rect->left, rect->top, rect->right, rect->bottom, (const unsigned char *)bitmap, 3, stride) ? 1 : 0;
}

int rss_jpeg_decode(const unsigned char *data, size_t size, const rss_img_sink *sink)
{
    if (!data || size < 4 || !sink || !sink->rows) return RSS_IMG_BROKEN;
    void *pool = malloc(RSS_JPEG_POOL);
    if (!pool) return RSS_IMG_BROKEN;
    JDEC jd;
    memset(&jd, 0, sizeof jd);
    rss_jpeg_io io = {data, size, 0, sink};
    int rc = RSS_IMG_OK;
    JRESULT r = jd_prepare(&jd, rss_jpeg_in, pool, RSS_JPEG_POOL, &io);
    if (r == JDR_FMT3 || r == JDR_FMT2) rc = RSS_IMG_UNSUPPORTED;
    else if (r != JDR_OK || jd.width == 0 || jd.height == 0) rc = RSS_IMG_BROKEN;
    else if (sink->begin && !sink->begin(sink->user, jd.width, jd.height)) rc = RSS_IMG_STOPPED;
    else {
        r = jd_decomp(&jd, rss_jpeg_out, 0);
        if (r == JDR_INTR) rc = RSS_IMG_STOPPED;
        else if (r == JDR_FMT3 || r == JDR_FMT2) rc = RSS_IMG_UNSUPPORTED;
        else if (r != JDR_OK) rc = RSS_IMG_BROKEN;
    }
    free(pool);
    return rc;
}

int rss_jpeg_available(void) { return 1; }

#else /* RSS_NO_TJPGD */

int rss_jpeg_decode(const unsigned char *data, size_t size, const rss_img_sink *sink)
{
    (void)data;
    (void)size;
    (void)sink;
    return RSS_IMG_NO_DECODER;
}

int rss_jpeg_available(void) { return 0; }

#endif

/* ================================================================== PNG: lodepng */

#if LV_USE_LODEPNG
#include "lvgl/src/libs/lodepng/lodepng.h"

int rss_png_inspect(const unsigned char *data, size_t size, unsigned *width, unsigned *height)
{
    LodePNGState st;
    unsigned w = 0, h = 0;
    lodepng_state_init(&st);
    unsigned err = lodepng_inspect(&w, &h, &st, data, size);
    lodepng_state_cleanup(&st);
    if (width) *width = err ? 0 : w;
    if (height) *height = err ? 0 : h;
    return err == 0 && w > 0 && h > 0;
}

int rss_png_decode(const unsigned char *data, size_t size, const rss_img_sink *sink)
{
    unsigned w = 0, h = 0;
    if (!data || !sink || !sink->rows || !rss_png_inspect(data, size, &w, &h)) return RSS_IMG_BROKEN;
    if (sink->begin && !sink->begin(sink->user, w, h)) return RSS_IMG_STOPPED;
    /* LVGL's lodepng hands back an lv_draw_buf_t (ARGB8888-sized, the bytes in lodepng's R G B A order) */
    unsigned char *raw = NULL;
    unsigned err = lodepng_decode32(&raw, &w, &h, data, size);
    lv_draw_buf_t *buf = (lv_draw_buf_t *)raw;
    if (err || !buf) {
        if (buf) lv_draw_buf_destroy(buf);
        return RSS_IMG_BROKEN;
    }
    const size_t stride = buf->header.stride ? buf->header.stride : (size_t)w * 4;
    int rc = RSS_IMG_OK;
    for (unsigned y = 0; y < h; y += 16) {          /* bands of 16 rows: the sink checks its cancel at each call */
        const unsigned bottom = y + 15 < h ? y + 15 : h - 1;
        if (!sink->rows(sink->user, 0, y, w - 1, bottom, buf->data + (size_t)y * stride, 4, stride)) {
            rc = RSS_IMG_STOPPED;
            break;
        }
    }
    lv_draw_buf_destroy(buf);
    return rc;
}

int rss_png_available(void) { return 1; }
#else
int rss_png_inspect(const unsigned char *data, size_t size, unsigned *width, unsigned *height)
{
    (void)data;
    (void)size;
    if (width) *width = 0;
    if (height) *height = 0;
    return 0;
}
int rss_png_decode(const unsigned char *data, size_t size, const rss_img_sink *sink)
{
    (void)data;
    (void)size;
    (void)sink;
    return RSS_IMG_NO_DECODER;
}
int rss_png_available(void) { return 0; }
#endif

/* ================================================================== GIF: AnimatedGIF
 *
 * Off unless built with -DRSS_USE_ANIMATEDGIF: the pin's AnimatedGIF is written for GIFs the program ships, not for what
 * a feed links to: the mutation fuzz of tests/unit/img_tests.cpp (a valid GIF with a few bytes changed) makes it memmove a
 * negative size in GIFParseInfo (gif.c:360, under ASan). Without it every GIF shows the "not supported" placeholder. */

#if LV_USE_GIF && defined(RSS_USE_ANIMATEDGIF)
#include "lvgl/src/libs/gif/AnimatedGIF.h"

typedef struct {
    const rss_img_sink *sink;
    int stopped;
    unsigned char line[MAX_WIDTH * 4];
} rss_gif_state;

static void rss_gif_line(GIFDRAW *d)
{
    rss_gif_state *s = (rss_gif_state *)d->pUser;
    if (!s || s->stopped) return;
    const int w = d->iWidth < MAX_WIDTH ? d->iWidth : MAX_WIDTH;
    for (int x = 0; x < w; ++x) {
        const unsigned ix = d->pPixels[x];
        unsigned char *o = s->line + x * 4;
        o[0] = d->pPalette24[ix * 3];
        o[1] = d->pPalette24[ix * 3 + 1];
        o[2] = d->pPalette24[ix * 3 + 2];
        o[3] = (d->ucHasTransparency && ix == d->ucTransparent) ? 0 : 255;
    }
    const unsigned y = (unsigned)(d->iY + d->y);
    if (w > 0 && !s->sink->rows(s->sink->user, (unsigned)d->iX, y, (unsigned)(d->iX + w - 1), y, s->line, 4, (size_t)w * 4)) s->stopped = 1;
}

int rss_gif_decode(const unsigned char *data, size_t size, const rss_img_sink *sink)
{
    if (!data || size < 13 || size > 0x7FFFFFFF || !sink || !sink->rows) return RSS_IMG_BROKEN;
    GIFIMAGE *g = (GIFIMAGE *)calloc(1, sizeof(GIFIMAGE));
    rss_gif_state *s = (rss_gif_state *)calloc(1, sizeof(rss_gif_state));
    if (!g || !s) {
        free(g);
        free(s);
        return RSS_IMG_BROKEN;
    }
    s->sink = sink;
    int rc = RSS_IMG_OK;
    GIF_begin(g, GIF_PALETTE_RGB888);
    /* AnimatedGIF takes a non-const pointer but only reads the bytes */
    if (!GIF_openRAM(g, (uint8_t *)data, (int)size, rss_gif_line)) {
        rc = GIF_getLastError(g) == GIF_TOO_WIDE ? RSS_IMG_UNSUPPORTED : RSS_IMG_BROKEN;
    } else {
        const int cw = GIF_getCanvasWidth(g), ch = GIF_getCanvasHeight(g);
        if (cw <= 0 || ch <= 0) rc = RSS_IMG_BROKEN;
        else if (sink->begin && !sink->begin(sink->user, (unsigned)cw, (unsigned)ch)) rc = RSS_IMG_STOPPED;
        else {
            int delay = 0;
            GIF_playFrame(g, &delay, s);
            const int e = GIF_getLastError(g);
            if (s->stopped) rc = RSS_IMG_STOPPED;
            else if (e != GIF_SUCCESS && e != GIF_EMPTY_FRAME) rc = RSS_IMG_BROKEN;
        }
        GIF_close(g);
    }
    free(s);
    free(g);
    return rc;
}

int rss_gif_available(void) { return 1; }
int rss_gif_max_width(void) { return MAX_WIDTH; }
#else
int rss_gif_decode(const unsigned char *data, size_t size, const rss_img_sink *sink)
{
    (void)data;
    (void)size;
    (void)sink;
    return RSS_IMG_NO_DECODER;
}
int rss_gif_available(void) { return 0; }
int rss_gif_max_width(void) { return 0; }
#endif
