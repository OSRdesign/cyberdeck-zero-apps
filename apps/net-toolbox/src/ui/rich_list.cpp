/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: RichList and ActionBar (see rich_list.hpp).
 */
#include "rich_list.hpp"

#include "style.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>

namespace nt {
namespace ui {

namespace {

constexpr std::uint32_t kBarFocused = CP0_UI_LIST_BAR_RGB; /* the selected row, the list has the focus */
constexpr std::uint32_t kBarIdle = 0x202020;               /* the selected row, the focus is elsewhere */

cp0_ui_list_key_t list_key(std::uint32_t key)
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
    default: return CP0_UI_LIST_KEY_OTHER;
    }
}

std::uint32_t value_rgb(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return CP0_UI_LIST_OK_RGB;
    case Tone::Warn: return CP0_UI_LIST_WARN_RGB;
    case Tone::Bad: return CP0_UI_LIST_BAD_RGB;
    default: return CP0_UI_LIST_VALUE_RGB;
    }
}

constexpr std::uint32_t kKeyTab = 15; /* the keyboard bridge hands Tab over as its evdev code (KEY_TAB): LVGL's own focus step is not used */

std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

} // namespace

/* ---------------------------------------------------------------- rows */

RichRow RichRow::item(std::string label, std::string value, Tone tone, int id)
{
    RichRow r;
    r.kind = Kind::Item;
    r.label = std::move(label);
    r.value = std::move(value);
    r.tone = tone;
    r.id = id;
    return r;
}

RichRow RichRow::section(std::string label)
{
    RichRow r;
    r.kind = Kind::Section;
    r.label = std::move(label);
    r.selectable = false;
    return r;
}

RichRow RichRow::text(std::string text, Tone tone, bool selectable, bool small, bool cut)
{
    RichRow r;
    r.kind = Kind::Text;
    r.value = std::move(text);
    r.tone = tone;
    r.selectable = selectable;
    r.small = small;
    r.cut = cut;
    return r;
}

RichRow RichRow::custom(std::shared_ptr<RowView> view, int height, bool selectable, int id)
{
    RichRow r;
    r.kind = Kind::Custom;
    r.view = std::move(view);
    r.height = height;
    r.selectable = selectable;
    r.id = id;
    return r;
}

/* ---------------------------------------------------------------- RichList */

RichList::RichList(lv_obj_t *parent, int x, int y, int w, int h, RichListEvents events) : events_(std::move(events))
{
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMPACT, x, y, w, std::max(1, h), &layout_);
    pad_ = std::max(4, layout_.gap / 2);
    list_ = lv_obj_create(parent);
    lv_obj_remove_style_all(list_);
    if (lv_obj_get_group(list_)) lv_group_remove_obj(list_); /* the page adds it to its own group */
    lv_obj_set_pos(list_, layout_.x, layout_.y);
    lv_obj_set_size(list_, layout_.w, layout_.h);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list_, layout_.scrollbar_w, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(list_, layout_.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(list_, layout_.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(list_, layout_.scrollbar_pad, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list_, lv_color_hex(CP0_UI_LIST_SCROLLBAR_RGB), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(list_, list_event_cb, LV_EVENT_ALL, this);
    tops_.assign(1, 0);
}

RichList::~RichList()
{
    if (!list_) return;
    lv_obj_remove_event_cb_with_user_data(list_, list_event_cb, this);
    for (Row &r : rows_)
        if (r.spec.view) r.spec.view->detach();
    lv_obj_delete(list_);
    list_ = nullptr;
}

const lv_font_t *RichList::label_font() const { return font_medium(layout_.text_px); }
const lv_font_t *RichList::value_font() const { return font_medium(layout_.value_px); }
const lv_font_t *RichList::section_font() const { return font_medium(layout_.section_px); }
const lv_font_t *RichList::small_font() const { return font_medium(std::max(10, layout_.value_px - 2)); }

int RichList::measure(const RichRow &spec, bool &wrapped) const
{
    wrapped = false;
    const int avail = std::max(1, layout_.w - 2 * layout_.inset);
    switch (spec.kind) {
    case RichRow::Kind::Section: return layout_.section_h;
    case RichRow::Kind::Custom: return std::max(1, spec.height);
    case RichRow::Kind::Item: {
        if (spec.value.empty()) return layout_.row_h;
        const int lw = text_width(spec.label.c_str(), label_font());
        const int vw = text_width(spec.value.c_str(), value_font());
        if (lw + layout_.gap + vw <= avail) return layout_.row_h;
        wrapped = true;
        lv_point_t size{};
        lv_text_get_size(&size, spec.value.c_str(), value_font(), 0, 0, avail, LV_TEXT_FLAG_NONE);
        return std::max(layout_.row_h, pad_ + line_height(section_font()) + 2 + static_cast<int>(size.y) + pad_);
    }
    case RichRow::Kind::Text: {
        const lv_font_t *font = spec.small ? small_font() : value_font();
        if (spec.cut) return line_height(font) + pad_;
        lv_point_t size{};
        lv_text_get_size(&size, spec.value.c_str(), font, 0, 0, avail, LV_TEXT_FLAG_NONE);
        return std::max(line_height(font), static_cast<int>(size.y)) + 2 * pad_;
    }
    }
    return layout_.row_h;
}

void RichList::destroy_host(Row &row)
{
    if (row.spec.view) row.spec.view->detach();
    if (row.host) lv_obj_delete(row.host);
    row.host = row.label = row.value = nullptr;
}

/* The texts of an Item, a Section or a Text, and where they sit. */
void RichList::fill_texts(Row &row)
{
    const int avail = std::max(1, layout_.w - 2 * layout_.inset);
    switch (row.spec.kind) {
    case RichRow::Kind::Section: {
        const lv_font_t *font = section_font();
        lv_obj_set_style_text_letter_space(row.label, 1, 0);
        set_text_if(row.label, upper(row.spec.label));
        lv_obj_set_pos(row.label, layout_.inset, (row.h - line_height(font)) / 2);
        lv_obj_set_size(row.label, avail, line_height(font));
        break;
    }
    case RichRow::Kind::Item: {
        const lv_font_t *lf = label_font(), *vf = value_font();
        set_text_if(row.label, row.spec.label);
        set_text_if(row.value, row.spec.value);
        set_color_if(row.value, value_rgb(row.spec.tone));
        if (row.wrapped) {
            lv_obj_set_style_text_font(row.label, section_font(), 0);
            lv_obj_set_style_text_color(row.label, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
            lv_label_set_long_mode(row.label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_pos(row.label, layout_.inset, pad_);
            lv_obj_set_size(row.label, avail, line_height(section_font()));
            lv_label_set_long_mode(row.value, LV_LABEL_LONG_MODE_WRAP);
            lv_point_t size{};
            lv_text_get_size(&size, row.spec.value.c_str(), vf, 0, 0, avail, LV_TEXT_FLAG_NONE);
            lv_obj_set_pos(row.value, layout_.inset, pad_ + line_height(section_font()) + 2);
            lv_obj_set_size(row.value, avail, size.y);
            lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_LEFT, 0);
        } else {
            lv_obj_set_style_text_font(row.label, lf, 0);
            lv_obj_set_style_text_color(row.label, lv_color_hex(CP0_UI_LIST_TEXT_RGB), 0);
            lv_label_set_long_mode(row.label, LV_LABEL_LONG_MODE_DOTS);
            lv_label_set_long_mode(row.value, LV_LABEL_LONG_MODE_CLIP);
            const int vw = text_width(row.spec.value.c_str(), vf);
            const int room = cp0_ui_list_label_w(layout_.w, layout_.inset, layout_.gap, vw);
            lv_obj_set_pos(row.label, layout_.inset, (row.h - line_height(lf)) / 2);
            lv_obj_set_size(row.label, std::max(1, room), line_height(lf));
            lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(row.value, layout_.w - layout_.inset - vw - 1, (row.h - line_height(vf)) / 2);
            lv_obj_set_size(row.value, vw + 1, line_height(vf));
        }
        break;
    }
    case RichRow::Kind::Text: {
        const lv_font_t *font = row.spec.small ? small_font() : value_font();
        const std::uint32_t rgb = row.spec.tone == Tone::Normal ? kText : tone_rgb(row.spec.tone);
        set_text_if(row.label, row.spec.value);
        set_color_if(row.label, rgb);
        lv_obj_set_style_text_font(row.label, font, 0);
        if (row.spec.cut) {
            lv_label_set_long_mode(row.label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_pos(row.label, layout_.inset, (row.h - line_height(font)) / 2);
            lv_obj_set_size(row.label, avail, line_height(font));
        } else {
            lv_point_t size{};
            lv_text_get_size(&size, row.spec.value.c_str(), font, 0, 0, avail, LV_TEXT_FLAG_NONE);
            lv_label_set_long_mode(row.label, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_pos(row.label, layout_.inset, pad_);
            lv_obj_set_size(row.label, avail, std::max(line_height(font), static_cast<int>(size.y)));
        }
        break;
    }
    case RichRow::Kind::Custom: break;
    }
}

void RichList::build_host(Row &row, int top)
{
    lv_obj_t *host = lv_obj_create(list_);
    lv_obj_remove_style_all(host);
    lv_obj_remove_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(host, 0, top);
    lv_obj_set_size(host, layout_.w, row.h);
    lv_obj_set_style_bg_color(host, lv_color_hex(kBarFocused), 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_TRANSP, 0);
    row.host = host;
    switch (row.spec.kind) {
    case RichRow::Kind::Section:
        row.label = make_label(host, section_font(), CP0_UI_LIST_SECTION_RGB);
        fill_texts(row);
        break;
    case RichRow::Kind::Item:
        row.label = make_label(host, label_font(), CP0_UI_LIST_TEXT_RGB);
        row.value = make_label(host, value_font(), CP0_UI_LIST_VALUE_RGB);
        fill_texts(row);
        break;
    case RichRow::Kind::Text:
        row.label = make_label(host, value_font(), kText);
        fill_texts(row);
        break;
    case RichRow::Kind::Custom:
        if (row.spec.view) row.spec.view->attach(host, layout_.w, row.h);
        break;
    }
}

/* tops, kinds, the hosts that are still to be made, the bar */
void RichList::relayout()
{
    const std::size_t n = rows_.size();
    kinds_.resize(n);
    tops_.assign(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const Row &r = rows_[i];
        const bool item = r.spec.kind != RichRow::Kind::Section && r.spec.selectable;
        kinds_[i] = item ? CP0_UI_LIST_ITEM : CP0_UI_LIST_SECTION;
        tops_[i + 1] = tops_[i] + r.h;
    }
    for (std::size_t i = 0; i < n; ++i) {
        Row &r = rows_[i];
        if (!r.host) build_host(r, tops_[i]);
        else lv_obj_set_pos(r.host, 0, tops_[i]);
    }
}

void RichList::restyle()
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        Row &r = rows_[i];
        if (!r.host) continue;
        const bool on = static_cast<int>(i) == selected_ && kinds_[i] == CP0_UI_LIST_ITEM;
        lv_obj_set_style_bg_color(r.host, lv_color_hex(focused_ ? kBarFocused : kBarIdle), 0);
        lv_obj_set_style_bg_opa(r.host, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
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
    s.scroll_y = scroll_y();
    s.read_only = 0;
    s.wrap = 0;
    return s;
}

int RichList::scroll_y() const
{
    return list_ ? static_cast<int>(lv_obj_get_scroll_y(list_)) : 0;
}

void RichList::set_rows(const std::vector<RichRow> &rows)
{
    if (!list_) return;
    while (rows_.size() > rows.size()) {
        destroy_host(rows_.back());
        rows_.pop_back();
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        bool wrapped = false;
        const int h = measure(rows[i], wrapped);
        if (i >= rows_.size()) {
            rows_.push_back(Row{});
            Row &r = rows_.back();
            r.spec = rows[i];
            r.h = h;
            r.wrapped = wrapped;
            continue;
        }
        Row &r = rows_[i];
        const bool custom = rows[i].kind == RichRow::Kind::Custom;
        if (r.spec.kind != rows[i].kind || (custom && r.spec.view != rows[i].view) || (!custom && r.wrapped != wrapped) ||
            (!custom && r.h != h)) {
            destroy_host(r);
            r.spec = rows[i];
            r.h = h;
            r.wrapped = wrapped;
            continue;
        }
        if (custom) {
            r.spec = rows[i];
            if (r.h != h) {
                r.h = h;
                if (r.host) lv_obj_set_height(r.host, h);
                if (r.spec.view) r.spec.view->resized(layout_.w, h);
            }
            continue;
        }
        const bool changed = r.spec.label != rows[i].label || r.spec.value != rows[i].value || r.spec.tone != rows[i].tone ||
                             r.spec.small != rows[i].small || r.spec.cut != rows[i].cut;
        r.spec = rows[i];
        if (changed && r.host) fill_texts(r);
    }
    relayout();
    lv_obj_update_layout(list_);
    cp0_ui_list_state_t s = state();
    const cp0_ui_list_result_t keep = cp0_ui_list_keep(&s);
    selected_ = keep.action == CP0_UI_LIST_NONE ? -1 : keep.index;
    const int max = cp0_ui_list_max_scroll(tops_.back(), layout_.h);
    if (scroll_y() > max) lv_obj_scroll_to_y(list_, max, LV_ANIM_OFF);
    restyle();
}

void RichList::reset_rows(const std::vector<RichRow> &rows, int selected)
{
    if (!list_) return;
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    set_rows(rows);
    if (rows_.empty()) return;
    const int target = cp0_ui_list_step(kinds_.data(), count(), std::clamp(selected, 0, count() - 1), 0, 0);
    selected_ = target;
    if (target >= 0) lv_obj_scroll_to_y(list_, cp0_ui_list_scroll_to_show(tops_.data(), count(), target, 0, layout_.h, 1), LV_ANIM_OFF);
    restyle();
}

void RichList::select(int index)
{
    if (!list_ || index < 0 || index >= count() || kinds_[static_cast<std::size_t>(index)] != CP0_UI_LIST_ITEM) return;
    selected_ = index;
    lv_obj_scroll_to_y(list_, cp0_ui_list_scroll_to_show(tops_.data(), count(), index, scroll_y(), layout_.h, 1), LV_ANIM_OFF);
    restyle();
}

void RichList::scroll_row_to_top(int index)
{
    if (!list_ || index < 0 || index >= count()) return;
    const int max = cp0_ui_list_max_scroll(tops_.back(), layout_.h);
    lv_obj_scroll_to_y(list_, std::min(max, tops_[static_cast<std::size_t>(index)]), LV_ANIM_OFF);
}

int RichList::selected_id() const
{
    return selected_ >= 0 && selected_ < count() ? rows_[static_cast<std::size_t>(selected_)].spec.id : 0;
}

int RichList::index_of_id(int id) const
{
    if (id == 0) return -1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].spec.id == id) return static_cast<int>(i);
    return -1;
}

int RichList::row_top(int index) const
{
    return index >= 0 && index < count() ? tops_[static_cast<std::size_t>(index)] : 0;
}

int RichList::row_height(int index) const
{
    return index >= 0 && index < count() ? rows_[static_cast<std::size_t>(index)].h : 0;
}

int RichList::rows_in_view() const
{
    const int top = scroll_y(), bottom = top + layout_.h;
    int n = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (tops_[i] >= top && tops_[i + 1] <= bottom) ++n;
    return n;
}

void RichList::apply(const cp0_ui_list_result_t &result)
{
    if (result.action == CP0_UI_LIST_MOVE) {
        selected_ = result.index;
        lv_obj_scroll_to_y(list_, result.scroll_y, LV_ANIM_OFF);
        restyle();
    }
}

void RichList::on_key(std::uint32_t key)
{
    if (key == kKeyTab) {
        if (events_.tab) events_.tab();
        return;
    }
    const cp0_ui_list_key_t k = list_key(key);
    if (k == CP0_UI_LIST_KEY_OTHER) return;
    if (k == CP0_UI_LIST_KEY_UP && events_.up_out && selected_ >= 0 && selected_ == cp0_ui_list_first(kinds_.data(), count())) {
        if (scroll_y() > 0) lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
        else events_.up_out();
        return;
    }
    const cp0_ui_list_state_t s = state();
    const cp0_ui_list_result_t r = cp0_ui_list_key(&s, k);
    switch (r.action) {
    case CP0_UI_LIST_MOVE:
    case CP0_UI_LIST_SCROLL: apply(r); break;
    case CP0_UI_LIST_ACTIVATE:
        if (events_.activate && r.index >= 0 && r.index < count())
            events_.activate(r.index, rows_[static_cast<std::size_t>(r.index)].spec.id);
        break;
    case CP0_UI_LIST_BACK:
        if (events_.back) events_.back();
        break;
    default: break;
    }
}

/* A pointer tap: the row under the finger (a key press makes a click too: only the keypad's own KEY event counts). */
void RichList::on_click()
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p{};
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(list_, &a);
    const int y = static_cast<int>(p.y) - static_cast<int>(a.y1) + scroll_y();
    const int index = cp0_ui_list_row_at(tops_.data(), count(), y);
    if (index < 0) return;
    const cp0_ui_list_state_t s = state();
    const cp0_ui_list_result_t r = cp0_ui_list_tap(&s, index, 0);
    if (r.action == CP0_UI_LIST_MOVE) {
        selected_ = r.index;
        lv_obj_scroll_to_y(list_, r.scroll_y, LV_ANIM_OFF);
        restyle();
    } else if (r.action == CP0_UI_LIST_ACTIVATE && events_.activate && r.index >= 0 && r.index < count()) {
        events_.activate(r.index, rows_[static_cast<std::size_t>(r.index)].spec.id);
    }
}

void RichList::list_event_cb(lv_event_t *event)
{
    auto *self = static_cast<RichList *>(lv_event_get_user_data(event));
    if (!self || !self->list_) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_KEY: self->on_key(lv_event_get_key(event)); break;
    case LV_EVENT_CLICKED: self->on_click(); break;
    case LV_EVENT_FOCUSED:
        self->focused_ = true;
        self->restyle();
        break;
    case LV_EVENT_DEFOCUSED:
        self->focused_ = false;
        self->restyle();
        break;
    case LV_EVENT_DELETE:
        for (Row &r : self->rows_)
            if (r.spec.view) r.spec.view->detach();
        self->rows_.clear();
        self->list_ = nullptr;
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- ActionBar */

ActionBar::ActionBar(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *label_font, const lv_font_t *value_font,
                     std::vector<ActionCell> cells, ActionBarEvents events)
    : events_(std::move(events)), label_font_(label_font), value_font_(value_font), h_(h)
{
    root_ = plain(parent);
    lv_obj_set_pos(root_, x, y);
    lv_obj_set_size(root_, w, h);
    lv_obj_add_event_cb(root_, root_delete_cb, LV_EVENT_DELETE, this);
    const int gap = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int n = static_cast<int>(cells.size());
    cell_w_ = n > 0 ? std::max(1, (w - (n - 1) * gap) / n) : w;
    cells_.reserve(cells.size());
    for (int i = 0; i < n; ++i) {
        Cell c;
        c.spec = cells[static_cast<std::size_t>(i)];
        c.obj = lv_obj_create(root_);
        lv_obj_remove_style_all(c.obj);
        lv_obj_remove_flag(c.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(c.obj, i * (cell_w_ + gap), 0);
        lv_obj_set_size(c.obj, cell_w_, h);
        lv_obj_set_style_radius(c.obj, cp0_ui_tok(CP0_TOK_RADIUS_S), 0);
        lv_obj_set_style_bg_opa(c.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(0x161616), 0);
        lv_obj_set_style_border_width(c.obj, 1, 0);
        lv_obj_set_style_border_color(c.obj, lv_color_hex(kBorder), 0);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(CP0_UI_LIST_BAR_RGB), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(c.obj, lv_color_hex(kLabel), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(c.obj, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
        lv_obj_set_user_data(c.obj, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(c.obj, cell_event_cb, LV_EVENT_ALL, this);
        c.label = make_label(c.obj, label_font_, CP0_UI_LIST_TEXT_RGB);
        c.value = make_label(c.obj, value_font_, kLabel);
        lv_label_set_long_mode(c.label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(c.value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(c.label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(c.value, LV_TEXT_ALIGN_CENTER, 0);
        cells_.push_back(c);
        place(cells_.back());
    }
}

ActionBar::~ActionBar()
{
    if (!root_) return;
    lv_obj_remove_event_cb_with_user_data(root_, root_delete_cb, this);
    lv_obj_delete(root_);
    root_ = nullptr;
}

void ActionBar::root_delete_cb(lv_event_t *event)
{
    auto *self = static_cast<ActionBar *>(lv_event_get_user_data(event));
    if (!self) return;
    self->root_ = nullptr;
    self->cells_.clear();
}

void ActionBar::place(Cell &c)
{
    const int pad = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int w = std::max(1, cell_w_ - 2 * pad);
    const int lh = line_height(label_font_), vh = line_height(value_font_);
    const bool two = !c.spec.value.empty();
    const int block = two ? lh + vh : lh;
    const int top = std::max(0, (h_ - block) / 2);
    set_text_if(c.label, c.spec.label);
    set_text_if(c.value, c.spec.value);
    lv_obj_set_pos(c.label, pad, top);
    lv_obj_set_size(c.label, w, lh);
    lv_obj_set_pos(c.value, pad, top + lh);
    lv_obj_set_size(c.value, w, vh);
    if (two) lv_obj_remove_flag(c.value, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(c.value, LV_OBJ_FLAG_HIDDEN);
}

void ActionBar::set_cell(int index, const std::string &label, const std::string &value)
{
    if (index < 0 || index >= count()) return;
    Cell &c = cells_[static_cast<std::size_t>(index)];
    if (c.spec.label == label && c.spec.value == value) return;
    c.spec.label = label;
    c.spec.value = value;
    place(c);
}

lv_obj_t *ActionBar::cell_object(int index) const
{
    return index >= 0 && index < count() ? cells_[static_cast<std::size_t>(index)].obj : nullptr;
}

std::vector<lv_obj_t *> ActionBar::focus_objects() const
{
    std::vector<lv_obj_t *> out;
    for (const Cell &c : cells_) out.push_back(c.obj);
    return out;
}

void ActionBar::on_key(int index, std::uint32_t key)
{
    const int n = count();
    switch (key) {
    case LV_KEY_RIGHT:
        if (index + 1 < n) lv_group_focus_obj(cells_[static_cast<std::size_t>(index) + 1].obj);
        break;
    case LV_KEY_LEFT:
        if (index > 0) lv_group_focus_obj(cells_[static_cast<std::size_t>(index) - 1].obj);
        else if (events_.back) events_.back();
        break;
    case LV_KEY_DOWN:
        if (events_.down) events_.down();
        break;
    case LV_KEY_ESC:
        if (events_.back) events_.back();
        break;
    case kKeyTab: /* the cells in turn, then the list under them */
        if (index + 1 < n) lv_group_focus_obj(cells_[static_cast<std::size_t>(index) + 1].obj);
        else if (events_.down) events_.down();
        break;
    case LV_KEY_ENTER:
        if (events_.activate) events_.activate(cells_[static_cast<std::size_t>(index)].spec.id);
        break;
    default: break;
    }
}

void ActionBar::cell_event_cb(lv_event_t *event)
{
    auto *self = static_cast<ActionBar *>(lv_event_get_user_data(event));
    if (!self || !self->root_) return;
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj)));
    if (index < 0 || index >= self->count()) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_KEY: self->on_key(index, lv_event_get_key(event)); break;
    case LV_EVENT_CLICKED: { /* a tap; the click a key makes belongs to the key (and may land on the next page's cell) */
        lv_indev_t *indev = lv_indev_active();
        if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) break;
        if (self->events_.activate) self->events_.activate(self->cells_[static_cast<std::size_t>(index)].spec.id);
        break;
    }
    default: break;
    }
}

} // namespace ui
} // namespace nt
