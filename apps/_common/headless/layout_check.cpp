/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness: the layout checks (see headless.hpp check_layout()). They read the LVGL object tree the app built,
 * so they hold for any app made of cp0_lvgl widgets, at any size.
 */
#include "headless.hpp"

#include "cp0_ui_metrics.h"
#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_osk.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/src/core/lv_obj_class_private.h" /* the class name, for the reports */
#include "lvgl/src/misc/lv_event_private.h"     /* the event filters: what a press does */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

namespace headless {
namespace {

bool is_root(lv_obj_t *obj)
{
    return obj && lv_obj_get_parent(obj) == nullptr; /* a screen or a display layer */
}

bool empty(const lv_area_t &a) { return a.x2 < a.x1 || a.y2 < a.y1; }

bool intersect(const lv_area_t &a, const lv_area_t &b, lv_area_t *out)
{
    lv_area_t r;
    r.x1 = std::max(a.x1, b.x1);
    r.y1 = std::max(a.y1, b.y1);
    r.x2 = std::min(a.x2, b.x2);
    r.y2 = std::min(a.y2, b.y2);
    if (out) *out = r;
    return !empty(r);
}

std::string area_text(const lv_area_t &a)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d@(%d,%d)", a.x2 - a.x1 + 1, a.y2 - a.y1 + 1, static_cast<int>(a.x1),
                  static_cast<int>(a.y1));
    return buf;
}

/* coords clipped by every ancestor below the root (the screen edge is not applied) */
lv_area_t clipped_by_ancestors(lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    for (lv_obj_t *p = lv_obj_get_parent(obj); p && !is_root(p); p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) continue;
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (!intersect(a, pa, &a)) {
            a.x2 = a.x1 - 1;
            break;
        }
    }
    return a;
}

bool hidden(lv_obj_t *obj)
{
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(obj, LV_PART_MAIN) == LV_OPA_TRANSP;
}

/* A short description: the first label text inside, else the class name. */
std::string describe(lv_obj_t *obj)
{
    std::string text;
    std::function<void(lv_obj_t *)> find = [&](lv_obj_t *o) {
        if (!text.empty()) return;
        if (lv_obj_check_type(o, &lv_label_class)) {
            const char *t = lv_label_get_text(o);
            if (t && t[0]) text = t;
            return;
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) find(lv_obj_get_child(o, static_cast<int32_t>(i)));
    };
    find(obj);
    if (text.size() > 28) text = text.substr(0, 25) + "...";
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    const char *name = cls && cls->name ? cls->name : "obj";
    return std::string(name) + (text.empty() ? "" : " \"" + text + "\"");
}

struct Leaf {
    lv_obj_t *obj;
    lv_area_t coords;
    lv_area_t visible;
    bool osk;
};

struct Walk {
    int w = 0, h = 0;
    std::vector<Leaf> leaves;
    std::vector<std::string> cut;
};

/* Something to press: clickable and either a pressable widget class or listening to a press / click (a scroll area
 * that is only clickable to be dragged, a dialog's text viewport, is not). */
bool interactive(lv_obj_t *obj)
{
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) return false;
    if (lv_obj_check_type(obj, &lv_button_class) || lv_obj_check_type(obj, &lv_switch_class) ||
        lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_checkbox_class))
        return true;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        const lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, i);
        if (!dsc) continue;
        const uint32_t code = dsc->filter & ~static_cast<uint32_t>(LV_EVENT_PREPROCESS);
        if (code == LV_EVENT_ALL || code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED ||
            code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_LONG_PRESSED ||
            code == LV_EVENT_LONG_PRESSED_REPEAT || code == LV_EVENT_SINGLE_CLICKED)
            return true;
    }
    return false;
}

/* Returns true when obj or a descendant is a visible interactive object. */
bool visit(Walk &walk, lv_obj_t *obj, bool osk)
{
    if (hidden(obj)) return false;
    const bool root = is_root(obj);
    lv_area_t vis = root ? lv_area_t{0, 0, static_cast<int32_t>(walk.w - 1), static_cast<int32_t>(walk.h - 1)}
                         : clipped_by_ancestors(obj);
    if (empty(vis)) return false;
    if (!root) {
        const lv_area_t screen{0, 0, static_cast<int32_t>(walk.w - 1), static_cast<int32_t>(walk.h - 1)};
        lv_area_t inside;
        if (!intersect(vis, screen, &inside)) return false; /* wholly off the screen (scrolled away, parked) */
        if (inside.x1 != vis.x1 || inside.y1 != vis.y1 || inside.x2 != vis.x2 || inside.y2 != vis.y2)
            walk.cut.push_back(describe(obj) + " " + area_text(vis) + " is cut by the screen edge");
        vis = inside;
    }
    bool clickable_below = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        clickable_below |= visit(walk, lv_obj_get_child(obj, static_cast<int32_t>(i)), osk);
    const bool clickable = !root && interactive(obj);
    if (clickable && !clickable_below) {
        Leaf leaf{obj, {}, vis, osk};
        lv_obj_get_coords(obj, &leaf.coords);
        walk.leaves.push_back(leaf);
    }
    return clickable || clickable_below;
}

lv_obj_t *find_label_in(lv_obj_t *obj, const std::string &text)
{
    if (!obj || hidden(obj)) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        if (t && text == t && !empty(visible_area(obj))) return obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (lv_obj_t *found = find_label_in(lv_obj_get_child(obj, static_cast<int32_t>(i)), text)) return found;
    return nullptr;
}

} // namespace

lv_area_t visible_area(lv_obj_t *obj)
{
    lv_area_t a = clipped_by_ancestors(obj);
    const Platform &p = platform();
    const lv_area_t screen{0, 0, static_cast<int32_t>(p.w - 1), static_cast<int32_t>(p.h - 1)};
    if (!intersect(a, screen, &a)) a.x2 = a.x1 - 1;
    return a;
}

lv_obj_t *find_label(const std::string &text, bool osk)
{
    lv_display_t *d = platform().display;
    lv_obj_t *screen = lv_display_get_screen_active(d);
    lv_obj_t *top = lv_display_get_layer_top(d);
    lv_obj_t *first = osk ? top : screen, *second = osk ? screen : top;
    if (lv_obj_t *found = find_label_in(first, text)) return found;
    return find_label_in(second, text);
}

lv_obj_t *clickable_of(lv_obj_t *obj)
{
    for (lv_obj_t *o = obj; o && !is_root(o); o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return o;
    return obj;
}

CheckResult check_layout(bool notice_expected)
{
    CheckResult result;
    const Platform &p = platform();
    lv_obj_update_layout(lv_display_get_screen_active(p.display));
    lv_obj_update_layout(lv_display_get_layer_top(p.display));
    Walk walk;
    walk.w = p.w;
    walk.h = p.h;
    visit(walk, lv_display_get_screen_active(p.display), false);
    visit(walk, lv_display_get_layer_top(p.display), true);
    result.interactive = static_cast<int>(walk.leaves.size());
    for (const std::string &c : walk.cut) result.failures.push_back("on-screen: " + c);

    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page_t page{};
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &page);
    const int row = m->tok[CP0_TOK_ROW_H], target = m->tok[CP0_TOK_TARGET];
    // a row of a scrolling list may be the LIST_ROW token: cp0::ui::CompactList's compact density (cp0_ui_list.h), under
    // the ROW rule on purpose, because a tap there only moves the highlight (activation needs a second tap)
    const int list_row = std::min(row, m->tok[CP0_TOK_LIST_ROW_H]);
    const int header_h = page.header_h;
    const int header_rule = std::min(target, header_h - 2 * m->shell.status_top);

    if (notice_expected) {
        if (walk.leaves.size() != 1)
            result.failures.push_back("notice: " + std::to_string(walk.leaves.size()) +
                                      " things to press, expected only the notice");
        result.notes.push_back("notice screen");
        return result;
    }

    const bool osk_up = cp0::ui::osk::visible();
    lv_area_t osk_area{0, 0, -1, -1};
    int min_key_h = 1 << 20;
    int min_row = 1 << 20, min_button = 1 << 20, min_header = 1 << 20, min_list_row = 1 << 20;
    for (const Leaf &leaf : walk.leaves) {
        const int h = leaf.coords.y2 - leaf.coords.y1 + 1;
        const std::string what = describe(leaf.obj) + " " + area_text(leaf.coords);
        if (leaf.osk) {
            min_key_h = std::min(min_key_h, h);
            if (empty(osk_area)) osk_area = leaf.visible;
            else {
                osk_area.x1 = std::min(osk_area.x1, leaf.visible.x1);
                osk_area.y1 = std::min(osk_area.y1, leaf.visible.y1);
                osk_area.x2 = std::max(osk_area.x2, leaf.visible.x2);
                osk_area.y2 = std::max(osk_area.y2, leaf.visible.y2);
            }
            if (h < CP0_UI_OSK_KEY_MIN_PX)
                result.failures.push_back("size: key " + what + " is under " + std::to_string(CP0_UI_OSK_KEY_MIN_PX) + " px");
            continue;
        }
        const bool in_header = leaf.coords.y2 < header_h;
        if (in_header) {
            min_header = std::min(min_header, h);
            if (h < header_rule)
                result.failures.push_back("size: header " + what + " is under " + std::to_string(header_rule) + " px");
            continue;
        }
        if (leaf.visible.y1 < header_h)
            result.failures.push_back("header: " + what + " reaches under the header (" + std::to_string(header_h) +
                                      " px)");
        if (lv_obj_check_type(leaf.obj, &lv_button_class)) {
            min_button = std::min(min_button, h);
            if (h < target)
                result.failures.push_back("size: button " + what + " is under TARGET " + std::to_string(target) + " px");
        } else {
            lv_obj_t *parent = lv_obj_get_parent(leaf.obj);
            const bool in_list = parent && lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE) &&
                                 (lv_obj_get_scroll_dir(parent) & LV_DIR_VER);
            if (in_list && h < row) {
                min_list_row = std::min(min_list_row, h);
                if (h < list_row)
                    result.failures.push_back("size: list row " + what + " is under LIST_ROW " + std::to_string(list_row) +
                                              " px");
            } else {
                min_row = std::min(min_row, h);
                if (h < row) result.failures.push_back("size: " + what + " is under ROW " + std::to_string(row) + " px");
            }
        }
    }
    if (osk_up) {
        lv_obj_t *owner = cp0::ui::osk::owner();
        if (owner) {
            const lv_area_t v = visible_area(owner);
            if (empty(v)) result.failures.push_back("keyboard: the field being edited is not visible");
            else if (!empty(osk_area) && v.y2 >= osk_area.y1)
                result.failures.push_back("keyboard: the field being edited " + area_text(v) + " reaches under it (" +
                                          std::to_string(osk_area.y1) + ")");
        }
        for (const Leaf &leaf : walk.leaves) {
            if (leaf.osk || empty(osk_area)) continue;
            if (intersect(leaf.visible, osk_area, nullptr))
                result.failures.push_back("keyboard: " + describe(leaf.obj) + " " + area_text(leaf.visible) +
                                          " is under the on-screen keyboard");
        }
    }
    // the writing zone's band (report 044) covers the rows from its strip while it shows: the same rule as the keyboard
    const bool band_up = band_shown();
    const int band_y = band_strip_y();
    if (band_up) {
        lv_obj_t *owner = cp0::ui::osk::owner();
        if (owner) {
            const lv_area_t v = visible_area(owner);
            if (empty(v)) result.failures.push_back("band: the field being edited is not visible");
            else if (v.y2 >= band_y)
                result.failures.push_back("band: the field being edited " + area_text(v) + " reaches under it (" +
                                          std::to_string(band_y) + ")");
        }
        for (const Leaf &leaf : walk.leaves)
            if (!leaf.osk && leaf.visible.y2 >= band_y)
                result.failures.push_back("band: " + describe(leaf.obj) + " " + area_text(leaf.visible) +
                                          " is under the writing zone's band (" + std::to_string(band_y) + ")");
    }
    char note[256];
    std::snprintf(note, sizeof(note),
                  "%d interactive; rows/fields min %d (ROW %d), buttons min %d (TARGET %d), header min %d (rule %d), "
                  "keyboard %s",
                  result.interactive, min_row == (1 << 20) ? 0 : min_row, row, min_button == (1 << 20) ? 0 : min_button,
                  target, min_header == (1 << 20) ? 0 : min_header, header_rule, osk_up ? "up" : "off");
    result.notes.push_back(note);
    if (min_list_row != (1 << 20)) {
        std::snprintf(note, sizeof(note), "compact list rows min %d (LIST_ROW %d)", min_list_row, list_row);
        result.notes.push_back(note);
    }
    std::snprintf(note, sizeof(note), "header %d px, strip from x %d, Esc %dx%d@(%d,%d)", header_h, page.strip_left,
                  page.esc_w, page.esc_h, page.esc_x, page.esc_y);
    result.notes.push_back(note);
    if (osk_up) {
        std::snprintf(note, sizeof(note), "keyboard %s, key height min %d", area_text(osk_area).c_str(),
                      min_key_h == (1 << 20) ? 0 : min_key_h);
        result.notes.push_back(note);
    }
    if (band_on()) {
        std::snprintf(note, sizeof(note), "band %s (from row %d), inset %d", band_up ? "shown" : "hidden", band_y,
                      cp0::ui::osk::bottom_inset());
        result.notes.push_back(note);
    }
    return result;
}

} // namespace headless
