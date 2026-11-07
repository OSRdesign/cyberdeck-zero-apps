/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the map credit and the scale bar at the bottom edge (core/attribution.cpp): the plain text the
 * widget shows, the standard abbreviation, the word wrap that never cuts, the engine's overlay rows moved out of sight
 * (engine_reserve) and the bottom row's layout (one line / abbreviated / wrapped, flush with the bottom edge, one
 * baseline, no overlaps) at 640x480, 480x320 and 320x480 with the longest credit (OpenTopoMap's).
 */
#include "attribution.hpp"
#include "check.hpp"

#include <algorithm>
#include <cstring>

using namespace atlas;

namespace {
int px5(const std::string &s) { return static_cast<int>(s.size()) * 5; } /* about the smallest font's average glyph */
int px7(const std::string &s) { return static_cast<int>(s.size()) * 7; }
const MeasureFn kMeasure5 = px5;
const MeasureFn kMeasure7 = px7;

/* The visible map view of each panel (the header and the tab strip taken off), the toolkit's tokens of that class. */
struct Panel {
    const char *name;
    int w, view_h, target, space, caption_h;
};
const Panel kPanels[] = {
    {"640x480", 640, 345, 44, 16, 22},
    {"480x320", 480, 200, 44, 12, 18},
    {"320x480", 320, 360, 44, 12, 18},
};

const char *const kShort = "(C) OpenStreetMap contributors";
const char *const kLong = "Map data: (C) OpenStreetMap contributors, SRTM | Map style: (C) OpenTopoMap (CC-BY-SA)";

CreditInput credit_in(const Panel &p)
{
    CreditInput in;
    in.w = p.w;
    in.h = p.view_h;
    in.text_h = 9;
    AttrInput a;
    a.w = p.w;
    a.h = p.view_h;
    a.target = p.target;
    a.space = p.space;
    a.text_h = p.caption_h;
    in.pad = map_metrics(a).pad;
    return in;
}

std::string joined(const std::vector<std::string> &l)
{
    std::string s;
    for (const std::string &x : l) s += (s.empty() ? "" : " ") + x;
    return s;
}

/* the geometry rules every layout keeps */
void check_row(const CreditInput &in, const CreditLayout &L, const std::string &credit_text)
{
    /* the scale bar's widest plate: left, flush with the bottom edge, one row high */
    CHECK_EQ(L.scale.x, 0);
    CHECK_EQ(L.scale.y + L.scale.h, in.h);
    CHECK_EQ(L.scale.h, L.row_h);
    /* one baseline: the last credit line's text and the scale bar's text start at the same y */
    CHECK_EQ(L.text_y, in.h - L.row_h + L.vp);
    if (credit_text.empty()) {
        CHECK(L.credit.empty());
        return;
    }
    CHECK(!L.credit.empty());
    CHECK_EQ(L.credit.x + L.credit.w, in.right > 0 ? in.right : in.w); /* flush right (at the limit the zoom buttons set) */
    CHECK_EQ(L.credit.y + L.credit.h, in.h);      /* flush with the bottom edge */
    CHECK_EQ(L.credit.h, static_cast<int>(L.lines.size()) * in.text_h + 2 * L.vp);
    CHECK_EQ(L.credit.y + L.vp + (static_cast<int>(L.lines.size()) - 1) * in.text_h, L.text_y); /* last line on the baseline */
    CHECK(L.credit.x >= L.scale.x + L.scale.w + L.hp); /* never over the scale bar, a gap between */
    CHECK(L.credit.y >= 0 && L.credit.x >= 0);
    /* nothing cut, nothing dropped: the lines are the credit or its abbreviation, word for word */
    const std::string j = joined(L.lines);
    CHECK(j == credit_text || j == attribution_abbrev(credit_text));
    CHECK(j.find("...") == std::string::npos);
    for (const std::string &l : L.lines) CHECK(px5(l) + 2 * L.hp <= L.credit.w);
}
} // namespace

void test_attribution()
{
    /* the plain text */
    CHECK_EQ(attribution_plain("&copy; <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a> contributors"),
             "(C) OpenStreetMap contributors");
    CHECK_EQ(attribution_plain("Maps &#169; Thunderforest, Data &amp; style"), "Maps (C) Thunderforest, Data & style");
    CHECK_EQ(attribution_plain("  a \n\t b  "), "a b");
    CHECK_EQ(attribution_plain("\xC2\xA9 OSM"), "(C) OSM");
    CHECK_EQ(attribution_plain("broken <tag"), "broken");
    CHECK_EQ(attribution_plain(""), "");

    /* the abbreviation */
    CHECK_EQ(attribution_abbrev("(C) OpenStreetMap contributors"), "(C) OSM contributors");
    CHECK_EQ(attribution_abbrev("OpenStreetMap and OpenStreetMap"), "OSM and OSM");
    CHECK_EQ(attribution_abbrev("nothing to shorten"), "nothing to shorten");

    /* the wrap: greedy, nothing cut */
    {
        const auto l = wrap_words("aaa bbb ccc ddd", 7 * 7, kMeasure7); /* 7 chars per line */
        CHECK_EQ(l.size(), static_cast<std::size_t>(2));
        CHECK_EQ(l[0], "aaa bbb");
        CHECK_EQ(l[1], "ccc ddd");
        CHECK_EQ(wrap_words("aaa bbb", 100, kMeasure7).size(), static_cast<std::size_t>(1));
        const auto many = wrap_words("aaa bbb ccc ddd eee fff", 7 * 7, kMeasure7); /* as many lines as it takes */
        CHECK_EQ(many.size(), static_cast<std::size_t>(3));
        CHECK_EQ(joined(many), "aaa bbb ccc ddd eee fff");
        const auto word = wrap_words("abcdefghijklmnopqrstuvwxyz", 10 * 7, kMeasure7); /* one word too wide: split */
        CHECK_EQ(word.size(), static_cast<std::size_t>(3));
        for (const std::string &w : word) CHECK(px7(w) <= 70);
        std::string back;
        for (const std::string &w : word) back += w;
        CHECK_EQ(back, "abcdefghijklmnopqrstuvwxyz");
        CHECK(wrap_words("", 100, kMeasure7).empty());
    }

    /* the engine's rules for 480x240 repeated by hand: target 44 -> plate 33 (15 % of 240 = 36), margin 12, pad 6, bar 3;
     * scale bar max 120 px, plate 132 x (6 + 14 + 3 + 3 + 6 = 32), top y = 240 - 12 - 32 = 196 */
    {
        AttrInput in;
        in.w = 480;
        in.h = 240;
        in.target = 44;
        in.space = 12;
        in.text_h = 14;
        const MapMetrics m = map_metrics(in);
        CHECK_EQ(m.plate, 33);
        CHECK_EQ(m.margin, 12);
        CHECK_EQ(m.pad, 6);
        CHECK_EQ(m.sb_w, 132);
        CHECK_EQ(m.sb_h, 32);
        CHECK_EQ(m.sb_y, 196);
        CHECK_EQ(m.row_h, 26);
    }

    /* engine_reserve: the engine's scale bar and credit end up wholly below the view's bottom edge. A short credit sits
     * beside the scale bar (the bar's top is the topmost row), a long one a row higher (its top is). */
    for (const Panel &p : kPanels) {
        for (int credit_w : {0, 150, 400}) {
            AttrInput v;
            v.w = p.w;
            v.h = p.view_h;
            v.target = p.target;
            v.space = p.space;
            v.text_h = p.caption_h;
            const int r = engine_reserve(v, credit_w);
            CHECK(r > 0);
            AttrInput t = v;
            t.h = v.h + r;
            const MapMetrics m = map_metrics(t);
            int top = m.sb_y;
            if (credit_w > 0) {
                const int full = credit_w + 2 * m.pad, beside = t.w - m.margin - (m.margin + m.sb_w) - m.margin;
                top = full <= beside ? std::min(top, t.h - m.margin - m.row_h) : std::min(top, m.sb_y - m.pad - m.row_h);
            }
            CHECK(top >= v.h);          /* every engine row starts at or below the view's edge */
            CHECK(top - v.h < m.row_h); /* and no more than needed is reserved */
        }
    }
    /* the reserve of a credit that does not fit beside the scale bar is larger by the row it moves up */
    {
        const Panel &p = kPanels[2];
        AttrInput v;
        v.w = p.w;
        v.h = p.view_h;
        v.target = p.target;
        v.space = p.space;
        v.text_h = p.caption_h;
        CHECK(engine_reserve(v, 400) > engine_reserve(v, 100));
    }

    /* the bottom row at the three panel sizes: a short credit is whole on one line */
    for (const Panel &p : kPanels) {
        const CreditInput in = credit_in(p);
        const CreditLayout L = plan_credit(in, kShort, kMeasure5);
        check_row(in, L, kShort);
        CHECK_EQ(L.lines.size(), static_cast<std::size_t>(1));
        CHECK(!L.abbreviated);
        CHECK(!L.wrapped);
        CHECK_EQ(L.lines[0], kShort);
        CHECK_EQ(L.credit.h, L.row_h);
    }
    /* the longest credit (OpenTopoMap's, 88 characters = 440 px at 5 px): the full text when it fits, then the abbreviation
     * on one line, then wrapped; per panel */
    {
        const CreditInput wide = credit_in(kPanels[0]);
        const CreditLayout L = plan_credit(wide, kLong, kMeasure5);
        check_row(wide, L, kLong);
        CHECK(L.abbreviated);          /* 440 px does not fit beside the scale bar of 640 px, the abbreviation does */
        CHECK(!L.wrapped);
        CHECK_EQ(L.lines[0], attribution_abbrev(kLong));
        const CreditInput mid = credit_in(kPanels[1]);
        const CreditLayout M = plan_credit(mid, kLong, kMeasure5);
        check_row(mid, M, kLong);
        CHECK(M.wrapped);              /* last resort: two lines, the full text (it fits in two) */
        CHECK_EQ(M.lines.size(), static_cast<std::size_t>(2));
        CHECK(!M.abbreviated);
        const CreditInput narrow = credit_in(kPanels[2]);
        const CreditLayout N = plan_credit(narrow, kLong, kMeasure5);
        check_row(narrow, N, kLong);
        CHECK(N.wrapped);
        CHECK(N.lines.size() >= 2);
        CHECK(N.abbreviated);          /* more than two lines in full: the abbreviation, wrapped */
    }
    /* any credit length at any panel keeps the rules; the plates never meet whatever the scale bar's length (1x, 1.5x and 2x
     * change the bar's pixels and its label, up to the reserved widest) */
    for (const Panel &p : kPanels) {
        const CreditInput in = credit_in(p);
        for (int n = 0; n <= 40; n += 3) {
            std::string text;
            for (int i = 0; i < n; ++i) text += "abcd ";
            text += "(C) OpenStreetMap contributors";
            const CreditLayout L = plan_credit(in, text, kMeasure5);
            check_row(in, L, text);
            for (int bar = 1; bar <= L.sb_bar_max; bar += 13)
                for (int label = 20; label <= L.sb_label_w; label += 5) {
                    const Rect s = scale_plate(L, bar, label);
                    CHECK(s.x + s.w <= L.credit.x);
                    CHECK_EQ(s.y + s.h, in.h);
                    CHECK_EQ(s.h, L.row_h);
                }
        }
    }
    /* the zoom buttons reach the credit's rows (a short view, a long credit): the credit stops left of their column and
     * grows a line instead; still whole, still on the baseline, still clear of the scale bar */
    for (const Panel &p : kPanels) {
        CreditInput in = credit_in(p);
        const CreditLayout free_run = plan_credit(in, kLong, kMeasure5);
        in.right = p.w - 60;
        const CreditLayout L = plan_credit(in, kLong, kMeasure5);
        check_row(in, L, kLong);
        CHECK(L.credit.x + L.credit.w <= p.w - 60);
        CHECK(L.lines.size() >= free_run.lines.size());
    }
    /* no credit: only the scale bar's row; the credit text is empty */
    for (const Panel &p : kPanels) {
        const CreditInput in = credit_in(p);
        const CreditLayout L = plan_credit(in, "", kMeasure5);
        check_row(in, L, "");
        CHECK(L.lines.empty());
    }
    /* a missing byte-order or html credit goes through attribution_plain first (the caller's job): the layout keeps "(C)" */
    {
        const CreditInput in = credit_in(kPanels[1]);
        const CreditLayout L = plan_credit(in, attribution_plain("&copy; OpenStreetMap contributors"), kMeasure5);
        CHECK_EQ(L.lines[0], "(C) OpenStreetMap contributors");
    }
}
