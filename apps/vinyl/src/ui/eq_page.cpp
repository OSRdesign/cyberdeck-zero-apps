/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the Equalizer page (see eq_page.hpp). Every position comes from core/layout eq_geom; colours are the canvas's
 * (the track #2A2A2A, the middle line #555555, the fill teal, the selected outline and label cream, the labels grey).
 */
#include "eq_page.hpp"

#include "gfx.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vinyl {
namespace ui {

using layout::R;

namespace {

int ri(double v) { return static_cast<int>(std::lround(v)); }

constexpr uint32_t kMidLine = 0x555555;

}  // namespace

struct EqPage::Impl {
    layout::EqGeom g;
    int target = 44;
    std::function<void(int, int, int)> on_band;
    std::function<void(int)> on_button;
    lv_obj_t *root = nullptr;
    lv_obj_t *band_text = nullptr, *chip = nullptr, *chip_text = nullptr, *preset = nullptr;
    struct Bar {
        lv_obj_t *track = nullptr, *fill = nullptr, *fill_sq = nullptr, *label = nullptr, *area = nullptr;
    } bars[kEqBands];
    lv_obj_t *btn[layout::kEqButtons] = {}, *btn_icon[2] = {}, *btn_word[layout::kEqButtons] = {}, *btn_area[layout::kEqButtons] = {};
    EqView shown;
    bool first = true;
    /* the drag on a band's column */
    int press_band = -1, press_y = 0;
    bool dragging = false;

    int slop() const { return std::max(6, target / 6); } /* the toolkit's tap slop (the map widget's rule, as Knock-knock) */

    static void band_cb(lv_event_t *e) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
        lv_obj_t *o = lv_event_get_target_obj(e);
        int band = -1;
        for (int i = 0; i < kEqBands; ++i)
            if (self->bars[i].area == o) band = i;
        if (band < 0) return;
        lv_point_t pt{0, 0};
        if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &pt);
        const lv_event_code_t c = lv_event_get_code(e);
        if (c == LV_EVENT_PRESSED) {
            self->press_band = band;
            self->press_y = pt.y;
            self->dragging = false;
            if (self->on_band) self->on_band(band, 0, 0);
        } else if (c == LV_EVENT_PRESSING) {
            if (self->press_band != band) return;
            if (!self->dragging && std::abs(pt.y - self->press_y) > self->slop()) self->dragging = true;
            if (self->dragging && self->on_band) self->on_band(band, 1, layout::eq_db_at(self->g, pt.y));
        } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
            if (self->press_band == band && self->on_band) self->on_band(band, c == LV_EVENT_RELEASED ? 2 : 3, 0);
            self->press_band = -1;
            self->dragging = false;
        }
    }

    static void button_cb(lv_event_t *e) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
        lv_obj_t *o = lv_event_get_target_obj(e);
        for (int i = 0; i < layout::kEqButtons; ++i)
            if (self->btn_area[i] == o && self->on_button) self->on_button(i);
    }

    void build(std::function<lv_obj_t *()> make_root, int body_y) {
        (void)body_y;
        const double s = g.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        root = make_root();
        box(root, ri(g.panel.x), ri(g.panel.y), ri(g.panel.w), ri(g.panel.h), col::kList, S(10));
        /* the band line: "Band 4 kHz:" and the chip */
        const int bpx = std::max(1, ri(g.band_px));
        const lv_font_t *bf = font(Face::Bold, bpx);
        band_text = label(root, Face::Bold, bpx, col::kRowText, ri(g.band_line.x), ri(g.band_line.y + (g.band_line.h - lv_font_get_line_height(bf)) / 2), 0);
        chip = box(root, 0, 0, 10, lv_font_get_line_height(bf) + 2 * S(1), col::kCream, S(4));
        chip_text = label(chip, Face::Bold, bpx, col::kTeal, ri(g.chip_pad), S(1), 0);
        /* the preset: right of the band line (landscape), under it (portrait) */
        const int ppx = std::max(1, ri(g.preset_px));
        const lv_font_t *pf = font(Face::Bold, ppx);
        preset = label(root, Face::Bold, ppx, col::kRowValue, ri(g.preset_line.x), ri(g.preset_line.y + (g.preset_line.h - lv_font_get_line_height(pf)) / 2),
                       ri(g.preset_line.w), g.portrait ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, ri(0.04 * g.preset_px));
        /* the bars */
        const int lpx = std::max(1, ri(g.label_px));
        for (int i = 0; i < kEqBands; ++i) {
            Bar &b = bars[i];
            const R &t = g.track[i];
            b.track = box(root, ri(t.x), ri(t.y), ri(t.w), ri(t.h), col::kRowSel, ri(g.radius));
            lv_obj_set_style_clip_corner(b.track, true, 0);
            lv_obj_set_style_outline_color(b.track, lv_color_hex(col::kCream), 0);
            lv_obj_set_style_outline_pad(b.track, 0, 0);
            box(b.track, 0, ri(t.h) / 2, ri(t.w), 1, kMidLine, 0);
            b.fill = box(b.track, 0, 0, ri(t.w), 1, col::kTeal, ri(g.radius));
            b.fill_sq = box(b.track, 0, 0, ri(t.w), 1, col::kTeal, 0);
            show(b.fill, false);
            show(b.fill_sq, false);
            const R &l = g.label[i];
            b.label = label(root, Face::Mono, lpx, col::kRowValue, ri(l.x), ri(l.y), ri(l.w), LV_TEXT_ALIGN_CENTER);
            set_text(b.label, eq_band_label(i));
            /* the column's touch area: select on a press, set by a drag */
            b.area = lv_obj_create(root);
            plain(b.area);
            lv_obj_add_flag(b.area, LV_OBJ_FLAG_CLICKABLE);
            const R &a = g.band_area[i];
            const int x0 = ri(a.x), y0 = ri(a.y), x1 = ri(a.right()), y1 = ri(a.bottom());
            lv_obj_set_pos(b.area, x0, y0);
            lv_obj_set_size(b.area, std::max(1, x1 - x0), std::max(1, y1 - y0));
            for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
                lv_obj_add_event_cb(b.area, band_cb, c, this);
        }
        /* the buttons: "-", "+", Preset, Reset, On */
        const int wpx = std::max(1, ri(g.button_px));
        const lv_font_t *wf = font(Face::Bold, wpx);
        static const char *const words[layout::kEqButtons] = {"", "", "Preset", "Reset", "On"};
        for (int i = 0; i < layout::kEqButtons; ++i) {
            const R &r = g.button[i];
            const int w = ri(r.w), h = ri(r.h);
            btn[i] = box(root, ri(r.x), ri(r.y), w, h, col::kButton, S(8));
            if (i < 2) {
                const int ip = S(20);
                btn_icon[i] = icon_obj(btn[i], i == 0 ? Icon::Minus : Icon::Plus, (w - ip) / 2, (h - ip) / 2, ip, ip, ip, col::kInk);
            } else {
                btn_word[i] = label(btn[i], Face::Bold, wpx, col::kInk, 0, (h - lv_font_get_line_height(wf)) / 2, w, LV_TEXT_ALIGN_CENTER);
                set_text(btn_word[i], words[i]);
            }
            btn_area[i] = lv_obj_create(root);
            plain(btn_area[i]);
            lv_obj_add_flag(btn_area[i], LV_OBJ_FLAG_CLICKABLE);
            const R &a = g.button_area[i];
            const int x0 = ri(a.x), y0 = ri(a.y), x1 = ri(a.right()), y1 = ri(a.bottom());
            lv_obj_set_pos(btn_area[i], x0, y0);
            lv_obj_set_size(btn_area[i], std::max(1, x1 - x0), std::max(1, y1 - y0));
            lv_obj_add_event_cb(btn_area[i], button_cb, LV_EVENT_CLICKED, this);
        }
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }

    void draw_bar(int i, int db, bool selected) {
        Bar &b = bars[i];
        const R &t = g.track[i];
        const int th = ri(t.h), tw = ri(t.w), mid = th / 2, half = th / 2;
        const int fh = std::min(half, ri(std::fabs(db) / static_cast<double>(kEqMaxDb) * half));
        lv_obj_set_style_outline_width(b.track, selected ? ri(g.outline) : 0, 0);
        lv_obj_set_style_text_color(b.label, lv_color_hex(selected ? col::kCream : col::kRowValue), 0);
        if (fh <= 0) {
            show(b.fill, false);
            show(b.fill_sq, false);
            return;
        }
        /* the rounded end away from the middle line, the square end on it (the canvas's border radius 5 5 0 0) */
        const int sq = std::min(fh, ri(g.radius));
        show(b.fill, true);
        show(b.fill_sq, true);
        if (db > 0) {
            lv_obj_set_pos(b.fill, 0, mid - fh);
            lv_obj_set_size(b.fill, tw, fh);
            lv_obj_set_pos(b.fill_sq, 0, mid - sq);
        } else {
            lv_obj_set_pos(b.fill, 0, mid + 1);
            lv_obj_set_size(b.fill, tw, fh);
            lv_obj_set_pos(b.fill_sq, 0, mid + 1);
        }
        lv_obj_set_size(b.fill_sq, tw, sq);
    }

    void update(const EqView &v) {
        if (!first && v == shown) return;
        const double s = g.s;
        auto S = [&](double x) { return std::max(1, ri(x * s)); };
        for (int i = 0; i < kEqBands; ++i)
            if (first || v.gains[static_cast<size_t>(i)] != shown.gains[static_cast<size_t>(i)] || (i == v.band) != (i == shown.band))
                draw_bar(i, v.gains[static_cast<size_t>(i)], i == v.band);
        const int band = std::max(0, std::min(kEqBands - 1, v.band));
        set_text(band_text, "Band " + eq_band_name(band) + ":");
        lv_obj_update_layout(band_text);
        const std::string db = eq_db_text(v.gains[static_cast<size_t>(band)]);
        set_text(chip_text, db);
        const int cw = text_width(font(Face::Bold, std::max(1, ri(g.band_px))), db) + 2 + 2 * ri(g.chip_pad);
        const int ch = lv_obj_get_height(chip);
        lv_obj_set_size(chip, cw, ch);
        lv_obj_set_pos(chip, lv_obj_get_x(band_text) + lv_obj_get_width(band_text) + S(6), ri(g.band_line.y + (g.band_line.h - ch) / 2));
        set_text(preset, v.preset);
        /* On / Off */
        set_bg(btn[4], v.on ? col::kTeal : col::kButton);
        lv_obj_set_style_text_color(btn_word[4], lv_color_hex(v.on ? col::kWhite : col::kInk), 0);
        set_text(btn_word[4], v.on ? "On" : "Off");
        shown = v;
        first = false;
    }
};

EqPage::EqPage(std::function<lv_obj_t *()> make_root, int screen_w, int screen_h, int body_y, int target, int row,
               std::function<void(int, int, int)> on_band, std::function<void(int)> on_button)
    : d_(std::make_unique<Impl>()) {
    d_->g = layout::eq_geom(screen_w, screen_h, body_y, target, row);
    d_->target = target;
    d_->on_band = std::move(on_band);
    d_->on_button = std::move(on_button);
    d_->build(std::move(make_root), body_y);
}

EqPage::~EqPage() = default;

lv_obj_t *EqPage::root() const { return d_->root; }

void EqPage::update(const EqView &v) { d_->update(v); }

layout::R EqPage::notice_room() const {
    const layout::EqGeom &g = d_->g;
    const double h = std::round(42 * g.s), m8 = std::round(8 * g.s);
    return R{g.panel.x + m8, g.panel.bottom() - m8 - h, g.panel.w - 2 * m8, h};
}

}  // namespace ui
}  // namespace vinyl
