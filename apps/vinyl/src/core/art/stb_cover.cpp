/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl art: the only file that includes stb_image (third_party/stb, v2.30, public domain or MIT), compiled as C++ so the
 * unit tests link it with the rest of core/, for embedded covers:
 * JPEG and PNG only, no stdio, no HDR, a counting allocator with a hard ceiling (cover.hpp). One decode at a time (the
 * cover worker): plain counters behind a mutex-free contract, as RSS Reader's image_codecs.c.
 */
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define VINYL_HDR 16
static size_t g_live, g_peak, g_limit;

static void *vinyl_stb_malloc(size_t n)
{
    if (n > (size_t)PTRDIFF_MAX - VINYL_HDR || (g_limit && g_live + n > g_limit)) return NULL;
    unsigned char *p = (unsigned char *)malloc(n + VINYL_HDR);
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    g_live += n;
    if (g_live > g_peak) g_peak = g_live;
    return p + VINYL_HDR;
}

static void vinyl_stb_free(void *q)
{
    if (!q) return;
    unsigned char *p = (unsigned char *)q - VINYL_HDR;
    size_t n;
    memcpy(&n, p, sizeof n);
    g_live -= n;
    free(p);
}

static void *vinyl_stb_realloc(void *q, size_t oldsz, size_t newsz)
{
    (void)oldsz;
    if (!q) return vinyl_stb_malloc(newsz);
    unsigned char *p = (unsigned char *)q - VINYL_HDR;
    size_t n;
    memcpy(&n, p, sizeof n);
    if (newsz > (size_t)PTRDIFF_MAX - VINYL_HDR || (g_limit && g_live - n + newsz > g_limit)) return NULL;
    unsigned char *r = (unsigned char *)realloc(p, newsz + VINYL_HDR);
    if (!r) return NULL;
    memcpy(r, &newsz, sizeof newsz);
    g_live = g_live - n + newsz;
    if (g_live > g_peak) g_peak = g_live;
    return r + VINYL_HDR;
}

#define STBI_MALLOC(sz) vinyl_stb_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) vinyl_stb_realloc(p, oldsz, newsz)
#define STBI_FREE(p) vinyl_stb_free(p)
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "../../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace vinyl {

/* The header's size and channels: 1 when stb_image reads the format. */
int vinyl_cover_info(const unsigned char *data, int len, int *w, int *h)
{
    int comp = 0;
    return stbi_info_from_memory(data, len, w, h, &comp);
}

/* Decodes to RGB (3 bytes a pixel) under the memory ceiling; free with vinyl_cover_free. */
unsigned char *vinyl_cover_decode(const unsigned char *data, int len, int *w, int *h, size_t ceiling)
{
    int comp = 0;
    g_live = 0;
    g_peak = 0;
    g_limit = ceiling;
    unsigned char *px = stbi_load_from_memory(data, len, w, h, &comp, 3);
    return px;
}

void vinyl_cover_free(unsigned char *px)
{
    if (px) stbi_image_free(px);
    g_limit = 0;
}

size_t vinyl_cover_peak(void) { return g_peak; }

}  // namespace vinyl
