/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl layout: the drafts' coordinates (see layout.hpp). Every number below is read from docs/design (the .dc.html drafts): the
 * panel's inner positions plus the panel's own offset (8, 44), so they are the screen pixels of the drawn size.
 */
#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vinyl {
namespace layout {

SizeClass size_class(int w, int h) {
    if (w <= 0 || h <= 0) return SizeClass::Small;
    const double a = static_cast<double>(w) / h;
    if (a < 0.9) return SizeClass::Portrait;
    if (a < 1.2) return SizeClass::Square;
    if (a < 1.6) return SizeClass::Small;
    return SizeClass::Large;
}

const char *class_name(SizeClass c) {
    switch (c) {
    case SizeClass::Portrait: return "portrait";
    case SizeClass::Square: return "square";
    case SizeClass::Large: return "large";
    default: return "small";
    }
}

namespace {

Btn btn(const char *id, double x, double y, double w, double h, double icon = 22, bool teal = false) {
    Btn b;
    b.id = id;
    b.box = R{x, y, w, h};
    b.icon = icon;
    b.teal = teal;
    return b;
}

Btn word(const char *id, double x, double y, double w, double h, bool enabled = true) {
    Btn b = btn(id, x, y, w, h, 18);
    b.label = true;
    b.enabled = enabled;
    return b;
}

Text text(double x, double y, double w, double h, double px, bool bold, double lh = 0, int lines = 1) {
    Text t;
    t.box = R{x, y, w, h};
    t.px = px;
    t.bold = bold;
    t.lh = lh;
    t.lines = lines;
    return t;
}

}  // namespace

NowGeom now_geom(SizeClass c) {
    NowGeom g;
    g.cls = c;
    switch (c) {
    case SizeClass::Small: /* Main.dc.html, 480x320 */
        g.dw = 480;
        g.dh = 320;
        g.panel = R{8, 44, 464, 268};
        g.disc = R{64, 56, 148, 148};
        g.sleeve = R{20, 56, 148, 148};
        g.label_d = 50;
        g.hole_d = 6;
        g.title = text(228, 56, 232, 26, 19, true);
        g.artist = text(228, 84, 232, 20, 15, true);
        g.album = text(228, 106, 232, 16, 12, false);
        g.chip0 = R{228, 136, 0, 26};
        g.chip_px = 12;
        g.chip_pad = 10;
        g.chip_gap = 6;
        g.chip_icon = 12;
        g.chip_step = 32;
        g.chip_rows = 2;
        g.chip_layout = {{0, 1}, {2}};
        g.short_format = true;
        g.quality_in_chip = true;
        g.time = text(20, 220, 48, 28, 14, false, 28);
        g.remain = text(412, 220, 48, 28, 14, false, 28);
        g.seek = R{72, 229, 336, 10};
        g.transport = {btn("shuffle", 20, 258, 56, 48), btn("prev", 90, 258, 56, 48), btn("play", 160, 258, 88, 48, 26, true),
                       btn("next", 262, 258, 56, 48), btn("repeat", 332, 258, 56, 48), btn("queue", 402, 258, 58, 48)};
        g.repeat_dash = R{354, 298, 12, 4};
        g.seams = {170, 210, 264};
        break;
    case SizeClass::Portrait: /* NowPlaying-320x480 */
        g.dw = 320;
        g.dh = 480;
        g.panel = R{8, 44, 304, 428};
        g.disc = R{116, 56, 164, 164};
        g.sleeve = R{28, 56, 164, 164};
        g.label_d = 54;
        g.hole_d = 6;
        g.title = text(24, 228, 272, 24, 18, true);
        g.artist = text(24, 254, 272, 20, 15, true);
        g.album = text(24, 274, 272, 16, 12, false);
        g.chip0 = R{24, 298, 0, 24};
        g.chip_px = 11;
        g.chip_pad = 9;
        g.chip_gap = 5;
        g.chip_icon = 11;
        g.chips_in_row = true;
        g.chip_rows = 1;
        g.chip_layout = {{0}}; /* the output is the button beside the format chip, the volume the bottom group */
        g.short_format = true;
        g.quality_in_chip = false;
        g.time = text(24, 332, 44, 24, 13, false, 24);
        g.remain = text(252, 332, 44, 24, 13, false, 24);
        g.seek = R{72, 339, 176, 10};
        g.transport = {btn("shuffle", 24, 368, 48, 48), btn("prev", 78, 368, 48, 48), btn("play", 132, 368, 56, 48, 26, true),
                       btn("next", 194, 368, 48, 48), btn("repeat", 248, 368, 48, 48)};
        g.repeat_dash = R{266, 408, 12, 4};
        g.bottom = {word("queue", 24, 426, 100, 40)};
        g.vol_group = true; /* owner, 2026-11-13: the Output key becomes the volume group */
        g.vol_minus = R{132, 426, 48, 40};
        g.vol_value = R{184, 426, 60, 40};
        g.vol_plus = R{248, 426, 48, 40};
        g.out_button = R{132, 298, 164, 24};
        g.seams = {224, 326, 362, 420};
        break;
    case SizeClass::Square: /* NowPlaying-480x480 */
        g.dw = 480;
        g.dh = 480;
        g.panel = R{8, 44, 464, 428};
        g.disc = R{88, 56, 196, 196};
        g.sleeve = R{20, 56, 196, 196};
        g.label_d = 64;
        g.hole_d = 10;
        g.title = text(298, 56, 162, 44, 17, true, 22, 2);
        g.artist = text(298, 110, 162, 20, 15, true);
        g.album = text(298, 132, 162, 16, 12, false);
        g.chip0 = R{298, 162, 0, 26};
        g.chip_step = 32;
        g.chip_rows = 3;
        g.chip_layout = {{0}, {}, {3}}; /* row 1 is the output button (out_button) */
        g.short_format = true;
        g.quality_in_chip = false;
        g.time = text(20, 266, 48, 28, 14, false, 28);
        g.remain = text(412, 266, 48, 28, 14, false, 28);
        g.seek = R{72, 275, 336, 10};
        g.transport = {btn("shuffle", 20, 304, 56, 48), btn("prev", 90, 304, 56, 48), btn("play", 160, 304, 88, 48, 26, true),
                       btn("next", 262, 304, 56, 48), btn("repeat", 332, 304, 56, 48), btn("sleep", 402, 304, 58, 48)};
        g.transport.back().enabled = false; /* sleep timer: P2 */
        g.repeat_dash = R{354, 344, 12, 4};
        g.has_upnext = true;
        g.upnext = R{20, 366, 440, 44};
        g.bottom = {word("browse", 20, 420, 120, 42), word("queue", 150, 420, 120, 42)};
        g.vol_group = true; /* owner, 2026-11-13: the Output key becomes the volume group */
        g.vol_minus = R{280, 420, 52, 42};
        g.vol_value = R{336, 420, 68, 42};
        g.vol_plus = R{408, 420, 52, 42};
        g.out_button = R{298, 194, 162, 26};
        g.seams = {258, 300, 360, 414};
        break;
    case SizeClass::Large: /* NowPlaying-800x480 */
        g.dw = 800;
        g.dh = 480;
        g.panel = R{8, 44, 784, 428};
        g.has_queue_pane = true;
        g.queue_pane = R{20, 56, 300, 404};
        g.queue_row_h = 40;
        g.queue_heading_px = 11;
        g.disc = R{380, 56, 190, 190};
        g.sleeve = R{330, 56, 190, 190};
        g.label_d = 64;
        g.hole_d = 10;
        g.title = text(590, 56, 190, 46, 18, true, 23, 2);
        g.artist = text(590, 110, 190, 20, 15, true);
        g.album = text(590, 132, 190, 16, 12, false);
        g.chip0 = R{590, 164, 0, 26};
        g.chip_step = 32;
        g.chip_rows = 3;
        g.chip_layout = {{0}, {2, 1}, {3}};
        g.short_format = true;
        g.quality_in_chip = false;
        g.time = text(330, 274, 48, 28, 14, false, 28);
        g.remain = text(722, 274, 48, 28, 14, false, 28);
        g.seek = R{382, 283, 336, 10};
        g.transport = {btn("shuffle", 330, 312, 56, 48), btn("prev", 400, 312, 56, 48), btn("play", 470, 312, 88, 48, 26, true),
                       btn("next", 572, 312, 56, 48), btn("repeat", 642, 312, 56, 48), btn("sleep", 712, 312, 58, 48)};
        g.transport.back().enabled = false; /* sleep timer: P2 */
        g.repeat_dash = R{664, 352, 12, 4};
        g.bottom = {word("browse", 330, 384, 140, 42), word("output", 480, 384, 140, 42), word("eq", 630, 384, 140, 42, false)};
        g.seams = {262, 306, 372};
        break;
    }
    return g;
}

ui::Fit now_fit(const NowGeom &g, int screen_w, int screen_h, int body_y) {
    return ui::make_fit(screen_w, screen_h, body_y, g.dw, g.dh, static_cast<int>(kDesignBody), g.seams);
}

R notice_box_now(const NowGeom &g, double height) {
    const double x0 = g.time.box.x - 4, x1 = g.remain.box.right() + 4;
    const double bottom = g.time.box.bottom();
    return R{x0, bottom - height, x1 - x0, height};
}

std::string track_time_text(uint64_t ms, bool hours, bool lead_zero) {
    const uint64_t t = ms / 1000;
    char b[32];
    if (hours) std::snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(t / 3600), unsigned((t / 60) % 60), unsigned(t % 60));
    else if (lead_zero) std::snprintf(b, sizeof b, "%02u:%02u", unsigned(t / 60), unsigned(t % 60));
    else std::snprintf(b, sizeof b, "%u:%02u", unsigned(t / 60), unsigned(t % 60));
    return b;
}

const char *track_time_widest(bool hours, bool remaining) {
    if (hours) return remaining ? "-00:00:00" : "00:00:00"; /* up to 99 h: a wider label never cuts a long file */
    return remaining ? "-00:00" : "00:00";
}

std::vector<std::vector<int>> flow_chip_rows(const std::vector<std::vector<int>> &rows, const int natural[4], int avail,
                                             int space) {
    auto width = [&](const std::vector<int> &row) {
        int w = 0, n = 0;
        for (int id : row)
            if (id >= 0 && id <= 3 && natural[id] > 0) {
                w += natural[id];
                ++n;
            }
        return n > 1 ? w + (n - 1) * space : w;
    };
    std::vector<std::vector<int>> out = rows;
    int vr = -1;
    for (size_t r = 0; r < out.size(); ++r)
        for (int id : out[r])
            if (id == 1) vr = static_cast<int>(r);
    if (vr < 0 || natural[1] <= 0 || width(out[static_cast<size_t>(vr)]) <= avail) return out;
    bool shared = false;
    for (int id : out[static_cast<size_t>(vr)])
        if (id != 1 && id >= 0 && id <= 3 && natural[id] > 0) shared = true;
    if (!shared) return out;
    for (size_t r = 0; r < out.size(); ++r) {
        if (static_cast<int>(r) == vr) continue;
        const int w = width(out[r]);
        if (natural[1] + (w > 0 ? w + space : 0) <= avail) {
            std::vector<int> &from = out[static_cast<size_t>(vr)];
            from.erase(std::remove(from.begin(), from.end(), 1), from.end());
            out[r].push_back(1);
            break;
        }
    }
    return out;
}

R grow_hit(const R &d, double min_w, double min_h) {
    const double w = std::max(d.w, min_w), h = std::max(d.h, min_h);
    return R{d.x - (w - d.w) / 2, d.y - (h - d.h) / 2, w, h};
}

bool overlaps(const R &a, const R &b) { return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom(); }

void split_pair(HitArea &a, HitArea &b, double min) {
    if (!overlaps(a.area, b.area)) return;
    const double gx = std::max(b.drawn.x - a.drawn.right(), a.drawn.x - b.drawn.right());
    const double gy = std::max(b.drawn.y - a.drawn.bottom(), a.drawn.y - b.drawn.bottom());
    if (gx < 0 && gy < 0) return; /* the drawings themselves overlap */
    if (gx >= gy) {
        HitArea &l = a.drawn.x <= b.drawn.x ? a : b, &r = &l == &a ? b : a;
        const double m = (l.drawn.right() + r.drawn.x) / 2;
        const double l0 = std::min(l.area.x, m - min), r1 = std::max(r.area.right(), m + min);
        l.area.x = l0;
        l.area.w = m - l0;
        r.area.x = m;
        r.area.w = r1 - m;
    } else {
        HitArea &t = a.drawn.y <= b.drawn.y ? a : b, &u = &t == &a ? b : a;
        const double m = (t.drawn.bottom() + u.drawn.y) / 2;
        const double t0 = std::min(t.area.y, m - min), u1 = std::max(u.area.bottom(), m + min);
        t.area.y = t0;
        t.area.h = m - t0;
        u.area.y = m;
        u.area.h = u1 - m;
    }
}

namespace {
bool across(const R &a, const R &b) { return a.x < b.right() && b.x < a.right(); } /* the x ranges overlap */

/* the area starts at `start` (never below its drawn top) and reaches at least `min` down, at most to `limit` */
void start_at(R &a, const R &drawn, double start, double min, double limit) {
    start = std::min(start, drawn.y);
    const double bottom = std::min(limit, std::max(a.bottom(), start + min));
    a.y = start;
    a.h = std::max(0.0, bottom - start);
}
}  // namespace

void place_strip_hits(NowHitSet &h, double target, double top_limit, double screen_h) {
    /* the strip's buttons: the band between the chips above and the keys below them */
    for (HitArea &st : h.strip) {
        double upper = top_limit, lower = screen_h;
        for (const HitArea &c : h.chips)
            if (across(c.area, st.area) && c.drawn.bottom() <= st.drawn.y) upper = std::max(upper, c.drawn.bottom());
        for (const HitArea &k : h.keys)
            if (across(k.area, st.area) && k.drawn.y >= st.drawn.bottom()) lower = std::min(lower, k.drawn.y);
        upper = std::min(upper, st.drawn.y);
        lower = std::max(lower, st.drawn.bottom());
        if (lower - upper >= target) {
            const double cy = st.drawn.y + st.drawn.h / 2;
            st.area.h = target;
            st.area.y = std::clamp(cy - target / 2, upper, lower - target);
        } else {
            st.area.y = upper;
            st.area.h = lower - upper;
        }
    }
    /* the chips, the lowest first: each stops above the areas below it that it reaches and grows upwards */
    std::vector<HitArea *> chips;
    for (HitArea &c : h.chips) chips.push_back(&c);
    std::stable_sort(chips.begin(), chips.end(), [](const HitArea *a, const HitArea *b) { return a->drawn.y > b->drawn.y; });
    std::vector<const HitArea *> below;
    for (const HitArea &st : h.strip) below.push_back(&st);
    for (HitArea *c : chips) {
        double limit = c->area.bottom();
        for (const HitArea *o : below)
            if (overlaps(c->area, o->area)) limit = std::min(limit, o->area.y);
        if (limit < c->area.bottom()) {
            limit = std::max(limit, c->drawn.bottom());
            const double top = std::max(top_limit, std::min(c->area.y, limit - target));
            c->area.y = std::min(top, c->drawn.y);
            c->area.h = limit - c->area.y;
        }
        below.push_back(c);
    }
    /* the transport keys start below the strip areas they reach, the bottom row below the keys it reaches */
    for (HitArea &k : h.keys) {
        double start = k.area.y;
        for (const HitArea &st : h.strip)
            if (overlaps(k.area, st.area)) start = std::max(start, st.area.bottom());
        if (start > k.area.y) start_at(k.area, k.drawn, start, target, screen_h);
    }
    for (HitArea &b : h.bottom) {
        double start = b.area.y;
        for (const HitArea &k : h.keys)
            if (overlaps(b.area, k.area)) start = std::max(start, k.area.bottom());
        if (start > b.area.y) start_at(b.area, b.drawn, start, target, screen_h);
    }
}

ListGeom list_geom(ListPage page, int sw, int sh, int body_y) {
    ListGeom l;
    const NowGeom g = now_geom(size_class(sw, sh));
    const ui::Fit f = now_fit(g, sw, sh, body_y);
    const double s = f.s;
    l.s = s;
    const double px0 = f.x(8), px1 = f.x(g.dw - 8);
    const double py0 = f.y(44);
    const double py1 = sh - std::round(8 * s);
    l.panel = R{px0, py0, px1 - px0, py1 - py0};
    l.row_h = std::round((page == ListPage::Browser ? 36 : page == ListPage::Queue ? 34 : 40) * s);
    l.heading_px = 12 * s;
    const double m8 = std::round(8 * s), m16 = std::round(16 * s);
    const double strip_h = std::round(42 * s);
    l.strip = R{px0 + m8, py1 - m8 - strip_h, (px1 - px0) - 2 * m8, strip_h};
    if (page == ListPage::Queue) {
        const double bw = std::round(106 * s), gap = std::round(8 * s);
        if (l.strip.w >= 4 * bw + 3 * gap) {
            for (int i = 0; i < 4; ++i) l.queue_buttons.push_back(R{l.strip.x + i * (bw + gap), l.strip.y, bw, strip_h});
        } else { /* two rows of two */
            const double two_h = 2 * strip_h + gap;
            l.strip = R{px0 + m8, py1 - m8 - two_h, (px1 - px0) - 2 * m8, two_h};
            const double w2 = std::floor((l.strip.w - gap) / 2);
            for (int i = 0; i < 4; ++i)
                l.queue_buttons.push_back(R{l.strip.x + (i % 2) * (w2 + gap), l.strip.y + (i / 2) * (strip_h + gap), w2, strip_h});
        }
    }
    const double head_h = page == ListPage::Output ? std::round(6 * s) : std::round(30 * s);
    l.heading = R{px0 + m16, py0, (px1 - px0) - 2 * m16, head_h};
    l.list = R{px0, py0 + head_h, px1 - px0, l.strip.y - (py0 + head_h)};
    return l;
}

}  // namespace layout
}  // namespace vinyl
