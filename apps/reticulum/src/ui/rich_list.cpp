/*
 * SPDX-License-Identifier: MIT
 *
 * RichList (see rich_list.hpp). The maths and the rules are cp0_lvgl's pure functions (cp0_ui_list.h): row tops from our
 * own heights, keys, taps and "keep the selection in view" from cp0_ui_list_key / _tap / _scroll_to_show.
 */
#include "rich_list.hpp"

#include "cp0_font_service.hpp"

#include <algorithm>
#include <cstdint>

namespace retui {

namespace {

using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;

const lv_font_t *font_px(int px, bool bold)
{
    return cp0_fonts().get(bold ? "Montserrat-Bold.ttf" : "Montserrat-Medium.ttf", static_cast<uint16_t>(px));
}

int text_w(const std::string &text, const lv_font_t *font)
{
    if (text.empty() || !font) return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

cp0_ui_list_tone_t tone_of(ValueTone t)
{
    switch (t) {
    case ValueTone::Ok: return CP0_UI_LIST_TONE_OK;
    case ValueTone::Warn: return CP0_UI_LIST_TONE_WARN;
    case ValueTone::Bad: return CP0_UI_LIST_TONE_BAD;
    case ValueTone::Muted: return CP0_UI_LIST_TONE_MUTED;
    default: return CP0_UI_LIST_TONE_NORMAL;
    }
}

cp0_ui_list_affordance_t aff_of(RowAffordance a)
{
    switch (a) {
    case RowAffordance::Info: return CP0_UI_LIST_AFFORDANCE_INFO;
    case RowAffordance::Open: return CP0_UI_LIST_AFFORDANCE_OPEN;
    case RowAffordance::Edit: return CP0_UI_LIST_AFFORDANCE_EDIT;
    case RowAffordance::Action: return CP0_UI_LIST_AFFORDANCE_ACTION;
    case RowAffordance::ActionRunning: return CP0_UI_LIST_AFFORDANCE_ACTION_RUNNING;
    default: return CP0_UI_LIST_AFFORDANCE_UNSPECIFIED;
    }
}

cp0_ui_list_key_t list_key(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP: return CP0_UI_LIST_KEY_UP;
    case LV_KEY_DOWN: return CP0_UI_LIST_KEY_DOWN;
    case LV_KEY_LEFT: return CP0_UI_LIST_KEY_LEFT;
    case LV_KEY_RIGHT: return CP0_UI_LIST_KEY_RIGHT;
    case LV_KEY_ENTER: return CP0_UI_LIST_KEY_ENTER;
    case LV_KEY_ESC: return CP0_UI_LIST_KEY_ESC;
    case LV_KEY_HOME: return CP0_UI_LIST_KEY_HOME;
    case LV_KEY_END: return CP0_UI_LIST_KEY_END;
    case LV_KEY_PREV: return CP0_UI_LIST_KEY_PAGE_UP;
    case LV_KEY_NEXT: return CP0_UI_LIST_KEY_PAGE_DOWN;
    default: return CP0_UI_LIST_KEY_OTHER;
    }
}

std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

} // namespace

bool RichRow::same(const RichRow &o) const
{
    return kind == o.kind && panel == o.panel && label == o.label && value == o.value && tone == o.tone && time == o.time && sub == o.sub &&
           sub_tone == o.sub_tone && bold == o.bold && badge == o.badge && affordance == o.affordance && id == o.id;
}

RichList::RichList(lv_obj_t *parent, const cp0_ui_list_layout_t &layout, Activate activate, std::function<void()> back)
    : layout_(layout), activate_(std::move(activate)), back_(std::move(back))
{
    list_ = lv_obj_create(parent);
    lv_obj_remove_style_all(list_);
    if (lv_obj_get_group(list_)) lv_group_remove_obj(list_); /* the page adds it to its own group */
    lv_obj_set_pos(list_, layout_.x, layout_.y);
    lv_obj_set_size(list_, layout_.w, layout_.h);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list_, layout_.scrollbar_w, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(list_, layout_.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list_, lv_color_hex(CP0_UI_LIST_SCROLLBAR_RGB), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(list_, list_event_cb, LV_EVENT_KEY, this);
    lv_obj_add_event_cb(list_, list_delete_cb, LV_EVENT_DELETE, this);
    tops_.assign(1, 0);

    const lv_font_t *t = font_px(layout_.text_px, false), *s = font_px(layout_.value_px, false);
    const int pad = std::max(2, layout_.gap / 2);
    const int lt = t ? lv_font_get_line_height(t) : layout_.row_h / 2;
    const int ls = s ? lv_font_get_line_height(s) : layout_.row_h / 3;
    two_h_ = std::max(layout_.row_h, pad + lt + std::max(1, layout_.gap / 4) + ls + pad);
}

RichList::~RichList()
{
    if (list_) {
        lv_obj_remove_event_cb_with_user_data(list_, list_delete_cb, this);
        lv_obj_remove_event_cb_with_user_data(list_, list_event_cb, this);
        lv_obj_delete(list_);
    }
}

void RichList::list_delete_cb(lv_event_t *event)
{
    auto *self = static_cast<RichList *>(lv_event_get_user_data(event));
    if (!self) return;
    self->list_ = nullptr;
    self->objs_.clear();
}

const lv_font_t *RichList::block_font(const RichRow &row) const
{
    return row.panel ? font_px(layout_.value_px, false) : font_px(layout_.text_px, true);
}

/* The width the text of a Block may take: the inset on both sides, and inside the frame of a panel. */
int RichList::block_text_w(const RichRow &row) const
{
    const int w = layout_.w - 2 * layout_.inset;
    return std::max(1, row.panel ? w - 2 * layout_.gap - 4 : w);
}

/* The lines of a Block. A note (panel) is one wrapped text. The address is bold with 1 px between letters, and the label
 * wraps wherever the width ends (the last group alone on a second line, 480x320, board QA of 0.3.2): when it does not fit one
 * line it is cut in two at the space nearest the middle, so that the groups come in two even lines. Every line is its own
 * label: the height is measured with the same letter space the label draws with (it was not, so the second line was never
 * counted and the row under it covered it). */
std::vector<std::string> RichList::block_lines(const RichRow &row) const
{
    const lv_font_t *f = block_font(row);
    if (row.panel || !f) return {row.label};
    const int ls = 1;
    lv_point_t one{};
    lv_text_get_size(&one, row.label.c_str(), f, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (one.x <= block_text_w(row)) return {row.label};
    std::size_t best = std::string::npos;
    const std::size_t mid = row.label.size() / 2;
    for (std::size_t i = row.label.find(' '); i != std::string::npos; i = row.label.find(' ', i + 1))
        if (best == std::string::npos || (i > mid ? i - mid : mid - i) < (best > mid ? best - mid : mid - best)) best = i;
    if (best == std::string::npos) return {row.label};
    return {row.label.substr(0, best), row.label.substr(best + 1)};
}

int RichList::height_of(const RichRow &row) const
{
    switch (row.kind) {
    case RichRow::Kind::Section: return layout_.section_h;
    case RichRow::Kind::Two: return two_h_;
    case RichRow::Kind::Block: {
        const lv_font_t *f = block_font(row);
        const int pad = std::max(2, layout_.gap / 2);
        int text_h = 0;
        for (const std::string &line : block_lines(row)) {
            lv_point_t size{};
            if (f) lv_text_get_size(&size, line.c_str(), f, row.panel ? 0 : 1, 0, block_text_w(row), LV_TEXT_FLAG_NONE);
            text_h += size.y;
        }
        return row.panel ? text_h + 2 * layout_.gap + 4 + 2 * pad : text_h + 2 * pad;
    }
    default: return layout_.row_h;
    }
}

void RichList::build_row(int index, int top, int h)
{
    const RichRow &r = rows_[static_cast<std::size_t>(index)];
    lv_obj_t *row = lv_obj_create(list_);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(row, 0, top);
    lv_obj_set_size(row, layout_.w, h);
    lv_obj_set_style_bg_color(row, lv_color_hex(CP0_UI_LIST_BAR_RGB), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    objs_[static_cast<std::size_t>(index)].row = row;

    if (r.kind == RichRow::Kind::Block) {
        const lv_font_t *f = block_font(r);
        const int pad = std::max(2, layout_.gap / 2);
        auto make_label = [&](const std::string &text) {
            lv_obj_t *l = lv_label_create(row);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_font(l, f, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(CP0_UI_LIST_TEXT_RGB), 0);
            if (!r.panel) lv_obj_set_style_text_letter_space(l, 1, 0);
            lv_label_set_text(l, text.c_str());
            return l;
        };
        if (r.panel) {
            lv_obj_t *l = make_label(r.label);
            lv_obj_t *box = lv_obj_create(row);
            lv_obj_remove_style_all(box);
            lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(box, layout_.inset, pad);
            lv_obj_set_size(box, layout_.w - 2 * layout_.inset, h - 2 * pad);
            lv_obj_set_style_border_width(box, 2, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(CP0_UI_LIST_WARN_RGB), 0);
            lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(box, layout_.gap, 0);
            lv_obj_set_parent(l, box);
            lv_obj_set_width(l, block_text_w(r));
            lv_obj_set_pos(l, layout_.gap - 2, layout_.gap - 2);
        } else {
            int y = pad;
            for (const std::string &line : block_lines(r)) {
                lv_obj_t *l = make_label(line);
                lv_obj_set_width(l, block_text_w(r));
                lv_obj_set_pos(l, layout_.inset, y);
                lv_obj_update_layout(l);
                y += lv_obj_get_height(l);
            }
        }
        return;
    }
    const bool section = r.kind == RichRow::Kind::Section;
    if (section) {
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(l, font_px(layout_.section_px, false), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_label_set_text(l, upper(r.label).c_str());
        lv_obj_set_width(l, layout_.w - 2 * layout_.inset);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, layout_.inset, 0);
        return;
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, this);

    const cp0_ui_list_affordance_t aff = aff_of(r.affordance);
    const int trail = cp0_ui_list_trail_w(&layout_, aff);
    const lv_font_t *tf = font_px(layout_.text_px, r.bold);
    const lv_font_t *vf = font_px(layout_.value_px, false);
    const int right = layout_.w - layout_.inset - trail; /* where text and values end */
    const unsigned label_rgb = cp0_ui_list_label_rgb(aff);

    if (r.kind == RichRow::Kind::Item) {
        const int vw = text_w(r.value, vf);
        if (vw > 0) {
            lv_obj_t *v = lv_label_create(row);
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_font(v, vf, 0);
            lv_obj_set_style_text_color(v, lv_color_hex(cp0_ui_list_value_rgb(tone_of(r.tone), aff)), 0);
            lv_label_set_text(v, r.value.c_str());
            const int room = std::max(0, layout_.w - 2 * layout_.inset - trail);
            lv_obj_set_width(v, std::min(vw, room) + 1);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -(layout_.inset + trail), 0);
        }
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(l, tf, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(label_rgb), 0);
        lv_label_set_text(l, r.label.c_str());
        objs_[static_cast<std::size_t>(index)].label = l;
        lv_obj_set_width(l, cp0_ui_list_label_w(layout_.w - trail, layout_.inset, layout_.gap, vw));
        lv_obj_set_height(l, tf ? lv_font_get_line_height(tf) : h);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, layout_.inset, 0);
    } else {
        const int pad = std::max(2, layout_.gap / 2);
        const int lt = tf ? lv_font_get_line_height(tf) : h / 2;
        const int ls = vf ? lv_font_get_line_height(vf) : h / 3;
        const int tw = text_w(r.time, vf);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(name, tf, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(label_rgb), 0);
        lv_label_set_text(name, r.label.c_str());
        objs_[static_cast<std::size_t>(index)].label = name;
        lv_obj_set_size(name, std::max(1, right - layout_.inset - (tw ? tw + layout_.gap : 0)), lt);
        lv_obj_set_pos(name, layout_.inset, pad);
        if (tw) {
            lv_obj_t *t = lv_label_create(row);
            lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_font(t, vf, 0);
            lv_obj_set_style_text_color(t, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
            lv_label_set_text(t, r.time.c_str());
            lv_obj_set_size(t, tw + 1, ls);
            lv_obj_set_pos(t, right - tw, pad + std::max(0, (lt - ls) / 2));
        }
        int badge_w = 0;
        const int y2 = pad + lt + std::max(1, layout_.gap / 4);
        if (r.badge > 0) {
            const std::string txt = std::to_string(r.badge);
            const int bh = ls;
            badge_w = std::max(bh, text_w(txt, vf) + layout_.gap);
            lv_obj_t *b = lv_obj_create(row);
            lv_obj_remove_style_all(b);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(b, badge_w, bh);
            lv_obj_set_pos(b, right - badge_w, y2);
            lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(CP0_UI_LIST_WARN_RGB), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_t *bl = lv_label_create(b);
            lv_obj_set_style_text_font(bl, font_px(layout_.value_px, true), 0);
            lv_obj_set_style_text_color(bl, lv_color_hex(0x000000), 0);
            lv_label_set_text(bl, txt.c_str());
            lv_obj_center(bl);
        }
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(sub, vf, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(cp0_ui_list_value_rgb(tone_of(r.sub_tone), CP0_UI_LIST_AFFORDANCE_INFO)), 0);
        if (r.sub_tone == ValueTone::Normal) lv_obj_set_style_text_color(sub, lv_color_hex(0xE6E6E6), 0);
        lv_label_set_text(sub, r.sub.c_str());
        lv_obj_set_size(sub, std::max(1, right - layout_.inset - (badge_w ? badge_w + layout_.gap : 0)), ls);
        lv_obj_set_pos(sub, layout_.inset, y2);
    }

    if (cp0_ui_list_has_chevron(aff)) {
        const lv_font_t *font = font_px(cp0_ui_list_chevron_px(&layout_), false);
        const int box_w = cp0_ui_list_chevron_w(&layout_);
        const char *text = CP0_UI_LIST_CHEVRON_TEXT;
        lv_font_glyph_dsc_t glyph{};
        int line_h = h, ink_top = 0, ink_h = h;
        if (font) {
            if (!lv_font_get_glyph_dsc(font, &glyph, CP0_UI_LIST_CHEVRON_CODEPOINT, 0)) {
                text = CP0_UI_LIST_CHEVRON_FALLBACK;
                lv_font_get_glyph_dsc(font, &glyph, static_cast<uint32_t>(CP0_UI_LIST_CHEVRON_FALLBACK[0]), 0);
            }
            line_h = lv_font_get_line_height(font);
            ink_top = (line_h - font->base_line) - glyph.box_h - glyph.ofs_y;
            ink_h = glyph.box_h;
        }
        lv_obj_t *c = lv_label_create(row);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(c, text);
        if (font) lv_obj_set_style_text_font(c, font, 0);
        lv_obj_set_style_text_color(c, lv_color_hex(CP0_UI_LIST_CHEVRON_RGB), 0);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(c, box_w, line_h);
        lv_obj_set_pos(c, layout_.w - layout_.inset - box_w, cp0_ui_list_chevron_y(h, ink_top, ink_h));
    }
    if (cp0_ui_list_has_dot(aff)) {
        const int d = cp0_ui_list_dot_px(&layout_);
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(cp0_ui_list_dot_rgb(aff)), 0);
        lv_obj_set_size(dot, d, d);
        lv_obj_set_pos(dot, cp0_ui_list_dot_x(&layout_, layout_.w), cp0_ui_list_chevron_y(h, 0, d));
    }
}

/* The highlight bar behind the selected item (its name goes bold as in CompactList: by rebuilding it). */
void RichList::style_selected(int index)
{
    if (index < 0 || index >= count() || !objs_[static_cast<std::size_t>(index)].row) return;
    const Obj &o = objs_[static_cast<std::size_t>(index)];
    const RichRow &r = rows_[static_cast<std::size_t>(index)];
    lv_obj_set_style_bg_opa(o.row, index == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (o.label) lv_obj_set_style_text_font(o.label, font_px(layout_.text_px, index == selected_ || r.bold), 0);
}

void RichList::rebuild()
{
    if (!list_) return;
    lv_obj_clean(list_);
    const int n = count();
    objs_.assign(static_cast<std::size_t>(n), Obj{});
    kinds_.assign(static_cast<std::size_t>(n), CP0_UI_LIST_ITEM);
    tops_.assign(static_cast<std::size_t>(n) + 1, 0);
    int y = 0;
    for (int i = 0; i < n; ++i) {
        const RichRow &r = rows_[static_cast<std::size_t>(i)];
        kinds_[static_cast<std::size_t>(i)] =
            r.kind == RichRow::Kind::Section || r.kind == RichRow::Kind::Block ? CP0_UI_LIST_SECTION : CP0_UI_LIST_ITEM;
        tops_[static_cast<std::size_t>(i)] = y;
        const int h = height_of(r);
        build_row(i, y, h);
        y += h;
    }
    tops_[static_cast<std::size_t>(n)] = y;
    style_selected(selected_);
}

cp0_ui_list_state_t RichList::state() const
{
    cp0_ui_list_state_t s{};
    s.kinds = kinds_.data();
    s.tops = tops_.data();
    s.count = count();
    s.view_h = layout_.h;
    s.row_h = layout_.row_h;
    s.selected = selected_;
    s.scroll_y = list_ ? static_cast<int>(lv_obj_get_scroll_y(list_)) : 0;
    return s;
}

void RichList::set_rows(const std::vector<RichRow> &rows)
{
    if (!list_) return;
    bool same = rows.size() == rows_.size();
    for (std::size_t i = 0; same && i < rows.size(); ++i) same = rows[i].same(rows_[i]);
    if (same) return;
    const int scroll = static_cast<int>(lv_obj_get_scroll_y(list_));
    rows_ = rows;
    rebuild();
    lv_obj_update_layout(list_);
    cp0_ui_list_state_t s = state();
    s.scroll_y = scroll;
    const cp0_ui_list_result_t keep = cp0_ui_list_keep(&s);
    const int previous = selected_;
    selected_ = keep.index;
    if (previous != selected_) style_selected(previous);
    style_selected(selected_);
    const int max = cp0_ui_list_max_scroll(tops_.back(), layout_.h);
    lv_obj_scroll_to_y(list_, std::min(scroll, max), LV_ANIM_OFF);
}

void RichList::reset_rows(const std::vector<RichRow> &rows, int selected)
{
    if (!list_) return;
    rows_ = rows;
    selected_ = -1;
    rebuild();
    lv_obj_update_layout(list_);
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    if (rows_.empty()) return;
    const int target = cp0_ui_list_step(kinds_.data(), count(), std::clamp(selected, 0, count() - 1), 0, 0);
    if (target < 0) return;
    selected_ = target;
    style_selected(target);
    const int y = cp0_ui_list_scroll_to_show(tops_.data(), count(), target, 0, layout_.h, 1);
    lv_obj_scroll_to_y(list_, y, LV_ANIM_OFF);
}

void RichList::move_to(int index, int scroll_y)
{
    const int previous = selected_;
    selected_ = index;
    style_selected(previous);
    style_selected(selected_);
    lv_obj_scroll_to_y(list_, scroll_y, LV_ANIM_OFF);
}

void RichList::on_key(uint32_t key)
{
    const cp0_ui_list_state_t s = state();
    const cp0_ui_list_result_t r = cp0_ui_list_key(&s, list_key(key));
    switch (r.action) {
    case CP0_UI_LIST_MOVE:
    case CP0_UI_LIST_SCROLL:
        if (r.action == CP0_UI_LIST_MOVE) move_to(r.index, r.scroll_y);
        else lv_obj_scroll_to_y(list_, r.scroll_y, LV_ANIM_OFF);
        break;
    case CP0_UI_LIST_ACTIVATE:
        if (activate_) activate_(r.index, rows_[static_cast<std::size_t>(r.index)].id);
        break;
    case CP0_UI_LIST_BACK:
        if (back_) back_();
        break;
    case CP0_UI_LIST_NONE: break;
    }
}

void RichList::on_tap(int index)
{
    const cp0_ui_list_state_t s = state();
    const cp0_ui_list_result_t r = cp0_ui_list_tap(&s, index, 0);
    if (r.action == CP0_UI_LIST_MOVE) move_to(r.index, r.scroll_y);
    else if (r.action == CP0_UI_LIST_ACTIVATE && activate_)
        activate_(r.index, rows_[static_cast<std::size_t>(r.index)].id); /* may replace the rows: last */
}

void RichList::list_event_cb(lv_event_t *event)
{
    auto *self = static_cast<RichList *>(lv_event_get_user_data(event));
    if (!self || !self->list_) return;
    if (lv_event_get_code(event) == LV_EVENT_KEY) {
        lv_event_stop_processing(event);
        self->on_key(lv_event_get_key(event));
    }
}

void RichList::row_event_cb(lv_event_t *event)
{
    auto *self = static_cast<RichList *>(lv_event_get_user_data(event));
    auto *row = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (!self || !row || !self->list_) return;
    self->on_tap(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))));
}

} // namespace retui
