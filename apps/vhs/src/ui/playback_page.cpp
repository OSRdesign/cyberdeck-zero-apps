/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Playback page (see playback_page.hpp).
 */
#include "playback_page.hpp"

#include "app.hpp"
#include "core/scenes/index.hpp"
#include "theme.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace vhs {
namespace ui {
namespace {

void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
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

void place(lv_obj_t *o, const Rect &r)
{
    lv_obj_set_pos(o, r.x, r.y);
    lv_obj_set_size(o, r.w, r.h);
}

} // namespace

PlaybackPage::PlaybackPage(lv_obj_t *root, const PlaybackLayout &geo, PlaybackActions act) : geo_(geo), act_(std::move(act))
{
    root_ = lv_obj_create(root);
    plain(root_);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
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

    pause_ = button(geo_.pause, "Pause", geo_.pause_px, true, 0);
    pause_text_ = lv_obj_get_child(pause_, 0);
    minus_ = button(geo_.minus, "-", geo_.pm_px, false, 1);
    plus_ = button(geo_.plus, "+", geo_.pm_px, false, 2);
    volume_ = label(root_, geo_.volume_px, col::kMuted);
    lv_obj_set_width(volume_, geo_.volume.w);
    lv_obj_set_style_text_align(volume_, geo_.column ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(volume_, geo_.volume.x, geo_.volume.y);

    /* the seek bar: the track, the gold fill, the knob; a touch box at least ROW high over it */
    track_ = lv_obj_create(root_);
    plain(track_);
    place(track_, geo_.bar);
    lv_obj_set_style_radius(track_, geo_.bar.h / 2, 0);
    lv_obj_set_style_bg_color(track_, lv_color_hex(col::kButton), 0);
    lv_obj_set_style_bg_opa(track_, LV_OPA_COVER, 0);
    fill_ = lv_obj_create(track_);
    plain(fill_);
    lv_obj_set_pos(fill_, 0, 0);
    lv_obj_set_size(fill_, 0, geo_.bar.h);
    lv_obj_set_style_radius(fill_, geo_.bar.h / 2, 0);
    lv_obj_set_style_bg_color(fill_, lv_color_hex(col::kAccent), 0);
    lv_obj_set_style_bg_opa(fill_, LV_OPA_COVER, 0);
    knob_ = lv_obj_create(root_);
    plain(knob_);
    lv_obj_set_size(knob_, geo_.knob, geo_.knob);
    lv_obj_set_style_radius(knob_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob_, LV_OPA_COVER, 0);
    hit_ = lv_obj_create(root_);
    plain(hit_);
    lv_obj_add_flag(hit_, LV_OBJ_FLAG_CLICKABLE);
    place(hit_, geo_.bar_hit);
    lv_obj_add_event_cb(hit_, bar_cb, LV_EVENT_ALL, this);

    t_left_ = label(root_, geo_.time_px, col::kText);
    lv_obj_set_pos(t_left_, geo_.time_left.x, geo_.time_left.y);
    t_right_ = label(root_, geo_.time_px, col::kMuted);
    lv_obj_set_width(t_right_, geo_.time_right.w);
    lv_obj_set_style_text_align(t_right_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(t_right_, geo_.time_right.x, geo_.time_right.y);
    name_ = label(root_, geo_.name_px, col::kText);
    lv_obj_set_pos(name_, geo_.name.x, geo_.name.y);
    lv_obj_set_size(name_, geo_.name.w, geo_.name_px * 5 / 4);
    lv_label_set_long_mode(name_, LV_LABEL_LONG_MODE_CLIP);
    toast_ = std::make_unique<Toast>(root_, geo_.toast, geo_.toast_px, nullptr);
    place_knob(0, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

PlaybackPage::~PlaybackPage()
{
    if (img_ && lv_obj_is_valid(img_)) lv_image_set_src(img_, nullptr);
}

lv_obj_t *PlaybackPage::button(const Rect &r, const char *text, int px, bool primary, int id)
{
    lv_obj_t *b = lv_button_create(root_);
    lv_obj_remove_style_all(b);
    place(b, r);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? col::kPrimary : col::kButton), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, geo_.radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col::kButtonPressed), LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b, lv_color_hex(col::kText), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_t *l = label(b, px, primary ? col::kWhite : col::kText);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_user_data(b, reinterpret_cast<void *>(static_cast<std::intptr_t>(id)));
    lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, this);
    return b;
}

void PlaybackPage::button_cb(lv_event_t *e)
{
    auto *self = static_cast<PlaybackPage *>(lv_event_get_user_data(e));
    const int id = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(e)))));
    if (id == 0 && self->act_.toggle) self->act_.toggle();
    else if (id == 1 && self->act_.minus) self->act_.minus();
    else if (id == 2 && self->act_.plus) self->act_.plus();
}

void PlaybackPage::bar_cb(lv_event_t *e)
{
    auto *self = static_cast<PlaybackPage *>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p{};
    if (in) lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        /* the knob follows the finger; nothing seeks until the release (one seek per gesture) */
        self->dragging_ = true;
        self->drag_ms_ = seek_ms_at(self->geo_, p.x, self->total_);
        self->place_knob(self->drag_ms_, self->total_);
        set_text(self->t_left_, duration_text(self->drag_ms_));
    } else if (code == LV_EVENT_RELEASED && self->dragging_) {
        self->dragging_ = false;
        if (self->act_.seek && self->total_ > 0) self->act_.seek(self->drag_ms_);
    } else {
        self->dragging_ = false;
    }
}

void PlaybackPage::place_knob(std::int64_t ms, std::int64_t total)
{
    const int x = seek_x_of(geo_, ms, total);
    lv_obj_set_size(fill_, std::max(0, x - geo_.bar.x), geo_.bar.h);
    lv_obj_set_pos(knob_, x - geo_.knob / 2, geo_.bar.y + geo_.bar.h / 2 - geo_.knob / 2);
}

void PlaybackPage::show(bool on)
{
    if (on) lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_group_t *g = cdk::input_group();
    for (lv_obj_t *b : {pause_, minus_, plus_}) {
        if (!g) break;
        if (on) lv_group_add_obj(g, b);
        else lv_group_remove_obj(b);
    }
    if (!on) clear_picture();
}

void PlaybackPage::focus_first()
{
    if (lv_group_t *g = cdk::input_group()) lv_group_focus_obj(pause_);
}

void PlaybackPage::update(const PlaybackView &v, const Notice &notice)
{
    set_text(pause_text_, v.playing ? "Pause" : "Play");
    set_text(volume_, v.volume >= 0 ? "Volume " + std::to_string(v.volume) + "%" : std::string("Volume"));
    total_ = std::max<std::int64_t>(0, v.duration_ms);
    if (!dragging_) {
        place_knob(v.pos_ms, total_);
        set_text(t_left_, duration_text(v.pos_ms));
    }
    set_text(t_right_, duration_text(v.duration_ms));
    set_text(name_, v.title);
    toast_->set(notice);
}

void PlaybackPage::show_picture(const Picture &p)
{
    if (p.w <= 0 || p.h <= 0) return;
    const Rect at = place_in_box(p.w, p.h, geo_.picture.w, geo_.picture.h);
    std::memset(&dsc_, 0, sizeof dsc_);
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = p.fmt == PixFmt::RGB565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_XRGB8888;
    dsc_.header.w = static_cast<std::uint32_t>(p.w);
    dsc_.header.h = static_cast<std::uint32_t>(p.h);
    dsc_.header.stride = static_cast<std::uint32_t>(p.stride);
    dsc_.data = p.px.data();
    dsc_.data_size = static_cast<std::uint32_t>(p.px.size());
    lv_image_cache_drop(&dsc_);
    lv_image_set_src(img_, &dsc_);
    if (lv_obj_get_x(img_) != at.x || lv_obj_get_y(img_) != at.y) lv_obj_set_pos(img_, at.x, at.y);
    have_picture_ = true;
}

void PlaybackPage::clear_picture()
{
    if (have_picture_) lv_image_set_src(img_, nullptr);
    have_picture_ = false;
}

} // namespace ui
} // namespace vhs
