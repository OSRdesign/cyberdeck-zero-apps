/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl art: cover decoding and the drawn sleeve, record and label (see cover.hpp).
 */
#include "cover.hpp"

#include <algorithm>
#include <cmath>

namespace vinyl {

int vinyl_cover_info(const unsigned char *data, int len, int *w, int *h);
unsigned char *vinyl_cover_decode(const unsigned char *data, int len, int *w, int *h, size_t ceiling);
void vinyl_cover_free(unsigned char *px);
size_t vinyl_cover_peak(void);

const char *cover_status_name(CoverStatus s) {
    switch (s) {
    case CoverStatus::Ok: return "ok";
    case CoverStatus::None: return "none";
    case CoverStatus::TooLarge: return "too large";
    default: return "broken";
    }
}

size_t cover_peak_bytes() { return vinyl_cover_peak(); }

bool cover_size(const Picture &pic, int &w, int &h) {
    w = h = 0;
    if (pic.data.size() < 16 || pic.data.size() > static_cast<size_t>(INT32_MAX)) return false;
    return vinyl_cover_info(pic.data.data(), static_cast<int>(pic.data.size()), &w, &h) == 1 && w > 0 && h > 0;
}

namespace {

void put565(uint8_t *p, int r, int g, int b) {
    const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put8888(uint8_t *p, uint32_t rgb, double a) {
    const int alpha = static_cast<int>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0));
    p[0] = static_cast<uint8_t>(rgb & 0xFF);
    p[1] = static_cast<uint8_t>((rgb >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((rgb >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>(alpha);
}

uint32_t mix(uint32_t a, uint32_t b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [&](int s) {
        const double x = ((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t;
        return static_cast<uint32_t>(std::lround(x)) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

/* Coverage of a pixel by the disc of radius r around the centre (a one-pixel ramp at the edge). */
double cover_of(double d, double r) { return std::clamp(r - d + 0.5, 0.0, 1.0); }

}  // namespace

CoverStatus decode_cover(const Picture &pic, int side, PixelImage &out, int max_side) {
    out = PixelImage();
    if (pic.data.empty() || side <= 0) return CoverStatus::None;
    int w = 0, h = 0;
    if (!cover_size(pic, w, h)) return CoverStatus::Broken;
    if (std::max(w, h) > max_side) return CoverStatus::TooLarge; /* never decoded: the memory cap */
    int dw = 0, dh = 0;
    unsigned char *rgb = vinyl_cover_decode(pic.data.data(), static_cast<int>(pic.data.size()), &dw, &dh, kCoverMemCeiling);
    if (!rgb || dw <= 0 || dh <= 0) {
        vinyl_cover_free(rgb);
        return CoverStatus::Broken;
    }
    /* the centre square, area-averaged down (or nearest up) to side x side */
    const int sq = std::min(dw, dh);
    const int x0 = (dw - sq) / 2, y0 = (dh - sq) / 2;
    out.w = out.h = side;
    out.rgb565 = true;
    out.px.assign(static_cast<size_t>(side) * side * 2, 0);
    const double step = static_cast<double>(sq) / side;
    for (int y = 0; y < side; ++y) {
        const int sy0 = y0 + static_cast<int>(y * step);
        const int sy1 = std::max(sy0 + 1, std::min(y0 + sq, y0 + static_cast<int>((y + 1) * step)));
        for (int x = 0; x < side; ++x) {
            const int sx0 = x0 + static_cast<int>(x * step);
            const int sx1 = std::max(sx0 + 1, std::min(x0 + sq, x0 + static_cast<int>((x + 1) * step)));
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; ++yy) {
                const unsigned char *row = rgb + (static_cast<size_t>(yy) * dw + sx0) * 3;
                for (int xx = sx0; xx < sx1; ++xx, row += 3) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    ++n;
                }
            }
            if (!n) n = 1;
            put565(out.px.data() + (static_cast<size_t>(y) * side + x) * 2, static_cast<int>(r / n), static_cast<int>(g / n),
                   static_cast<int>(b / n));
        }
    }
    vinyl_cover_free(rgb); /* the full picture goes at once */
    return CoverStatus::Ok;
}

PixelImage render_sleeve(int side) {
    PixelImage img;
    img.w = img.h = side;
    img.px.assign(static_cast<size_t>(side) * side * 4, 0);
    /* linear-gradient(135deg, #c0392b, #8a5a2b 55%, #1f7a72); ring: border 6/148 of the side, rgba(242,234,208,.8),
     * outer diameter 80/148 of the side, centred; corners rounded by 4/148 */
    const double s = side / 148.0;
    const double cx = side / 2.0, cy = side / 2.0;
    const double ro = 40.0 * s, ri = ro - 6.0 * s;
    const double rad = 4.0 * s;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const double t = (x + y + 1.0) / (2.0 * side);
            uint32_t c = t < 0.55 ? mix(0xC0392B, 0x8A5A2B, t / 0.55) : mix(0x8A5A2B, 0x1F7A72, (t - 0.55) / 0.45);
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double ring = cover_of(d, ro) - cover_of(d, ri);
            c = mix(c, 0xF2EAD0, 0.8 * ring);
            /* rounded corners */
            double a = 1.0;
            const double qx = std::max({rad - (x + 0.5), (x + 0.5) - (side - rad), 0.0});
            const double qy = std::max({rad - (y + 0.5), (y + 0.5) - (side - rad), 0.0});
            if (qx > 0 && qy > 0) a = cover_of(std::hypot(qx, qy), rad);
            put8888(img.px.data() + (static_cast<size_t>(y) * side + x) * 4, c, a);
        }
    }
    return img;
}

PixelImage render_disc(int diameter, double ds) {
    PixelImage img;
    img.w = img.h = diameter;
    img.px.assign(static_cast<size_t>(diameter) * diameter * 4, 0);
    const double r = diameter / 2.0;
    const double period = std::max(2.0, 4.0 * ds); /* repeating-radial-gradient(#141414 0 2px, #1f1f1f 2px 4px) */
    for (int y = 0; y < diameter; ++y) {
        for (int x = 0; x < diameter; ++x) {
            const double d = std::hypot(x + 0.5 - r, y + 0.5 - r);
            const double a = cover_of(d, r);
            if (a <= 0) continue;
            const double ph = std::fmod(d, period) / period;
            const uint32_t c = ph < 0.5 ? 0x141414 : 0x1F1F1F;
            put8888(img.px.data() + (static_cast<size_t>(y) * diameter + x) * 4, c, a);
        }
    }
    return img;
}

PixelImage render_label(int label_d, int hole_d, double ds) {
    PixelImage img;
    img.w = img.h = label_d;
    img.px.assign(static_cast<size_t>(label_d) * label_d * 4, 0);
    const double r = label_d / 2.0, rh = hole_d / 2.0;
    /* the printed arc: a quarter ring a shade darker than the teal, so a turn can be seen */
    const double arc_r = r * 0.66, arc_w = std::max(1.5, 2.0 * ds);
    for (int y = 0; y < label_d; ++y) {
        for (int x = 0; x < label_d; ++x) {
            const double dx = x + 0.5 - r, dy = y + 0.5 - r;
            const double d = std::hypot(dx, dy);
            const double a = cover_of(d, r);
            if (a <= 0) continue;
            uint32_t c = 0x1F7A72;
            const double ang = std::atan2(dy, dx);
            if (ang > -2.6 && ang < -1.2) {
                const double on = std::clamp(arc_w / 2 - std::fabs(d - arc_r) + 0.5, 0.0, 1.0);
                c = mix(c, 0x175E58, on);
            }
            const double hole = cover_of(d, rh);
            c = mix(c, 0xDCD4BD, hole);
            put8888(img.px.data() + (static_cast<size_t>(y) * label_d + x) * 4, c, a);
        }
    }
    return img;
}

}  // namespace vinyl
