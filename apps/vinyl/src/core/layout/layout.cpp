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
    if (a >= 1.6) return SizeClass::Large;
    /* aspect 1.2..1.6 (the coordinator, 2026-11-13): two panes from 800 wide and 480 high (800x600, 1024x768), the 4:3
     * single pane from 640 wide and 480 high, the 480x320 layout below */
    if (h >= 480) return w >= 800 ? SizeClass::Large : (w >= 640 ? SizeClass::Medium : SizeClass::Small);
    return SizeClass::Small;
}

const char *class_name(SizeClass c) {
    switch (c) {
    case SizeClass::Medium: return "medium";
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
    case SizeClass::Small: /* NP-480x320 of the full-UX canvas (owner, 2026-11-13: "ok for the 480x320 layout") */
        g.dw = 480;
        g.dh = 320;
        g.panel = R{8, 44, 464, 268};
        g.disc = R{89, 61, 139, 139};
        g.sleeve = R{20, 56, 148, 148};
        g.label_d = 57;
        g.hole_d = 7;
        g.title = text(236, 56, 224, 26, 19, true);
        g.artist = text(236, 84, 224, 20, 15, true);
        g.album = text(236, 106, 224, 16, 12, false);
        /* one plain line: "AAC 44.1 / 161k, gapless" */
        g.chip0 = R{236, 126, 0, 16};
        g.chip_px = 12;
        g.chip_pad = 0;
        g.chip_gap = 6;
        g.chip_icon = 12;
        g.chip_step = 20;
        g.chip_rows = 1;
        g.chip_layout = {{0}};
        g.short_format = true;
        g.gapless_in_format = true;
        g.quality_in_chip = false;
        g.out_button_on = true;
        g.out_button = R{236, 146, 224, 28};
        g.out_px = 12;
        g.out_icon = 12;
        g.out_pad = 8;
        g.out_gap = 6;
        /* the volume group under the output button, as drawn (the "-" narrower than the "+") */
        g.vol_minus = R{236, 178, 56, 34};
        g.vol_value = R{296, 178, 80, 34};
        g.vol_plus = R{380, 178, 80, 34};
        g.vol_text_px = 17;
        g.vol_icon = 18;
        g.time = text(20, 220, 48, 28, 14, false, 28);
        g.remain = text(412, 220, 48, 28, 14, false, 28);
        g.seek = R{72, 229, 336, 10};
        /* P1: six keys; the draft's seventh, Menu (P2), at 20 + 60 i with 46 wide keys and an 80 wide play key:
         * shuffle 20, previous 80, play 140, next 234, repeat 294, queue 354, menu 414, all 48 high at y 258 */
        g.transport = {btn("shuffle", 20, 258, 56, 48), btn("prev", 90, 258, 56, 48), btn("play", 160, 258, 88, 48, 26, true),
                       btn("next", 262, 258, 56, 48), btn("repeat", 332, 258, 56, 48), btn("queue", 402, 258, 58, 48)};
        g.repeat_dash = R{354, 298, 12, 4};
        g.seams = {215, 252, 309};
        break;
    case SizeClass::Medium: /* NP-640x480 of the full-UX canvas (owner, 2026-11-13: "try without the up next", "that works") */
        g.dw = 640;
        g.dh = 480;
        g.panel = R{8, 44, 624, 428};
        g.disc = R{131, 63, 210, 210};
        g.sleeve = R{24, 56, 224, 224};
        g.label_d = 84;
        g.hole_d = 8;
        g.title = text(360, 56, 260, 56, 22, true, 28, 2);
        g.artist = text(360, 120, 260, 22, 17, true);
        g.album = text(360, 146, 260, 18, 13, false);
        /* the format and gapless chips, flat, side by side */
        g.chip0 = R{360, 178, 0, 28};
        g.chip_px = 13;
        g.chip_pad = 12;
        g.chip_gap = 6;
        g.chip_icon = 13;
        g.chip_space = 8;
        g.chip_step = 34;
        g.chip_rows = 1;
        g.chip_layout = {{0, 3}};
        g.format_khz = true;
        g.chip_flat = true;
        g.quality_in_chip = false;
        g.out_button_on = true;
        g.out_button = R{360, 220, 260, 42};
        g.out_px = 14;
        g.out_icon = 16;
        g.out_pad = 12;
        g.out_gap = 8;
        g.time = text(24, 292, 56, 28, 15, false, 28);
        g.remain = text(560, 292, 60, 28, 15, false, 28);
        g.seek = R{86, 302, 468, 10};
        g.transport = {btn("shuffle", 24, 336, 80, 56, 24), btn("prev", 120, 336, 80, 56, 24),
                       btn("play", 216, 336, 208, 56, 28, true), btn("next", 440, 336, 80, 56, 24),
                       btn("repeat", 536, 336, 80, 56, 24)};
        g.repeat_dash = R{570, 384, 12, 4};
        /* P1 has no Menu key: Browse and Queue share its width (the draft, P2: Browse 24, Queue 160, Menu 296, each 128
         * wide, 8 apart, at y 410, 52 high), the volume group as drawn */
        g.bottom = {word("browse", 24, 410, 196, 52), word("queue", 228, 410, 196, 52)};
        g.vol_minus = R{440, 410, 52, 52};
        g.vol_value = R{496, 410, 60, 52};
        g.vol_plus = R{560, 410, 56, 52};
        g.seams = {286, 328, 401};
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
        g.out_button_on = true; /* owner, 2026-11-13: the Output key becomes the volume group */
        g.out_px = 11;
        g.out_icon = 11;
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
        g.out_button_on = true; /* owner, 2026-11-13: the Output key becomes the volume group */
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
        g.chip_layout = {{0}, {2}, {3}}; /* owner, 2026-11-13: no Vol chip, the volume is the bottom group */
        g.out_button_on = false;
        g.short_format = true;
        g.quality_in_chip = false;
        g.time = text(330, 274, 48, 28, 14, false, 28);
        g.remain = text(722, 274, 48, 28, 14, false, 28);
        g.seek = R{382, 283, 336, 10};
        g.transport = {btn("shuffle", 330, 312, 56, 48), btn("prev", 400, 312, 56, 48), btn("play", 470, 312, 88, 48, 26, true),
                       btn("next", 572, 312, 56, 48), btn("repeat", 642, 312, 56, 48), btn("sleep", 712, 312, 58, 48)};
        g.transport.back().enabled = false; /* sleep timer: P2 */
        g.repeat_dash = R{664, 352, 12, 4};
        /* owner, 2026-11-13 (NP-800x480 of the full-UX canvas): Browse, Output (the Menu key's place, P2), the volume
         * group where the dimmed Equalizer key was */
        g.bottom = {word("browse", 330, 384, 140, 42), word("output", 480, 384, 140, 42)};
        g.vol_minus = R{630, 384, 40, 42};
        g.vol_value = R{674, 384, 52, 42};
        g.vol_plus = R{730, 384, 40, 42};
        g.seams = {262, 306, 372};
        break;
    }
    /* the record, one rule on every class: 94 % of the sleeve's height, centred on it (the owner's deck report,
     * 2026-11-13: "the disc going out of the song cover picture seems to be too low, it appears under the cover"); the
     * label 40 % of the record, about a third of it showing: the record's centre 0.15 label diameters inside the sleeve's
     * right edge (the coordinator, 2026-11-13: about a tenth; the approved NP-640x480 and NP-480x320 drafts measure 12 of
     * 84 and 9.5 of 57); never into the info column beside the sleeve (2 design px clear of it: the large class, whose
     * record then shows less of its label, and 1 px less on the square class); the spindle hole a tenth of the label */
    const double d = std::round(g.sleeve.h * 0.94);
    g.label_d = std::round(d * 0.40);
    g.hole_d = std::max(3.0, std::round(g.label_d * 0.10));
    double cx = g.sleeve.right() - 0.15 * g.label_d;
    if (g.title.box.x > g.sleeve.right()) cx = std::min(cx, g.title.box.x - 2 - d / 2);
    g.disc = R{cx - d / 2, g.sleeve.y + (g.sleeve.h - d) / 2, d, d};
    return g;
}

bool key_active(const Btn &b, bool queue_empty) {
    if (!b.enabled) return false;
    if (b.id == "queue" || b.id == "browse" || b.id == "output") return true;
    return !queue_empty;
}

bool shows_gapless(const NowGeom &g) {
    if (g.gapless_in_format) return true;
    for (const std::vector<int> &row : g.chip_layout)
        for (int id : row)
            if (id == 3) return true;
    return false;
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

/* the rows of now playing's areas (drawn boxes that share a height), top to bottom */
struct HitRow {
    std::vector<HitArea *> a;
    double d0 = 0, d1 = 0;   /* drawn top and bottom */
    double x0 = 0, x1 = 0;   /* the areas' x extent */
    double top = 0, bottom = 0;
};

std::vector<HitRow> hit_rows(NowHitSet &h) {
    std::vector<HitArea *> all;
    for (std::vector<HitArea> *v : {&h.chips, &h.seek, &h.keys, &h.bottom})
        for (HitArea &a : *v)
            if (a.drawn.w > 0 && a.drawn.h > 0) all.push_back(&a);
    std::stable_sort(all.begin(), all.end(), [](const HitArea *p, const HitArea *q) { return p->drawn.y < q->drawn.y; });
    std::vector<HitRow> rows;
    for (HitArea *a : all) {
        if (rows.empty() || a->drawn.y >= rows.back().d1 - 1e-9) {
            HitRow r;
            r.d0 = a->drawn.y;
            r.d1 = a->drawn.bottom();
            rows.push_back(r);
        }
        HitRow &r = rows.back();
        r.a.push_back(a);
        r.d1 = std::max(r.d1, a->drawn.bottom());
    }
    return rows;
}

/* The rows' tops and bottoms for a common least height hh: false when they do not fit. lo_* the highest placement,
 * hi_* the lowest; conflict[i]: row i and row i + 1 share x (they must not overlap). */
bool rows_fit(const std::vector<HitRow> &rows, const std::vector<bool> &conflict, double hh, double top_limit, double screen_h,
              std::vector<double> &lo_top, std::vector<double> &hi_top) {
    const size_t n = rows.size();
    lo_top.assign(n, 0);
    hi_top.assign(n, 0);
    std::vector<double> lo_bottom(n), hi_bottom(n);
    for (size_t i = 0; i < n; ++i) {
        lo_top[i] = (i > 0 && conflict[i - 1]) ? lo_bottom[i - 1] : top_limit;
        if (lo_top[i] > rows[i].d0 + 1e-9) return false;
        lo_bottom[i] = std::max(rows[i].d1, lo_top[i] + hh);
    }
    for (size_t k = n; k-- > 0;) {
        hi_bottom[k] = (k + 1 < n && conflict[k]) ? hi_top[k + 1] : screen_h;
        if (hi_bottom[k] < rows[k].d1 - 1e-9) return false;
        hi_top[k] = std::min(rows[k].d0, hi_bottom[k] - hh);
    }
    for (size_t i = 0; i < n; ++i)
        if (lo_top[i] > hi_top[i] + 1e-9) return false;
    return true;
}

}  // namespace

void place_now_hits(NowHitSet &h, double target, double top_limit, double screen_w, double screen_h) {
    std::vector<HitRow> rows = hit_rows(h);
    /* across: in a row, neighbours meet in the middle of the gap between their drawn boxes; on the screen */
    for (HitRow &r : rows) {
        std::stable_sort(r.a.begin(), r.a.end(), [](const HitArea *p, const HitArea *q) { return p->drawn.x < q->drawn.x; });
        for (size_t i = 0; i + 1 < r.a.size(); ++i) {
            HitArea &l = *r.a[i], &u = *r.a[i + 1];
            if (l.area.right() <= u.area.x || l.drawn.right() > u.drawn.x) continue;
            const double m = (l.drawn.right() + u.drawn.x) / 2;
            if (l.area.right() > m) l.area.w = m - l.area.x;
            if (u.area.x < m) {
                const double right = u.area.right();
                u.area.x = m;
                u.area.w = right - m;
            }
        }
        r.x0 = screen_w;
        r.x1 = 0;
        for (HitArea *a : r.a) {
            const double x0 = std::max(0.0, a->area.x), x1 = std::min(screen_w, a->area.right());
            a->area.x = x0;
            a->area.w = std::max(0.0, x1 - x0);
            r.x0 = std::min(r.x0, x0);
            r.x1 = std::max(r.x1, x1);
        }
    }
    if (rows.empty()) return;
    /* down: the largest common height up to the target, then each boundary as near the middle of its gap as that allows */
    const size_t n = rows.size();
    std::vector<bool> conflict(n, false);
    for (size_t i = 0; i + 1 < n; ++i) conflict[i] = rows[i].x0 < rows[i + 1].x1 && rows[i + 1].x0 < rows[i].x1;
    std::vector<double> lo_top, hi_top;
    double hh = target;
    if (!rows_fit(rows, conflict, hh, top_limit, screen_h, lo_top, hi_top)) {
        double lo = 0, hi = target;
        for (int it = 0; it < 40; ++it) {
            const double mid = (lo + hi) / 2;
            if (rows_fit(rows, conflict, mid, top_limit, screen_h, lo_top, hi_top)) lo = mid;
            else hi = mid;
        }
        hh = std::floor((lo + 1e-6) * 4) / 4; /* to the quarter px under the limit */
        if (!rows_fit(rows, conflict, hh, top_limit, screen_h, lo_top, hi_top)) {
            hh = std::floor(lo * 4) / 4;
            rows_fit(rows, conflict, hh, top_limit, screen_h, lo_top, hi_top);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        HitRow &r = rows[i];
        const double full = std::max(target, r.d1 - r.d0), nat_top = (r.d0 + r.d1) / 2 - full / 2;
        const bool free_top = i == 0 || !conflict[i - 1];
        if (free_top) r.top = std::clamp(nat_top, lo_top[i], hi_top[i]);
        else r.top = rows[i - 1].bottom;
        if (i + 1 < n && conflict[i]) {
            const double lo_b = std::max(r.d1, r.top + hh), hi_b = hi_top[i + 1];
            r.bottom = std::clamp(std::round((r.d1 + rows[i + 1].d0) / 2), lo_b, std::max(lo_b, hi_b));
        } else {
            r.bottom = std::min(screen_h, std::max({r.d1, r.top + target, nat_top + full}));
        }
        if (free_top) r.top = std::max(top_limit, std::min(r.top, r.bottom - target)); /* the free side to the target */
    }
    /* no row higher than it needs (the target, or its drawn height): a boundary in the middle of a wide gap must not make
     * a large invisible area over what lies between (the square class's up-next box) */
    for (HitRow &r : rows) {
        const double full = std::max(target, r.d1 - r.d0);
        if (r.bottom - r.top <= full) continue;
        const double t = std::clamp((r.d0 + r.d1) / 2 - full / 2, r.top, r.bottom - full);
        r.top = t;
        r.bottom = t + full;
    }
    for (HitRow &r : rows)
        for (HitArea *a : r.a) {
            a->area.y = r.top;
            a->area.h = r.bottom - r.top;
        }
}

double now_rows_min_height(const NowHitSet &h) {
    double m = 1e9;
    for (const std::vector<HitArea> *v : {&h.chips, &h.seek, &h.keys, &h.bottom})
        for (const HitArea &a : *v)
            if (a.drawn.w > 0) m = std::min(m, a.area.h);
    return m;
}

namespace {
double px_of(const ui::Fit &f, double v) { return std::max(1.0, std::round(v * f.s)); }
R box_of(const ui::Fit &f, const R &r) { return R{double(f.x(r.x)), double(f.y(r.y)), double(f.w(r.x, r.w)), double(f.h(r.y, r.h))}; }
}  // namespace

NowHitSet now_hit_set(const NowGeom &g, const ui::Fit &f, double target, int keys_dy) {
    NowHitSet h;
    for (const Btn &b : g.transport)
        if (b.enabled) {
            R d = box_of(f, b.box);
            d.y += keys_dy;
            h.keys.push_back({d, grow_hit(d, target, target)});
        }
    for (const Btn &b : g.bottom)
        if (b.enabled) {
            const R d = box_of(f, b.box);
            h.bottom.push_back({d, grow_hit(d, target, target)});
        }
    if (g.vol_group)
        for (const R &r : {g.vol_minus, g.vol_plus}) {
            const R d = box_of(f, r);
            h.bottom.push_back({d, grow_hit(d, target, target)});
        }
    const double sx = f.x(g.seek.x), sw = std::max(1, f.w(g.seek.x, g.seek.w));
    const double sh = std::max(2, f.h(g.seek.y, g.seek.h)), kd = px_of(f, 18);
    const R d{sx - std::floor(kd / 2), f.y(g.seek.y) + std::floor(sh / 2) - std::floor(kd / 2), sw + kd, kd};
    h.seek.push_back({d, grow_hit(d, 0, target)});
    return h;
}

int now_keys_shift(const NowGeom &g, const ui::Fit &f, double target, const std::vector<R> &outputs, double top_limit,
                   double screen_w, double screen_h) {
    auto least = [&](int dy) {
        NowHitSet h = now_hit_set(g, f, target, dy);
        for (const R &o : outputs) h.chips.push_back({o, grow_hit(o, target, target)});
        place_now_hits(h, target, top_limit, screen_w, screen_h);
        return now_rows_min_height(h);
    };
    double best_h = least(0);
    if (best_h >= target - 1e-6) return 0;
    double keys_top = screen_h, keys_bottom = 0;
    for (const Btn &b : g.transport) {
        keys_top = std::min(keys_top, double(f.y(b.box.y)));
        keys_bottom = std::max(keys_bottom, double(f.y(b.box.y) + f.h(b.box.y, b.box.h)));
    }
    double below = f.y(g.panel.bottom());
    if (g.has_upnext) below = std::min(below, double(f.y(g.upnext.y)));
    for (const Btn &b : g.bottom) below = std::min(below, double(f.y(b.box.y)));
    if (g.vol_group && f.y(g.vol_minus.y) > keys_top) below = std::min(below, double(f.y(g.vol_minus.y)));
    const int four = static_cast<int>(px_of(f, 4));
    int best = 0;
    for (int dy = 1; dy <= four && keys_bottom + dy <= below - four + 1e-9; ++dy) {
        const double m = least(dy);
        if (m > best_h + 1e-6) {
            best = dy;
            best_h = m;
        }
        if (m >= target - 1e-6) break;
    }
    return best;
}

bool notice_in_seek_row(const R &seek_area, double top_lim, double bot_lim, double x, double w, double h, double min_h,
                        R &box, R &area) {
    const double top = std::ceil(std::max(seek_area.y, top_lim)), bottom = std::floor(std::min(seek_area.bottom(), bot_lim));
    const double band = bottom - top;
    if (band < min_h - 1e-9 || band <= 0) return false;
    const double hh = std::floor(std::min(h, band));
    box = R{x, top + std::floor((band - hh) / 2), w, hh};
    area = R{x, seek_area.y, w, seek_area.h};
    return true;
}

R notice_box_over_strip(const R &strip, double list_bottom, double h) {
    const double bottom = std::round(strip.bottom());
    const double top = std::max(bottom - std::round(h), std::ceil(list_bottom));
    return R{std::round(strip.x), top, std::round(strip.w), std::max(1.0, bottom - top)};
}

R notice_area_over(const R &box, const std::vector<R> &covered) {
    double x0 = box.x, y0 = box.y, x1 = box.right(), y1 = box.bottom();
    for (const R &c : covered) {
        x0 = std::min(x0, c.x);
        y0 = std::min(y0, c.y);
        x1 = std::max(x1, c.right());
        y1 = std::max(y1, c.bottom());
    }
    return R{x0, y0, x1 - x0, y1 - y0};
}

std::vector<R> queue_touch_areas(const ListGeom &lg, int screen_h, double target, double &list_bottom) {
    list_bottom = lg.list.bottom();
    std::vector<R> areas;
    const std::vector<R> &b = lg.queue_buttons;
    if (b.empty()) return areas;
    const bool two_rows = b.size() == 4 && b[2].y > b[0].y;
    std::vector<HitArea> h;
    for (const R &r : b) h.push_back({r, grow_hit(r, target, 0)});
    /* across: neighbours in a row meet in the middle of their gap */
    for (size_t i = 0; i < h.size(); ++i)
        for (size_t j = i + 1; j < h.size(); ++j)
            if (std::fabs(h[i].drawn.y - h[j].drawn.y) < 1) split_pair(h[i], h[j], target);
    /* down: the target high; two rows meet in the middle of their gap; from the strip's top down, moved up where the
     * screen's bottom is nearer */
    const double hh = std::max(b[0].h, target);
    if (!two_rows) {
        const double y0 = std::min(b[0].y, screen_h - hh);
        for (HitArea &a : h) {
            a.area.y = y0;
            a.area.h = hh;
        }
        list_bottom = std::min(list_bottom, y0);
    } else {
        double mid = (b[0].bottom() + b[2].y) / 2;
        double low_bottom = std::max(b[2].bottom(), mid + hh);
        if (low_bottom > screen_h) {
            low_bottom = screen_h;
            mid = low_bottom - hh;
        }
        const double top = std::min(b[0].y, mid - hh);
        for (size_t i = 0; i < h.size(); ++i) {
            if (i < 2) {
                h[i].area.y = top;
                h[i].area.h = mid - top;
            } else {
                h[i].area.y = mid;
                h[i].area.h = low_bottom - mid;
            }
        }
        list_bottom = std::min(list_bottom, top);
    }
    for (const HitArea &a : h) areas.push_back(a.area);
    return areas;
}

MiniBoxes mini_touch_areas(const ListGeom &lg, int screen_w, int screen_h, double target) {
    MiniBoxes m;
    auto S = [&](double v) { return std::max(1.0, std::round(v * lg.s)); };
    const double sx = std::round(lg.strip.x), sy = std::round(lg.strip.y), sw = std::round(lg.strip.w), shh = std::round(lg.strip.h);
    m.open = R{sx, sy, sw - S(100), shh};
    m.play = R{sx + sw - S(94), sy + S(3), S(44), S(36)};
    m.next = R{sx + sw - S(46), sy + S(3), S(40), S(36)};
    const double hit_h = std::min(double(screen_h), std::max(shh, target));
    const double hit_y = std::min(sy, screen_h - hit_h);
    HitArea ap{R{m.play.x, sy, m.play.w, shh}, {}}, an{R{m.next.x, sy, m.next.w, shh}, {}};
    ap.area = grow_hit(ap.drawn, target, 0);
    an.area = grow_hit(an.drawn, target, 0);
    split_pair(ap, an, target);
    if (an.area.right() > screen_w) {
        const double d = an.area.right() - screen_w;
        an.area.x -= d;
        ap.area.x -= d;
    }
    m.play_area = R{ap.area.x, hit_y, ap.area.w, hit_h};
    m.next_area = R{an.area.x, hit_y, an.area.w, hit_h};
    m.open_area = R{sx, hit_y, std::max(1.0, ap.area.x - sx), hit_h};
    m.list_bottom = std::min(lg.list.bottom(), hit_y);
    return m;
}

ListGeom list_geom(ListPage page, int sw, int sh, int body_y) {
    ListGeom l;
    /* the list pages keep the 480x320 drawing on the medium class too (the deck: scaled 4/3, as before the 640x480
     * now-playing draft): that draft is of now playing only */
    SizeClass c = size_class(sw, sh);
    if (c == SizeClass::Medium) c = SizeClass::Small;
    const NowGeom g = now_geom(c);
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
