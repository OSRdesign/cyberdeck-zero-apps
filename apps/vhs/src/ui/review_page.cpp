/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the photo review (see review_page.hpp).
 */
#include "review_page.hpp"

#include "app.hpp"
#include "theme.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace vhs {
namespace ui {
namespace {

constexpr const char *kReviewHint = "Left and Right show the other photos; Delete asks first.";

void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

void place(lv_obj_t *o, const Rect &r)
{
    lv_obj_set_pos(o, r.x, r.y);
    lv_obj_set_size(o, r.w, r.h);
}

lv_obj_t *label(lv_obj_t *parent, int px, std::uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, cp0_ui_font_px(px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, "");
    return l;
}

void set_text(lv_obj_t *l, const std::string &t)
{
    if (std::strcmp(lv_label_get_text(l), t.c_str()) != 0) lv_label_set_text(l, t.c_str());
}

void vis(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* A label too wide for its button (a dense panel: 480x480 at 11.3 px/mm) takes a font up to 20 % smaller, then wraps
 * at its spaces onto two centred lines, instead of being cut; a label that fits keeps its font and natural size (the
 * canvas sizes are unchanged). */
void fit_label(lv_obj_t *l, int px, int room)
{
    if (room <= 0) return;
    const char *text = lv_label_get_text(l);
    lv_point_t sz;
    int p = px;
    for (; p >= std::max(10, px * 8 / 10); --p) {
        lv_text_get_size(&sz, text, cp0_ui_font_px(p), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= room) break;
    }
    if (p == px) return;
    if (p < std::max(10, px * 8 / 10)) p = std::max(10, px * 8 / 10);
    lv_obj_set_style_text_font(l, cp0_ui_font_px(p), 0);
    if (sz.x > room && std::strchr(text, ' ')) {
        lv_obj_set_width(l, room);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

} // namespace

ReviewPage::ReviewPage(lv_obj_t *root, const ReviewLayout &geo, ReviewActions act) : root_(root), geo_(geo), act_(std::move(act))
{
    const int u8 = static_cast<int>(8 * geo_.u);
    panel_ = lv_obj_create(root_);
    plain(panel_);
    place(panel_, geo_.panel);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(col::kPanel), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel_, static_cast<int>(10 * geo_.u), 0);
    box_ = lv_obj_create(root_);
    plain(box_);
    place(box_, geo_.picture);
    lv_obj_set_style_bg_color(box_, lv_color_hex(col::kPicture), 0);
    lv_obj_set_style_bg_opa(box_, LV_OPA_COVER, 0);
    img_ = lv_image_create(box_);
    lv_obj_remove_flag(img_, LV_OBJ_FLAG_CLICKABLE);
    chip_ = label(box_, geo_.chip_px, col::kWhite);
    lv_obj_set_style_text_opa(chip_, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(chip_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chip_, 115, 0);
    lv_obj_set_style_radius(chip_, static_cast<int>(4 * geo_.u), 0);
    lv_obj_set_style_pad_hor(chip_, static_cast<int>(6 * geo_.u), 0);
    lv_obj_set_style_pad_ver(chip_, static_cast<int>(2 * geo_.u), 0);
    lv_obj_set_pos(chip_, geo_.chip_x, geo_.chip_y);
    prev_ = button(geo_.prev, "Previous", 0);
    next_ = button(geo_.next, "Next", 1);
    del_ = button(geo_.del, "Delete", 2);
    title_ = label(root_, geo_.title_px, col::kText);
    lv_obj_set_pos(title_, geo_.title.x, geo_.title.y);
    lv_obj_set_width(title_, geo_.title.w);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
    note_ = label(root_, geo_.note_px, col::kMuted);
    lv_obj_set_pos(note_, geo_.note.x, geo_.note.y);
    lv_obj_set_width(note_, geo_.note.w);
    lv_label_set_long_mode(note_, LV_LABEL_LONG_MODE_WRAP);
    (void)u8;
    show(false);
}

ReviewPage::~ReviewPage()
{
    if (img_ && lv_obj_is_valid(img_)) lv_image_set_src(img_, nullptr);
}

lv_obj_t *ReviewPage::button(const Rect &r, const char *text, int i)
{
    lv_obj_t *b = lv_button_create(root_);
    lv_obj_remove_style_all(b);
    place(b, r);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(col::kButton), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, static_cast<int>(8 * geo_.u), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col::kButtonPressed), LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b, lv_color_hex(col::kText), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(b, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = label(b, geo_.button_px, col::kText);
    lv_label_set_text(l, text);
    fit_label(l, geo_.button_px, r.w - 2 * std::max(2, static_cast<int>(4 * geo_.u)));
    lv_obj_set_user_data(b, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
    lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, this);
    return b;
}

void ReviewPage::button_cb(lv_event_t *e)
{
    auto *self = static_cast<ReviewPage *>(lv_event_get_user_data(e));
    lv_obj_t *b = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    switch (static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(b)))) {
    case 0: if (self->act_.prev) self->act_.prev(); break;
    case 1: if (self->act_.next) self->act_.next(); break;
    case 2: if (self->act_.del) self->act_.del(); break;
    default: break;
    }
}

void ReviewPage::show(bool on)
{
    shown_ = on;
    for (lv_obj_t *o : {panel_, box_, prev_, next_, del_, title_, note_}) vis(o, on);
    lv_group_t *g = cdk::input_group();
    for (lv_obj_t *b : {prev_, next_, del_}) {
        if (!g) break;
        if (on) lv_group_add_obj(g, b);
        else lv_group_remove_obj(b);
    }
    if (!on) {
        lv_image_set_src(img_, nullptr);
        pic_gen_ = 0;
    }
}

void ReviewPage::focus_first()
{
    if (lv_group_t *g = cdk::input_group()) lv_group_focus_obj(next_);
}

void ReviewPage::update(const ReviewView &v, const Picture *pic)
{
    if (!shown_) return;
    if (v.picture_gen != pic_gen_) {
        pic_gen_ = v.picture_gen;
        if (pic && pic->w > 0) {
            const Rect at = place_in_box(pic->w, pic->h, geo_.picture.w, geo_.picture.h);
            std::memset(&dsc_, 0, sizeof dsc_);
            dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
            dsc_.header.cf = pic->fmt == PixFmt::RGB565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_XRGB8888;
            dsc_.header.w = static_cast<std::uint32_t>(pic->w);
            dsc_.header.h = static_cast<std::uint32_t>(pic->h);
            dsc_.header.stride = static_cast<std::uint32_t>(pic->stride);
            dsc_.data = pic->px.data();
            dsc_.data_size = static_cast<std::uint32_t>(pic->px.size());
            lv_image_cache_drop(&dsc_);
            lv_image_set_src(img_, &dsc_);
            lv_obj_set_pos(img_, at.x, at.y);
        } else {
            lv_image_set_src(img_, nullptr);
        }
    }
    vis(chip_, v.count > 0);
    if (v.count > 0) set_text(chip_, std::to_string(v.index + 1) + " of " + std::to_string(v.count));
    set_text(title_, v.name);
    set_text(note_, v.meta.empty() ? std::string(kReviewHint) : v.meta + ". " + kReviewHint);
    lv_obj_move_foreground(chip_);
}

} // namespace ui
} // namespace vhs
