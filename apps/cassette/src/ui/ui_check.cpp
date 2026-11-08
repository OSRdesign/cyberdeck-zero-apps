/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the scenes' layout check (see ui_check.hpp).
 */
#include "ui_check.hpp"

#include "paint.hpp"

#include "lvgl/src/misc/lv_event_private.h" /* the event filters: what a press does */

#include <algorithm>
#include <cstdio>

namespace cassette {
namespace ui {

namespace {

bool hidden_up(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}

bool inter(const lv_area_t &a, const lv_area_t &b, lv_area_t *out)
{
    lv_area_t r{std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    if (out) *out = r;
    return r.x1 <= r.x2 && r.y1 <= r.y2;
}

/* obj's area clipped by every ancestor; *scroll_clip: a scrolling ancestor cut it */
lv_area_t clipped(lv_obj_t *obj, bool *scroll_clip)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (scroll_clip) *scroll_clip = false;
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        lv_area_t r;
        const bool any = inter(a, pa, &r);
        if (!any || r.x1 != a.x1 || r.y1 != a.y1 || r.x2 != a.x2 || r.y2 != a.y2) {
            if (scroll_clip && lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) *scroll_clip = true;
        }
        if (!any) return lv_area_t{0, 0, -1, -1};
        a = r;
    }
    return a;
}

bool listens(lv_obj_t *o)
{
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return false;
    if (lv_obj_check_type(o, &lv_button_class)) return true;
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); ++i) {
        const lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (!d) continue;
        const uint32_t code = d->filter & ~static_cast<uint32_t>(LV_EVENT_PREPROCESS);
        if (code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_ALL)
            return true;
    }
    return false;
}

struct Ink {
    lv_obj_t *obj;
    lv_area_t a;
    std::string text;
};

std::string area_text(const lv_area_t &a)
{
    char b[64];
    std::snprintf(b, sizeof b, "(%d,%d %dx%d)", static_cast<int>(a.x1), static_cast<int>(a.y1),
                  static_cast<int>(a.x2 - a.x1 + 1), static_cast<int>(a.y2 - a.y1 + 1));
    return b;
}

void walk(lv_obj_t *o, int sw, int sh, const std::vector<lv_obj_t *> &narrow_ok, std::vector<std::string> &out,
          std::vector<Ink> &inks, CheckStats &st, bool &has_target_below)
{
    has_target_below = false;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    bool scroll_clip = false;
    const lv_area_t vis = clipped(o, &scroll_clip);
    if (vis.x2 < vis.x1 || vis.y2 < vis.y1) return; /* scrolled away */
    lv_area_t c;
    lv_obj_get_coords(o, &c);

    bool below = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        bool b = false;
        walk(lv_obj_get_child(o, static_cast<int32_t>(i)), sw, sh, narrow_ok, out, inks, st, b);
        below |= b;
    }

    const bool inside_screen = c.x1 >= 0 && c.y1 >= 0 && c.x2 < sw && c.y2 < sh;
    if (!inside_screen && !scroll_clip) out.push_back("screen: an object " + area_text(c) + " leaves the screen");

    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *t = lv_label_get_text(o);
        if (t && *t) {
            ++st.labels;
            const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
            const int ls = lv_obj_get_style_text_letter_space(o, LV_PART_MAIN);
            const int cw = lv_obj_get_content_width(o);
            const lv_label_long_mode_t mode = lv_label_get_long_mode(o);
            const int tw = text_width(f, t, ls);
            const int lh = lv_font_get_line_height(f);
            lv_area_t ink = c;
            if (mode == LV_LABEL_LONG_MODE_WRAP) {
                lv_point_t sz;
                lv_text_get_size(&sz, t, f, ls, 0, cw, LV_TEXT_FLAG_NONE);
                if (sz.y > lv_obj_get_content_height(o) + 1)
                    out.push_back(std::string("text: \"") + t + "\" needs " + std::to_string(sz.y) + " px high, has " +
                                  std::to_string(lv_obj_get_content_height(o)));
                ink.y2 = ink.y1 + sz.y - 1;
            } else {
                if (tw > cw + 1)
                    out.push_back(std::string("text: \"") + t + "\" needs " + std::to_string(tw) + " px, has " +
                                  std::to_string(cw));
                const lv_text_align_t al = lv_obj_get_style_text_align(o, LV_PART_MAIN);
                const int w = std::min(tw, cw);
                if (al == LV_TEXT_ALIGN_CENTER) ink.x1 = c.x1 + (cw - w) / 2;
                else if (al == LV_TEXT_ALIGN_RIGHT) ink.x1 = c.x2 - w + 1;
                ink.x2 = ink.x1 + w - 1;
                ink.y2 = ink.y1 + lh - 1;
            }
            /* the glyphs' box (the line minus a little of its leading) must not be cut by a non-scrolling parent */
            lv_area_t core = ink;
            core.y1 += lh / 6;
            core.y2 -= lh / 6;
            lv_area_t r;
            if (!scroll_clip && (!inter(core, vis, &r) || r.x1 != core.x1 || r.x2 != core.x2 || r.y1 != core.y1 || r.y2 != core.y2))
                out.push_back(std::string("text: \"") + t + "\" is cut by its parent at " + area_text(vis));
            if (!scroll_clip) inks.push_back({o, core, t});
        }
    }

    const bool target = listens(o);
    if (target && !below) {
        ++st.targets;
        const int w = vis.x2 - vis.x1 + 1, h = vis.y2 - vis.y1 + 1;
        const int fw = c.x2 - c.x1 + 1, fh = c.y2 - c.y1 + 1;
        lv_obj_t *parent = lv_obj_get_parent(o);
        const bool in_list = parent && lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        const bool narrow = std::find(narrow_ok.begin(), narrow_ok.end(), o) != narrow_ok.end();
        if (!scroll_clip || (w == fw && h == fh)) {
            st.min_target = std::min(st.min_target, std::min(fw, fh));
            if (in_list) {
                if (fh < 40) out.push_back("target: list row " + area_text(c) + " under 40 px");
            } else if (narrow) {
                if (fw < 40 || fh < 44) out.push_back("target: " + area_text(c) + " under 40 x 44 px");
            } else if (fw < 44 || fh < 44) {
                out.push_back("target: " + area_text(c) + " under 44 px");
            }
        }
    }
    has_target_below = below || target;
}

} // namespace

std::vector<std::string> check_layout(lv_obj_t *root, int sw, int sh, const std::vector<lv_obj_t *> &narrow_ok, CheckStats *stats)
{
    std::vector<std::string> out;
    CheckStats st;
    if (!root || hidden_up(root)) return out;
    lv_obj_update_layout(lv_obj_get_screen(root));
    std::vector<Ink> inks;
    bool b = false;
    walk(root, sw, sh, narrow_ok, out, inks, st, b);
    for (std::size_t i = 0; i < inks.size(); ++i)
        for (std::size_t j = i + 1; j < inks.size(); ++j)
            if (inter(inks[i].a, inks[j].a, nullptr))
                out.push_back("overlap: \"" + inks[i].text + "\" " + area_text(inks[i].a) + " and \"" + inks[j].text + "\" " +
                              area_text(inks[j].a));
    if (stats) *stats = st;
    return out;
}

} // namespace ui
} // namespace cassette
