/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the map credit (core/attribution.cpp): the plain text the widget shows, the standard abbreviation,
 * the two-line wrap and the plan of the plate that covers the engine's cut credit (where it goes: above the scale bar,
 * left of the button column, never over either).
 */
#include "attribution.hpp"
#include "check.hpp"

#include <cstring>

using namespace atlas;

namespace {
int px7(const std::string &s) { return static_cast<int>(s.size()) * 7; }
const MeasureFn kMeasure = px7;

/* the engine's rules, repeated by hand for 480x240 (a 480x320 panel's map area) as an independent check */
AttrInput zero8_landscape() { AttrInput in; in.w = 480; in.h = 240; in.target = 44; in.space = 12; in.text_h = 14; return in; }
AttrInput portrait() { AttrInput in; in.w = 320; in.h = 400; in.target = 44; in.space = 12; in.text_h = 14; return in; }
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

    /* the wrap */
    {
        const auto l = wrap_lines("aaa bbb ccc ddd", 7 * 7, 2, kMeasure); /* 7 chars per line */
        CHECK_EQ(l.size(), static_cast<std::size_t>(2));
        CHECK_EQ(l[0], "aaa bbb");
        CHECK_EQ(l[1], "ccc ddd");
        const auto one = wrap_lines("aaa bbb", 100, 2, kMeasure);
        CHECK_EQ(one.size(), static_cast<std::size_t>(1));
        const auto cut = wrap_lines("aaa bbb ccc ddd eee fff", 7 * 7, 2, kMeasure); /* the rest ends in dots */
        CHECK_EQ(cut.size(), static_cast<std::size_t>(2));
        CHECK_EQ(cut[0], "aaa bbb");
        CHECK(cut[1].size() >= 3 && cut[1].compare(cut[1].size() - 3, 3, "...") == 0);
        CHECK(px7(cut[1]) <= 7 * 7);
        const auto word = wrap_lines("abcdefghijklmnopqrstuvwxyz", 10 * 7, 2, kMeasure); /* one word too wide */
        CHECK_EQ(word.size(), static_cast<std::size_t>(1));
        CHECK(px7(word[0]) <= 70);
        CHECK(wrap_lines("", 100, 2, kMeasure).empty());
    }

    /* the plan: a short credit is the engine's business */
    {
        const AttrPlan p = plan_attribution(zero8_landscape(), "(C) OpenStreetMap contributors", kMeasure);
        CHECK(!p.cover); /* 30 chars = 210 px: fits beside the scale bar of 480 px */
        CHECK(!plan_attribution(zero8_landscape(), "", kMeasure).cover);
    }
    /* a long credit on the landscape zero8 map: the engine's rules: target 44 -> plate 33 (15 % of 240 = 36), margin 12
     * -> 12 (plate/2 = 16), pad 6; scale bar max 120 px, width 132, height 6+14+3+3+6 = 32, y = 240-12-32 = 196;
     * right = 480-12-33-12 = 423; the row-higher box is 411 px wide (x 12..423); text fits in 399 px */
    {
        const std::string text = "(C) OpenStreetMap contributors, (C) OpenTopoMap (CC-BY-SA), SRTM, data from many other places";
        const AttrPlan p = plan_attribution(zero8_landscape(), text, kMeasure);
        CHECK(p.cover);
        CHECK_EQ(p.pad, 6);
        CHECK_EQ(p.x, 12);
        CHECK_EQ(p.w, 411);
        CHECK_EQ(p.lines.size(), static_cast<std::size_t>(2));
        CHECK_EQ(p.h, 2 * 14 + 12);
        CHECK_EQ(p.y + p.h, 196 - 6); /* ends just above the scale bar (its top is 196), a pad clear */
        CHECK(p.x + p.w <= 423);      /* left of the button column */
        for (const std::string &l : p.lines) CHECK(px7(l) <= p.w - 2 * p.pad);
        CHECK(p.lines[0].find("OSM") != std::string::npos); /* abbreviated */
    }
    /* a credit the engine cuts, that fits on one line once abbreviated: still covered (its cut row would show) */
    {
        const std::string text = "(C) OpenStreetMap contributors, (C) OpenTopoMap (CC-BY-SA)"; /* 58 chars = 406 + 12 px */
        const AttrPlan p = plan_attribution(zero8_landscape(), text, kMeasure);
        CHECK(p.cover); /* 418 px does not fit in the 411 px box: cut by the engine, covered by us */
        CHECK_EQ(p.lines.size(), static_cast<std::size_t>(1)); /* the abbreviation saves 70 px: one line now */
        CHECK(p.lines[0].find("OSM") != std::string::npos);
        CHECK(p.lines[0].find("...") == std::string::npos);
    }
    /* portrait 320x400 */
    {
        const std::string text = "(C) OpenStreetMap contributors, (C) OpenTopoMap (CC-BY-SA)";
        const AttrInput in = portrait();
        const AttrPlan p = plan_attribution(in, text, kMeasure);
        CHECK(p.cover);
        CHECK(p.x >= 0 && p.x + p.w <= in.w);
        CHECK(p.y >= 0 && p.y + p.h <= in.h);
        /* target 44 -> 44 (a third of 320 is 106), plate 33 (15 % of 320 = 48), margin 12, pad 6, bar 3; the scale bar's top is
         * 400-12-32 = 356, the plate ends 6 px above it; right = 320-12-33-12 = 263 */
        CHECK_EQ(p.y + p.h, 356 - 6);
        CHECK(p.x + p.w <= 263);
        CHECK_EQ(p.lines.size(), static_cast<std::size_t>(2));
        for (const std::string &l : p.lines) CHECK(px7(l) <= p.w - 2 * p.pad);
    }
    /* OpenTopoMap's own credit on the portrait map: three lines, nothing cut, still clear of the scale bar */
    {
        const std::string text = "Map data: (C) OpenStreetMap contributors, SRTM | Map style: (C) OpenTopoMap (CC-BY-SA)";
        const AttrPlan p = plan_attribution(portrait(), text, kMeasure);
        CHECK(p.cover);
        CHECK_EQ(p.lines.size(), static_cast<std::size_t>(3));
        for (const std::string &l : p.lines) {
            CHECK(px7(l) <= p.w - 2 * p.pad);
            CHECK(l.find("...") == std::string::npos);
        }
        CHECK_EQ(p.y + p.h, 356 - 6);
        CHECK_EQ(p.h, 3 * 14 + 12);
        CHECK_EQ(p.lines[0], "Map data: (C) OSM contributors,");
        CHECK_EQ(p.lines[2], "(CC-BY-SA)");
    }
    /* the same credit on the landscape zero8 map: two lines */
    {
        const std::string text = "Map data: (C) OpenStreetMap contributors, SRTM | Map style: (C) OpenTopoMap (CC-BY-SA)";
        const AttrPlan p = plan_attribution(zero8_landscape(), text, kMeasure);
        CHECK(p.cover);
        CHECK_EQ(p.lines.size(), static_cast<std::size_t>(2));
    }
}
