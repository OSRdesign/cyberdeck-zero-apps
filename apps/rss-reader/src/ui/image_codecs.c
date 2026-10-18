/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the image decoder (stb_image) behind image_codecs.h (see there and docs/IMAGES.md). Nothing here touches
 * LVGL: it runs on a worker thread with the C library's malloc behind a counting allocator.
 */
#include "image_codecs.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- the counting allocator: every block carries its size in a 16-byte header; a request past the ceiling fails (stb_image
 * then reports "outofmem"). One decode at a time: plain counters, no lock. */
#define RSS_HDR 16
static size_t g_live, g_peak, g_limit;

static void *rss_stb_malloc(size_t n)
{
    if (n > (size_t)PTRDIFF_MAX - RSS_HDR || (g_limit && g_live + n > g_limit)) return NULL;
    unsigned char *p = (unsigned char *)malloc(n + RSS_HDR);
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    g_live += n;
    if (g_live > g_peak) g_peak = g_live;
    return p + RSS_HDR;
}

static void rss_stb_free(void *q)
{
    if (!q) return;
    unsigned char *p = (unsigned char *)q - RSS_HDR;
    size_t n;
    memcpy(&n, p, sizeof n);
    g_live -= n;
    free(p);
}

static void *rss_stb_realloc(void *q, size_t oldsz, size_t newsz)
{
    (void)oldsz;
    if (!q) return rss_stb_malloc(newsz);
    size_t old;
    memcpy(&old, (unsigned char *)q - RSS_HDR, sizeof old);
    void *r = rss_stb_malloc(newsz);
    if (!r) return NULL;
    memcpy(r, q, old < newsz ? old : newsz);
    rss_stb_free(q);
    return r;
}

/* ---- stb_image: only what the reader shows, no file access, no float, no SIMD (the code that is tested on the PC is the
 * code that runs on the board). STBI_MAX_DIMENSIONS: a header over 16384 on a side is refused by the library itself. */
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 16384
#define STBI_MALLOC(sz) rss_stb_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) rss_stb_realloc(p, oldsz, newsz)
#define STBI_FREE(p) rss_stb_free(p)
#define STB_IMAGE_IMPLEMENTATION
/* stb_image's JPEG IDCT multiplies ints that a broken stream makes overflow (UBSan sees it in the mutation fuzz of
 * tests/unit/img_tests.cpp, stb_image.h:2502): wrapping is what the code relies on, so make it the defined behaviour for
 * the functions of this one header (the file itself stays the unmodified upstream copy). */
#pragma GCC push_options
#pragma GCC optimize("wrapv")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "../../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop
#pragma GCC pop_options

/* The library gives a reason string, not a code: refusals by size or memory, then variants it does not read, else broken. */
static int rss_stb_error(void)
{
    const char *r = stbi_failure_reason();
    if (!r) return RSS_IMG_BROKEN;
    if (strstr(r, "too large") || strstr(r, "outofmem")) return RSS_IMG_TOO_LARGE;
    if (strstr(r, "unsupported") || strstr(r, "only 8-bit")) return RSS_IMG_UNSUPPORTED;
    return RSS_IMG_BROKEN;
}

int rss_image_info(const unsigned char *data, size_t size, unsigned *width, unsigned *height)
{
    int w = 0, h = 0, n = 0;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!data || size < 4 || size > INT_MAX) return 0;
    if (!stbi_info_from_memory(data, (int)size, &w, &h, &n) || w <= 0 || h <= 0) return 0;
    if (width) *width = (unsigned)w;
    if (height) *height = (unsigned)h;
    return 1;
}

int rss_image_decode(const unsigned char *data, size_t size, int bytes_per_pixel, size_t mem_limit, const rss_img_sink *sink)
{
    if (!data || size < 4 || size > INT_MAX || !sink || !sink->rows) return RSS_IMG_BROKEN;
    if (bytes_per_pixel != 3 && bytes_per_pixel != 4) return RSS_IMG_BROKEN;
    g_live = 0;
    g_peak = 0;
    g_limit = mem_limit;
    int w = 0, h = 0, n = 0;
    /* the header first: the size is checked (by the caller, in `begin`) before the library allocates a pixel */
    if (!stbi_info_from_memory(data, (int)size, &w, &h, &n) || w <= 0 || h <= 0) return rss_stb_error();
    if (sink->begin && !sink->begin(sink->user, (unsigned)w, (unsigned)h)) return RSS_IMG_STOPPED;
    int w2 = 0, h2 = 0;
    unsigned char *px = stbi_load_from_memory(data, (int)size, &w2, &h2, &n, bytes_per_pixel);
    if (!px) return rss_stb_error();
    int rc = RSS_IMG_OK;
    if (w2 != w || h2 != h) {
        rc = RSS_IMG_BROKEN;                           /* the header and the data disagree */
    } else {
        const size_t stride = (size_t)w * (size_t)bytes_per_pixel;
        for (int y = 0; y < h; y += 16) {              /* bands of 16 rows: the sink checks its cancel at each call */
            const int bottom = y + 15 < h ? y + 15 : h - 1;
            if (!sink->rows(sink->user, 0, (unsigned)y, (unsigned)w - 1, (unsigned)bottom, px + (size_t)y * stride, bytes_per_pixel, stride)) {
                rc = RSS_IMG_STOPPED;
                break;
            }
        }
    }
    stbi_image_free(px);
    return rc;
}

size_t rss_image_last_peak(void) { return g_peak; }

unsigned rss_image_max_dimension(void) { return STBI_MAX_DIMENSIONS; }
