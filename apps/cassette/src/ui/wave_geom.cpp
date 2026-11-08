/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the waveform geometry (see wave_geom.hpp).
 */
#include "wave_geom.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

double WaveWindow::x_of(std::int64_t t) const
{
    return x + static_cast<double>(t - start_ms) * w / static_cast<double>(span_ms);
}

std::int64_t WaveWindow::t_of(double px) const
{
    return start_ms + static_cast<std::int64_t>(std::llround((px - x) * static_cast<double>(span_ms) / w));
}

WaveWindow wave_window(std::int64_t marker_ms, std::int64_t span_ms, double x, double w)
{
    WaveWindow win;
    win.span_ms = std::max<std::int64_t>(1, span_ms);
    win.start_ms = marker_ms - win.span_ms / 2;
    win.x = x;
    win.w = w > 0 ? w : 1;
    return win;
}

WaveEdge wave_edge(const WaveWindow &win, std::int64_t t)
{
    if (t < win.start_ms) return WaveEdge::Left;
    if (t > win.start_ms + win.span_ms) return WaveEdge::Right;
    return WaveEdge::Inside;
}

double overview_x(std::int64_t t, std::int64_t length_ms, double x, double w)
{
    const std::int64_t len = std::max<std::int64_t>(1, length_ms);
    t = std::clamp<std::int64_t>(t, 0, len);
    return x + static_cast<double>(t) * w / static_cast<double>(len);
}

int bar_count(double w, double pitch)
{
    if (w <= 0 || pitch <= 0) return 0;
    return static_cast<int>(std::floor((w + 1e-9) / pitch));
}

std::vector<float> resample_max(const std::vector<float> &in, int n)
{
    std::vector<float> out(static_cast<std::size_t>(std::max(0, n)), 0.0f);
    if (in.empty() || n <= 0) return out;
    const std::size_t m = in.size();
    for (int i = 0; i < n; ++i) {
        std::size_t a = static_cast<std::size_t>(static_cast<double>(i) * m / n);
        std::size_t b = static_cast<std::size_t>(static_cast<double>(i + 1) * m / n);
        if (a >= m) a = m - 1;
        if (b <= a) b = a + 1;
        if (b > m) b = m;
        float v = 0.0f;
        for (std::size_t k = a; k < b; ++k) v = std::max(v, in[k]);
        out[static_cast<std::size_t>(i)] = std::clamp(v, 0.0f, 1.0f);
    }
    return out;
}

int flag_at(const WaveWindow &win, const std::vector<std::int64_t> &splits, double x, double slop_px)
{
    int best = -1;
    double best_d = slop_px;
    for (std::size_t i = 0; i < splits.size(); ++i) {
        if (wave_edge(win, splits[i]) != WaveEdge::Inside) continue;
        const double d = std::fabs(win.x_of(splits[i]) - x);
        if (d <= best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::int64_t scrub_marker(const WaveWindow &win, std::int64_t marker_at_start, double dx_px, std::int64_t length_ms)
{
    const double ms = -dx_px * static_cast<double>(win.span_ms) / win.w;
    const std::int64_t m = marker_at_start + static_cast<std::int64_t>(std::llround(ms));
    return std::clamp<std::int64_t>(m, 0, std::max<std::int64_t>(0, length_ms));
}

std::int64_t nudge_step_ms(bool shift)
{
    return shift ? 1000 : 100;
}

std::vector<RulerTick> ruler_ticks(const WaveWindow &win, int n)
{
    std::vector<RulerTick> ticks;
    if (n < 2) n = 2;
    for (int i = 0; i < n; ++i) {
        const double f = static_cast<double>(i) / (n - 1);
        RulerTick t;
        t.x = win.x + f * win.w;
        t.t = win.start_ms + static_cast<std::int64_t>(std::llround(f * static_cast<double>(win.span_ms)));
        ticks.push_back(t);
    }
    return ticks;
}

} // namespace ui
} // namespace cassette
