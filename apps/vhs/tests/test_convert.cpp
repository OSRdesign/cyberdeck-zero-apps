/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the fit rule (the whole frame in the box: fit size, the largest decode denominator that still covers it,
 * centred placement, brief 4.1 rule 7 and the table of 4.2), the bilinear resample to the fit size (edges, odd sizes,
 * RGB565 and XRGB8888), the BGRX -> RGB565 conversion and the YUYV live view.
 */
#include "check.hpp"

#include "live/convert.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace vhs;

namespace {

std::uint16_t px16(const std::vector<std::uint8_t> &buf, std::size_t i)
{
    std::uint16_t v;
    std::memcpy(&v, buf.data() + i * 2, 2);
    return v;
}

struct Fit {
    int fw, fh, d;
};

Fit fit(int w, int h, int bw, int bh)
{
    Fit f{0, 0, 0};
    fit_size(w, h, bw, bh, f.fw, f.fh);
    f.d = fit_denom(w, h, f.fw, f.fh);
    return f;
}

void fit_rule()
{
    CHECK_EQ(scaled_up(1280, 1), 1280);
    CHECK_EQ(scaled_up(1280, 2), 640);
    CHECK_EQ(scaled_up(1081, 8), 136); /* TJSCALED rounds up */
    CHECK_EQ(scaled_down(1081, 8), 135);
    /* zero8 (320x480, box 288x162): 1080p at 1/4 = 480x270, fitted to 288x162 */
    Fit f = fit(1920, 1080, 288, 162);
    CHECK_EQ(f.fw, 288);
    CHECK_EQ(f.fh, 162);
    CHECK_EQ(f.d, 4);
    CHECK_EQ(fit(1280, 720, 288, 162).d, 4); /* 320x180 */
    /* the 480x320 page's 352x198 box */
    CHECK_EQ(fit(1920, 1080, 352, 198).d, 4); /* 480x270; 240x135 at 1/8 is too small */
    CHECK_EQ(fit(1280, 720, 352, 198).d, 2);  /* 640x360; 320x180 is too small */
    /* RPI52 / palmpi (box about 640x360): 1080p at 1/2, 720p at 1/2 */
    CHECK_EQ(fit(1920, 1080, 640, 360).d, 2);
    CHECK_EQ(fit(1280, 720, 640, 360).d, 2);
    /* the 800x480 class box (528x297): 1/2 */
    f = fit(1920, 1080, 528, 297);
    CHECK_EQ(f.fw, 528);
    CHECK_EQ(f.fh, 297);
    CHECK_EQ(f.d, 2);
    /* the C270 (4:3) in a 16:9 box: pillarboxed, 480x360 from 1/2 */
    f = fit(1280, 960, 640, 360);
    CHECK_EQ(f.fw, 480);
    CHECK_EQ(f.fh, 360);
    CHECK_EQ(f.d, 2);
    f = fit(640, 480, 352, 198);
    CHECK_EQ(f.fw, 264);
    CHECK_EQ(f.fh, 198);
    CHECK_EQ(f.d, 2);
    /* the FineCam turned (position A) in a landscape box: a narrow upright picture, never cropped */
    f = fit(1080, 1920, 352, 198);
    CHECK_EQ(f.fh, 198);
    CHECK_EQ(f.fw, 111);
    CHECK_EQ(f.d, 8); /* 135x240 covers 111x198 */
    f = fit(720, 1280, 288, 162);
    CHECK_EQ(f.fw, 91);
    CHECK_EQ(f.fh, 162);
    /* a box larger than the frame: 1/1 and the resample scales up */
    f = fit(640, 360, 1200, 675);
    CHECK_EQ(f.fw, 1200);
    CHECK_EQ(f.d, 1);
    /* degenerate */
    int fw = -1, fh = -1;
    fit_size(0, 720, 352, 198, fw, fh);
    CHECK_EQ(fw, 0);
    CHECK_EQ(fh, 0);
    fit_size(4000, 10, 352, 198, fw, fh);
    CHECK(fw == 352 && fh >= 1);
    /* placement: centred, the rest a border */
    CHECK(place_in_box(264, 198, 352, 198) == (Rect{44, 0, 264, 198}));
    CHECK(place_in_box(352, 198, 352, 198) == (Rect{0, 0, 352, 198}));
    CHECK(place_in_box(111, 198, 352, 198) == (Rect{120, 0, 111, 198}));

    int d = -1;
    CHECK(parse_scale("auto", d));
    CHECK_EQ(d, 0);
    CHECK(parse_scale("4", d));
    CHECK_EQ(d, 4);
    CHECK(parse_scale("1/8", d));
    CHECK_EQ(d, 8);
    CHECK(!parse_scale("3", d));
    CHECK(!parse_scale("", d));
    CHECK_EQ(scale_text(0), std::string("auto"));
    CHECK_EQ(scale_text(2), std::string("1/2"));
    PixFmt pf = PixFmt::XRGB8888;
    CHECK(parse_pixfmt("RGB565", pf));
    CHECK(pf == PixFmt::RGB565);
    CHECK(parse_pixfmt("argb8888", pf));
    CHECK(pf == PixFmt::XRGB8888);
    CHECK(!parse_pixfmt("yuv", pf));
}

void bgrx()
{
    /* 3 x 2 BGRX: white, red, green / blue, black, grey */
    const std::uint8_t src[] = {255, 255, 255, 255, 0, 0, 255, 255, 0, 255, 0, 255,
                                255, 0, 0, 255,     0, 0, 0, 255,     128, 128, 128, 255};
    std::vector<std::uint8_t> dst(3 * 2 * 2);
    bgrx_to_rgb565(src, 12, dst.data(), 6, 3, 2);
    CHECK_EQ(px16(dst, 0), static_cast<std::uint16_t>(0xFFFF));
    CHECK_EQ(px16(dst, 1), static_cast<std::uint16_t>(0xF800));
    CHECK_EQ(px16(dst, 2), static_cast<std::uint16_t>(0x07E0));
    CHECK_EQ(px16(dst, 3), static_cast<std::uint16_t>(0x001F));
    CHECK_EQ(px16(dst, 4), static_cast<std::uint16_t>(0x0000));
    CHECK_EQ(px16(dst, 5), rgb565(128, 128, 128));
    /* the resampler at equal sizes is this conversion */
    std::vector<std::uint8_t> same(3 * 2 * 2);
    Axis ax, ay;
    resample_bgrx(src, 3, 2, 12, PixFmt::RGB565, same.data(), 3, 2, 6, ax, ay);
    CHECK(same == dst);
    std::vector<std::uint8_t> copy(3 * 2 * 4);
    resample_bgrx(src, 3, 2, 12, PixFmt::XRGB8888, copy.data(), 3, 2, 12, ax, ay);
    CHECK(std::memcmp(copy.data(), src, copy.size()) == 0);
}

std::vector<std::uint8_t> solid(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r)
{
    std::vector<std::uint8_t> v(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < v.size(); i += 4) {
        v[i] = b;
        v[i + 1] = g;
        v[i + 2] = r;
        v[i + 3] = 255;
    }
    return v;
}

void resample()
{
    /* a solid picture stays exactly solid at odd sizes, both formats */
    const std::vector<std::uint8_t> s = solid(17, 9, 10, 200, 99);
    Axis ax, ay;
    std::vector<std::uint8_t> o32(11 * 5 * 4, 0), o16(11 * 5 * 2, 0);
    resample_bgrx(s.data(), 17, 9, 17 * 4, PixFmt::XRGB8888, o32.data(), 11, 5, 11 * 4, ax, ay);
    bool ok = true;
    for (std::size_t i = 0; i < o32.size(); i += 4) ok &= o32[i] == 10 && o32[i + 1] == 200 && o32[i + 2] == 99 && o32[i + 3] == 255;
    CHECK(ok);
    resample_bgrx(s.data(), 17, 9, 17 * 4, PixFmt::RGB565, o16.data(), 11, 5, 11 * 2, ax, ay);
    ok = true;
    for (std::size_t i = 0; i < 11 * 5; ++i) ok &= px16(o16, i) == rgb565(99, 200, 10);
    CHECK(ok);
    /* exactly half: each output is the mean of two source columns (centre-aligned) */
    std::vector<std::uint8_t> ramp(8 * 2 * 4);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 8; ++x)
            for (int k = 0; k < 3; ++k) ramp[static_cast<std::size_t>((y * 8 + x) * 4 + k)] = static_cast<std::uint8_t>(x * 30);
    std::vector<std::uint8_t> half(4 * 1 * 4);
    Axis hx, hy;
    resample_bgrx(ramp.data(), 8, 2, 8 * 4, PixFmt::XRGB8888, half.data(), 4, 1, 4 * 4, hx, hy);
    CHECK_EQ(static_cast<int>(half[0]), 15);       /* (0 + 30) / 2 */
    CHECK_EQ(static_cast<int>(half[1 * 4]), 75);   /* (60 + 90) / 2 */
    CHECK_EQ(static_cast<int>(half[3 * 4]), 195);  /* (180 + 210) / 2 */
    /* up: 2 -> 4 columns: 0, 50, 150, 200 (the edges held, no read past them) */
    std::vector<std::uint8_t> two = solid(2, 1, 0, 0, 0);
    two[4] = two[5] = two[6] = 200;
    std::vector<std::uint8_t> four(4 * 1 * 4);
    Axis ux, uy;
    resample_bgrx(two.data(), 2, 1, 2 * 4, PixFmt::XRGB8888, four.data(), 4, 1, 4 * 4, ux, uy);
    CHECK_EQ(static_cast<int>(four[0]), 0);
    CHECK_EQ(static_cast<int>(four[4]), 50);
    CHECK_EQ(static_cast<int>(four[8]), 150);
    CHECK_EQ(static_cast<int>(four[12]), 200);
    /* the axis table: centre-aligned, never past the last source pixel */
    Axis a;
    make_axis(480, 352, a);
    CHECK_EQ(a.i0.size(), static_cast<std::size_t>(352));
    CHECK(a.i0.back() <= 479);
    CHECK(a.i0.front() == 0);
    bool mono = true;
    for (std::size_t i = 1; i < a.i0.size(); ++i) mono &= a.i0[i] >= a.i0[i - 1];
    CHECK(mono);
    /* a wide frame into a short box (stride wider than the picture): no overrun (ASan), values in range */
    const std::vector<std::uint8_t> g = solid(33, 7, 1, 2, 3);
    std::vector<std::uint8_t> sm(5 * 3 * 2 + 8, 0xAB);
    resample_bgrx(g.data(), 33, 7, 33 * 4, PixFmt::RGB565, sm.data(), 5, 3, 5 * 2, ax, ay);
    CHECK_EQ(static_cast<int>(sm[5 * 3 * 2]), 0xAB); /* nothing past the picture */
}

void yuyv()
{
    int worst = 0;
    for (int r = 0; r < 256; r += 51)
        for (int g = 0; g < 256; g += 51)
            for (int b = 0; b < 256; b += 51) {
                int y, u, v, r2, g2, b2;
                rgb_to_yuv601(r, g, b, y, u, v);
                yuv601_to_rgb(y, u, v, r2, g2, b2);
                worst = std::max({worst, std::abs(r - r2), std::abs(g - g2), std::abs(b - b2)});
            }
    CHECK(worst <= 4);
    int r, g, b;
    yuv601_to_rgb(235, 128, 128, r, g, b);
    CHECK_EQ(r + g + b, 765);
    yuv601_to_rgb(16, 128, 128, r, g, b);
    CHECK_EQ(r + g + b, 0);
    /* an 8 x 4 YUYV frame of greys: Y = 16 + 8 x + 40 y; fitted to 4 x 2 by nearest sample (x 2x+1, y 2y+1) */
    const int w = 8, h = 4, stride = w * 2;
    std::vector<std::uint8_t> f(static_cast<std::size_t>(stride) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            f[static_cast<std::size_t>(y * stride + x * 2)] = static_cast<std::uint8_t>(16 + 8 * x + 40 * y);
            f[static_cast<std::size_t>(y * stride + x * 2 + 1)] = 128;
        }
    auto grey = [](int Y) {
        int rr, gg, bb;
        yuv601_to_rgb(Y, 128, 128, rr, gg, bb);
        return rr;
    };
    std::vector<std::uint8_t> out(4 * 2 * 4);
    yuyv_fit(PixFmt::XRGB8888, f.data(), w, h, stride, out.data(), 4, 2, 4 * 4);
    CHECK_EQ(static_cast<int>(out[0]), grey(16 + 8 * 1 + 40 * 1));           /* (1,1): an odd pixel, its own Y */
    CHECK_EQ(static_cast<int>(out[3 * 4]), grey(16 + 8 * 7 + 40 * 1));       /* (7,1) */
    CHECK_EQ(static_cast<int>(out[(4 + 2) * 4]), grey(16 + 8 * 5 + 40 * 3)); /* (5,3) */
    CHECK_EQ(static_cast<int>(out[3]), 0xFF);
    std::vector<std::uint8_t> o16(4 * 2 * 2 + 4, 0xAB);
    yuyv_fit(PixFmt::RGB565, f.data(), w, h, stride, o16.data(), 4, 2, 4 * 2);
    const int g11 = grey(16 + 8 + 40);
    CHECK_EQ(px16(o16, 0), rgb565(g11, g11, g11));
    CHECK_EQ(static_cast<int>(o16[16]), 0xAB); /* nothing past the picture */
}

} // namespace

void test_convert()
{
    fit_rule();
    bgrx();
    resample();
    yuyv();
}
