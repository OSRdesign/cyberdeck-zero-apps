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

namespace {

std::int64_t floor_div(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) --q;
    return q;
}

} // namespace

double WaveWindow::x_of(std::int64_t t) const
{
    return x + static_cast<double>(t - start_ms) * w / static_cast<double>(span_ms);
}

double WaveWindow::x_of_ms(double t) const
{
    return x + (t - static_cast<double>(start_ms)) * w / static_cast<double>(span_ms);
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

BarGrid bar_grid(std::int64_t marker_frame, std::int64_t visible_frames, double across)
{
    BarGrid g;
    const std::int64_t vis = std::max<std::int64_t>(1, visible_frames);
    if (!(across >= 1.0)) across = 1.0;
    g.fpb = std::max<std::int64_t>(1, std::llround(static_cast<double>(vis) / across));
    /* the window [a, a + vis] touches at most ceil(vis / fpb) + 1 bars from the one holding a: one more at each side */
    const std::int64_t a = marker_frame - vis / 2;
    g.first = floor_div(a, g.fpb) - 1;
    g.count = static_cast<int>((vis + g.fpb - 1) / g.fpb + 3);
    return g;
}

double bars_across(double w, double pitch)
{
    if (!(pitch > 0)) return 1.0;
    return std::max(1.0, w / pitch);
}

double grid_bar_x(const WaveWindow &win, std::int64_t k, std::int64_t fpb, std::uint32_t rate)
{
    const double r = rate ? static_cast<double>(rate) : 48000.0;
    return win.x_of_ms(static_cast<double>(k) * static_cast<double>(std::max<std::int64_t>(1, fpb)) * 1000.0 / r);
}

BarSpan clip_bar(int x, int w, int left, int right)
{
    BarSpan s;
    s.x1 = std::max(x, left);
    s.x2 = std::max(s.x1, std::min(x + w, right));
    return s;
}

float fit_gain_for(float take_peak)
{
    if (!(take_peak >= kFitSilent)) return 0.0f;
    return std::min(kFitMaxGain, kFitTarget / std::min(1.0f, take_peak));
}

float fit_level(float level, float gain)
{
    if (gain > 0.0f) level *= gain;
    return std::clamp(level, 0.0f, 1.0f);
}

std::string wave_chip_text(bool fit, float gain)
{
    if (!fit) return "TRUE";
    if (!(gain > 0.0f)) return "FIT";
    return "FIT x" + std::to_string(std::max(1L, std::lround(gain)));
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

TagSpan tag_span(WaveEdge edge, bool is_out, int line_x, int w, int left, int right, int inset)
{
    w = std::max(0, std::min(w, right - left));
    int x1;
    switch (edge) {
    case WaveEdge::Left: x1 = left + inset; break;
    case WaveEdge::Right: x1 = right - w; break;
    default: x1 = is_out ? line_x - w : line_x; break;
    }
    x1 = std::clamp(x1, left, right - w);
    return {x1, x1 + w};
}

TagRows tag_rows(const TagSpan &in, const TagSpan &out, const TagSpan &chip, int gap, int max_rows)
{
    const int last = std::max(1, max_rows) - 1;
    TagRows r;
    r.in = std::min(last, in.overlaps(chip, gap) ? 1 : 0);
    r.out = last;
    for (int row = 0; row <= last; ++row) {
        if (row == 0 && out.overlaps(chip, gap)) continue;
        if (row == r.in && out.overlaps(in, gap)) continue;
        r.out = row;
        break;
    }
    return r;
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

RulerRow ruler_row(int inner_bottom, int line_h, int tick_h, int gap, int pad, int main_bottom)
{
    RulerRow r;
    r.label_h = std::max(0, line_h);
    r.label_y = inner_bottom - std::max(0, pad) - r.label_h;
    r.tick_h = std::max(0, tick_h);
    r.tick_y = r.label_y - std::max(0, gap) - r.tick_h;
    r.main_bottom = std::min(main_bottom, r.tick_y - std::max(0, gap));
    return r;
}

std::vector<RulerLabel> ruler_labels(const std::vector<RulerTick> &ticks, const std::vector<int> &widths, int left, int right,
                                     int gap)
{
    std::vector<RulerLabel> out(ticks.size());
    bool have_prev = false;
    int prev_x2 = 0;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const int w = i < widths.size() ? widths[i] : 0;
        if (w <= 0 || w > right - left) continue;
        const int x1 = std::clamp(static_cast<int>(std::lround(ticks[i].x)) - w / 2, left, right - w);
        if (have_prev && x1 < prev_x2 + gap) continue;
        out[i] = RulerLabel{x1, x1 + w, true};
        prev_x2 = x1 + w;
        have_prev = true;
    }
    return out;
}

Fit trim_fit(int screen_w, int screen_h, int body_y, bool portrait, double t)
{
    if (portrait)
        return make_fit(screen_w, screen_h, body_y, 320, 480, 40, {200},
                        {{122, 272, 0, 0, true}, {306, 350, t, 0, false}, {392, 456, t, 0, false}});
    return make_fit(screen_w, screen_h, body_y, 480, 320, 40, {145},
                    {{114, 176, 0, 0, true}, {198, 242, t, 0, false}, {252, 302, t, 0, false}});
}

WavePanel wave_panel(const Fit &fit, bool p, int ruler_line_h)
{
    WavePanel g;
    const double py = p ? 90 : 86, pw = p ? 288 : 448, ph = p ? 210 : 108;
    g.x = fit.x(16), g.y = fit.y(py), g.w = fit.w(16, pw), g.h = fit.h(py, ph);
    g.border = fit.px(3);
    if (p) {
        g.over_x = fit.x(24), g.over_y = fit.y(96), g.over_w = fit.w(24, 272), g.over_h = fit.h(96, 18);
        g.main_x = fit.x(24), g.main_y = fit.y(122), g.main_w = fit.w(24, 272), g.main_h = fit.h(122, 150);
    } else {
        g.over_x = fit.x(20), g.over_y = fit.y(90), g.over_w = fit.w(20, 440), g.over_h = fit.h(90, 18);
        g.main_x = fit.x(20), g.main_y = fit.y(114), g.main_w = fit.w(20, 440), g.main_h = fit.h(114, 62);
    }
    g.pitch = 4 * fit.s;
    g.bar_w = 3 * fit.s;
    g.bars = bar_count(g.main_w + (g.pitch - g.bar_w), g.pitch);
    g.bars_w = std::max(1, static_cast<int>(std::lround(g.bars * g.pitch - (g.pitch - g.bar_w))));
    /* the ruler: its labels' line box inside the frame (2 px clear of it), the ticks above them, the waveform above those */
    g.ruler = ruler_row(g.y + g.h - g.border, ruler_line_h, fit.px(4), fit.px(1), fit.px(2), g.main_y + g.main_h);
    g.main_h = std::max(1, g.ruler.main_bottom - g.main_y);
    return g;
}

} // namespace ui
} // namespace cassette
