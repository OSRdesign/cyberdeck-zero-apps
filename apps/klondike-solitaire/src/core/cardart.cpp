/*
 * SPDX-License-Identifier: MIT
 */
#include "cardart.hpp"

#include "card.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace klondike {
namespace art {

namespace {

using Inside = std::function<bool(double, double)>; /* normalised box, y down */

bool in_poly(const double (*p)[2], int n, double x, double y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i][1] > y) != (p[j][1] > y)) && (x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0]))
            in = !in;
    }
    return in;
}

bool in_circle(double x, double y, double cx, double cy, double r)
{
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

bool in_rect(double x, double y, double x0, double y0, double x1, double y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

Mask rasterise(int w, int h, const Inside &inside)
{
    Mask m;
    m.w = w;
    m.h = h;
    m.a.assign(static_cast<size_t>(w) * h, 0);
    constexpr int kS = 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int hit = 0;
            for (int sy = 0; sy < kS; ++sy)
                for (int sx = 0; sx < kS; ++sx) {
                    const double nx = (x + (sx + 0.5) / kS) / w;
                    const double ny = (y + (sy + 0.5) / kS) / h;
                    if (inside(nx, ny)) ++hit;
                }
            m.a[static_cast<size_t>(y) * w + x] = static_cast<std::uint8_t>(hit * 255 / (kS * kS));
        }
    return m;
}

bool heart(double nx, double ny)
{
    const double x = (nx * 2 - 1) * 1.18;
    const double y = 1.25 - 2.25 * ny;
    const double a = x * x + y * y - 1;
    return a * a * a - x * x * y * y * y <= 0;
}

bool spade(double nx, double ny)
{
    const double ny2 = ny / 0.86;
    if (ny2 <= 1.0) {
        const double x = (nx * 2 - 1) * 1.18;
        const double y = -1.0 + 2.25 * ny2;
        const double a = x * x + y * y - 1;
        if (a * a * a - x * x * y * y * y <= 0) return true;
    }
    if (ny >= 0.60 && ny <= 1.0) {
        const double hw = 0.035 + (ny - 0.60) / 0.40 * 0.17;
        if (std::fabs(nx - 0.5) <= hw) return true;
    }
    return false;
}

bool diamond(double nx, double ny)
{
    return std::fabs(nx - 0.5) / 0.38 + std::fabs(ny - 0.5) / 0.5 <= 1.0;
}

bool club(double nx, double ny)
{
    if (in_circle(nx, ny, 0.5, 0.27, 0.25)) return true;
    if (in_circle(nx, ny, 0.265, 0.60, 0.25)) return true;
    if (in_circle(nx, ny, 0.735, 0.60, 0.25)) return true;
    if (in_circle(nx, ny, 0.5, 0.5, 0.17)) return true;
    static const double stem[4][2] = {{0.45, 0.50}, {0.55, 0.50}, {0.70, 1.0}, {0.30, 1.0}};
    return in_poly(stem, 4, nx, ny);
}

bool sword(double nx, double ny)
{
    static const double blade[5][2] = {{0.5, 0.0}, {0.585, 0.12}, {0.575, 0.64}, {0.425, 0.64}, {0.415, 0.12}};
    if (in_poly(blade, 5, nx, ny)) return true;
    if (in_rect(nx, ny, 0.22, 0.64, 0.78, 0.73)) return true;
    if (in_rect(nx, ny, 0.465, 0.73, 0.535, 0.90)) return true;
    return in_circle(nx, ny, 0.5, 0.94, 0.06);
}

bool tiara(double nx, double ny)
{
    static const double body[9][2] = {{0.08, 0.72}, {0.14, 0.30}, {0.31, 0.54}, {0.50, 0.22}, {0.69, 0.54},
                                      {0.86, 0.30}, {0.92, 0.72}, {0.92, 0.72}, {0.08, 0.72}};
    if (in_poly(body, 9, nx, ny)) return true;
    if (in_rect(nx, ny, 0.08, 0.76, 0.92, 0.90)) return true;
    return in_circle(nx, ny, 0.14, 0.25, 0.065) || in_circle(nx, ny, 0.50, 0.15, 0.075) || in_circle(nx, ny, 0.86, 0.25, 0.065);
}

bool crown(double nx, double ny)
{
    static const double body[7][2] = {{0.06, 0.72}, {0.06, 0.26}, {0.30, 0.50}, {0.50, 0.16}, {0.70, 0.50}, {0.94, 0.26}, {0.94, 0.72}};
    if (in_poly(body, 7, nx, ny)) return true;
    if (in_rect(nx, ny, 0.06, 0.76, 0.94, 0.92)) return true;
    return in_circle(nx, ny, 0.06, 0.21, 0.065) || in_circle(nx, ny, 0.50, 0.10, 0.08) || in_circle(nx, ny, 0.94, 0.21, 0.065);
}

struct Rgb {
    double r, g, b;
};
Rgb hex(std::uint32_t c)
{
    return {double((c >> 16) & 255), double((c >> 8) & 255), double(c & 255)};
}
Rgb mix(Rgb a, Rgb b, double t)
{
    t = std::min(1.0, std::max(0.0, t));
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
double smooth_line(double d, double half, double soft)
{
    /* 1 inside the half width, 0 beyond half + soft */
    if (d <= half) return 1.0;
    if (d >= half + soft) return 0.0;
    return 1.0 - (d - half) / soft;
}
double frac_dist(double v) /* distance to the nearest integer */
{
    const double f = v - std::floor(v);
    return std::min(f, 1.0 - f);
}

Rgb back_blue(double x, double y, double w, double h)
{
    const Rgb base = hex(0x1F4E8C), line = hex(0x6C9BD8), pale = hex(0xDCE7F7), frame = hex(0xC9DDF5);
    Rgb c = base;
    const double p = w / 6.0;
    const double d1 = frac_dist((x + y) / p), d2 = frac_dist((x - y) / p);
    const double dl = std::min(d1, d2) * p / 1.41421356;
    c = mix(c, line, smooth_line(dl, 0.55, 0.9) * 0.9);
    /* the medallion */
    const double cx = w / 2, cy = h / 2;
    const double e = std::sqrt(std::pow((x - cx) / (w * 0.30), 2) + std::pow((y - cy) / (h * 0.21), 2));
    c = mix(c, base, smooth_line(e, 1.12, 0.12));
    c = mix(c, pale, smooth_line(std::fabs(e - 1.0) * w * 0.3, 0.9, 0.7));
    const double e2 = std::sqrt(std::pow((x - cx) / (w * 0.15), 2) + std::pow((y - cy) / (h * 0.105), 2));
    c = mix(c, pale, smooth_line(e2, 0.9, 0.12) * 0.9);
    /* frame */
    const double edge = std::min(std::min(x, w - x), std::min(y, h - y));
    c = mix(c, frame, smooth_line(std::fabs(edge - 3.2), 0.6, 0.8));
    return c;
}

Rgb back_red(double x, double y, double w, double h)
{
    const Rgb base = hex(0x8F1D2C), dot = hex(0xB9505C), gold = hex(0xE6BC6E), frame = hex(0xF1D9A8);
    Rgb c = base;
    const double p = w / 7.0;
    const double gx = std::fabs(frac_dist(x / p + 0.5) * p), gy = std::fabs(frac_dist(y / p + 0.5) * p);
    c = mix(c, dot, smooth_line(std::sqrt(gx * gx + gy * gy), p * 0.14, 1.0) * 0.9);
    const double cx = w / 2, cy = h / 2;
    const double dx = x - cx, dy = y - cy;
    const double r = std::sqrt(dx * dx + dy * dy);
    const double th = std::atan2(dy, dx);
    const double R = w * 0.38;
    const double petal = R * (0.62 + 0.30 * std::pow(std::fabs(std::cos(4 * th)), 1.5));
    c = mix(c, base, smooth_line(r, petal + 2.0, 1.0));
    c = mix(c, gold, smooth_line(std::fabs(r - petal), 1.0, 1.0));
    c = mix(c, gold, smooth_line(r, R * 0.22, 1.0));
    c = mix(c, base, smooth_line(r, R * 0.11, 1.0));
    const double edge = std::min(std::min(x, w - x), std::min(y, h - y));
    c = mix(c, frame, smooth_line(std::fabs(edge - 3.2), 0.6, 0.8));
    return c;
}

} // namespace

Mask render_suit(int suit, int size)
{
    size = std::max(4, size);
    switch (suit) {
    case Hearts: return rasterise(size, size, heart);
    case Spades: return rasterise(size, size, spade);
    case Diamonds: return rasterise(size, size, diamond);
    default: return rasterise(size, size, club);
    }
}

Mask render_emblem(int kind, int w, int h)
{
    w = std::max(4, w);
    h = std::max(4, h);
    switch (kind) {
    case Jack: return rasterise(w, h, sword);
    case Queen: return rasterise(w, h, tiara);
    default: return rasterise(w, h, crown);
    }
}

std::vector<std::uint32_t> render_back(int design, int w, int h)
{
    std::vector<std::uint32_t> px(static_cast<size_t>(w) * h);
    const double rr = std::max(2.0, w / 12.0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    const double fx = x + (sx + 0.5) / 2, fy = y + (sy + 0.5) / 2;
                    const Rgb c = design == 1 ? back_red(fx, fy, w, h) : back_blue(fx, fy, w, h);
                    r += c.r;
                    g += c.g;
                    b += c.b;
                }
            r /= 4;
            g /= 4;
            b /= 4;
            /* the rounded corner as alpha */
            double a = 1.0;
            const double qx = std::min(x + 0.5, w - x - 0.5), qy = std::min(y + 0.5, h - y - 0.5);
            if (qx < rr && qy < rr) {
                const double d = std::sqrt((rr - qx) * (rr - qx) + (rr - qy) * (rr - qy));
                a = std::min(1.0, std::max(0.0, rr - d + 0.5));
            }
            px[static_cast<size_t>(y) * w + x] = (static_cast<std::uint32_t>(a * 255 + 0.5) << 24) |
                                                  (static_cast<std::uint32_t>(r + 0.5) << 16) |
                                                  (static_cast<std::uint32_t>(g + 0.5) << 8) |
                                                  static_cast<std::uint32_t>(b + 0.5);
        }
    return px;
}

int pip_layout(int rank, Pip out[10])
{
    const float L = 0.26f, R = 0.74f, C = 0.5f;
    const float t = 0.07f, b = 0.93f, m = 0.5f;
    int n = 0;
    auto add = [&](float x, float y) { out[n++] = Pip{x, y, y > 0.5f}; };
    switch (rank) {
    case 2: add(C, t); add(C, b); break;
    case 3: add(C, t); add(C, m); add(C, b); break;
    case 4: add(L, t); add(R, t); add(L, b); add(R, b); break;
    case 5: add(L, t); add(R, t); add(C, m); add(L, b); add(R, b); break;
    case 6: add(L, t); add(R, t); add(L, m); add(R, m); add(L, b); add(R, b); break;
    case 7: add(L, t); add(R, t); add(C, 0.285f); add(L, m); add(R, m); add(L, b); add(R, b); break;
    case 8: add(L, t); add(R, t); add(C, 0.285f); add(L, m); add(R, m); add(C, 0.715f); add(L, b); add(R, b); break;
    case 9: {
        const float y1 = 0.357f, y2 = 0.643f;
        add(L, t); add(R, t); add(L, y1); add(R, y1); add(C, m); add(L, y2); add(R, y2); add(L, b); add(R, b);
        break;
    }
    case 10: {
        const float y1 = 0.357f, y2 = 0.643f;
        add(L, t); add(R, t); add(C, 0.215f); add(L, y1); add(R, y1); add(L, y2); add(R, y2); add(C, 0.785f); add(L, b); add(R, b);
        break;
    }
    default: break;
    }
    return n;
}

std::uint32_t suit_rgb(int suit, bool four_colour)
{
    switch (suit) {
    case Hearts: return 0xC62828;
    case Diamonds: return four_colour ? 0x1565C0 : 0xC62828;
    case Clubs: return four_colour ? 0x2E7D32 : 0x1B1B1F;
    default: return 0x1B1B1F;
    }
}

std::uint32_t felt_rgb(int felt)
{
    static const std::uint32_t c[kFelts] = {0x1D6B3B, 0x1F4E79, 0x6A2433};
    return c[std::min(std::max(felt, 0), kFelts - 1)];
}

const char *felt_name(int felt)
{
    static const char *n[kFelts] = {"Green", "Blue", "Burgundy"};
    return n[std::min(std::max(felt, 0), kFelts - 1)];
}

const char *back_name(int design)
{
    return design == 1 ? "Red rosette" : "Blue lattice";
}

} // namespace art
} // namespace klondike
