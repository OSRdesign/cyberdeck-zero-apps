/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the Library pages (see library_pages.hpp). Every position comes from core/layout list_geom (the canvas boards'
 * numbers through the class's scale, as the Browser's); paddings, radii and text sizes are the boards' own, scaled.
 */
#include "library_pages.hpp"

#include "gfx.hpp"
#include "row_list.hpp"
#include "theme.hpp"

#include "cp0_ui_widgets.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vinyl {
namespace ui {

using layout::R;

namespace {

int ri(double v) { return static_cast<int>(std::lround(v)); }

struct HitData {
    std::function<void()> fn;
};
void hit_click_cb(lv_event_t *e) {
    auto *d = static_cast<HitData *>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
    if (d && d->fn) d->fn();
}
void hit_delete_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    delete static_cast<HitData *>(lv_obj_get_user_data(o));
    lv_obj_set_user_data(o, nullptr);
}

void wrap_label(lv_obj_t *l, int w) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, std::max(10, w));
    lv_obj_set_height(l, LV_SIZE_CONTENT);
}

}  // namespace

/* ------------------------------------------------------------------------------------------------ shared pieces */

lv_obj_t *lib_hit(lv_obj_t *parent, int x, int y, int w, int h, std::function<void()> fn) {
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, std::max(1, w), std::max(1, h));
    lv_obj_set_user_data(o, new HitData{std::move(fn)});
    lv_obj_add_event_cb(o, hit_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(o, hit_delete_cb, LV_EVENT_DELETE, nullptr);
    return o;
}

void LibButton::build(lv_obj_t *parent, int x_, int y_, int w_, int h_, int px, uint32_t bg, uint32_t ink,
                      const std::string &text, int target, std::function<void()> on_tap) {
    x = x_;
    y = y_;
    w = w_;
    h = h_;
    vis = box(parent, x, y, w, h, bg, std::max(2, h / 5));
    const lv_font_t *f = font(Face::Bold, px);
    word = label(vis, Face::Bold, px, ink, 0, (h - lv_font_get_line_height(f)) / 2, w, LV_TEXT_ALIGN_CENTER);
    ui::set_text(word, text);
    const int hw = std::max(w, target), hh = std::max(h, target);
    hitbox = lib_hit(parent, x - (hw - w) / 2, y - (hh - h) / 2, hw, hh, std::move(on_tap));
}

void LibButton::set_text(const std::string &t) { ui::set_text(word, t); }

void LibButton::set_enabled(bool on) {
    if (!vis) return;
    lv_obj_set_style_opa(vis, on ? LV_OPA_COVER : LV_OPA_40, 0);
    if (on) lv_obj_add_flag(hitbox, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(hitbox, LV_OBJ_FLAG_CLICKABLE);
}

void LibButton::show(bool shown) {
    ui::show(vis, shown);
    ui::show(hitbox, shown);
}

void LibEmpty::build(lv_obj_t *parent, const R &panel, double s, int target, std::function<void()> on_button) {
    auto S = [&](double v) { return std::max(1, ri(v * s)); };
    const int px = ri(panel.x), pw = ri(panel.w);
    title = label(parent, Face::Bold, S(18), col::kRowText, px, ri(panel.y) + S(70), pw, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(title, std::max(10, pw - 2 * S(16)));
    lv_obj_set_x(title, px + S(16));
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    const int sw = std::min(S(360), pw - 2 * S(16));
    sub = label(parent, Face::Bold, S(13), col::kRowValue, px + (pw - sw) / 2, ri(panel.y) + S(104), 0, LV_TEXT_ALIGN_CENTER);
    wrap_label(sub, sw);
    lv_obj_set_style_text_line_space(sub, S(18) - lv_font_get_line_height(font(Face::Bold, S(13))), 0);
    const int bw = std::min(S(180), pw - 2 * S(16));
    bx = px + (pw - bw) / 2;
    by = ri(panel.y) + S(170);
    button.build(parent, bx, by, bw, S(30), S(13), col::kPrimary, col::kWhite, "", target, std::move(on_button));
    show(false);
}

void LibEmpty::set(const std::string &t, const std::string &sub_text, const std::string &button_text) {
    set_text(title, t);
    set_text(sub, sub_text);
    lv_obj_update_layout(title);
    lv_obj_update_layout(sub);
    const int by2 = lv_obj_get_y(sub) + lv_obj_get_height(sub) + std::max(8, lv_font_get_line_height(font(Face::Bold, 13)));
    const bool has_button = !button_text.empty();
    button.set_text(button_text);
    if (has_button) {
        lv_obj_set_y(button.vis, by2);
        const int hh = lv_obj_get_height(button.hitbox);
        lv_obj_set_y(button.hitbox, by2 - (hh - button.h) / 2);
    }
    if (shown_) button.show(has_button);
}

void LibEmpty::show(bool on) {
    shown_ = on;
    ui::show(title, on);
    ui::show(sub, on);
    button.show(on && lv_label_get_text(button.word) && *lv_label_get_text(button.word));
}

void LibNoteStrip::build(lv_obj_t *parent, int x_, int w_, double s_) {
    x = x_;
    w = w_;
    s = s_;
    auto S = [&](double v) { return std::max(1, ri(v * s)); };
    box = ui::box(parent, x, 0, w, S(44), col::kRowSel, S(8));
    edge = ui::box(box, 0, 0, S(4), S(44), col::kAmber, 0);
    text = label(box, Face::Bold, S(12), col::kRowText, S(12), S(5), 0);
    wrap_label(text, w - 2 * S(12));
    lv_obj_set_style_text_line_space(text, S(18) - lv_font_get_line_height(font(Face::Bold, S(12))), 0);
    bar = ui::box(box, S(12), S(30), w - 2 * S(12), S(6), col::kBarTrack, S(3));
    fill = ui::box(bar, 0, 0, 1, S(6), col::kAmber, S(3));
    show(false);
}

int LibNoteStrip::set(const LibNote &n) {
    auto S = [&](double v) { return std::max(1, ri(v * s)); };
    if (n.kind == LibNote::Kind::None) return 0;
    const bool scan = n.kind == LibNote::Kind::Scan;
    const int px = scan ? S(12) : S(13);
    lv_obj_set_style_text_font(text, font(Face::Bold, px), 0);
    wrap_label(text, w - 2 * S(12) - (scan ? 0 : S(4)));
    lv_obj_set_style_text_line_space(text, S(scan ? 18 : 19) - lv_font_get_line_height(font(Face::Bold, px)), 0);
    set_text(text, n.text);
    lv_obj_update_layout(text);
    const int th = lv_obj_get_height(text);
    int h = 0;
    if (scan) {
        const bool has_bar = n.pct >= 0;
        h = S(5) + th + (has_bar ? S(7) + S(6) + S(8) : S(8));
        ui::show(bar, has_bar);
        lv_obj_set_y(bar, S(5) + th + S(7));
        const int bw = lv_obj_get_width(bar);
        lv_obj_set_width(fill, std::max(1, bw * std::clamp(n.pct, 0, 100) / 100));
        set_bg(fill, n.hot ? col::kHot : col::kAmber);
        ui::show(edge, false);
        lv_obj_set_pos(text, S(12), S(5));
    } else {
        h = std::max(S(56), th + S(18));
        ui::show(bar, false);
        ui::show(edge, true);
        lv_obj_set_height(edge, h);
        lv_obj_set_pos(text, S(16), (h - th) / 2);
    }
    return h;
}

void LibNoteStrip::place(int y, int h) {
    lv_obj_set_pos(box, x, y);
    lv_obj_set_height(box, h);
}

void LibNoteStrip::show(bool on) { ui::show(box, on); }

/* ------------------------------------------------------------------------------------------------ the pages */

struct LibraryPages::Impl {
    std::function<lv_obj_t *()> make_root;
    cp0_ui_roller_page_t tk{};
    int W = 0, H = 0, body_y = 0, target = 44, list_row = 24;
    LibraryCallbacks cb;
    layout::ListGeom lg;
    double s = 1;

    struct P {
        Page page = Page::LibArtists;
        lv_obj_t *root = nullptr, *heading = nullptr;
        LibButton chip;
        bool has_chip = false;
        std::unique_ptr<RowList> list;
        int list_y = 0, list_h = 0;
        uint64_t rows_gen = UINT64_MAX, rows_ver = UINT64_MAX;
        int sel = -2, nrows = 0;
        LibEmpty empty;
        /* Track info */
        lv_obj_t *info = nullptr;
        std::vector<std::pair<std::string, std::string>> info_shown;
        /* Library settings: the cream block */
        lv_obj_t *block = nullptr, *block_title = nullptr, *block_bar = nullptr, *block_fill = nullptr, *block_text = nullptr;
        LibNote block_shown;
        /* Music folders */
        LibButton buttons[2];
        lv_obj_t *foot = nullptr;
        bool placed = false;
        bool last_empty = false;
        int head_h = 0;            /* the heading's own band */
        bool band_below = false;   /* the chip on a line under the heading */
        std::string heading_seen;
        bool osk_room = true; /* Search: the results fit above the on-screen keyboard (else they are hidden) */
        /* Search */
        std::unique_ptr<cp0::ui::TextField> field;
        int field_h = 0;
    };
    std::array<P, 11> pages;
    P &at(Page p) { return pages[static_cast<size_t>(static_cast<int>(p) - static_cast<int>(Page::LibHub))]; }
    const P &at(Page p) const { return pages[static_cast<size_t>(static_cast<int>(p) - static_cast<int>(Page::LibHub))]; }

    /* the sheet */
    lv_obj_t *sheet_root = nullptr;
    SheetView sheet_view;
    bool sheet_on = false;
    std::vector<lv_obj_t *> sheet_rows;
    std::vector<bool> sheet_enabled;
    int sheet_sel = 0;
    lv_obj_t *sheet_sel_box = nullptr;
    std::vector<int> sheet_row_y;
    int osk_listener = 0;
    int osk_inset = 0;

    int S(double v) const { return std::max(1, ri(v * s)); }
    int row_h() const { return std::max(ri(lg.row_h), list_row); }

    void build_page(P &L, Page p) {
        L.page = p;
        L.root = make_root();
        lv_obj_t *r = L.root;
        box(r, ri(lg.panel.x), ri(lg.panel.y), ri(lg.panel.w), ri(lg.panel.h), col::kList, S(10));
        const bool has_heading = p != Page::LibSearch;
        L.has_chip = p == Page::LibAlbum;
        int band = ri(lg.heading.h);
        /* the chip shares the heading's line as the 480x320 board draws it; when the heading does not fit beside it (a long
         * title, a narrow panel: 320x480) the chip gets a line of its own under the heading (apply_band) */
        const bool chip_below = false;
        const int head_h = band;
        L.head_h = head_h;
        if (L.has_chip) band = chip_below ? head_h + std::max(S(30), target) : std::max(band, target);
        if (has_heading) {
            L.heading = label(r, Face::Bold, S(12), col::kRowValue, ri(lg.heading.x), ri(lg.heading.y),
                              ri(lg.heading.w) - (L.has_chip && !chip_below ? S(116) : 0), LV_TEXT_ALIGN_LEFT, ri(0.04 * 12 * s));
            const lv_font_t *hf = font(Face::Bold, S(12));
            lv_obj_set_y(L.heading, ri(lg.heading.y) + ((chip_below ? head_h : band) - lv_font_get_line_height(hf)) / 2);
        }
        if (L.has_chip) {
            const int cw = S(100), ch = S(24);
            const int cy = chip_below ? ri(lg.panel.y) + head_h + (band - head_h - ch) / 2 : ri(lg.panel.y) + (band - ch) / 2;
            L.chip.build(r, ri(lg.panel.right()) - S(12) - cw, cy, cw, ch, S(13), col::kRowSel, col::kRowText,
                         "Play album", target, [this, p] {
                             if (cb.on_chip) cb.on_chip(p);
                         });
        }
        L.list_y = ri(lg.panel.y) + (has_heading ? band : S(2));
        const int bottom = ri(lg.panel.bottom()) - S(4);
        L.list_h = std::max(1, bottom - L.list_y);
        if (p == Page::LibInfo) {
            L.info = lv_obj_create(r);
            plain(L.info);
            lv_obj_set_pos(L.info, ri(lg.panel.x), L.list_y);
            lv_obj_set_size(L.info, ri(lg.panel.w), L.list_h);
            lv_obj_add_flag(L.info, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(L.info, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(L.info, LV_SCROLLBAR_MODE_ACTIVE);
            lv_obj_set_style_width(L.info, std::max(2, ri(3 * s)), LV_PART_SCROLLBAR);
            lv_obj_set_style_bg_color(L.info, lv_color_hex(col::kRowValue), LV_PART_SCROLLBAR);
            lv_obj_set_style_bg_opa(L.info, LV_OPA_50, LV_PART_SCROLLBAR);
        } else {
            if (p == Page::LibSearch) {
                cp0_ui_roller_page_t t = tk;
                t.roller_w = std::max(10, ri(lg.panel.w) - 2 * S(8));
                cp0::ui::TextFieldSpec spec;
                spec.label = "";
                spec.placeholder = "Search";
                spec.max_bytes = 96;
                cp0::ui::TextFieldEvents ev;
                ev.changed = [this](const std::string &text) {
                    if (cb.on_query) cb.on_query(text);
                };
                ev.submit = [this](const std::string &) {
                    if (cb.on_submit) cb.on_submit();
                };
                ev.escape = [this] {
                    if (cb.on_field_escape) cb.on_field_escape();
                };
                L.field = std::make_unique<cp0::ui::TextField>(r, t, std::move(spec), std::move(ev));
                lv_obj_t *row = L.field->row();
                if (lv_obj_get_child_count(row) > 1) lv_obj_add_flag(lv_obj_get_child(row, 0), LV_OBJ_FLAG_HIDDEN); /* no label */
                lv_obj_set_pos(row, ri(lg.panel.x) + S(8), ri(lg.panel.y) + S(8));
                L.field_h = t.row_h;
                L.list_y = ri(lg.panel.y) + S(8) + L.field_h + S(8);
                L.list_h = std::max(1, bottom - L.list_y);
            }
            L.list = std::make_unique<RowList>(r, ri(lg.list.x), L.list_y, ri(lg.list.w), L.list_h, row_h(), s, S(15), S(13),
                                               16 * s, 40 * s, 16 * s, 14 * s, [this, p](int i) {
                                                   if (cb.on_row) cb.on_row(p, i);
                                               });
            L.list->on_hold = [this, p](int i) {
                if (cb.on_hold) cb.on_hold(p, i);
            };
            L.list->enable_hold_cue(r, ri(lg.panel.x) + S(8), ri(lg.panel.bottom()) - S(8), ri(lg.panel.w) - 2 * S(8));
        }
        if (p == Page::LibSearch) L.empty.build(r, R{lg.panel.x, double(L.list_y - S(40)), lg.panel.w, 0}, s, target, nullptr);
        if (p == Page::LibSettings) {
            L.block = box(r, ri(lg.panel.x) + S(8), 0, ri(lg.panel.w) - 2 * S(8), S(82), col::kCream, S(8));
            L.block_title = label(L.block, Face::Bold, S(12), col::kInk3, S(12), S(8), 0, LV_TEXT_ALIGN_LEFT, ri(0.04 * 12 * s));
            lv_label_set_long_mode(L.block_title, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(L.block_title, ri(lg.panel.w) - 2 * S(8) - S(24));
            lv_obj_set_height(L.block_title, lv_font_get_line_height(font(Face::Bold, S(12))));
            L.block_bar = box(L.block, S(12), S(30), ri(lg.panel.w) - 2 * S(8) - S(24), S(10), col::kChip, S(5));
            L.block_fill = box(L.block_bar, 0, 0, 1, S(10), col::kTeal, S(5));
            L.block_text = label(L.block, Face::Bold, S(11), col::kInk2, S(12), S(48), 0);
            wrap_label(L.block_text, ri(lg.panel.w) - 2 * S(8) - S(24));
            lv_obj_set_style_text_line_space(L.block_text, S(15) - lv_font_get_line_height(font(Face::Bold, S(11))), 0);
            lv_obj_add_flag(L.block, LV_OBJ_FLAG_HIDDEN);
        }
        if (p == Page::LibFolders) {
            L.foot = label(r, Face::Bold, S(11), col::kRowValue, ri(lg.panel.x) + S(16), 0, 0);
            wrap_label(L.foot, ri(lg.panel.w) - 2 * S(16));
            lv_obj_set_style_text_line_space(L.foot, S(15) - lv_font_get_line_height(font(Face::Bold, S(11))), 0);
            for (int i = 0; i < 2; ++i) {
                L.buttons[i].build(r, ri(lg.panel.x) + S(16), 0, S(100), S(30), S(13), i == 0 ? col::kPrimary : col::kRowSel,
                                   i == 0 ? col::kWhite : col::kRowText, "", target, [this, p, i] {
                                       if (cb.on_button) cb.on_button(p, i);
                                   });
            }
        }
        lv_obj_add_flag(L.root, LV_OBJ_FLAG_HIDDEN);
    }

    /* the Music folders page: the rows above, then the buttons and the note at the panel's bottom */
    void place_folders(P &L, const LibView &v) {
        const int px = ri(lg.panel.x), pw = ri(lg.panel.w), bottom = ri(lg.panel.bottom()) - S(8);
        set_text(L.foot, v.footnote);
        lv_obj_update_layout(L.foot);
        const int fh = v.footnote.empty() ? 0 : lv_obj_get_height(L.foot);
        show(L.foot, !v.footnote.empty());
        lv_obj_set_y(L.foot, bottom - fh);
        const int bh = std::max(S(30), 1), gap = S(8);
        const int total = pw - 2 * S(16);
        const int n = static_cast<int>(v.buttons.size());
        const lv_font_t *bf = font(Face::Bold, S(13));
        int need = 0;
        for (const std::string &t : v.buttons) need += text_width(bf, t) + 2 * S(14);
        /* side by side when both words fit, else one over the other (a narrow panel) */
        const bool stacked = n >= 2 && need + gap > total;
        const int rows_n = stacked ? n : 1;
        const int row_gap = std::max(gap, target - bh);
        const int top = bottom - fh - (fh ? S(8) : 0) - rows_n * bh - (rows_n - 1) * row_gap;
        const int bw0 = n >= 2 && !stacked ? std::min(S(200), (total - gap) * 9 / 20) : total;
        for (int i = 0; i < 2; ++i) {
            LibButton &b = L.buttons[i];
            const bool on = i < n;
            b.show(on);
            if (!on) continue;
            b.set_text(v.buttons[static_cast<size_t>(i)]);
            const int bw = stacked || n < 2 ? total : (i == 0 ? bw0 : total - bw0 - gap);
            const int by = stacked ? top + i * (bh + row_gap) : top;
            b.x = px + S(16) + (stacked || i == 0 ? 0 : bw0 + gap);
            b.w = bw;
            lv_obj_set_pos(b.vis, b.x, by);
            lv_obj_set_width(b.vis, bw);
            lv_obj_set_width(b.word, bw);
            const int hw = std::max(bw, target), hh = std::max(bh, target);
            lv_obj_set_pos(b.hitbox, b.x - (hw - bw) / 2, by - (hh - bh) / 2);
            lv_obj_set_size(b.hitbox, hw, hh);
            b.set_enabled(v.button_enabled[i]);
        }
        const int list_bottom = std::min(top - std::max(S(8), (target - bh) / 2), bottom);
        L.list_h = std::max(row_h(), list_bottom - L.list_y);
        L.list->resize(L.list_h);
        L.placed = true;
    }

    void update_info(P &L, const LibView &v) {
        if (v.info == L.info_shown) return;
        L.info_shown = v.info;
        lv_obj_clean(L.info);
        const int pw = ri(lg.panel.w), kx = S(16);
        const int kw = std::min(S(130), pw / 3);
        const int vx = kx + kw + S(4);
        int y = S(2);
        const lv_font_t *kf = font(Face::Bold, S(12)), *vf = font(Face::Bold, S(14));
        const int line = std::max(lv_font_get_line_height(vf), S(22));
        for (const auto &kv : v.info) {
            lv_obj_t *k = label(L.info, Face::Bold, S(12), col::kRowValue, kx, y + (line - lv_font_get_line_height(kf)) / 2, kw);
            set_text(k, kv.first);
            lv_obj_t *val = label(L.info, Face::Bold, S(14), col::kRowText, vx, y + (line - lv_font_get_line_height(vf)) / 2, 0);
            wrap_label(val, pw - vx - S(12));
            lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_WRAP);
            set_text(val, kv.second);
            lv_obj_update_layout(val);
            const int vh = lv_obj_get_height(val);
            y += std::max(line, vh + S(4));
        }
        lv_obj_scroll_to_y(L.info, 0, LV_ANIM_OFF);
    }

    void update_block(P &L, const LibView &v) {
        const bool on = v.block.kind == LibNote::Kind::Scan;
        if (on) {
            set_text(L.block_title, v.block_title);
            set_text(L.block_text, v.block_text);
            const bool has_bar = v.block.pct >= 0;
            show(L.block_bar, has_bar);
            lv_obj_set_width(L.block_fill, std::max(1, lv_obj_get_width(L.block_bar) * std::clamp(v.block.pct, 0, 100) / 100));
            set_bg(L.block_fill, v.block.hot ? col::kHot : col::kTeal);
            lv_obj_set_y(L.block_text, has_bar ? S(48) : S(30));
            lv_obj_update_layout(L.block_text);
            const int h = (has_bar ? S(48) : S(30)) + lv_obj_get_height(L.block_text) + S(8);
            const int by = ri(lg.panel.bottom()) - S(8) - h;
            lv_obj_set_y(L.block, by);
            lv_obj_set_height(L.block, h);
            L.list->resize(std::max(row_h(), by - S(4) - L.list_y));
        } else if (L.block_shown.kind != LibNote::Kind::None) {
            L.list->resize(L.list_h);
        }
        show(L.block, on);
        L.block_shown = v.block;
    }

    /* the heading band with the chip beside the heading, or under it when the heading does not fit beside it */
    void apply_band(P &L, bool below) {
        L.band_below = below;
        const int ch = S(24);
        const int beside = std::max(L.head_h, target);
        const int band = below ? L.head_h + std::max(S(30), target) : beside;
        const lv_font_t *hf = font(Face::Bold, S(12));
        lv_obj_set_width(L.heading, ri(lg.heading.w) - (below ? 0 : S(116)));
        lv_obj_set_y(L.heading, ri(lg.heading.y) + ((below ? L.head_h : band) - lv_font_get_line_height(hf)) / 2);
        const int cy = below ? ri(lg.panel.y) + L.head_h + (band - L.head_h - ch) / 2 : ri(lg.panel.y) + (band - ch) / 2;
        const int hh = std::max(ch, target);
        lv_obj_set_y(L.chip.vis, cy);
        lv_obj_set_y(L.chip.hitbox, cy - (hh - ch) / 2);
        L.list_y = ri(lg.panel.y) + band;
        L.list_h = std::max(1, ri(lg.panel.bottom()) - S(4) - L.list_y);
        lv_obj_set_y(L.list->obj(), L.list_y);
        L.list->resize(L.list_h);
    }

    void update_page(P &L, const LibView &v) {
        if (L.heading) set_text(L.heading, v.heading);
        if (L.has_chip && v.heading != L.heading_seen) {
            L.heading_seen = v.heading;
            const lv_font_t *hf = font(Face::Bold, S(12));
            const int need = text_width(hf, v.heading, ri(0.04 * 12 * s)) + S(4);
            const bool below = need > ri(lg.heading.w) - S(116);
            if (below != L.band_below) apply_band(L, below);
        }
        if (L.has_chip) {
            L.chip.set_text(v.chip);
            L.chip.show(!v.chip.empty());
        }
        if (L.info) {
            update_info(L, v);
            return;
        }
        const bool reset = v.rows_gen != L.rows_gen;
        L.rows_gen = v.rows_gen;
        if (v.rows && (reset || v.rows_ver != L.rows_ver || v.selected != L.sel)) {
            if (!reset && v.rows_ver == L.rows_ver) L.list->set_selected(v.selected, true);
            else L.list->set_rows(*v.rows, v.selected, reset, v.scroll_to); /* a place given back: its top row, once its rows are in */
            L.rows_ver = v.rows_ver;
            L.sel = v.selected;
            L.nrows = static_cast<int>(v.rows->size());
        }
        if (L.page == Page::LibSearch) {
            show(L.list->obj(), !v.empty && L.osk_room);
            if (v.empty) L.empty.set(v.empty_title, v.empty_sub, "");
            L.empty.show(v.empty && L.osk_room);
            L.last_empty = v.empty;
        }
        if (L.page == Page::LibSettings) update_block(L, v);
        if (L.page == Page::LibFolders) place_folders(L, v);
    }

    /* ---------------------------------------------------------------------------------------------- the sheet */

    static void sheet_row_cb(lv_event_t *e) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
        lv_obj_t *o = lv_event_get_target_obj(e);
        for (size_t i = 0; i < self->sheet_rows.size(); ++i)
            if (self->sheet_rows[i] == o) {
                if (self->sheet_enabled[i] && self->cb.on_sheet_pick) self->cb.on_sheet_pick(static_cast<int>(i));
                return;
            }
    }

    void build_sheet() {
        sheet_root = make_root();
        lv_obj_add_flag(sheet_root, LV_OBJ_FLAG_HIDDEN);
    }

    void show_sheet(const SheetView &v) {
        lv_obj_clean(sheet_root);
        sheet_rows.clear();
        sheet_enabled.clear();
        sheet_view = v;
        sheet_on = true;
        const int px = ri(lg.panel.x), pw = ri(lg.panel.w), py = ri(lg.panel.y), pb = ri(lg.panel.bottom());
        /* the page behind it dimmed (the canvases draw it at 35 %): a tap on it closes the sheet */
        lv_obj_t *dim = box(sheet_root, px, py, pw, pb - py, col::kList, S(10), LV_OPA_70);
        lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dim, [](lv_event_t *e) {
            auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
            if (self->cb.on_sheet_dismiss) self->cb.on_sheet_dismiss();
        }, LV_EVENT_CLICKED, this);
        const int sw = pw - 2 * S(8), sx = px + S(8);
        const int rh = std::max(row_h(), target); /* the sheet's rows act: at least the touch target, not the compact list's rows */
        const int title_h = S(26);
        lv_obj_t *hint = label(sheet_root, Face::Bold, S(11), col::kInk3, sx + S(14), 0, 0);
        wrap_label(hint, sw - 2 * S(14));
        lv_obj_set_style_text_line_space(hint, S(15) - lv_font_get_line_height(font(Face::Bold, S(11))), 0);
        set_text(hint, v.hint);
        lv_obj_update_layout(hint);
        const int hint_h = v.hint.empty() ? 0 : lv_obj_get_height(hint) + S(10);
        const int n = static_cast<int>(v.rows.size());
        const int sh = title_h + n * rh + hint_h + S(8);
        const int sy = std::max(py + S(4), pb - S(8) - sh);
        box(sheet_root, sx, sy, sw, sh, col::kCream, S(10));
        lv_obj_t *title = label(sheet_root, Face::Bold, S(12), col::kInk3, sx + S(14), sy + S(6), sw - 2 * S(14), LV_TEXT_ALIGN_LEFT,
                                ri(0.04 * 12 * s));
        set_text(title, v.title);
        lv_obj_move_foreground(title);
        sheet_row_y.clear();
        for (int i = 0; i < n; ++i) {
            const SheetView::Row &r = v.rows[static_cast<size_t>(i)];
            const int ry = sy + title_h + i * rh;
            sheet_row_y.push_back(ry);
            lv_obj_t *row = box(sheet_root, sx, ry, sw, rh, col::kChip, 0, i == v.selected ? LV_OPA_COVER : LV_OPA_TRANSP);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, sheet_row_cb, LV_EVENT_CLICKED, this);
            const lv_font_t *lf = font(Face::Bold, S(15)), *kf = font(Face::Bold, S(13));
            lv_obj_t *t = label(row, Face::Bold, S(15), col::kInk, S(16), (rh - lv_font_get_line_height(lf)) / 2, sw - S(32) - S(70));
            set_text(t, r.label);
            lv_obj_t *k = label(row, Face::Bold, S(13), col::kInk3, sw - S(16) - S(64), (rh - lv_font_get_line_height(kf)) / 2, S(64),
                                LV_TEXT_ALIGN_RIGHT);
            set_text(k, r.key);
            if (!r.enabled) { /* drawn, inert: dimmed (the same 40 % as the other dimmed keys) */
                lv_obj_set_style_opa(t, LV_OPA_40, 0);
                lv_obj_set_style_opa(k, LV_OPA_40, 0);
            }
            sheet_rows.push_back(row);
            sheet_enabled.push_back(r.enabled);
        }
        lv_obj_set_pos(hint, sx + S(14), sy + title_h + n * rh + S(2));
        lv_obj_move_foreground(hint);
        sheet_sel = v.selected;
        lv_obj_remove_flag(sheet_root, LV_OBJ_FLAG_HIDDEN);
    }

    void hide_sheet() {
        if (!sheet_on) return;
        sheet_on = false;
        lv_obj_add_flag(sheet_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(sheet_root);
        sheet_rows.clear();
        sheet_enabled.clear();
    }

    void sheet_select(int i) {
        if (!sheet_on) return;
        sheet_sel = i;
        for (size_t k = 0; k < sheet_rows.size(); ++k)
            lv_obj_set_style_bg_opa(sheet_rows[k], static_cast<int>(k) == i ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }

    void on_inset(int inset) {
        osk_inset = inset;
        P &L = at(Page::LibSearch);
        if (!L.list) return;
        const int bottom = std::min(ri(lg.panel.bottom()) - S(4), H - inset - S(4));
        L.osk_room = bottom - L.list_y >= row_h();
        L.list_h = std::max(row_h(), bottom - L.list_y);
        if (L.osk_room) L.list->resize(L.list_h);
        show(L.list->obj(), L.osk_room && !L.last_empty);
        L.empty.show(L.osk_room && L.last_empty);
    }
};

LibraryPages::LibraryPages(std::function<lv_obj_t *()> make_root, lv_obj_t *, const cp0_ui_roller_page_t &tk, int screen_w,
                           int screen_h, int body_y, int target, int list_row, LibraryCallbacks cb)
    : d_(std::make_unique<Impl>()) {
    Impl &d = *d_;
    d.make_root = std::move(make_root);
    d.tk = tk;
    d.W = screen_w;
    d.H = screen_h;
    d.body_y = body_y;
    d.target = target;
    d.list_row = list_row;
    d.cb = std::move(cb);
    d.lg = layout::list_geom(layout::ListPage::Browser, screen_w, screen_h, body_y);
    d.s = d.lg.s;
    for (int i = 1; i < static_cast<int>(d.pages.size()); ++i) d.build_page(d.pages[static_cast<size_t>(i)], static_cast<Page>(static_cast<int>(Page::LibHub) + i));
    d.build_sheet();
    d.osk_listener = cp0::ui::osk::add_inset_listener([&d](int inset) { d.on_inset(inset); });
}

LibraryPages::~LibraryPages() {
    if (d_->osk_listener) cp0::ui::osk::remove_inset_listener(d_->osk_listener);
    d_->at(Page::LibSearch).field.reset();
}

lv_obj_t *LibraryPages::root(Page p) const { return p == Page::LibHub ? nullptr : d_->at(p).root; }

void LibraryPages::update(Page p, const LibView &v) {
    if (p == Page::LibHub) return;
    d_->update_page(d_->at(p), v);
}

int LibraryPages::rows(Page p) const { return p == Page::LibHub ? 0 : d_->at(p).nrows; }

int LibraryPages::top_row(Page p) const {
    if (p == Page::LibHub) return 0;
    const Impl::P &L = d_->at(p);
    return L.list ? L.list->top_row() : 0;
}

void LibraryPages::search_open(const std::string &text, bool edit) {
    Impl::P &L = d_->at(Page::LibSearch);
    if (!L.field) return;
    L.field->set_text(text);
    if (edit) L.field->begin_edit();
    else L.field->end_edit();
}

void LibraryPages::scroll(Page p, int rows) {
    Impl::P &L = d_->at(p);
    if (!L.info) return;
    const int step = std::max(1, d_->S(22));
    lv_obj_update_layout(L.info);
    const int max_y = std::max(0, static_cast<int>(lv_obj_get_scroll_bottom(L.info)) + lv_obj_get_scroll_y(L.info));
    lv_obj_scroll_to_y(L.info, std::clamp(lv_obj_get_scroll_y(L.info) + rows * step, 0, max_y), LV_ANIM_OFF);
}

void LibraryPages::search_edit() {
    Impl::P &L = d_->at(Page::LibSearch);
    if (L.field) L.field->begin_edit();
}

void LibraryPages::search_end_edit() {
    Impl::P &L = d_->at(Page::LibSearch);
    if (L.field) L.field->end_edit();
}

bool LibraryPages::search_editing() const {
    const Impl::P &L = d_->at(Page::LibSearch);
    return L.field && L.field->editing();
}

std::string LibraryPages::search_text() const {
    const Impl::P &L = d_->at(Page::LibSearch);
    return L.field ? L.field->text() : std::string();
}

void LibraryPages::show_sheet(const SheetView &v) { d_->show_sheet(v); }
void LibraryPages::hide_sheet() { d_->hide_sheet(); }
bool LibraryPages::sheet_shown() const { return d_->sheet_on; }
void LibraryPages::sheet_select(int i) { d_->sheet_select(i); }

layout::R LibraryPages::notice_room(Page) const {
    const double h = std::round(42 * d_->s), m8 = std::round(8 * d_->s);
    const layout::ListGeom &g = d_->lg;
    return R{g.panel.x + m8, g.panel.bottom() - m8 - h, g.panel.w - 2 * m8, h};
}

} // namespace ui
} // namespace vinyl
