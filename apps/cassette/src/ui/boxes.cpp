/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the save box (boards CassetteSave-480x320 / -320x480, note n8) and the small confirm box ("Delete the
 * original take?", not drawn on the boards: the save box's look, two keys); the merge box (the save box's frame and keys:
 * KEEP ORIGINALS, DELETE ORIGINALS, CANCEL) and the progress box (the confirm box's frame, one CANCEL key). All dim the
 * whole screen (66 % black, the header too, as the board) and take every tap meant for the page under them; Esc is their
 * CANCEL (the root).
 */
#include "pages.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

namespace {

/* A Fit for a box drawn at (bx, by, bw, bh) on the board, centred on the screen as on the board (the board's offset
 * from the centre kept, scaled); its button bands get the toolkit's touch target, so the box grows by what they need. */
Fit box_fit(const Fit &page, int sw, int sh, double dw, double dh, double bx, double by, double bw, double bh,
            const std::vector<Band> &bands)
{
    Fit f = page;
    f.share = 0;
    f.dbody = by;
    f.segs = fit_segments(by, by + bh, page.s, bands, 1e9);
    double h = 0;
    for (const Fit::Seg &g : f.segs) h += g.h;
    const double w = bw * page.s;
    const double x = (sw - w) / 2.0 + (bx - (dw - bw) / 2.0) * page.s;
    const double y = (sh - h) / 2.0 + (by - (dh - bh) / 2.0) * page.s;
    f.ox = x - bx * page.s;
    f.oy = std::clamp(y, 0.0, std::max(0.0, sh - h));
    return f;
}

/* The keyboard focus of a box: a 2 px white outline (the look of the selected split flag), shown once a key moved it. */
void outline(lv_obj_t *b, const Fit &f, bool on)
{
    lv_obj_set_style_outline_width(b, on ? f.px(2) : 0, 0);
    lv_obj_set_style_outline_color(b, lv_color_hex(col::kWhite), 0);
    lv_obj_set_style_outline_pad(b, on ? f.px(2) : 0, 0);
    lv_obj_set_style_outline_opa(b, LV_OPA_COVER, 0);
}

enum class BoxKey { Green, Outline, Charcoal, Red };

/* A box button: a gradient (or the outlined dark red), a 3 px lip, a caption and an optional line under it. */
lv_obj_t *box_button(lv_obj_t *parent, const Fit &f, DRect r, BoxKey look, const char *title, const char *sub,
                     lv_event_cb_t cb, void *user, intptr_t id)
{
    const int x = f.x(r.x), y = f.y(r.y), w = f.w(r.x, r.w), h = f.h(r.y, r.h), lip = f.px(3), rad = f.px(6);
    lv_obj_t *lipo = box(parent, x, y + lip, w, h, col::kHinge, rad);
    (void)lipo;
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (lv_obj_get_group(b)) lv_group_remove_obj(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, rad, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    std::uint32_t top = col::kKeyTop, bottom = col::kKeyBottom, ink = col::kKeyText;
    if (look == BoxKey::Green) top = col::kOkTop, bottom = col::kOkBottom, ink = col::kWhite;
    if (look == BoxKey::Red) top = col::kRecTop, bottom = col::kRecBottom, ink = col::kWhite;
    if (look == BoxKey::Outline) {
        lv_obj_set_style_bg_color(b, lv_color_hex(col::kReplaceBg), 0);
        border(b, f.px(2), col::kRecTop);
        ink = col::kReplaceText;
    } else {
        lv_obj_set_style_bg_color(b, lv_color_hex(top), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(bottom), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    }
    lv_obj_set_style_translate_y(b, f.px(2), LV_STATE_PRESSED);
    lv_obj_set_user_data(b, reinterpret_cast<void *>(id));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    const lv_font_t *tf = font(Face::Bold, f.px(16));
    const int tlh = lv_font_get_line_height(tf);
    const int sub_h = sub ? f.px(14) : 0;
    const int block = f.px(18) + (sub ? f.px(1) + sub_h : 0);
    const int top_y = (h - block) / 2;
    lv_obj_t *t = label(b, Face::Bold, f.px(16), ink, 0, top_y + (f.px(18) - tlh) / 2, w, LV_TEXT_ALIGN_CENTER,
                        ls_screen(1, f.s));
    set_fitted(t, Face::Bold, f.px(16), ls_screen(1, f.s), std::max(9, f.px(12)), title);
    if (sub) {
        const lv_font_t *sf = font(Face::Medium, f.px(12));
        lv_obj_t *sl = label(b, Face::Medium, f.px(12), ink, 0,
                             top_y + f.px(18) + f.px(1) + (sub_h - lv_font_get_line_height(sf)) / 2, w, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_text_opa(sl, static_cast<lv_opa_t>(230), 0);
        set_fitted(sl, Face::Medium, f.px(12), 0, std::max(8, f.px(10)), sub);
    }
    return b;
}

lv_obj_t *dim_layer(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *dim = box(parent, 0, 0, w, h, 0x000000, 0, static_cast<lv_opa_t>(168)); /* 66 % */
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE); /* the page under it gets no tap */
    lv_obj_add_flag(dim, LV_OBJ_FLAG_HIDDEN);
    return dim;
}

lv_obj_t *cream_box(lv_obj_t *parent, const Fit &f, DRect r)
{
    lv_obj_t *b = box(parent, f.x(r.x), f.y(r.y), f.w(r.x, r.w), f.h(r.y, r.h), col::kBody, f.px(10));
    border(b, f.px(2), col::kFrame);
    lv_obj_set_style_shadow_width(b, f.px(24), 0);
    lv_obj_set_style_shadow_offset_y(b, f.px(8), 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(b, static_cast<lv_opa_t>(153), 0);
    return b;
}

} // namespace

/* ------------------------------------------------------------------------------------------------ SaveBox */

SaveBox::SaveBox(lv_obj_t *parent, const Frame &frame, const UiActions &actions) : act_(actions)
{
    const bool p = frame.portrait;
    narrow_ = p;
    const Fit page = frame.fit({}, {});
    const double t = frame.target;
    fit_ = p ? box_fit(page, frame.w, frame.h, 320, 480, 12, 48, 296, 369,
                       {{183, 239, t, 0, false}, {249, 305, t, 0, false}, {317, 361, t, 0, false}})
             : box_fit(page, frame.w, frame.h, 480, 320, 40, 28, 400, 261, {{157, 209, t, 0, false}, {221, 265, t, 0, false}});
    dim_ = dim_layer(parent, frame.w, frame.h);
    const double bx = p ? 12 : 40, by = p ? 48 : 28, bw = p ? 296 : 400, bh = p ? 369 : 261;
    cream_box(dim_, fit_, {bx, by, bw, bh});
    const double cx = bx + 14, cw = bw - 28;
    title_ = fit_label(dim_, fit_, Face::Bold, 20, col::kInk, cx, by + 8, cw, 22, LV_TEXT_ALIGN_LEFT, 1.5);
    lv_label_set_text(title_, "SAVE");
    summary_ = fit_label(dim_, fit_, Face::Medium, 14, col::kInkSoft, cx, by + 32, cw, 16);
    files_ = box(dim_, fit_.x(cx), fit_.y(by + 54), fit_.w(cx, cw), fit_.px(59), col::kStrip, fit_.px(5));
    if (p) {
        buttons_.push_back(box_button(dim_, fit_, {26, 183, 268, 56}, BoxKey::Green, "SAVE AS NEW FILES", "original take kept", button_cb, this, 0));
        buttons_.push_back(box_button(dim_, fit_, {26, 249, 268, 56}, BoxKey::Outline, "REPLACE ORIGINAL", "original deleted after saving", button_cb, this, 1));
        buttons_.push_back(box_button(dim_, fit_, {26, 317, 268, 44}, BoxKey::Charcoal, "CANCEL", nullptr, button_cb, this, 2));
    } else {
        buttons_.push_back(box_button(dim_, fit_, {54, 157, 182, 52}, BoxKey::Green, "SAVE AS NEW FILES", "original take kept", button_cb, this, 0));
        buttons_.push_back(box_button(dim_, fit_, {244, 157, 182, 52}, BoxKey::Outline, "REPLACE ORIGINAL", "original deleted after saving", button_cb, this, 1));
        buttons_.push_back(box_button(dim_, fit_, {54, 221, 372, 44}, BoxKey::Charcoal, "CANCEL", nullptr, button_cb, this, 2));
    }
}

void SaveBox::update(const SaveVm &s, const TrimVm &t)
{
    if (s.shown != shown_ && s.shown) {
        lv_obj_move_foreground(dim_);
        focus_ = 0; /* SAVE AS NEW FILES, the default */
        focus_shown_ = false;
        for (lv_obj_t *b : buttons_) outline(b, fit_, false);
    }
    shown_ = s.shown;
    show(dim_, s.shown);
    if (!s.shown) return;
    std::string key;
    for (const SaveFile &f : s.files) key += f.name + "=" + std::to_string(f.length_ms) + ";";
    key += std::to_string(t.in_ms) + "/" + std::to_string(t.out_ms) + "/" + std::to_string(t.splits.size());
    if (key == key_) return;
    key_ = key;
    const int cw = lv_obj_get_style_width(files_, LV_PART_MAIN);
    set_fitted(summary_, Face::Medium, fit_.px(14), 0, std::max(9, fit_.px(11)), save_summary(t, s.files.size(), narrow_));
    /* the file list: one 17 px line each inside 4 px padding */
    lv_obj_clean(files_);
    names_.clear();
    lengths_.clear();
    const int pad_x = fit_.px(8), pad_y = fit_.px(4), line_h = fit_.px(17);
    const lv_font_t *f13 = font(Face::Bold, fit_.px(13));
    const int lh = lv_font_get_line_height(f13);
    const int ls = ls_screen(0.3, fit_.s);
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        const std::string len = fmt_tenths_short(s.files[i].length_ms);
        const int lw = text_width(f13, len.c_str(), ls) + 2;
        const int y = pad_y + static_cast<int>(i) * line_h + (line_h - lh) / 2;
        lv_obj_t *l = label(files_, Face::Bold, fit_.px(13), col::kInk, cw - pad_x - lw, y, lw, LV_TEXT_ALIGN_RIGHT, ls);
        lv_label_set_text(l, len.c_str());
        lv_obj_t *n = label(files_, Face::Bold, fit_.px(13), col::kInk, pad_x, y, cw - 2 * pad_x - lw - fit_.px(6),
                            LV_TEXT_ALIGN_LEFT, ls);
        set_fitted(n, Face::Bold, fit_.px(13), ls, std::max(9, fit_.px(10)), narrow_ ? strip_wav(s.files[i].name) : s.files[i].name);
        names_.push_back(n);
        lengths_.push_back(l);
    }
    lv_obj_set_height(files_, 2 * pad_y + static_cast<int>(s.files.size()) * line_h);
}

void SaveBox::key_move(int dir)
{
    focus_ = (focus_ + dir + 3) % 3; /* from the default (SAVE AS NEW FILES) */
    focus_shown_ = true;
    for (std::size_t i = 0; i < buttons_.size(); ++i) outline(buttons_[i], fit_, static_cast<int>(i) == focus_);
}

void SaveBox::key_choose()
{
    if (act_.save_choose) act_.save_choose(focus_ == 0 ? SaveMode::NewFiles : focus_ == 1 ? SaveMode::Replace : SaveMode::Cancel);
}

void SaveBox::button_cb(lv_event_t *e)
{
    auto *self = static_cast<SaveBox *>(lv_event_get_user_data(e));
    const intptr_t id = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
    if (!self->act_.save_choose) return;
    self->act_.save_choose(id == 0 ? SaveMode::NewFiles : id == 1 ? SaveMode::Replace : SaveMode::Cancel);
}

/* ------------------------------------------------------------------------------------------------ ConfirmBox */

ConfirmBox::ConfirmBox(lv_obj_t *parent, const Frame &frame, const UiActions &actions) : act_(actions)
{
    const bool p = frame.portrait;
    const Fit page = frame.fit({}, {});
    /* the save box's width and margins, as tall as its content: title, two body lines, the keys */
    const double bx = p ? 12 : 40, bw = p ? 296 : 400, bh = 160;
    const double dh = p ? 480 : 320, by = (dh - bh) / 2;
    const double kyd = by + bh - 14 - 52;
    fit_ = box_fit(page, frame.w, frame.h, p ? 320 : 480, dh, bx, by, bw, bh, {{kyd, kyd + 52, double(frame.target), 0, false}});
    dim_ = dim_layer(parent, frame.w, frame.h);
    cream_box(dim_, fit_, {bx, by, bw, bh});
    const double cx = bx + 14, cw = bw - 28;
    title_ = fit_label(dim_, fit_, Face::Bold, 20, col::kInk, cx, by + 10, cw, 22);
    body_ = fit_label(dim_, fit_, Face::Medium, 14, col::kInkSoft, cx, by + 36, cw, 16);
    lv_label_set_long_mode(body_, LV_LABEL_LONG_MODE_WRAP);
    const double kw = (cw - 8) / 2, ky = by + bh - 14 - 52;
    lv_obj_t *yes = box_button(dim_, fit_, {cx, ky, kw, 52}, BoxKey::Red, "DELETE", nullptr, button_cb, this, 1);
    yes_label_ = lv_obj_get_child(yes, 0);
    buttons_.push_back(yes);
    buttons_.push_back(box_button(dim_, fit_, {cx + kw + 8, ky, kw, 52}, BoxKey::Charcoal, "CANCEL", nullptr, button_cb, this, 0));
}

void ConfirmBox::update(const ConfirmVm &c)
{
    if (c.shown != shown_ && c.shown) {
        lv_obj_move_foreground(dim_);
        focus_ = 1; /* CANCEL: the safe answer is the default */
        focus_shown_ = false;
        for (lv_obj_t *b : buttons_) outline(b, fit_, false);
    }
    shown_ = c.shown;
    show(dim_, c.shown);
    if (!c.shown) return;
    set_fitted(title_, Face::Bold, fit_.px(20), 0, std::max(10, fit_.px(14)), c.title);
    set_text(body_, c.body);
    if (yes_label_) set_fitted(yes_label_, Face::Bold, fit_.px(16), ls_screen(1, fit_.s), std::max(9, fit_.px(12)), c.yes);
}

void ConfirmBox::key_move(int dir)
{
    focus_ = (focus_ + dir + 2) % 2; /* from the default (CANCEL) */
    focus_shown_ = true;
    for (std::size_t i = 0; i < buttons_.size(); ++i) outline(buttons_[i], fit_, static_cast<int>(i) == focus_);
}

void ConfirmBox::key_choose()
{
    if (act_.confirm_answer) act_.confirm_answer(focus_ == 0);
}

void ConfirmBox::button_cb(lv_event_t *e)
{
    auto *self = static_cast<ConfirmBox *>(lv_event_get_user_data(e));
    const intptr_t id = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
    if (self->act_.confirm_answer) self->act_.confirm_answer(id == 1);
}

/* ------------------------------------------------------------------------------------------------ MergeBox */

MergeBox::MergeBox(lv_obj_t *parent, const Frame &frame, const UiActions &actions) : act_(actions)
{
    const bool p = frame.portrait;
    const Fit page = frame.fit({}, {});
    const double t = frame.target;
    /* the save box's frame and keys */
    fit_ = p ? box_fit(page, frame.w, frame.h, 320, 480, 12, 48, 296, 369,
                       {{183, 239, t, 0, false}, {249, 305, t, 0, false}, {317, 361, t, 0, false}})
             : box_fit(page, frame.w, frame.h, 480, 320, 40, 28, 400, 261, {{157, 209, t, 0, false}, {221, 265, t, 0, false}});
    dim_ = dim_layer(parent, frame.w, frame.h);
    const double bx = p ? 12 : 40, by = p ? 48 : 28, bw = p ? 296 : 400, bh = p ? 369 : 261;
    cream_box(dim_, fit_, {bx, by, bw, bh});
    const double cx = bx + 14, cw = bw - 28;
    title_ = fit_label(dim_, fit_, Face::Bold, 20, col::kInk, cx, by + 8, cw, 22, LV_TEXT_ALIGN_LEFT, 1.5);
    summary_ = fit_label(dim_, fit_, Face::Medium, 14, col::kInkSoft, cx, by + 32, cw, 16);
    strip_ = box(dim_, fit_.x(cx), fit_.y(by + 54), fit_.w(cx, cw), fit_.px(25), col::kStrip, fit_.px(5));
    const int pad_x = fit_.px(8);
    name_ = label(strip_, Face::Bold, fit_.px(13), col::kInk, pad_x, fit_.px(4), lv_obj_get_style_width(strip_, LV_PART_MAIN) - 2 * pad_x,
                  LV_TEXT_ALIGN_LEFT, ls_screen(0.3, fit_.s));
    lv_label_set_long_mode(name_, LV_LABEL_LONG_MODE_WRAP); /* a long name wraps, never cut */
    if (p) {
        buttons_.push_back(box_button(dim_, fit_, {26, 183, 268, 56}, BoxKey::Green, "KEEP ORIGINALS", "originals kept", button_cb, this, 0));
        buttons_.push_back(box_button(dim_, fit_, {26, 249, 268, 56}, BoxKey::Outline, "DELETE ORIGINALS", "deleted after merging", button_cb, this, 1));
        buttons_.push_back(box_button(dim_, fit_, {26, 317, 268, 44}, BoxKey::Charcoal, "CANCEL", nullptr, button_cb, this, 2));
    } else {
        buttons_.push_back(box_button(dim_, fit_, {54, 157, 182, 52}, BoxKey::Green, "KEEP ORIGINALS", "originals kept", button_cb, this, 0));
        buttons_.push_back(box_button(dim_, fit_, {244, 157, 182, 52}, BoxKey::Outline, "DELETE ORIGINALS", "deleted after merging", button_cb, this, 1));
        buttons_.push_back(box_button(dim_, fit_, {54, 221, 372, 44}, BoxKey::Charcoal, "CANCEL", nullptr, button_cb, this, 2));
    }
}

void MergeBox::update(const MergeBoxVm &m)
{
    if (m.shown != shown_ && m.shown) {
        lv_obj_move_foreground(dim_);
        focus_ = 0; /* KEEP ORIGINALS, the default */
        focus_shown_ = false;
        for (lv_obj_t *b : buttons_) outline(b, fit_, false);
    }
    shown_ = m.shown;
    show(dim_, m.shown);
    if (!m.shown) return;
    const std::string key = m.title + "|" + m.name + "|" + m.detail;
    if (key == key_) return;
    key_ = key;
    set_fitted(title_, Face::Bold, fit_.px(20), ls_screen(1.5, fit_.s), std::max(10, fit_.px(14)), m.title);
    set_fitted(summary_, Face::Medium, fit_.px(14), 0, std::max(9, fit_.px(11)), m.detail);
    set_text(name_, m.name);
    /* the strip as tall as the name's lines inside 4 px padding */
    lv_point_t sz;
    lv_text_get_size(&sz, m.name.c_str(), lv_obj_get_style_text_font(name_, LV_PART_MAIN), ls_screen(0.3, fit_.s), 0,
                     lv_obj_get_style_width(name_, LV_PART_MAIN), LV_TEXT_FLAG_NONE);
    const int lh = lv_font_get_line_height(lv_obj_get_style_text_font(name_, LV_PART_MAIN));
    lv_obj_set_height(strip_, 2 * fit_.px(4) + std::max(lh, static_cast<int>(sz.y)));
}

void MergeBox::key_move(int dir)
{
    focus_ = (focus_ + dir + 3) % 3; /* from the default (KEEP ORIGINALS) */
    focus_shown_ = true;
    for (std::size_t i = 0; i < buttons_.size(); ++i) outline(buttons_[i], fit_, static_cast<int>(i) == focus_);
}

void MergeBox::key_choose()
{
    if (act_.merge_choose)
        act_.merge_choose(focus_ == 0 ? MergeChoice::KeepOriginals : focus_ == 1 ? MergeChoice::DeleteOriginals : MergeChoice::Cancel);
}

void MergeBox::button_cb(lv_event_t *e)
{
    auto *self = static_cast<MergeBox *>(lv_event_get_user_data(e));
    const intptr_t id = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
    if (!self->act_.merge_choose) return;
    self->act_.merge_choose(id == 0 ? MergeChoice::KeepOriginals : id == 1 ? MergeChoice::DeleteOriginals : MergeChoice::Cancel);
}

/* ------------------------------------------------------------------------------------------------ ProgressBox */

ProgressBox::ProgressBox(lv_obj_t *parent, const Frame &frame, const UiActions &actions) : act_(actions)
{
    const bool p = frame.portrait;
    const Fit page = frame.fit({}, {});
    /* the confirm box's frame: title, the line, one key across */
    const double bx = p ? 12 : 40, bw = p ? 296 : 400, bh = 160;
    const double dh = p ? 480 : 320, by = (dh - bh) / 2;
    const double kyd = by + bh - 14 - 52;
    fit_ = box_fit(page, frame.w, frame.h, p ? 320 : 480, dh, bx, by, bw, bh, {{kyd, kyd + 52, double(frame.target), 0, false}});
    dim_ = dim_layer(parent, frame.w, frame.h);
    cream_box(dim_, fit_, {bx, by, bw, bh});
    const double cx = bx + 14, cw = bw - 28;
    title_ = fit_label(dim_, fit_, Face::Bold, 20, col::kInk, cx, by + 10, cw, 22);
    body_ = fit_label(dim_, fit_, Face::Medium, 14, col::kInkSoft, cx, by + 36, cw, 16);
    lv_label_set_long_mode(body_, LV_LABEL_LONG_MODE_WRAP);
    cancel_ = box_button(dim_, fit_, {cx, kyd, cw, 52}, BoxKey::Charcoal, "CANCEL", nullptr, button_cb, this, 0);
}

void ProgressBox::update(const ProgressVm &pv)
{
    if (pv.shown != shown_ && pv.shown) {
        lv_obj_move_foreground(dim_);
        focus_shown_ = false;
        outline(cancel_, fit_, false);
    }
    shown_ = pv.shown;
    show(dim_, pv.shown);
    if (!pv.shown) return;
    set_fitted(title_, Face::Bold, fit_.px(20), 0, std::max(10, fit_.px(14)), pv.title);
    set_text(body_, pv.body);
}

void ProgressBox::key_move(int)
{
    focus_shown_ = true;
    outline(cancel_, fit_, true);
}

void ProgressBox::key_choose()
{
    if (focus_shown_ && act_.merge_stop) act_.merge_stop();
}

void ProgressBox::button_cb(lv_event_t *e)
{
    auto *self = static_cast<ProgressBox *>(lv_event_get_user_data(e));
    if (self->act_.merge_stop) self->act_.merge_stop();
}

} // namespace ui
} // namespace cassette
