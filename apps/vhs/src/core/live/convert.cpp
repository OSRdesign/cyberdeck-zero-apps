/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: pixel conversion, the fit rule and the resampler (see convert.hpp).
 */
#include "convert.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace vhs {
namespace {

inline void store16(std::uint8_t *p, std::uint16_t v)
{
    std::memcpy(p, &v, sizeof v); /* a plain 16-bit store, without the aliasing question */
}

inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const char *pix_name(PixFmt f)
{
    return f == PixFmt::RGB565 ? "RGB565" : "XRGB8888";
}

bool parse_pixfmt(const std::string &s0, PixFmt &out)
{
    const std::string s = lower(s0);
    if (s == "rgb565" || s == "565" || s == "16") {
        out = PixFmt::RGB565;
        return true;
    }
    if (s == "xrgb8888" || s == "argb8888" || s == "8888" || s == "32") {
        out = PixFmt::XRGB8888;
        return true;
    }
    return false;
}

int scaled_up(int dim, int denom)
{
    return denom <= 1 ? dim : (dim + denom - 1) / denom;
}

int scaled_down(int dim, int denom)
{
    return denom <= 1 ? dim : dim / denom;
}

void fit_size(int src_w, int src_h, int box_w, int box_h, int &fw, int &fh)
{
    fw = fh = 0;
    if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) return;
    /* as wide as the box, unless that is too high */
    const long long h_at_w = (static_cast<long long>(box_w) * src_h + src_w / 2) / src_w;
    if (h_at_w <= box_h) {
        fw = box_w;
        fh = static_cast<int>(std::max<long long>(1, h_at_w));
    } else {
        fh = box_h;
        fw = static_cast<int>(std::max<long long>(1, (static_cast<long long>(box_h) * src_w + src_h / 2) / src_h));
    }
}

int fit_denom(int src_w, int src_h, int fw, int fh)
{
    int best = 1;
    for (int d : kDenoms)
        if (scaled_up(src_w, d) >= fw && scaled_up(src_h, d) >= fh) best = d;
    return best;
}

Rect place_in_box(int fw, int fh, int box_w, int box_h)
{
    Rect r;
    r.w = fw;
    r.h = fh;
    r.x = std::max(0, (box_w - fw) / 2);
    r.y = std::max(0, (box_h - fh) / 2);
    return r;
}

bool parse_scale(const std::string &s0, int &denom)
{
    const std::string s = lower(s0);
    if (s == "auto" || s == "0") {
        denom = 0;
        return true;
    }
    for (int d : kDenoms) {
        const std::string n = std::to_string(d);
        if (s == n || s == "1/" + n) {
            denom = d;
            return true;
        }
    }
    return false;
}

std::string scale_text(int denom)
{
    if (denom <= 0) return "auto";
    return "1/" + std::to_string(denom);
}

double picture_luma(PixFmt fmt, const std::uint8_t *px, int w, int h, int stride)
{
    if (!px || w <= 0 || h <= 0) return 0;
    const int sx = w >= 16 ? 4 : 1, sy = h >= 16 ? 4 : 1;
    long sum = 0, n = 0;
    for (int y = sy / 2; y < h; y += sy) {
        const std::uint8_t *row = px + static_cast<std::size_t>(y) * stride;
        for (int x = sx / 2; x < w; x += sx) {
            int r, g, b;
            if (fmt == PixFmt::RGB565) {
                const unsigned v = static_cast<unsigned>(row[2 * x]) | (static_cast<unsigned>(row[2 * x + 1]) << 8);
                r = static_cast<int>((v >> 11) & 31) * 255 / 31;
                g = static_cast<int>((v >> 5) & 63) * 255 / 63;
                b = static_cast<int>(v & 31) * 255 / 31;
            } else {
                b = row[4 * x];
                g = row[4 * x + 1];
                r = row[4 * x + 2];
            }
            sum += (77 * r + 150 * g + 29 * b) >> 8;
            ++n;
        }
    }
    return n ? static_cast<double>(sum) / static_cast<double>(n) : 0;
}

std::uint16_t rgb565(int r, int g, int b)
{
    return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void bgrx_to_rgb565(const std::uint8_t *src, int src_stride, std::uint8_t *dst, int dst_stride, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        const std::uint8_t *s = src + static_cast<std::ptrdiff_t>(y) * src_stride;
        std::uint8_t *d = dst + static_cast<std::ptrdiff_t>(y) * dst_stride;
        for (int x = 0; x < w; ++x) {
            const unsigned b = s[0], g = s[1], r = s[2];
            store16(d, static_cast<std::uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3)));
            s += 4;
            d += 2;
        }
    }
}

void make_axis(int src, int dst, Axis &out)
{
    out.src = src;
    out.dst = dst;
    out.i0.assign(static_cast<std::size_t>(std::max(0, dst)), 0);
    out.f.assign(static_cast<std::size_t>(std::max(0, dst)), 0);
    if (src <= 0 || dst <= 0) return;
    for (int d = 0; d < dst; ++d) {
        /* the centre of destination pixel d in source pixels, 16.16: (d + 0.5) * src / dst - 0.5 */
        long long s = ((2LL * d + 1) * src * 65536) / (2LL * dst) - 32768;
        if (s < 0) s = 0;
        int i = static_cast<int>(s >> 16);
        int f = static_cast<int>(s & 0xFFFF);
        if (i >= src - 1) {
            i = src - 1;
            f = 0;
        }
        out.i0[static_cast<std::size_t>(d)] = i;
        out.f[static_cast<std::size_t>(d)] = static_cast<std::uint16_t>(f);
    }
}

void resample_bgrx(const std::uint8_t *src, int src_w, int src_h, int src_stride, PixFmt fmt, std::uint8_t *dst,
                   int dst_w, int dst_h, int dst_stride, Axis &ax, Axis &ay)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    if (src_w == dst_w && src_h == dst_h) {
        if (fmt == PixFmt::RGB565) {
            bgrx_to_rgb565(src, src_stride, dst, dst_stride, dst_w, dst_h);
        } else {
            for (int y = 0; y < dst_h; ++y)
                std::memcpy(dst + static_cast<std::ptrdiff_t>(y) * dst_stride,
                            src + static_cast<std::ptrdiff_t>(y) * src_stride, static_cast<std::size_t>(dst_w) * 4);
        }
        return;
    }
    if (ax.src != src_w || ax.dst != dst_w || static_cast<int>(ax.i0.size()) != dst_w) make_axis(src_w, dst_w, ax);
    if (ay.src != src_h || ay.dst != dst_h || static_cast<int>(ay.i0.size()) != dst_h) make_axis(src_h, dst_h, ay);
    for (int y = 0; y < dst_h; ++y) {
        const int y0 = ay.i0[static_cast<std::size_t>(y)];
        const int y1 = std::min(y0 + 1, src_h - 1);
        const std::uint32_t fy = ay.f[static_cast<std::size_t>(y)], gy = 65536u - fy;
        const std::uint8_t *r0 = src + static_cast<std::ptrdiff_t>(y0) * src_stride;
        const std::uint8_t *r1 = src + static_cast<std::ptrdiff_t>(y1) * src_stride;
        std::uint8_t *out = dst + static_cast<std::ptrdiff_t>(y) * dst_stride;
        for (int x = 0; x < dst_w; ++x) {
            const int x0 = ax.i0[static_cast<std::size_t>(x)];
            const int x1 = std::min(x0 + 1, src_w - 1);
            const std::uint32_t fx = ax.f[static_cast<std::size_t>(x)], gx = 65536u - fx;
            const std::uint8_t *a = r0 + x0 * 4, *b = r0 + x1 * 4, *c = r1 + x0 * 4, *d = r1 + x1 * 4;
            std::uint32_t v[3];
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t top = (a[k] * gx + b[k] * fx) >> 8; /* <= 255 * 256 */
                const std::uint32_t bot = (c[k] * gx + d[k] * fx) >> 8;
                v[k] = (top * gy + bot * fy + (1u << 23)) >> 24;
            }
            if (fmt == PixFmt::RGB565) {
                store16(out + x * 2, static_cast<std::uint16_t>(((v[2] & 0xF8u) << 8) | ((v[1] & 0xFCu) << 3) | (v[0] >> 3)));
            } else {
                std::uint8_t *o = out + x * 4;
                o[0] = static_cast<std::uint8_t>(v[0]);
                o[1] = static_cast<std::uint8_t>(v[1]);
                o[2] = static_cast<std::uint8_t>(v[2]);
                o[3] = 0xFF;
            }
        }
    }
}

void rgb_to_yuv601(int r, int g, int b, int &y, int &u, int &v)
{
    y = clamp255(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    u = clamp255(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    v = clamp255(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

void yuv601_to_rgb(int y, int u, int v, int &r, int &g, int &b)
{
    const int c = 298 * (y - 16), d = u - 128, e = v - 128;
    r = clamp255((c + 409 * e + 128) >> 8);
    g = clamp255((c - 100 * d - 208 * e + 128) >> 8);
    b = clamp255((c + 516 * d + 128) >> 8);
}

void yuyv_fit(PixFmt fmt, const std::uint8_t *yuyv, int src_w, int src_h, int src_stride, std::uint8_t *dst, int dst_w,
              int dst_h, int dst_stride)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    for (int y = 0; y < dst_h; ++y) {
        const int sy = std::min(src_h - 1, static_cast<int>((2LL * y + 1) * src_h / (2LL * dst_h)));
        const std::uint8_t *row = yuyv + static_cast<std::ptrdiff_t>(sy) * src_stride;
        std::uint8_t *out = dst + static_cast<std::ptrdiff_t>(y) * dst_stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = std::min(src_w - 1, static_cast<int>((2LL * x + 1) * src_w / (2LL * dst_w)));
            const std::uint8_t *pair = row + (sx & ~1) * 2; /* Y0 U Y1 V */
            const int Y = pair[(sx & 1) ? 2 : 0], U = pair[1], V = pair[3];
            int r, g, b;
            yuv601_to_rgb(Y, U, V, r, g, b);
            if (fmt == PixFmt::RGB565) {
                store16(out + x * 2, rgb565(r, g, b));
            } else {
                std::uint8_t *o = out + x * 4;
                o[0] = static_cast<std::uint8_t>(b);
                o[1] = static_cast<std::uint8_t>(g);
                o[2] = static_cast<std::uint8_t>(r);
                o[3] = 0xFF;
            }
        }
    }
}

/* full-range (JPEG) YCbCr to RGB (BT.601) */
static inline void yuv601f_to_rgb(int y, int u, int v, int &r, int &g, int &b)
{
    const int d = u - 128, e = v - 128;
    r = clamp255(y + ((359 * e + 128) >> 8));
    g = clamp255(y - ((88 * d + 183 * e + 128) >> 8));
    b = clamp255(y + ((454 * d + 128) >> 8));
}

void yuv_fit(PixFmt fmt, const YuvPlanes &p, int src_w, int src_h, std::uint8_t *dst, int dst_w, int dst_h, int dst_stride)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !p.y || !p.u) return;
    std::vector<int> xs(static_cast<std::size_t>(dst_w));
    for (int x = 0; x < dst_w; ++x) xs[static_cast<std::size_t>(x)] = std::min(src_w - 1, (2 * x + 1) * src_w / (2 * dst_w));
    for (int y = 0; y < dst_h; ++y) {
        const int sy = std::min(src_h - 1, (2 * y + 1) * src_h / (2 * dst_h));
        const std::uint8_t *yr = p.y + static_cast<std::ptrdiff_t>(sy) * p.ys;
        const int cy = sy >> p.vshift;
        const std::uint8_t *ur = p.u + static_cast<std::ptrdiff_t>(cy) * p.us;
        const std::uint8_t *vr = p.nv12 ? ur : p.v + static_cast<std::ptrdiff_t>(cy) * p.vs;
        std::uint8_t *out = dst + static_cast<std::ptrdiff_t>(y) * dst_stride;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = xs[static_cast<std::size_t>(x)];
            const int Y = yr[sx];
            int U, V;
            if (p.nv12) {
                U = ur[(sx >> 1) * 2];
                V = ur[(sx >> 1) * 2 + 1];
            } else {
                U = ur[sx >> 1];
                V = vr[sx >> 1];
            }
            int r, g, b;
            if (p.full_range) yuv601f_to_rgb(Y, U, V, r, g, b);
            else yuv601_to_rgb(Y, U, V, r, g, b);
            if (fmt == PixFmt::RGB565) {
                store16(out + x * 2, rgb565(r, g, b));
            } else {
                std::uint8_t *o = out + x * 4;
                o[0] = static_cast<std::uint8_t>(b);
                o[1] = static_cast<std::uint8_t>(g);
                o[2] = static_cast<std::uint8_t>(r);
                o[3] = 0xFF;
            }
        }
    }
}

} // namespace vhs
