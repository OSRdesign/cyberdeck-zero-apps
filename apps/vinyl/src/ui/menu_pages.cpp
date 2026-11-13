/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the Menu pages (see menu_pages.hpp). Every position comes from core/layout menu_geom (the canvas boards'
 * numbers through the class's scale); paddings, radii and text sizes are the boards' own, scaled.
 */
#include "menu_pages.hpp"

#include "gfx.hpp"
#include "theme.hpp"

#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vinyl {
namespace ui {

using layout::R;

namespace {

int ri(double v) { return static_cast<int>(std::lround(v)); }

constexpr std::uint32_t kKeyChipBorder = 0x444444; /* Keys-480x320: the key chip's border */

struct Img {
    PixelImage px;
    lv_image_dsc_t dsc{};
    void bind(lv_obj_t *obj) {
        std::memset(&dsc, 0, sizeof dsc);
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = px.rgb565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
        dsc.header.w = static_cast<uint32_t>(px.w);
        dsc.header.h = static_cast<uint32_t>(px.h);
        dsc.header.stride = static_cast<uint32_t>(px.w * (px.rgb565 ? 2 : 4));
        dsc.data_size = static_cast<uint32_t>(px.px.size());
        dsc.data = px.px.data();
        lv_image_cache_drop(&dsc);
        lv_image_set_src(obj, &dsc);
    }
};

/* text in lines no wider than max_w, broken only after ": " and ", " (the pieces kept whole) */
std::string break_after(const std::string &text, const lv_font_t *f, int max_w) {
    std::vector<std::string> pieces;
    size_t start = 0;
    for (size_t i = 0; i + 1 < text.size(); ++i)
        if ((text[i] == ':' || text[i] == ',') && text[i + 1] == ' ') {
            pieces.push_back(text.substr(start, i + 1 - start));
            start = i + 2;
        }
    pieces.push_back(text.substr(start));
    std::string out, line;
    for (const std::string &p : pieces) {
        const std::string cand = line.empty() ? p : line + " " + p;
        if (!line.empty() && text_width(f, cand) > max_w) {
            out += (out.empty() ? "" : "\n") + line;
            line = p;
        } else {
            line = cand;
        }
    }
    if (!line.empty()) out += (out.empty() ? "" : "\n") + line;
    return out;
}

lv_obj_t *scroller(lv_obj_t *parent, const R &r, double s) {
    lv_obj_t *c = lv_obj_create(parent);
    plain(c);
    lv_obj_set_pos(c, ri(r.x), ri(r.y));
    lv_obj_set_size(c, std::max(1, ri(r.w)), std::max(1, ri(r.h)));
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(c, std::max(2, ri(3 * s)), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(c, lv_color_hex(col::kRowValue), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(c, LV_OPA_50, LV_PART_SCROLLBAR);
    return c;
}

}  // namespace

struct MenuPages::Impl {
    std::function<lv_obj_t *()> make_root;
    int W = 0, H = 0, body_y = 0, list_row = 24;
    std::function<void(Page, int)> on_row;

    struct RowObjs {
        lv_obj_t *row = nullptr, *text = nullptr, *value = nullptr, *chev = nullptr, *dot = nullptr;
        int y = 0, h = 0;
        menu::Row::Kind kind = menu::Row::Kind::Open;
    };
    struct ListPage {
        Page page = Page::Menu;
        layout::MenuGeom g;
        lv_obj_t *root = nullptr, *panel = nullptr, *heading = nullptr, *cont = nullptr;
        lv_obj_t *strip = nullptr, *strip_icon = nullptr, *strip_text = nullptr;
        std::vector<RowObjs> objs;
        std::vector<menu::Row> rows;
        int sel = -2;
        uint64_t gen = UINT64_MAX;
        bool value_bright = false;
        std::string hint;
        bool clock = false;
    };
    ListPage lists[4]; /* Menu, Sleep, Playback, ReplayGain */

    /* Keys */
    layout::MenuGeom kg;
    lv_obj_t *keys_root = nullptr, *keys_cont = nullptr;
    int keys_count = 0;
    /* About */
    layout::MenuGeom ag;
    lv_obj_t *about_root = nullptr, *about_cont = nullptr;
    lv_obj_t *a_disc = nullptr, *a_name = nullptr, *a_line = nullptr;
    lv_obj_t *a_sec[4] = {}, *a_text[4] = {}, *a_foot = nullptr;
    Img a_disc_img, a_label_img;

    static int list_index(Page p) {
        switch (p) {
        case Page::Menu: return 0;
        case Page::Sleep: return 1;
        case Page::Playback: return 2;
        case Page::ReplayGain: return 3;
        default: return -1;
        }
    }

    /* ------------------------------------------------------------------------------------------------ list pages */

    void build_list(ListPage &L, Page p) {
        L.page = p;
        const bool heading = p == Page::Sleep || p == Page::ReplayGain;
        const bool strip = p != Page::Menu;
        L.g = layout::menu_geom(W, H, body_y, p == Page::Playback ? 40 : 34, heading, strip, list_row);
        const layout::MenuGeom &g = L.g;
        const double s = g.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        L.root = make_root();
        L.panel = box(L.root, ri(g.panel.x), ri(g.panel.y), ri(g.panel.w), ri(g.panel.h), col::kList, S(10));
        if (heading) {
            L.heading = label(L.root, Face::Bold, S(12), col::kRowValue, ri(g.heading.x), ri(g.heading.y), ri(g.heading.w),
                              LV_TEXT_ALIGN_LEFT, ri(0.04 * 12 * s));
            const lv_font_t *hf = font(Face::Bold, S(12));
            lv_obj_set_y(L.heading, ri(g.heading.y) + (ri(g.heading.h) - lv_font_get_line_height(hf)) / 2);
        }
        L.cont = scroller(L.root, g.list, s);
        if (strip) {
            const int sx = ri(g.strip.x), sy = ri(g.strip.y), sw = ri(g.strip.w), shh = ri(g.strip.h);
            L.strip = box(L.root, sx, sy, sw, shh, col::kCream, S(8));
            const int ip = S(20);
            L.strip_icon = icon_obj(L.strip, Icon::Info, S(12), (shh - ip) / 2, ip, ip, ip, col::kInk2);
            const int px = p == Page::Sleep ? S(13) : S(12);
            L.strip_text = label(L.strip, Face::Bold, px, col::kInk2, S(12) + ip + S(10), 0, sw - S(24) - ip - S(10));
            lv_label_set_long_mode(L.strip_text, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_height(L.strip_text, LV_SIZE_CONTENT);
            lv_obj_set_style_text_line_space(L.strip_text, S(16) - lv_font_get_line_height(font(Face::Bold, px)), 0);
            lv_obj_add_flag(L.strip, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(L.root, LV_OBJ_FLAG_HIDDEN);
    }

    static void row_click_cb(lv_event_t *e) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
        lv_obj_t *o = lv_event_get_target_obj(e);
        for (ListPage &L : self->lists)
            for (size_t i = 0; i < L.objs.size(); ++i)
                if (L.objs[i].row == o && L.objs[i].kind != menu::Row::Kind::Heading) {
                    if (self->on_row) self->on_row(L.page, static_cast<int>(i));
                    return;
                }
    }

    /* the rows' objects for the rows' kinds (rebuilt when the kinds change, else updated in place) */
    void layout_rows(ListPage &L) {
        bool same = L.objs.size() == L.rows.size();
        for (size_t i = 0; same && i < L.rows.size(); ++i) same = L.objs[i].kind == L.rows[i].kind;
        if (!same) {
            lv_obj_clean(L.cont);
            L.objs.clear();
            const layout::MenuGeom &g = L.g;
            const int w = ri(g.list.w);
            int y = 0;
            for (const menu::Row &r : L.rows) {
                RowObjs o;
                o.kind = r.kind;
                o.h = ri(r.kind == menu::Row::Kind::Heading ? g.section_h : g.row_h);
                o.y = y;
                o.row = lv_obj_create(L.cont);
                plain(o.row);
                lv_obj_set_pos(o.row, 0, y);
                lv_obj_set_size(o.row, w, o.h);
                lv_obj_set_style_bg_color(o.row, lv_color_hex(col::kRowSel), 0);
                lv_obj_set_style_bg_opa(o.row, LV_OPA_TRANSP, 0);
                if (r.kind == menu::Row::Kind::Heading) {
                    const int px = std::max(1, ri(g.section_px));
                    o.text = label(o.row, Face::Bold, px, col::kRowValue, ri(g.text_x), 0, 0, LV_TEXT_ALIGN_LEFT, ri(0.06 * g.section_px));
                    lv_obj_set_y(o.text, (o.h - lv_font_get_line_height(font(Face::Bold, px))) / 2);
                } else {
                    lv_obj_add_flag(o.row, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_add_flag(o.row, LV_OBJ_FLAG_EVENT_BUBBLE); /* a drag scrolls the list */
                    lv_obj_add_event_cb(o.row, row_click_cb, LV_EVENT_CLICKED, this);
                    const int tpx = std::max(1, ri(g.text_px));
                    o.text = label(o.row, Face::Bold, tpx, col::kRowText, 0, 0, 0);
                    lv_label_set_long_mode(o.text, LV_LABEL_LONG_MODE_WRAP); /* two lines in a narrow row, never dots */
                    lv_obj_set_style_text_line_space(o.text, 0, 0);
                    if (r.kind == menu::Row::Kind::Choice) {
                        const int dot = std::max(4, ri(10 * g.s));
                        o.dot = box(o.row, ri(g.dot_x), (o.h - dot) / 2, dot, dot, col::kWhite, LV_RADIUS_CIRCLE);
                    } else {
                        const int cw = std::max(8, ri(g.chev_w));
                        o.chev = icon_obj(o.row, Icon::Chevron, w - ri(g.chev_r) - cw, (o.h - cw) / 2, cw, cw, cw, col::kRowValue);
                        o.value = label(o.row, Face::Bold, std::max(1, ri(g.value_px)), col::kRowValue, 0, 0, 0, LV_TEXT_ALIGN_RIGHT);
                    }
                }
                L.objs.push_back(o);
                y += o.h;
            }
        }
        const layout::MenuGeom &g = L.g;
        const int w = ri(g.list.w);
        for (size_t i = 0; i < L.rows.size(); ++i) {
            const menu::Row &r = L.rows[i];
            RowObjs &o = L.objs[i];
            set_text(o.text, r.text);
            if (r.kind == menu::Row::Kind::Heading) continue;
            const int text_x = ri(r.kind == menu::Row::Kind::Choice ? g.choice_text_x : g.text_x);
            int right = w - ri(g.text_x); /* where the text may end */
            if (o.dot) set_bg(o.dot, r.chosen ? col::kPlaying : col::kWhite);
            if (o.value) {
                const bool bright = L.value_bright;
                const int vpx = std::max(1, ri((bright ? 14 : 13) * g.s));
                const lv_font_t *vf = font(Face::Bold, vpx);
                if (lv_obj_get_style_text_font(o.value, LV_PART_MAIN) != vf) lv_obj_set_style_text_font(o.value, vf, 0);
                lv_obj_set_style_text_color(o.value, lv_color_hex(bright ? col::kRowText : col::kRowValue), 0);
                const int vw = r.value.empty() ? 0 : text_width(vf, r.value) + 2;
                const int vr = w - ri(g.value_r);
                show(o.value, vw > 0);
                lv_obj_set_size(o.value, std::max(1, vw), lv_font_get_line_height(vf));
                lv_obj_set_pos(o.value, vr - vw, (o.h - lv_font_get_line_height(vf)) / 2);
                set_text(o.value, r.value);
                right = (vw > 0 ? vr - vw : w - ri(g.chev_r) - ri(g.chev_w)) - ri(8 * g.s);
            }
            /* the text: one line, or two in a narrow row (centred in the row) */
            lv_obj_set_width(o.text, std::max(10, right - text_x));
            lv_obj_set_height(o.text, LV_SIZE_CONTENT);
            lv_obj_update_layout(o.text);
            lv_obj_set_pos(o.text, text_x, (o.h - lv_obj_get_height(o.text)) / 2);
        }
    }

    void select(ListPage &L, int sel, bool ensure_visible) {
        for (size_t i = 0; i < L.objs.size(); ++i)
            lv_obj_set_style_bg_opa(L.objs[i].row, static_cast<int>(i) == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        L.sel = sel;
        if (!ensure_visible || sel < 0 || sel >= static_cast<int>(L.objs.size())) return;
        const RowObjs &o = L.objs[static_cast<size_t>(sel)];
        const int top = lv_obj_get_scroll_y(L.cont), h = lv_obj_get_height(L.cont);
        /* a section heading just above the first item comes into view with it */
        int want_top = o.y;
        if (sel > 0 && L.objs[static_cast<size_t>(sel - 1)].kind == menu::Row::Kind::Heading) want_top = L.objs[static_cast<size_t>(sel - 1)].y;
        if (want_top < top) lv_obj_scroll_to_y(L.cont, want_top, LV_ANIM_OFF);
        else if (o.y + o.h > top + h) lv_obj_scroll_to_y(L.cont, o.y + o.h - h, LV_ANIM_OFF);
    }

    void update_list(ListPage &L, const MenuView &v) {
        const bool rows_changed = !(v.rows == L.rows) || v.value_bright != L.value_bright;
        L.value_bright = v.value_bright;
        if (rows_changed) {
            L.rows = v.rows;
            layout_rows(L);
        }
        if (L.heading) set_text(L.heading, v.heading);
        if (L.strip && (v.hint != L.hint || v.hint_clock != L.clock || L.gen == UINT64_MAX)) {
            L.hint = v.hint;
            L.clock = v.hint_clock;
            place_strip(L);
        }
        const bool reopened = v.gen != L.gen;
        L.gen = v.gen;
        if (reopened) lv_obj_scroll_to_y(L.cont, 0, LV_ANIM_OFF);
        if (rows_changed || reopened || v.selected != L.sel) select(L, v.selected, true);
    }

    /* the strip grows upwards to its text, the list gives up the height; no text, no strip (the list to the panel's
     * bottom) */
    void place_strip(ListPage &L) {
        const layout::MenuGeom &g = L.g;
        const double s = g.s;
        const int list_y = ri(g.list.y);
        if (L.hint.empty()) {
            lv_obj_add_flag(L.strip, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(L.cont, std::max(1, ri(g.panel.bottom() - 4 * s) - list_y));
            return;
        }
        lv_obj_remove_flag(L.strip, LV_OBJ_FLAG_HIDDEN);
        icon_set(L.strip_icon, L.clock ? Icon::Sleep : Icon::Info, col::kInk2);
        set_text(L.strip_text, L.hint);
        lv_obj_update_layout(L.strip_text);
        const int y0 = ri(g.strip.y), h0 = ri(g.strip.h);
        const int th = lv_obj_get_height(L.strip_text);
        const int h = std::max(h0, th + 2 * std::max(1, ri(8 * s)));
        const int y = y0 + h0 - h;
        lv_obj_set_y(L.strip, y);
        lv_obj_set_height(L.strip, h);
        lv_obj_set_y(L.strip_text, (h - th) / 2);
        lv_obj_set_y(L.strip_icon, (h - lv_obj_get_height(L.strip_icon)) / 2);
        lv_obj_set_height(L.cont, std::max(1, y - list_y));
    }

    /* ------------------------------------------------------------------------------------------------ Keys */

    void build_keys() {
        kg = layout::menu_geom(W, H, body_y, 32, false, false, list_row);
        const double s = kg.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        keys_root = make_root();
        box(keys_root, ri(kg.panel.x), ri(kg.panel.y), ri(kg.panel.w), ri(kg.panel.h), col::kList, S(10));
        keys_cont = scroller(keys_root, kg.list, s);
        lv_obj_add_flag(keys_root, LV_OBJ_FLAG_HIDDEN);
    }

    void set_keys(const std::vector<menu::KeyHelp> &rows) {
        lv_obj_clean(keys_cont);
        const double s = kg.s;
        const int w = ri(kg.list.w), rh = ri(kg.key_row_h);
        const int cpx = std::max(1, ri(kg.chip_px)), apx = std::max(1, ri(kg.action_px));
        const lv_font_t *cf = font(Face::Mono, cpx);
        /* landscape: one column for the actions, right of the widest key chip (Keys-480x320: at 150) */
        int col_x = ri(kg.action_x);
        for (const menu::KeyHelp &k : rows)
            col_x = std::max(col_x, ri(kg.chip_x) + std::max(ri(kg.chip_min_w), text_width(cf, k.keys) + 2 * ri(kg.chip_pad) + 2) +
                                        ri(8 * s));
        int y = 0;
        for (const menu::KeyHelp &k : rows) {
            lv_obj_t *row = lv_obj_create(keys_cont);
            plain(row);
            lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_set_pos(row, 0, y);
            lv_obj_set_size(row, w, rh);
            const int pad = ri(kg.chip_pad), ch = ri(kg.chip_h);
            const int cw = std::max(ri(kg.chip_min_w), text_width(cf, k.keys) + 2 * pad + 2);
            lv_obj_t *chip = box(row, ri(kg.chip_x), ri(kg.chip_top), cw, ch, col::kRowSel, std::max(1, ri(5 * s)));
            lv_obj_set_style_border_color(chip, lv_color_hex(kKeyChipBorder), 0);
            lv_obj_set_style_border_width(chip, std::max(1, ri(s)), 0);
            lv_obj_t *kt = label(chip, Face::Mono, cpx, col::kWhite, 0, 0, cw, LV_TEXT_ALIGN_CENTER);
            lv_obj_set_y(kt, (ch - lv_font_get_line_height(cf)) / 2 - std::max(1, ri(s)));
            set_text(kt, k.keys);
            /* the action: right of the chip (landscape), under it (portrait); its column's width, two lines at most */
            const int ax = kg.keys_two_lines ? ri(kg.action_x) : col_x;
            const int aw = std::max(10, w - ax - ri(12 * s));
            lv_obj_t *at = label(row, Face::Bold, apx, col::kRowText, ax, 0, 0);
            lv_label_set_long_mode(at, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_width(at, aw);
            lv_obj_set_height(at, LV_SIZE_CONTENT);
            set_text(at, k.action);
            lv_obj_update_layout(at);
            const int ah = lv_obj_get_height(at);
            const int ay = kg.keys_two_lines ? ri(kg.action_top) + (ri(kg.action_lh) - lv_font_get_line_height(font(Face::Bold, apx))) / 2
                                             : (rh - ah) / 2;
            lv_obj_set_y(at, ay);
            y += rh;
        }
        keys_count = static_cast<int>(rows.size());
    }

    /* ------------------------------------------------------------------------------------------------ About */

    void build_about() {
        ag = layout::menu_geom(W, H, body_y, 34, false, false, list_row);
        const double s = ag.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        about_root = make_root();
        box(about_root, ri(ag.panel.x), ri(ag.panel.y), ri(ag.panel.w), ri(ag.panel.h), col::kList, S(10));
        R area{ag.panel.x, ag.panel.y + S(4), ag.panel.w, ag.panel.h - S(8)};
        about_cont = scroller(about_root, area, s);
        const int dd = std::max(8, ri(ag.about_disc));
        a_disc_img.px = render_disc(dd, s * 0.5);
        a_disc = lv_image_create(about_cont);
        lv_obj_remove_flag(a_disc, LV_OBJ_FLAG_CLICKABLE);
        a_disc_img.bind(a_disc);
        const int ld = std::max(4, ri(14 * s));
        a_label_img.px = render_label(ld, 0, s);
        lv_obj_t *lab = lv_image_create(a_disc);
        lv_obj_remove_flag(lab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(lab, (dd - ld) / 2, (dd - ld) / 2);
        a_label_img.bind(lab);
        a_name = label(about_cont, Face::Bold, std::max(1, ri(ag.about_name_px)), col::kWhite, 0, 0, 0);
        auto wrap = [&](lv_obj_t *l) {
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_height(l, LV_SIZE_CONTENT);
        };
        a_line = label(about_cont, Face::Bold, std::max(1, ri(ag.about_line_px)), col::kRowValue, 0, 0, 0);
        wrap(a_line);
        static const char *const secs[4] = {"FORMATS", "THIS OUTPUT", "LIBRARIES", ""};
        for (int i = 0; i < 4; ++i) {
            if (secs[i][0]) {
                a_sec[i] = label(about_cont, Face::Bold, std::max(1, ri(ag.about_sec_px)), col::kRowValue, 0, 0, 0, LV_TEXT_ALIGN_LEFT,
                                 ri(0.06 * ag.about_sec_px));
                set_text(a_sec[i], secs[i]);
            }
            a_text[i] = label(about_cont, Face::Bold, std::max(1, ri(ag.about_text_px)), col::kRowText, 0, 0, 0);
            wrap(a_text[i]);
        }
        a_foot = label(about_cont, Face::Bold, std::max(1, ri(12 * s)), col::kRowValue, 0, 0, 0);
        wrap(a_foot);
        lv_obj_add_flag(about_root, LV_OBJ_FLAG_HIDDEN);
    }

    void set_about(const menu::About &a) {
        const double s = ag.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        const int w = lv_obj_get_width(about_cont), pad = ri(ag.about_pad);
        const int dd = std::max(8, ri(ag.about_disc));
        lv_obj_set_pos(a_disc, pad, S(8));
        const int nx = pad + dd + S(12);
        set_text(a_name, a.name);
        lv_obj_set_pos(a_name, nx, S(6));
        lv_obj_update_layout(a_name);
        set_text(a_line, a.line);
        lv_obj_set_width(a_line, std::max(10, w - nx - pad));
        lv_obj_set_pos(a_line, nx, S(6) + lv_obj_get_height(a_name));
        lv_obj_update_layout(a_line);
        int y = std::max(S(8) + dd, lv_obj_get_y(a_line) + lv_obj_get_height(a_line)) + S(10);
        /* THIS OUTPUT holds decimals ("2.07 dB"): LVGL may break a line after a '.', so the line is broken here, after
         * its ": " and ", " only, where it does not fit */
        const std::string out = break_after(a.output, font(Face::Bold, std::max(1, ri(ag.about_text_px))), std::max(10, w - 2 * pad));
        const std::string texts[4] = {a.formats, out, a.libraries, ""};
        for (int i = 0; i < 3; ++i) {
            lv_obj_set_pos(a_sec[i], pad, y);
            lv_obj_update_layout(a_sec[i]);
            y += lv_obj_get_height(a_sec[i]);
            set_text(a_text[i], texts[i]);
            lv_obj_set_width(a_text[i], std::max(10, w - 2 * pad));
            lv_obj_set_pos(a_text[i], pad, y);
            lv_obj_update_layout(a_text[i]);
            y += lv_obj_get_height(a_text[i]) + S(8);
        }
        show(a_text[3], false);
        set_text(a_foot, a.licences);
        lv_obj_set_width(a_foot, std::max(10, w - 2 * pad));
        lv_obj_set_pos(a_foot, pad, y + S(2));
    }
};

MenuPages::MenuPages(std::function<lv_obj_t *()> make_root, int screen_w, int screen_h, int body_y, int list_row,
                     std::function<void(Page, int)> on_row)
    : d_(std::make_unique<Impl>()) {
    Impl &d = *d_;
    d.make_root = std::move(make_root);
    d.W = screen_w;
    d.H = screen_h;
    d.body_y = body_y;
    d.list_row = list_row;
    d.on_row = std::move(on_row);
    d.build_list(d.lists[0], Page::Menu);
    d.build_list(d.lists[1], Page::Sleep);
    d.build_list(d.lists[2], Page::Playback);
    d.build_list(d.lists[3], Page::ReplayGain);
    d.build_keys();
    d.build_about();
}

MenuPages::~MenuPages() = default;

bool MenuPages::is_menu_page(Page p) {
    return p == Page::Menu || p == Page::Sleep || p == Page::Playback || p == Page::ReplayGain || p == Page::Keys ||
           p == Page::About;
}

lv_obj_t *MenuPages::root(Page p) const {
    const int i = Impl::list_index(p);
    if (i >= 0) return d_->lists[i].root;
    if (p == Page::Keys) return d_->keys_root;
    if (p == Page::About) return d_->about_root;
    return nullptr;
}

void MenuPages::update(Page p, const MenuView &v) {
    const int i = Impl::list_index(p);
    if (i >= 0) d_->update_list(d_->lists[i], v);
}

void MenuPages::set_keys(const std::vector<menu::KeyHelp> &rows) { d_->set_keys(rows); }
void MenuPages::set_about(const menu::About &a) { d_->set_about(a); }

void MenuPages::scroll(Page p, int rows) {
    lv_obj_t *c = p == Page::Keys ? d_->keys_cont : p == Page::About ? d_->about_cont : nullptr;
    if (!c) return;
    const int step = p == Page::Keys ? std::max(1, ri(d_->kg.key_row_h)) : std::max(1, ri(d_->ag.row_h));
    lv_obj_update_layout(c);
    const int max_y = std::max(0, static_cast<int>(lv_obj_get_scroll_bottom(c)) + lv_obj_get_scroll_y(c));
    const int y = std::clamp(lv_obj_get_scroll_y(c) + rows * step, 0, max_y);
    lv_obj_scroll_to_y(c, y, LV_ANIM_OFF);
}

layout::R MenuPages::notice_room(Page p) const {
    const int i = Impl::list_index(p);
    const layout::MenuGeom &g = i >= 0 ? d_->lists[i].g : p == Page::Keys ? d_->kg : d_->ag;
    const double h = std::round(42 * g.s), m8 = std::round(8 * g.s);
    return R{g.panel.x + m8, g.panel.bottom() - m8 - h, g.panel.w - 2 * m8, h};
}

int MenuPages::rows(Page p) const {
    const int i = Impl::list_index(p);
    if (i >= 0) return static_cast<int>(d_->lists[i].rows.size());
    if (p == Page::Keys) return d_->keys_count;
    return 0;
}

}  // namespace ui
}  // namespace vinyl
