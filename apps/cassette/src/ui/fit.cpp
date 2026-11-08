/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: board coordinates to screen coordinates (see fit.hpp).
 */
#include "fit.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

namespace {

struct Work {
    double y0, y1, base, min_px, weight;
    bool shrink;
};

std::vector<Work> cut(double d0, double d1, double s, const std::vector<Band> &bands)
{
    std::vector<double> pts = {d0, d1};
    for (const Band &b : bands) {
        if (b.y0 >= d0 && b.y0 <= d1) pts.push_back(b.y0);
        if (b.y1 >= d0 && b.y1 <= d1) pts.push_back(b.y1);
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    std::vector<Work> out;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        /* the seams at this point first, then the range that starts here */
        for (const Band &b : bands)
            if (b.y0 == b.y1 && b.y0 == pts[i]) out.push_back({b.y0, b.y0, 0, 0, b.weight, false});
        if (i + 1 == pts.size()) break;
        Work w{pts[i], pts[i + 1], (pts[i + 1] - pts[i]) * s, 0, 0, false};
        for (const Band &b : bands)
            if (b.y1 > b.y0 && b.y0 <= w.y0 && w.y1 <= b.y1) {
                w.min_px = b.min_px;
                w.weight = b.weight;
                w.shrink = b.shrink;
            }
        out.push_back(w);
    }
    return out;
}

/* heights for avail px: false when the minimums do not fit even with every shrinking band at its floor */
bool lay(std::vector<Work> &ws, std::vector<Fit::Seg> &segs, double avail, bool use_mins, double *share)
{
    double total = 0, weights = 0, shrinkable = 0;
    for (Work &w : ws) {
        w.base = (w.y1 - w.y0) > 0 ? std::max(w.base, use_mins ? w.min_px : 0.0) : 0;
        total += w.base;
        weights += w.weight;
        if (w.shrink) shrinkable += w.base;
    }
    const double left = avail - total;
    if (share) *share = 0;
    segs.clear();
    if (left < 0 && shrinkable * 0.6 + 1e-6 < -left) return false;
    for (const Work &w : ws) {
        double h = w.base;
        if (left > 0 && weights > 0 && w.weight > 0) h += std::floor(left * w.weight / weights);
        if (left < 0 && w.shrink && shrinkable > 0) h -= -left * w.base / shrinkable;
        segs.push_back({w.y0, w.y1, h});
    }
    if (share && left > 0 && weights > 0) *share = std::floor(left / weights);
    return true;
}

} // namespace

std::vector<Fit::Seg> fit_segments(double d0, double d1, double s, const std::vector<Band> &bands, double avail_px,
                                   double *share, bool *mins_dropped)
{
    std::vector<Work> ws = cut(d0, d1, s, bands);
    std::vector<Fit::Seg> segs;
    const bool ok = lay(ws, segs, avail_px, true, share);
    if (mins_dropped) *mins_dropped = !ok;
    if (!ok) {
        ws = cut(d0, d1, s, bands);
        lay(ws, segs, avail_px, false, share);
        if (segs.empty() || avail_px < 0) { /* not even the plain board fits: plain scale */
            segs.clear();
            for (const Work &w : cut(d0, d1, s, {})) segs.push_back({w.y0, w.y1, (w.y1 - w.y0) * s});
        }
    }
    return segs;
}

double Fit::yd(double dy) const
{
    if (segs.empty()) return oy + (dy - dbody) * s;
    double pos = oy;
    if (dy <= segs.front().y0) return oy + (dy - segs.front().y0) * s;
    for (const Seg &g : segs) {
        if (g.y1 == g.y0) {
            if (g.y0 < dy) pos += g.h;
            continue;
        }
        if (dy >= g.y1) {
            pos += g.h;
            continue;
        }
        if (dy > g.y0) pos += (dy - g.y0) * g.h / (g.y1 - g.y0);
        return pos;
    }
    return pos + (dy - segs.back().y1) * s;
}

int Fit::x(double dx) const
{
    return static_cast<int>(std::lround(ox + dx * s));
}

int Fit::y(double dy) const
{
    return static_cast<int>(std::lround(yd(dy)));
}

int Fit::px(double v) const
{
    if (v <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(v * s)));
}

Fit make_fit(int screen_w, int screen_h, int body_y, int design_w, int design_h, int design_body,
             std::vector<double> seams, std::vector<Band> bands)
{
    Fit f;
    f.sw = screen_w;
    f.sh = screen_h;
    f.dbody = design_body;
    const double body_h = static_cast<double>(screen_h - body_y);
    const double dbody_h = static_cast<double>(design_h - design_body);
    const double sx = static_cast<double>(screen_w) / design_w;
    const double sy = dbody_h > 0 ? body_h / dbody_h : sx;
    f.s = std::min(sx, sy);
    /* a hair under 1 from rounding (a header one px taller) is not a reason to shrink every font */
    if (f.s > 0.985 && f.s < 1.0) f.s = 1.0;
    f.ox = (screen_w - design_w * f.s) / 2.0;
    f.oy = body_y;
    for (double at : seams) bands.push_back({at, at, 0, 1, false});
    f.segs = fit_segments(design_body, design_h, f.s, bands, body_h, &f.share, &f.mins_dropped);
    return f;
}

} // namespace ui
} // namespace cassette
