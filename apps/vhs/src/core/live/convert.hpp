/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: from a decoded (or YUYV) frame to the pixels LVGL draws (from the spike), and the fit rule (brief 4.1 rule 7, the
 * Controller's design call in plan 16.5: the WHOLE frame is shown, letterboxed or pillarboxed, never cropped).
 *
 *   fit        the frame's aspect inside the picture box: fit size (fw, fh), centred, the rest of the box a border
 *   decode     at the LARGEST denominator d in {1, 2, 4, 8} with tj_scaled(w, d) >= fw and tj_scaled(h, d) >= fh (the
 *              IDCT does the big step and nothing is ever scaled up)
 *   resample   the decoded picture to (fw, fh) during the conversion to the display format (bilinear, 16.16 fixed point);
 *              when the decoded size already is the fit size, a plain conversion. LVGL never scales.
 *
 * Pixel formats: RGB565 (LVGL's LV_COLOR_FORMAT_RGB565, native little-endian: the 16-bit panels, zero8 and the Pi 3A+)
 * and XRGB8888 (LV_COLOR_FORMAT_XRGB8888: bytes B G R X, what TurboJPEG writes as TJPF_BGRX). LVGL-free.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vhs {

enum class PixFmt { RGB565, XRGB8888 };
inline int pix_bytes(PixFmt f) { return f == PixFmt::RGB565 ? 2 : 4; }
const char *pix_name(PixFmt f);                     /* "RGB565", "XRGB8888" */
bool parse_pixfmt(const std::string &s, PixFmt &out); /* "rgb565", "565", "xrgb8888", "argb8888", "8888", "32" */

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

/* The decode scale denominators. */
constexpr int kDenoms[] = {1, 2, 4, 8};
/* Size of dim at 1/denom: rounded up (TurboJPEG's TJSCALED) for JPEG, down (point sampling) for YUYV. */
int scaled_up(int dim, int denom);
int scaled_down(int dim, int denom);

/* The largest size with src's aspect inside the box (each side at least 1). */
void fit_size(int src_w, int src_h, int box_w, int box_h, int &fw, int &fh);
/* The largest denominator whose JPEG decode still covers fw x fh (1 when even 1/1 does not). */
int fit_denom(int src_w, int src_h, int fw, int fh);
/* Where a fw x fh picture goes in the box: centred. */
Rect place_in_box(int fw, int fh, int box_w, int box_h);

/* "auto", "1", "2", "4", "8" (also "1/2"...) -> 0 (auto) or the denominator; false on anything else. */
bool parse_scale(const std::string &s, int &denom);
std::string scale_text(int denom); /* 0 -> "auto", 2 -> "1/2" */

/* BGRX rows -> RGB565 rows (w x h pixels, strides in bytes). */
void bgrx_to_rgb565(const std::uint8_t *src, int src_stride, std::uint8_t *dst, int dst_stride, int w, int h);

/* The bilinear sampling table for one axis: for each of `dst` outputs the first source index and the weight of the
 * next one (0..65535), the source centre-aligned on the destination. */
struct Axis {
    std::vector<int> i0;
    std::vector<std::uint16_t> f;
    int src = 0, dst = 0;
};
void make_axis(int src, int dst, Axis &out);

/* BGRX src_w x src_h -> fmt dst_w x dst_h, bilinear (equal sizes: a plain conversion). The axes are cached by the
 * caller (they change only with the sizes). */
void resample_bgrx(const std::uint8_t *src, int src_w, int src_h, int src_stride, PixFmt fmt, std::uint8_t *dst,
                   int dst_w, int dst_h, int dst_stride, Axis &ax, Axis &ay);

/* YUYV (BT.601, limited range) src_w x src_h -> fmt dst_w x dst_h, nearest sample (the live view of a camera without
 * MJPEG; the whole frame). */
void yuyv_fit(PixFmt fmt, const std::uint8_t *yuyv, int src_w, int src_h, int src_stride, std::uint8_t *dst, int dst_w,
              int dst_h, int dst_stride);

/* One pixel (tests and the fake camera). */
void rgb_to_yuv601(int r, int g, int b, int &y, int &u, int &v);
void yuv601_to_rgb(int y, int u, int v, int &r, int &g, int &b);
std::uint16_t rgb565(int r, int g, int b);

/* Playback (brief 4.4: no libswscale): a decoded YUV picture fitted (the nearest sample, centred) into dst_w x dst_h
 * in the display format. Planar 4:2:0 (vshift 1) or 4:2:2 (vshift 0), limited range, or full range for the J formats;
 * NV12: u is the interleaved UV plane (v unused). */
struct YuvPlanes {
    const std::uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
    int ys = 0, us = 0, vs = 0;
    int vshift = 1;
    bool nv12 = false;
    bool full_range = false;
};
void yuv_fit(PixFmt fmt, const YuvPlanes &p, int src_w, int src_h, std::uint8_t *dst, int dst_w, int dst_h, int dst_stride);

} // namespace vhs
