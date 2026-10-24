/*
 * SPDX-License-Identifier: MIT
 */
#include "row_list.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_osk.h"

#include <algorithm>

namespace notesui {

namespace {

constexpr std::uint32_t kBackground = 0x000000;
constexpr std::uint32_t kSelected = CP0_UI_LIST_BAR_RGB;
constexpr std::uint32_t kPressed = CP0_UI_OSK_KEY_PRESSED_RGB;
constexpr std::uint32_t kAccent = CP0_UI_LIST_ACCENT_RGB;
constexpr std::uint32_t kSectionBg = 0x000000;

void set_hidden_obj(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    return b;
}

lv_point_t event_point(lv_event_t *e)
{
    lv_point_t p = {0, 0};
    if (lv_indev_t *indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    return p;
}

} // namespace

const lv_font_t *mark_font(int px)
{
    if (const lv_font_t *f = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

void style_list_scrollbar(lv_obj_t *obj)
{
    cp0_ui_list_layout_t l{};
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMPACT, 0, 0, 100, 100, &l);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(obj, l.scrollbar_w, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, l.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(obj, l.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(obj, l.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(CP0_UI_LIST_SCROLLBAR_RGB), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
}

void RowList::row_event_cb(lv_event_t *e)
{
    auto *self = static_cast<RowList *>(lv_event_get_user_data(e));
    if (!self) return;
    lv_obj_t *target = lv_event_get_current_target_obj(e);
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    const lv_point_t p = event_point(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        if (self->on_click) self->on_click(index);
        break;
    case LV_EVENT_PRESSED:
        if (self->on_press) self->on_press(0, index, p.x, p.y);
        break;
    case LV_EVENT_PRESSING:
        if (self->on_press) self->on_press(1, index, p.x, p.y);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (self->on_press) self->on_press(2, index, p.x, p.y);
        break;
    default: break;
    }
}

void RowList::scroll_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SCROLL) return;
    static_cast<RowList *>(lv_event_get_user_data(e))->update_window();
}

void RowList::create(lv_obj_t *parent, int x, int y, int w, int h, const Config &cfg)
{
    cfg_ = cfg;
    w_ = w;
    h_ = h;
    const lv_font_t *chev_font = mark_font(cp0_ui_list_chevron_px(&cfg.marks));
    const char *chev_text = CP0_UI_LIST_CHEVRON_TEXT;
    lv_font_glyph_dsc_t glyph{};
    if (!lv_font_get_glyph_dsc(chev_font, &glyph, CP0_UI_LIST_CHEVRON_CODEPOINT, 0)) {
        chev_text = CP0_UI_LIST_CHEVRON_FALLBACK;
        lv_font_get_glyph_dsc(chev_font, &glyph, static_cast<uint32_t>(CP0_UI_LIST_CHEVRON_FALLBACK[0]), 0);
    }
    chev_line_h_ = static_cast<int>(lv_font_get_line_height(chev_font));
    chev_ink_top_ = (chev_line_h_ - chev_font->base_line) - glyph.box_h - glyph.ofs_y;
    chev_ink_h_ = glyph.box_h;

    cont_ = lv_obj_create(parent);
    lv_obj_remove_style_all(cont_);
    lv_obj_set_pos(cont_, x, y);
    lv_obj_set_size(cont_, w, h);
    lv_obj_set_scroll_dir(cont_, LV_DIR_VER);
    lv_obj_remove_flag(cont_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(cont_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    style_list_scrollbar(cont_);
    lv_obj_add_event_cb(cont_, scroll_event_cb, LV_EVENT_SCROLL, this);
    spacer_ = box(cont_, 0, 0, w, 1);
    lv_obj_remove_flag(spacer_, LV_OBJ_FLAG_CLICKABLE);
    empty_ = lv_label_create(cont_);
    lv_label_set_long_mode(empty_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(empty_, cfg.marks.inset, cfg.marks.inset / 2);
    lv_obj_set_width(empty_, std::max(1, w - 2 * cfg.marks.inset));
    lv_obj_remove_flag(empty_, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(empty_, "");
    set_hidden_obj(empty_, true);

    const int pool = (h + std::max(1, cfg.min_row_h) - 1) / std::max(1, cfg.min_row_h) + 4;
    pool_.resize(static_cast<std::size_t>(pool));
    const int d = cp0_ui_list_dot_px(&cfg.marks);
    for (Slot &s : pool_) {
        s.row = box(cont_, 0, 0, w, cfg.min_row_h);
        lv_obj_set_style_bg_opa(s.row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s.row, lv_color_hex(kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_radius(s.row, cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S], 0);
        for (lv_event_code_t code : {LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(s.row, row_event_cb, code, this);
        for (int k = 0; k < cfg.max_cells; ++k) {
            lv_obj_t *l = lv_label_create(s.row);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            set_hidden_obj(l, true);
            s.labels.push_back(l);
        }
        s.lead = box(s.row, 0, 0, d, d);
        lv_obj_remove_flag(s.lead, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(s.lead, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s.lead, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(s.lead, LV_OPA_COVER, 0);
        set_hidden_obj(s.lead, true);
        s.chevron = lv_label_create(s.row);
        lv_obj_remove_flag(s.chevron, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(s.chevron, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(s.chevron, chev_text);
        lv_obj_set_style_text_font(s.chevron, chev_font, 0);
        lv_obj_set_style_text_color(s.chevron, lv_color_hex(CP0_UI_LIST_CHEVRON_RGB), 0);
        lv_obj_set_style_text_align(s.chevron, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(s.chevron, cp0_ui_list_chevron_w(&cfg.marks), std::max(1, chev_line_h_));
        set_hidden_obj(s.chevron, true);
        s.dot = box(s.row, 0, 0, d, d);
        lv_obj_remove_flag(s.dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(s.dot, LV_RADIUS_CIRCLE, 0);
        set_hidden_obj(s.dot, true);
        hide(s);
    }
}

void RowList::set_y(int y)
{
    if (cont_) lv_obj_set_y(cont_, y);
}

void RowList::set_hidden(bool hidden)
{
    set_hidden_obj(cont_, hidden);
}

void RowList::scroll_by(int dy)
{
    if (!cont_) return;
    const int max_scroll = std::max(0, total_ - h_);
    const int to = std::clamp(static_cast<int>(lv_obj_get_scroll_y(cont_)) + dy, 0, max_scroll);
    lv_obj_scroll_to_y(cont_, to, LV_ANIM_OFF);
    update_window();
}

void RowList::resize(int h)
{
    if (!cont_ || h == h_) return;
    h_ = h;
    lv_obj_set_height(cont_, h);
    lv_obj_update_layout(cont_);
    const int max_scroll = std::max(0, total_ - h_);
    if (lv_obj_get_scroll_y(cont_) > max_scroll) lv_obj_scroll_to_y(cont_, max_scroll, LV_ANIM_OFF);
    update_window();                 // (the pool was sized at create() for the largest room: a list only gets shorter)
}

void RowList::hide(Slot &s)
{
    s.index = -1;
    set_hidden_obj(s.row, true);
}

void RowList::bind(Slot &s, int i)
{
    const Row &r = rows_[static_cast<std::size_t>(i)];
    s.index = i;
    set_hidden_obj(s.row, false);
    lv_obj_set_pos(s.row, 0, off_[static_cast<std::size_t>(i)]);
    lv_obj_set_size(s.row, w_, r.h);
    lv_obj_set_user_data(s.row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    const bool sel = i == selected_ && r.selectable && !r.section;
    lv_obj_set_style_bg_opa(s.row, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s.row, lv_color_hex(sel ? kSelected : r.section ? kSectionBg : kBackground), 0);
    if (r.selectable && !r.section) lv_obj_add_flag(s.row, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(s.row, LV_OBJ_FLAG_CLICKABLE);
    std::size_t used = 0;
    for (const Cell &c : r.cells) {
        if (c.text.empty()) continue;
        if (used >= s.labels.size()) break;
        lv_obj_t *l = s.labels[used++];
        const lv_font_t *f = c.font ? c.font : cp0_ui_font(CP0_TEXT_BODY);
        const int lh = static_cast<int>(lv_font_get_line_height(f));
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(c.color), 0);
        lv_obj_set_style_text_align(l, c.align, 0);
        lv_label_set_long_mode(l, c.lines > 1 ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, std::max(1, c.w));
        lv_obj_set_height(l, lh * std::max(1, c.lines));
        lv_obj_set_pos(l, c.x, c.y);
        lv_label_set_text(l, c.text.c_str());
        set_hidden_obj(l, false);
    }
    for (std::size_t k = used; k < s.labels.size(); ++k) set_hidden_obj(s.labels[k], true);
    const int d = cp0_ui_list_dot_px(&cfg_.marks);
    if (r.lead_dot) lv_obj_set_pos(s.lead, cfg_.lead_x, cp0_ui_list_chevron_y(r.h, 0, d));
    set_hidden_obj(s.lead, !r.lead_dot);
    const cp0_ui_list_layout_t &ml = cfg_.marks;
    const bool chev = r.trail == Mark::Chevron;
    if (chev) lv_obj_set_pos(s.chevron, w_ - ml.inset - cp0_ui_list_chevron_w(&ml), cp0_ui_list_chevron_y(r.h, chev_ink_top_, chev_ink_h_));
    set_hidden_obj(s.chevron, !chev);
    const bool dot = r.trail == Mark::Dot || r.trail == Mark::DotRunning || r.trail == Mark::Ring || r.trail == Mark::Filled;
    if (dot) {
        const bool ring = r.trail == Mark::Ring;
        const int dd = ring ? d + 2 : d;
        lv_obj_set_size(s.dot, dd, dd);
        lv_obj_set_pos(s.dot, cp0_ui_list_dot_x(&ml, w_) - (dd - d) / 2, cp0_ui_list_chevron_y(r.h, 0, dd));
        const std::uint32_t fill = r.trail == Mark::Dot ? cp0_ui_list_dot_rgb(CP0_UI_LIST_AFFORDANCE_ACTION)
                                                        : cp0_ui_list_dot_rgb(CP0_UI_LIST_AFFORDANCE_ACTION_RUNNING);
        lv_obj_set_style_bg_color(s.dot, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(s.dot, ring ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s.dot, ring ? std::max(2, d / 7) : 0, 0);
        lv_obj_set_style_border_color(s.dot, lv_color_hex(CP0_UI_LIST_CHEVRON_RGB), 0);
        lv_obj_set_style_border_opa(s.dot, LV_OPA_COVER, 0);
    }
    set_hidden_obj(s.dot, !dot);
}

void RowList::update_window()
{
    const int n = static_cast<int>(rows_.size());
    if (n == 0 || !cont_) {
        for (Slot &s : pool_) hide(s);
        return;
    }
    const int y0 = lv_obj_get_scroll_y(cont_);
    int first = static_cast<int>(std::upper_bound(off_.begin(), off_.end(), y0) - off_.begin()) - 1;
    first = std::max(first, 0);
    int last = static_cast<int>(std::lower_bound(off_.begin(), off_.end(), y0 + h_) - off_.begin());
    last = std::min(last, n - 1);
    const int lo = std::max(0, first - 1), hi = std::min(n - 1, last + 1);
    std::vector<char> have(static_cast<std::size_t>(hi - lo + 1), 0);
    std::vector<Slot *> spare;
    for (Slot &s : pool_) {
        if (s.index >= lo && s.index <= hi) have[static_cast<std::size_t>(s.index - lo)] = 1;
        else spare.push_back(&s);
    }
    std::size_t next = 0;
    for (int i = lo; i <= hi; ++i) {
        if (have[static_cast<std::size_t>(i - lo)]) continue;
        if (next >= spare.size()) break;
        bind(*spare[next++], i);
    }
    for (; next < spare.size(); ++next)
        if (spare[next]->index >= 0) hide(*spare[next]);
}

void RowList::set(std::vector<Row> rows, int selected)
{
    if (!cont_) return;
    const int old_scroll = lv_obj_get_scroll_y(cont_);
    rows_ = std::move(rows);
    selected_ = selected;
    off_.assign(rows_.size(), 0);
    total_ = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        off_[i] = total_;
        total_ += rows_[i].h;
    }
    lv_obj_set_size(spacer_, w_, std::max(total_, 1));
    set_hidden_obj(empty_, !rows_.empty() || lv_label_get_text(empty_)[0] == 0);
    for (Slot &s : pool_) hide(s);
    lv_obj_update_layout(cont_);
    lv_obj_scroll_to_y(cont_, std::min(old_scroll, std::max(0, total_ - h_)), LV_ANIM_OFF);
    update_window();
}

void RowList::set_selected(int selected, bool scroll)
{
    if (!cont_) return;
    const int old = selected_;
    selected_ = selected;
    for (Slot &s : pool_)
        if (s.index >= 0 && (s.index == old || s.index == selected)) bind(s, s.index);
    if (scroll) scroll_to_selected();
}

void RowList::scroll_to_selected()
{
    const int n = static_cast<int>(rows_.size());
    if (selected_ < 0 || selected_ >= n || !cont_) return;
    const int y0 = lv_obj_get_scroll_y(cont_);
    // the selected row whole, and the row above / below it when there is room (one row of context, CompactList's rule)
    const int i = selected_;
    int top = off_[static_cast<std::size_t>(i)], bot = top + rows_[static_cast<std::size_t>(i)].h;
    const int ctx_top = i > 0 ? off_[static_cast<std::size_t>(i - 1)] : top;
    const int ctx_bot = i + 1 < n ? off_[static_cast<std::size_t>(i + 1)] + rows_[static_cast<std::size_t>(i + 1)].h : bot;
    if (ctx_bot - ctx_top <= h_) {
        top = ctx_top;
        bot = ctx_bot;
    }
    int target = y0;
    if (top < y0) target = top;
    else if (bot > y0 + h_) target = bot - h_;
    target = std::clamp(target, 0, std::max(0, total_ - h_));
    if (target != y0) lv_obj_scroll_to_y(cont_, target, LV_ANIM_OFF);
    update_window();
}

void RowList::scroll_to_top()
{
    if (!cont_) return;
    lv_obj_scroll_to_y(cont_, 0, LV_ANIM_OFF);
    update_window();
}

void RowList::set_empty_text(const std::string &text, const lv_font_t *font, std::uint32_t color)
{
    if (!empty_) return;
    lv_label_set_text(empty_, text.c_str());
    if (font) lv_obj_set_style_text_font(empty_, font, 0);
    lv_obj_set_style_text_color(empty_, lv_color_hex(color), 0);
    set_hidden_obj(empty_, !rows_.empty() || text.empty());
}

const std::string &RowList::id_at(int index) const
{
    static const std::string none;
    if (index < 0 || index >= static_cast<int>(rows_.size())) return none;
    return rows_[static_cast<std::size_t>(index)].id;
}

const Row *RowList::row(int index) const
{
    if (index < 0 || index >= static_cast<int>(rows_.size())) return nullptr;
    return &rows_[static_cast<std::size_t>(index)];
}

int RowList::index_of(const std::string &id) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].id == id) return static_cast<int>(i);
    return -1;
}

int RowList::bound_rows() const
{
    int n = 0;
    for (const Slot &s : pool_)
        if (s.index >= 0) ++n;
    return n;
}

} // namespace notesui
