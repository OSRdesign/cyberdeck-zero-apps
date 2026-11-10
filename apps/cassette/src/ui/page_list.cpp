/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the recordings list (boards CassetteList-480x320 / -320x480): the strip (take count, folder, free
 * space), one row per take (the label sticker in the bold face, name, date / length / size, the length in the counter
 * face in landscape, the chevron; the selected row darker), the boards' scroll bar, PLAY, RENAME, TRIM, DELETE (red)
 * and, in landscape, the hint line. A tap selects a row, a tap on the selected row activates it (Enter too: it previews
 * the take, a second one on the previewing row stops the preview); LOAD (key L) puts the take into the deck and shows the
 * recorder, dimmed while a take records; the list
 * scrolls from a drag that starts anywhere in it (the list is clickable, the mmWave lesson).
 */
#include "pages.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <algorithm>

namespace cassette {
namespace ui {

ListPage::ListPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav)
    : PageBase(parent, frame), act_(actions), nav_(nav)
{
    const bool p = frame.portrait;
    /* the four keys get the toolkit's touch target (the deck: 79 px), the list area gives the height back */
    const double t = frame.target;
    fit_ = frame.fit({219}, {389}, {{78, 216, 0, 0, true}, {222, 278, t, 0, false}},
                     {{78, 382, 0, 0, true}, {396, 460, t, 0, false}});
    deck_body(root_, fit_);
    const double sw = p ? 278 : 446;
    box(root_, fit_.x(16), fit_.y(52), fit_.w(16, sw), fit_.h(52, 22), col::kStrip, fit_.px(5));
    strip_left_ = fit_label(root_, fit_, Face::Bold, 14, col::kInk, 26, 52, sw / 2 + 40, 22, LV_TEXT_ALIGN_LEFT, 0.5);
    strip_right_ = fit_label(root_, fit_, Face::Bold, 14, col::kInk, 16 + sw / 2 - 30, 52, sw / 2 + 20, 22, LV_TEXT_ALIGN_RIGHT, 0.5);

    /* a tap selects a row (a second tap or Enter acts): the toolkit's list row minimum (LIST_ROW) applies */
    row_h_ = std::max(fit_.px(p ? 56 : 42), frame.list_row);
    row_gap_ = fit_.px(p ? 6 : 4);
    const double lw = p ? 276 : 430;
    list_ = lv_obj_create(root_);
    plain(list_);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_CLICKABLE); /* a drag from anywhere in it scrolls it */
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(list_, fit_.x(16), fit_.y(78));
    /* 78..212 (3 rows) landscape, 78..382 (5 rows) portrait on the boards, plus the seam's share */
    lv_obj_set_size(list_, fit_.w(16, lw), fit_.y(p ? 390 : 220) - fit_.y(78) - fit_.px(p ? 8 : 8));
    lv_obj_add_event_cb(list_, scroll_cb, LV_EVENT_SCROLL, this);
    const double tx = p ? 298 : 452;
    track_ = box(root_, fit_.x(tx), fit_.y(78), fit_.w(tx, 6), fit_.y(p ? 390 : 220) - fit_.y(78) - fit_.px(p ? 6 : 4),
                 col::kScrollTrack, fit_.px(3));
    thumb_ = box(root_, fit_.x(tx), fit_.y(78), fit_.w(tx, 6), fit_.px(40), col::kHinge, fit_.px(3));

    static const Icon icons[4] = {Icon::Load, Icon::Pencil, Icon::Scissors, Icon::Delete};
    static const char *const names[4] = {"LOAD", "RENAME", "TRIM", "DELETE"};
    for (int i = 0; i < 4; ++i) {
        const DRect r = p ? DRect{16.0 + 70 * i, 396, 66, 64} : DRect{16.0 + 109 * i, 222, 104, 56};
        keys_[i] = std::make_unique<PianoKey>(root_, fit_, r, KeyType{12, 1.5, 4}, [this, i](KeyEdge e) {
            if (e == KeyEdge::Click) key(i);
        });
        keys_[i]->set(icons[i], names[i], i == 3 ? KeyLook::Red : KeyLook::Charcoal, true, false);
    }
    if (!p) hint_ = fit_label(root_, fit_, Face::Medium, 13, col::kInkMuted, 16, 288, 446, 16);
    list_y0_ = lv_obj_get_style_y(list_, LV_PART_MAIN);
    list_h0_ = lv_obj_get_style_height(list_, LV_PART_MAIN);
    track_y0_ = lv_obj_get_style_y(track_, LV_PART_MAIN);
    track_h0_ = lv_obj_get_style_height(track_, LV_PART_MAIN);

    /* the notice: the hint line (landscape) or the top strip (portrait, which has no hint line) */
    NoticeSpec ns;
    ns.ls = ls_screen(0.5, fit_.s);
    ns.px = fit_.px(14);
    ns.min_px = std::max(9, fit_.px(11));
    ns.x = fit_.x(16);
    ns.w = fit_.w(16, sw);
    if (p) {
        ns.y = fit_.y(52);
        ns.h_min = fit_.h(52, 22);
        ns.h_max = std::max(ns.h_min, fit_.px(38));
    } else {
        ns.y = fit_.y(286);
        ns.h_min = fit_.h(286, 20);
        ns.h_max = std::max(ns.h_min, fit_.y(310) - fit_.y(286));
        ns.pad = fit_.px(10);
    }
    notice_ = std::make_unique<NoticeBox>(root_, ns);
}

void ListPage::set_notice(const std::string &text)
{
    const bool on = notice_->set(text);
    const bool p = frame_.portrait;
    show(hint_, !on);
    if (p) {
        show(strip_left_, !on);
        show(strip_right_, !on);
        /* a notice taller than the strip pushes the list down (what scrolls out of view stays in the list) */
        const int extra = on ? std::max(0, notice_->height() - fit_.h(52, 22)) : 0;
        if (extra != shift_) {
            shift_ = extra;
            lv_obj_set_y(list_, list_y0_ + extra);
            lv_obj_set_height(list_, std::max(1, list_h0_ - extra));
            lv_obj_set_y(track_, track_y0_ + extra);
            lv_obj_set_height(track_, std::max(1, track_h0_ - extra));
            update_scrollbar();
        }
    }
}

void ListPage::key(int which)
{
    const int i = vm_.selected;
    if (i < 0 || i >= static_cast<int>(vm_.takes.size())) return;
    switch (which) {
    case 0:
        if (!vm_.can_load) break;
        if (act_.list_load) act_.list_load(i);
        if (nav_.go) nav_.go(Screen::Recorder);
        break;
    case 1: if (nav_.rename) nav_.rename(i); break;
    case 2:
        if (act_.list_trim) act_.list_trim(i);
        if (nav_.go) nav_.go(Screen::Trim);
        break;
    case 3: if (act_.list_delete) act_.list_delete(i); break;
    default: break;
    }
}

void ListPage::activate()
{
    const int i = vm_.selected;
    if (i < 0 || i >= static_cast<int>(vm_.takes.size())) return;
    if (act_.list_activate) act_.list_activate(i);
}

void ListPage::update(const ListVm &l)
{
    if (have_ && l == vm_) return; /* nothing changed */
    have_ = true;
    const int old_sel = vm_.selected;
    vm_ = l;
    std::string key;
    for (const TakeRow &t : l.takes) key += t.id + ";";
    if (key != rows_key_) {
        rows_key_ = key;
        rebuild();
    }
    const bool narrow = frame_.portrait;
    {
        /* the strip: the right part as wide as its text (at most 60 %), the left part the rest */
        const double sw = narrow ? 278 : 446;
        const int x0 = fit_.x(26), x1 = fit_.x(16 + sw - 10), gap = fit_.px(10);
        const int ls = ls_screen(0.5, fit_.s);
        const std::string right = list_strip_right(l, narrow);
        const int rw = std::min((x1 - x0) * 3 / 5, text_width(font(Face::Bold, fit_.px(14)), right.c_str(), ls) + 2);
        lv_obj_set_width(strip_right_, rw);
        lv_obj_set_x(strip_right_, x1 - rw);
        lv_obj_set_width(strip_left_, std::max(10, x1 - rw - gap - x0));
        set_fitted(strip_left_, Face::Bold, fit_.px(14), ls, std::max(9, fit_.px(11)), list_strip_left(l, narrow));
        set_fitted(strip_right_, Face::Bold, fit_.px(14), ls, std::max(9, fit_.px(11)), right);
    }
    /* one size per column for every row: the largest up to the board's that fits the longest text */
    const int ls = ls_screen(0.3, fit_.s);
    int name_px = fit_.px(19), detail_px = fit_.px(14);
    int name_ls = ls;
    /* the detail stays one line: the longest form that fits at the smallest size of the font fit below (a board where
     * the full line fits after shrinking keeps it; the shorter forms drop the size, then shorten the date; the format
     * tag is in all of them); a row where none fits falls back to the font fit */
    std::vector<std::string> detail_text(l.takes.size());
    for (std::size_t i = 0; i < rows_.size() && i < l.takes.size(); ++i) {
        const int w = lv_obj_get_style_width(rows_[i].detail, LV_PART_MAIN);
        const lv_font_t *bf = font(Face::Medium, std::max(9, fit_.px(10))); /* the smallest size the fit goes down to */
        detail_text[i] = l.takes[i].detail;
        if (text_width(bf, detail_text[i].c_str(), 0) <= w) continue;
        for (const std::string &f : l.takes[i].detail_short) {
            detail_text[i] = f;
            if (text_width(bf, f.c_str(), 0) <= w) break;
        }
    }
    for (std::size_t i = 0; i < rows_.size() && i < l.takes.size(); ++i) {
        const int w = lv_obj_get_style_width(rows_[i].name, LV_PART_MAIN);
        int l1 = ls;
        name_px = std::min(name_px, fit_px(Face::Bold, fit_.px(19), ls, l.takes[i].name, w, std::max(10, fit_.px(14)), &l1));
        name_ls = std::min(name_ls, l1);
        detail_px = std::min(detail_px, fit_px(Face::Medium, fit_.px(14), 0, detail_text[i], w, std::max(9, fit_.px(10)), nullptr));
    }
    const lv_font_t *nf = font(Face::Bold, name_px), *df = font(Face::Medium, detail_px);
    for (std::size_t i = 0; i < rows_.size() && i < l.takes.size(); ++i) {
        const TakeRow &t = l.takes[i];
        Row &r = rows_[i];
        set_sticker(r.sticker, t.sticker);
        lv_obj_set_style_text_font(r.name, nf, 0);
        lv_obj_set_style_text_letter_space(r.name, name_ls, 0);
        set_text(r.name, t.name);
        lv_obj_set_style_text_font(r.detail, df, 0);
        set_text(r.detail, detail_text[i]);
        if (r.dur) set_text(r.dur, fmt_counter(t.length_ms));
        if (frame_.portrait) {
            /* the narrow board: the chevron follows the text block (no duration column pushes it to the edge) */
            const int tw = std::max(text_width(nf, t.name.c_str(), name_ls), text_width(df, detail_text[i].c_str(), 0));
            const int x = lv_obj_get_style_x(r.name, LV_PART_MAIN) + tw + fit_.px(10);
            const int right = lv_obj_get_style_width(list_, LV_PART_MAIN) - 2 - fit_.px(10) - fit_.px(10);
            lv_obj_set_x(r.chevron, std::min(x, right));
        }
        style_row(static_cast<int>(i));
    }
    if (hint_) set_fitted(hint_, Face::Medium, fit_.px(13), 0, std::max(9, fit_.px(11)), list_hint(l));
    const bool any = l.selected >= 0 && l.selected < static_cast<int>(l.takes.size());
    /* all four dimmed (35 %) without a selected row; LOAD also while a take records (can_load) */
    for (int i = 0; i < 4; ++i)
        lv_obj_set_style_opa_layered(keys_[static_cast<std::size_t>(i)]->obj(),
                                     static_cast<lv_opa_t>(any && (i != 0 || l.can_load) ? 255 : 89), 0);
    if (l.selected != old_sel && any) lv_obj_scroll_to_view(rows_[static_cast<std::size_t>(l.selected)].obj, LV_ANIM_OFF);
    update_scrollbar();
}

void ListPage::rebuild()
{
    lv_obj_clean(list_);
    rows_.clear();
    const bool p = frame_.portrait;
    const int w = lv_obj_get_style_width(list_, LV_PART_MAIN);
    const int pad = fit_.px(10), gap = fit_.px(10);
    for (std::size_t i = 0; i < vm_.takes.size(); ++i) {
        Row r;
        r.obj = box(list_, 0, static_cast<int>(i) * (row_h_ + row_gap_), w, row_h_, col::kRow, fit_.px(6));
        border(r.obj, 1, col::kBodyEdge);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(r.obj, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_CLICKED, this);
        const int inner_h = row_h_ - 2;
        /* the sticker: 58 x 34, stripe 6, the number at 5,6 in a 24 px line (19 px on the boards) */
        r.sticker = make_sticker(r.obj, fit_, 58, 34, 6, 19, 5, 6 + (24 - 19) / 2.0, fit_.px(3));
        lv_obj_set_pos(r.sticker, pad - 1, (inner_h - fit_.px(34)) / 2);
        const int tx = pad - 1 + fit_.px(58) + gap;
        const int chev_x = w - 2 - pad - fit_.px(10);
        r.chevron = make_chevron(r.obj, fit_, col::kInkMuted);
        lv_obj_set_pos(r.chevron, chev_x, (inner_h - fit_.px(16)) / 2);
        int text_right = chev_x - gap;
        if (!p) {
            r.dur = label(r.obj, Face::Mono, fit_.px(20), col::kInk, 0, 0, fit_.px(64), LV_TEXT_ALIGN_RIGHT);
            int dw = 0; /* the widest length of the list: "00:42", "1:02:00" */
            for (const TakeRow &tk : vm_.takes)
                dw = std::max(dw, text_width(font(Face::Mono, fit_.px(20)), fmt_counter(tk.length_ms).c_str(), 0));
            lv_obj_set_width(r.dur, dw + 2);
            lv_obj_set_pos(r.dur, text_right - dw - 1, (inner_h - lv_font_get_line_height(font(Face::Mono, fit_.px(20)))) / 2);
            text_right -= dw + gap;
        }
        /* name (line 20) over the details (line 16), centred as a block */
        const int block = fit_.px(20) + fit_.px(16);
        const int top = (inner_h - block) / 2;
        const int nlh = lv_font_get_line_height(font(Face::Bold, fit_.px(19)));
        const int dlh = lv_font_get_line_height(font(Face::Medium, fit_.px(14)));
        r.name = label(r.obj, Face::Bold, fit_.px(19), col::kInk, tx, top + (fit_.px(20) - nlh) / 2, text_right - tx,
                       LV_TEXT_ALIGN_LEFT, ls_screen(0.3, fit_.s));
        r.detail = label(r.obj, Face::Medium, fit_.px(14), col::kInkMuted, tx, top + fit_.px(20) + (fit_.px(16) - dlh) / 2,
                         text_right - tx);
        rows_.push_back(r);
    }
    lv_obj_update_layout(list_);
}

void ListPage::style_row(int i)
{
    if (i < 0 || i >= static_cast<int>(rows_.size())) return;
    const bool sel = i == vm_.selected;
    lv_obj_set_style_bg_color(rows_[static_cast<std::size_t>(i)].obj, lv_color_hex(sel ? col::kStrip : col::kRow), 0);
    lv_obj_set_style_border_color(rows_[static_cast<std::size_t>(i)].obj, lv_color_hex(sel ? col::kHinge : col::kBodyEdge), 0);
}

void ListPage::select(int i, bool from_touch)
{
    if (i < 0 || i >= static_cast<int>(rows_.size())) return;
    if (i == vm_.selected) {
        if (act_.list_activate) act_.list_activate(i);
        return;
    }
    const int old = vm_.selected;
    vm_.selected = i;
    style_row(old);
    style_row(i);
    if (!from_touch) lv_obj_scroll_to_view(rows_[static_cast<std::size_t>(i)].obj, LV_ANIM_OFF);
    if (hint_) set_fitted(hint_, Face::Medium, fit_.px(13), 0, std::max(9, fit_.px(11)), list_hint(vm_));
    if (act_.list_select) act_.list_select(i);
}

bool ListPage::on_key(std::uint32_t k)
{
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return false;
    if (k == LV_KEY_DOWN || k == LV_KEY_UP) {
        int i = vm_.selected < 0 ? 0 : vm_.selected + (k == LV_KEY_DOWN ? 1 : -1);
        i = std::clamp(i, 0, n - 1);
        if (i != vm_.selected) select(i, false);
        return true;
    }
    return false;
}

void ListPage::row_cb(lv_event_t *e)
{
    auto *self = static_cast<ListPage *>(lv_event_get_user_data(e));
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    self->select(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))), true);
}

void ListPage::scroll_cb(lv_event_t *e)
{
    static_cast<ListPage *>(lv_event_get_user_data(e))->update_scrollbar();
}

void ListPage::update_scrollbar()
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
    lv_obj_set_height(thumb_, h);
    lv_obj_set_y(thumb_, lv_obj_get_style_y(track_, LV_PART_MAIN) + (th - h) * std::clamp(lv_obj_get_scroll_y(list_), 0, range) / range);
}

} // namespace ui
} // namespace cassette
