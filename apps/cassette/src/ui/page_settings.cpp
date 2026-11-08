/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the settings page (boards CassetteSettings-480x320 / -320x480): cream rows of 42 px with a chevron on
 * every choice row, a switch on the toggles, a slider and its value on the slider rows, the boards' scroll bar at the
 * right, and (landscape) the "more below" line naming the rows under the visible part.
 *
 * Scrolling (the mmWave lesson): the list is CLICKABLE itself and every row is clickable, so a drag that starts
 * anywhere in it (a label, a switch, the gap between rows) scrolls it; a short tap acts on the row.
 */
#include "pages.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <algorithm>

namespace cassette {
namespace ui {

namespace {

constexpr double kRowH = 42;

double row_gap(bool portrait) { return portrait ? 9 : 4; }

} // namespace

SettingsPage::SettingsPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav)
    : PageBase(parent, frame), act_(actions), nav_(nav)
{
    /* the list grows into whatever a larger screen leaves: one seam under it */
    fit_ = frame.fit({283}, {460});
    deck_body(root_, fit_);
    const bool p = frame.portrait;
    const double lw = p ? 276 : 430;
    list_x_ = fit_.x(16);
    list_y_ = fit_.y(54);
    list_w_ = fit_.w(16, lw);
    /* 54..280 (landscape, 5 rows) or 54..453 (portrait, 8 rows) on the boards, plus the seam's share */
    list_h_ = fit_.y(p ? 461 : 284) - list_y_ - fit_.px(p ? 8 : 4);
    row_w_ = list_w_;
    list_ = lv_obj_create(root_);
    plain(list_);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_CLICKABLE); /* a drag from anywhere in it scrolls it */
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(list_, list_x_, list_y_);
    lv_obj_set_size(list_, list_w_, list_h_);
    lv_obj_add_event_cb(list_, scroll_cb, LV_EVENT_SCROLL, this);
    const double tx = p ? 298 : 452;
    /* the boards' track: 452,56 6 x 250 (landscape), 298,56 6 x 408 (portrait) */
    track_ = box(root_, fit_.x(tx), fit_.y(56), fit_.w(tx, 6), fit_.y(p ? 464 : 306) - fit_.y(56), col::kScrollTrack, fit_.px(3));
    thumb_ = box(root_, fit_.x(tx), fit_.y(56), fit_.w(tx, 6), fit_.px(40), col::kHinge, fit_.px(3));
    if (!p) hint_ = fit_label(root_, fit_, Face::Medium, 14, col::kInkMuted, 16, 286, 430, 18, LV_TEXT_ALIGN_CENTER);
}

int SettingsPage::row_h() const
{
    /* a tap acts at once (a switch flips, a picker opens): the toolkit's ROW (the deck: 79 px) */
    return std::max(fit_.px(kRowH), frame_.row);
}

void SettingsPage::update(const SettingsVm &s)
{
    if (have_ && s == vm_) return; /* nothing changed (the root calls every page at the meter's rate) */
    have_ = true;
    vm_ = s;
    refresh();
}

void SettingsPage::refresh()
{
    const SettingsVm &s = vm_;
    std::string key = std::to_string(picker_) + "|";
    for (const SettingRow &r : s.rows) key += r.id + ":" + std::to_string(static_cast<int>(r.kind)) + ";";
    if (picker_ >= 0 && picker_ < static_cast<int>(s.rows.size()))
        for (const std::string &c : s.rows[static_cast<std::size_t>(picker_)].choices) key += c + ",";
    if (key != layout_key_) {
        layout_key_ = key;
        rebuild();
    }
    for (Row &r : rows_) {
        if (picker_ < 0) {
            if (r.index >= 0 && r.index < static_cast<int>(s.rows.size())) fill_row(r, s.rows[static_cast<std::size_t>(r.index)]);
        } else {
            const SettingRow &sr = s.rows[static_cast<std::size_t>(picker_)];
            const bool cur = r.index == sr.choice;
            lv_obj_set_style_bg_color(r.obj, lv_color_hex(cur ? col::kStrip : col::kRow), 0);
            lv_obj_set_style_border_color(r.obj, lv_color_hex(cur ? col::kHinge : col::kBodyEdge), 0);
        }
    }
    highlight(sel_);
    update_scrollbar();
    update_hint();
}

void SettingsPage::rebuild()
{
    lv_obj_clean(list_);
    rows_.clear();
    const int h = row_h();
    const int gap = fit_.px(row_gap(frame_.portrait));
    const int pad = fit_.px(12);
    std::vector<std::string> labels;
    if (picker_ >= 0 && picker_ < static_cast<int>(vm_.rows.size())) labels = vm_.rows[static_cast<std::size_t>(picker_)].choices;
    else for (const SettingRow &r : vm_.rows) labels.push_back(r.label);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        Row r;
        r.index = static_cast<int>(i);
        r.obj = box(list_, 0, static_cast<int>(i) * (h + gap), row_w_, h, col::kRow, fit_.px(6));
        border(r.obj, 1, col::kBodyEdge);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_set_user_data(r.obj, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_RELEASED, this);
        const int lh19 = lv_font_get_line_height(font(Face::Bold, fit_.px(19)));
        r.label = label(r.obj, Face::Bold, fit_.px(19), col::kInk, pad - 1, (h - 2 - lh19) / 2, row_w_ - 2 * pad, LV_TEXT_ALIGN_LEFT,
                        ls_screen(0.3, fit_.s));
        lv_label_set_text(r.label, labels[i].c_str());
        if (picker_ < 0) {
            const SettingRow &s = vm_.rows[i];
            const int lh17 = lv_font_get_line_height(font(Face::Medium, fit_.px(17)));
            r.value = label(r.obj, Face::Medium, fit_.px(17), col::kInkMuted, 0, (h - 2 - lh17) / 2, 10, LV_TEXT_ALIGN_RIGHT);
            if (s.kind == SettingKind::Choice || s.kind == SettingKind::Text) {
                r.chevron = make_chevron(r.obj, fit_, col::kInkMuted);
                lv_obj_set_pos(r.chevron, row_w_ - 2 - pad - fit_.px(10), (h - 2 - fit_.px(16)) / 2);
            } else if (s.kind == SettingKind::Toggle) {
                r.toggle = make_toggle(r.obj, fit_, 44, 24);
                lv_obj_set_pos(r.toggle, row_w_ - 2 - pad - fit_.px(44), (h - 2 - fit_.px(24)) / 2);
                show(r.value, false);
            } else if (s.kind == SettingKind::Slider) {
                r.slider = make_slider_bar(r.obj, fit_, 100, 20);
            }
        }
        rows_.push_back(r);
    }
    lv_obj_update_layout(list_);
}

void SettingsPage::fill_row(Row &r, const SettingRow &s)
{
    const int h = row_h(), pad = fit_.px(12), gap = fit_.px(10);
    const int inner = row_w_ - 2; /* inside the 1 px border */
    lv_obj_set_style_opa_layered(r.obj, static_cast<lv_opa_t>(s.enabled ? 255 : 89), 0);
    int right = inner - pad; /* the right edge of the value part */
    std::string value;
    if (r.chevron) {
        right -= fit_.px(10) + gap;
        value = s.value;
    } else if (r.toggle) {
        set_toggle(r.toggle, s.on);
        right -= fit_.px(44);
    } else if (r.slider) {
        value = s.value_text;
    }
    const int lab_nat = text_width(font(Face::Bold, fit_.px(19)), s.label.c_str(), ls_screen(0.3, fit_.s));
    int value_w = 0;
    if (!value.empty()) {
        int avail = right - pad - std::min(lab_nat, (right - pad) * 45 / 100) - gap;
        if (r.slider) avail = std::min(avail, fit_.px(80));
        const int px = fit_px(Face::Medium, fit_.px(17), 0, value, avail, std::max(10, fit_.px(13)), nullptr);
        value_w = std::min(avail, text_width(font(Face::Medium, px), value.c_str(), 0));
        lv_obj_set_style_text_font(r.value, font(Face::Medium, px), 0);
        lv_obj_set_width(r.value, value_w + 2);
        lv_obj_set_x(r.value, right - value_w - 1);
        set_text(r.value, value);
    }
    int label_right = right - (value_w ? value_w + gap : (r.toggle ? gap : 0));
    if (r.slider) {
        const int sw = fit_.px(100);
        const int sx = right - value_w - gap - sw;
        lv_obj_set_pos(r.slider, sx, (h - 2 - fit_.px(20)) / 2);
        set_slider_bar(r.slider, s.min, s.max, s.pos);
        label_right = sx - gap;
    }
    lv_obj_set_width(r.label, std::max(10, label_right - pad + 1));
    set_fitted(r.label, Face::Bold, fit_.px(19), ls_screen(0.3, fit_.s), std::max(10, fit_.px(14)), s.label);
}

void SettingsPage::highlight(int i)
{
    for (std::size_t k = 0; k < rows_.size(); ++k) {
        if (picker_ >= 0) {
            const SettingRow &sr = vm_.rows[static_cast<std::size_t>(picker_)];
            /* the keyboard highlight, else the current choice */
            const bool cur = i >= 0 ? static_cast<int>(k) == i : rows_[k].index == sr.choice;
            lv_obj_set_style_bg_color(rows_[k].obj, lv_color_hex(cur ? col::kStrip : col::kRow), 0);
            lv_obj_set_style_border_color(rows_[k].obj, lv_color_hex(cur ? col::kHinge : col::kBodyEdge), 0);
        } else {
            const bool on = static_cast<int>(k) == i;
            lv_obj_set_style_bg_color(rows_[k].obj, lv_color_hex(on ? col::kStrip : col::kRow), 0);
            lv_obj_set_style_border_color(rows_[k].obj, lv_color_hex(on ? col::kHinge : col::kBodyEdge), 0);
        }
    }
    if (i >= 0 && i < static_cast<int>(rows_.size())) lv_obj_scroll_to_view(rows_[static_cast<std::size_t>(i)].obj, LV_ANIM_OFF);
}

void SettingsPage::activate(int i, int touch_x)
{
    if (i < 0 || i >= static_cast<int>(rows_.size())) return;
    if (picker_ >= 0) {
        const SettingRow &s = vm_.rows[static_cast<std::size_t>(picker_)];
        if (act_.setting_choice) act_.setting_choice(s.id, i);
        back();
        return;
    }
    const SettingRow &s = vm_.rows[static_cast<std::size_t>(i)];
    if (!s.enabled) return;
    switch (s.kind) {
    case SettingKind::Choice:
        picker_ = i;
        sel_ = -1;
        if (nav_.set_title) nav_.set_title(s.label);
        refresh();
        lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
        break;
    case SettingKind::Toggle:
        if (act_.setting_toggle) act_.setting_toggle(s.id, !s.on);
        break;
    case SettingKind::Slider: {
        Row &r = rows_[static_cast<std::size_t>(i)];
        if (touch_x < 0 || !r.slider) break;
        lv_area_t a;
        lv_obj_get_coords(r.slider, &a);
        const int knob = a.y2 - a.y1 + 1;
        const int w = a.x2 - a.x1 + 1 - knob;
        if (touch_x < a.x1 - knob / 2 || touch_x > a.x2 + knob / 2) break;
        const int pos = s.min + std::clamp((touch_x - a.x1 - knob / 2) * (s.max - s.min) / std::max(1, w) + 0, 0, s.max - s.min);
        set_slider_bar(r.slider, s.min, s.max, pos);
        set_text(r.value, std::to_string(pos) + " / " + std::to_string(s.max));
        if (act_.setting_slider && pos != s.pos) act_.setting_slider(s.id, pos);
        break;
    }
    case SettingKind::Text:
        if (nav_.edit_text) nav_.edit_text(s.id, s.label, s.value);
        break;
    }
}

bool SettingsPage::back()
{
    if (picker_ < 0) return false;
    const int was = picker_;
    picker_ = -1;
    sel_ = -1;
    if (nav_.set_title) nav_.set_title("Settings");
    refresh();
    if (was < static_cast<int>(rows_.size())) lv_obj_scroll_to_view(rows_[static_cast<std::size_t>(was)].obj, LV_ANIM_OFF);
    return true;
}

void SettingsPage::reset_view()
{
    if (picker_ >= 0) back();
    sel_ = -1;
    highlight(-1);
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    update_scrollbar();
    update_hint();
}

bool SettingsPage::on_key(std::uint32_t k)
{
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return false;
    if (k == LV_KEY_DOWN || k == LV_KEY_UP) {
        /* in a picker the highlight starts on the current choice */
        if (sel_ < 0 && picker_ >= 0 && picker_ < static_cast<int>(vm_.rows.size()))
            sel_ = vm_.rows[static_cast<std::size_t>(picker_)].choice;
        if (sel_ < 0) sel_ = k == LV_KEY_DOWN ? 0 : n - 1;
        else sel_ = std::clamp(sel_ + (k == LV_KEY_DOWN ? 1 : -1), 0, n - 1);
        highlight(sel_);
        return true;
    }
    if (k == LV_KEY_ENTER && sel_ >= 0) {
        activate(sel_, -1);
        return true;
    }
    return false;
}

bool SettingsPage::on_right()
{
    if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size())) return false;
    if (picker_ < 0 && sel_ < static_cast<int>(vm_.rows.size()) &&
        vm_.rows[static_cast<std::size_t>(sel_)].kind == SettingKind::Slider)
        return on_left_right(1);
    activate(sel_, -1);
    return true;
}

bool SettingsPage::on_left_right(int dir)
{
    if (picker_ >= 0 || sel_ < 0 || sel_ >= static_cast<int>(vm_.rows.size())) return false;
    const SettingRow &s = vm_.rows[static_cast<std::size_t>(sel_)];
    if (s.kind != SettingKind::Slider || !s.enabled) return false;
    const int pos = std::clamp(s.pos + dir, s.min, s.max);
    if (pos != s.pos && act_.setting_slider) act_.setting_slider(s.id, pos);
    return true;
}

void SettingsPage::row_cb(lv_event_t *e)
{
    auto *self = static_cast<SettingsPage *>(lv_event_get_user_data(e));
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p{-1, -1};
    if (indev) lv_indev_get_point(indev, &p);
    const lv_event_code_t code = lv_event_get_code(e);
    const bool slider_row = self->picker_ < 0 && i < static_cast<int>(self->rows_.size()) &&
                            self->rows_[static_cast<std::size_t>(i)].slider;
    if (code == LV_EVENT_PRESSED) {
        self->drag_row_ = -1;
        if (slider_row) {
            lv_area_t a;
            lv_obj_get_coords(self->rows_[static_cast<std::size_t>(i)].slider, &a);
            if (p.x >= a.x1 - 8 && p.x <= a.x2 + 8) self->drag_row_ = i;
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (self->drag_row_ == i && indev && !lv_indev_get_scroll_obj(indev)) {
            const SettingRow &s = self->vm_.rows[static_cast<std::size_t>(i)];
            Row &r = self->rows_[static_cast<std::size_t>(i)];
            lv_area_t a;
            lv_obj_get_coords(r.slider, &a);
            const int knob = a.y2 - a.y1 + 1, w = a.x2 - a.x1 + 1 - knob;
            const int pos = s.min + std::clamp((p.x - a.x1 - knob / 2) * (s.max - s.min) / std::max(1, w), 0, s.max - s.min);
            set_slider_bar(r.slider, s.min, s.max, pos);
            set_text(r.value, std::to_string(pos) + " / " + std::to_string(s.max));
        }
    } else if (code == LV_EVENT_RELEASED) {
        /* the drag's result is applied by CLICKED (a release without scrolling) */
    } else if (code == LV_EVENT_CLICKED) {
        self->sel_ = -1;
        self->highlight(-1);
        self->activate(i, slider_row ? p.x : -1);
        self->drag_row_ = -1;
    }
}

void SettingsPage::scroll_cb(lv_event_t *e)
{
    auto *self = static_cast<SettingsPage *>(lv_event_get_user_data(e));
    self->update_scrollbar();
    self->update_hint();
}

void SettingsPage::update_scrollbar()
{
    lv_obj_update_layout(list_);
    const int view = lv_obj_get_height(list_);
    const int content = view + lv_obj_get_scroll_bottom(list_) + lv_obj_get_scroll_y(list_);
    const bool need = content > view + 1;
    show(track_, need);
    show(thumb_, need);
    if (!need) return;
    const int th = lv_obj_get_style_height(track_, LV_PART_MAIN);
    const int h = std::max(fit_.px(24), th * view / content);
    const int range = std::max(1, content - view);
    const int y = lv_obj_get_style_y(track_, LV_PART_MAIN) + (th - h) * std::clamp(lv_obj_get_scroll_y(list_), 0, range) / range;
    lv_obj_set_height(thumb_, h);
    lv_obj_set_y(thumb_, y);
}

void SettingsPage::update_hint()
{
    if (!hint_) return;
    std::vector<std::string> below;
    if (picker_ < 0) {
        lv_area_t la;
        lv_obj_get_coords(list_, &la);
        for (std::size_t i = 0; i < rows_.size() && i < vm_.rows.size(); ++i) {
            lv_area_t a;
            lv_obj_get_coords(rows_[i].obj, &a);
            if (a.y2 > la.y2) below.push_back(vm_.rows[i].label);
        }
    }
    /* as many names as fit at the board's 14 px (Montserrat is wider than the board's face), then ", ..." */
    const lv_font_t *f = font(Face::Medium, fit_.px(14));
    const int w = lv_obj_get_style_width(hint_, LV_PART_MAIN);
    set_fitted(hint_, Face::Medium, fit_.px(14), 0, fit_.px(14),
               more_below_fit(below, [f, w](const std::string &t) { return text_width(f, t.c_str(), 0) <= w; }));
}

} // namespace ui
} // namespace cassette
