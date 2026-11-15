/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Capture page (see capture_page.hpp).
 */
#include "capture_page.hpp"

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

constexpr const char *kTurnedOverlay = "Camera turned. Restarting...";
constexpr const char *kTurnedNote = "The camera switch was moved. The picture comes back in about 3 seconds, upright, at "
                                    "the sizes the camera now offers.";
constexpr const char *kNoCameraTitle = "No camera found";
constexpr const char *kNoCameraText = "Plug the camera into a USB port. VHS shows the picture as soon as it appears.";
/* the canvas's Countdown note, with the timer's own seconds */
std::string countdown_note(int total)
{
    return "Self-timer " + std::to_string(total) +
           " s: the countdown runs over the live picture. A tap, Esc or Stop cancels it and nothing is taken. For a "
           "video the recording starts at zero; for a photo the picture flashes once.";
}

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

void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void set_text(lv_obj_t *l, const std::string &t)
{
    if (std::strcmp(lv_label_get_text(l), t.c_str()) != 0) lv_label_set_text(l, t.c_str());
}

void dim(lv_obj_t *b, bool on)
{
    lv_obj_set_style_opa(b, on ? LV_OPA_50 : LV_OPA_COVER, 0);
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

bool no_camera_page(CamState s)
{
    return s == CamState::NoCamera || s == CamState::NotBack || s == CamState::Failed;
}

} // namespace

CapturePage::CapturePage(lv_obj_t *root, const CaptureLayout &geo, CaptureActions act)
    : root_(root), geo_(geo), act_(std::move(act))
{
    /* one layer over the page's root holds every object of this page (shown / hidden with the review page) */
    root_ = lv_obj_create(root);
    plain(root_);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    fns_[0] = act_.shutter;
    fns_[1] = act_.toggle;
    fns_[2] = act_.scenes;
    fns_[3] = act_.settings;
    fns_[4] = act_.scenes;
    fns_[5] = act_.cancel;

    panel_ = lv_obj_create(root_);
    plain(panel_);
    place(panel_, geo_.panel);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(col::kPanel), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel_, geo_.panel_radius, 0);

    /* the picture box: black (the border of a letterboxed or pillarboxed frame); no rounded corners (an LVGL corner
     * mask over a 30 fps image costs the slow boards too much) */
    box_ = lv_obj_create(root_);
    plain(box_);
    place(box_, geo_.picture);
    lv_obj_set_style_bg_color(box_, lv_color_hex(col::kPicture), 0);
    lv_obj_set_style_bg_opa(box_, LV_OPA_COVER, 0);
    img_ = lv_image_create(box_);
    lv_obj_remove_flag(img_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(img_, 0, 0);

    chip_ = label(box_, geo_.chip_px, col::kWhite);
    lv_obj_set_style_text_opa(chip_, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(chip_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chip_, 115, 0); /* rgba(0,0,0,.45) */
    lv_obj_set_style_radius(chip_, geo_.chip_radius, 0);
    lv_obj_set_style_pad_hor(chip_, geo_.chip_pad_h, 0);
    lv_obj_set_style_pad_ver(chip_, geo_.chip_pad_v, 0);
    lv_obj_set_pos(chip_, geo_.chip_x, geo_.chip_y);

    dim_ = lv_obj_create(box_);
    plain(dim_);
    lv_obj_set_pos(dim_, 0, 0);
    lv_obj_set_size(dim_, geo_.picture.w, geo_.picture.h);
    lv_obj_set_style_bg_color(dim_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_, 140, 0); /* rgba(0,0,0,.55) */
    dim_text_ = label(dim_, geo_.overlay_px, col::kWhite);
    lv_obj_set_width(dim_text_, geo_.picture.w * 9 / 10);
    lv_obj_set_style_text_align(dim_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(dim_text_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_center(dim_text_);
    show(dim_, false);

    /* the countdown: the canvas's 96u ring (6u, red #e0553a) over a 35 % black disc, the number 52u white */
    cd_ring_ = lv_obj_create(box_);
    plain(cd_ring_);
    lv_obj_set_size(cd_ring_, geo_.cd_d, geo_.cd_d);
    lv_obj_set_style_radius(cd_ring_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cd_ring_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cd_ring_, 89, 0); /* rgba(0,0,0,.35) */
    lv_obj_set_style_border_color(cd_ring_, lv_color_hex(col::kHot), 0);
    lv_obj_set_style_border_width(cd_ring_, geo_.cd_border, 0);
    lv_obj_center(cd_ring_);
    cd_num_ = label(cd_ring_, geo_.cd_px, col::kWhite);
    lv_obj_center(cd_num_);
    show(cd_ring_, false);
    /* the flash: a white veil over the picture, faded out */
    flash_ = lv_obj_create(box_);
    plain(flash_);
    lv_obj_set_pos(flash_, 0, 0);
    lv_obj_set_size(flash_, geo_.picture.w, geo_.picture.h);
    lv_obj_set_style_bg_color(flash_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(flash_, LV_OPA_COVER, 0);
    show(flash_, false);
    /* a tap on the picture cancels the countdown (the box takes taps only then) */
    lv_obj_add_event_cb(box_, box_cb, LV_EVENT_CLICKED, this);

    shutter_ = lv_button_create(root_);
    lv_obj_remove_style_all(shutter_);
    place(shutter_, geo_.shutter);
    lv_obj_add_flag(shutter_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(shutter_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(shutter_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shutter_, lv_color_hex(col::kShutterBg), 0);
    lv_obj_set_style_bg_opa(shutter_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(shutter_, lv_color_hex(col::kShutterRing), 0);
    lv_obj_set_style_border_width(shutter_, geo_.ring, 0);
    lv_obj_set_style_outline_color(shutter_, lv_color_hex(col::kText), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(shutter_, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(shutter_, 2, LV_STATE_FOCUS_KEY);
    lv_obj_t *face = lv_obj_create(shutter_);
    plain(face);
    const int fd = geo_.shutter.w * 66 / 100;
    lv_obj_set_size(face, fd, fd);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(col::kWhite), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_center(face);
    lv_obj_add_event_cb(shutter_, button_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(shutter_, reinterpret_cast<void *>(static_cast<std::intptr_t>(0)));

    toggle_ = make_button(root_, geo_.toggle, "Photo | Video", geo_.toggle_px, nullptr);
    lv_obj_set_user_data(toggle_, reinterpret_cast<void *>(static_cast<std::intptr_t>(1)));
    scenes_ = make_button(root_, geo_.scenes, "Scenes", geo_.button_px, nullptr, &scenes_sub_);
    lv_obj_set_user_data(scenes_, reinterpret_cast<void *>(static_cast<std::intptr_t>(2)));
    settings_ = make_button(root_, geo_.settings, "Settings", geo_.button_px, nullptr);
    lv_obj_set_user_data(settings_, reinterpret_cast<void *>(static_cast<std::intptr_t>(3)));
    /* the countdown's Cancel: the canvas's primary button in Scenes' place */
    cancel_ = make_button(root_, geo_.scenes, "Cancel", geo_.button_px, nullptr);
    lv_obj_set_user_data(cancel_, reinterpret_cast<void *>(static_cast<std::intptr_t>(5)));
    lv_obj_set_style_bg_color(cancel_, lv_color_hex(col::kPrimary), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(cancel_, 0), lv_color_hex(col::kWhite), 0);
    show(cancel_, false);

    title_ = label(root_, geo_.title_px, col::kText);
    lv_obj_set_pos(title_, geo_.title.x, geo_.title.y);
    lv_obj_set_width(title_, geo_.title.w);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
    note_ = label(root_, geo_.note_px, col::kMuted);
    lv_obj_set_pos(note_, geo_.note.x, geo_.note.y);
    lv_obj_set_width(note_, geo_.note.w);
    lv_label_set_long_mode(note_, LV_LABEL_LONG_MODE_WRAP);

    nc_title_ = label(root_, geo_.nc_title_px, col::kText);
    lv_obj_set_style_text_align(nc_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nc_title_, LV_LABEL_LONG_MODE_WRAP);
    nc_text_ = label(root_, geo_.nc_text_px, col::kMuted);
    lv_obj_set_style_text_align(nc_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nc_text_, LV_LABEL_LONG_MODE_WRAP);
    nc_hint_ = label(root_, geo_.nc_hint_px, col::kMuted);
    lv_obj_set_style_text_align(nc_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nc_hint_, LV_LABEL_LONG_MODE_WRAP);
    nc_scenes_ = make_button(root_, geo_.nc_scenes, "Scenes", geo_.nc_scenes_px, nullptr);
    lv_obj_set_user_data(nc_scenes_, reinterpret_cast<void *>(static_cast<std::intptr_t>(4)));
    set_text(nc_title_, kNoCameraTitle);
    set_text(nc_text_, kNoCameraText);
    place_nocamera(false);

    toast_ = std::make_unique<Toast>(root_, geo_.toast, geo_.toast_px, nullptr);

    if (lv_group_t *g = cdk::input_group()) {
        for (lv_obj_t *b : {shutter_, toggle_, scenes_, cancel_, settings_, nc_scenes_}) lv_group_add_obj(g, b);
    }
    for (lv_obj_t *o : {nc_title_, nc_text_, nc_hint_, nc_scenes_}) show(o, false);
    show(chip_, false);
}

CapturePage::~CapturePage()
{
    if (img_ && lv_obj_is_valid(img_)) lv_image_set_src(img_, nullptr); /* before the pictures go */
}

lv_obj_t *CapturePage::make_button(lv_obj_t *parent, const Rect &r, const char *name, int px, std::function<void()> fn,
                                   lv_obj_t **sub)
{
    (void)fn;
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    place(b, r);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(col::kButton), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, geo_.button_radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col::kButtonPressed), LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b, lv_color_hex(col::kText), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(b, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *n = label(b, px, col::kText);
    lv_label_set_text(n, name);
    fit_label(n, px, r.w - 2 * std::max(2, static_cast<int>(4 * geo_.u)));
    if (sub) {
        *sub = label(b, geo_.sub_px, col::kMuted);
        show(*sub, false); /* the scene count comes with Scenes (M4) */
    }
    lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, this);
    return b;
}

void CapturePage::button_cb(lv_event_t *e)
{
    auto *self = static_cast<CapturePage *>(lv_event_get_user_data(e));
    lv_obj_t *b = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const int i = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(b)));
    if (i >= 0 && i < 6 && self->fns_[i]) self->fns_[i]();
}

void CapturePage::box_cb(lv_event_t *e)
{
    auto *self = static_cast<CapturePage *>(lv_event_get_user_data(e));
    if (self->counting_ && self->act_.cancel) self->act_.cancel();
}

void CapturePage::flash_anim_cb(void *obj, int32_t v)
{
    lv_obj_t *o = static_cast<lv_obj_t *>(obj);
    lv_obj_set_style_bg_opa(o, static_cast<lv_opa_t>(v), 0);
    if (v <= 0) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void CapturePage::place_nocamera(bool hint)
{
    const Rect t = hint ? geo_.nch_title : geo_.nc_title;
    const Rect x = hint ? geo_.nch_text : geo_.nc_text;
    const Rect b = hint ? geo_.nch_scenes : geo_.nc_scenes;
    lv_obj_set_pos(nc_title_, t.x, t.y);
    lv_obj_set_width(nc_title_, t.w);
    lv_obj_set_pos(nc_text_, x.x, x.y);
    lv_obj_set_width(nc_text_, x.w);
    lv_obj_set_pos(nc_hint_, geo_.nc_hint.x, geo_.nc_hint.y);
    lv_obj_set_width(nc_hint_, geo_.nc_hint.w);
    place(nc_scenes_, b);
}

void CapturePage::show_page(bool on)
{
    show(root_, on);
    lv_group_t *g = cdk::input_group();
    if (g) {
        for (lv_obj_t *b : {shutter_, toggle_, scenes_, cancel_, settings_, nc_scenes_}) {
            if (on) lv_group_add_obj(g, b);
            else lv_group_remove_obj(b);
        }
    }
    last_key_.clear(); /* everything set again at the next update */
}

void CapturePage::focus_first()
{
    lv_group_t *g = cdk::input_group();
    if (!g) return;
    lv_obj_t *f = lv_group_get_focused(g);
    if (f && !lv_obj_has_flag(f, LV_OBJ_FLAG_HIDDEN)) return;
    lv_group_focus_obj(lv_obj_has_flag(shutter_, LV_OBJ_FLAG_HIDDEN) ? nc_scenes_ : shutter_);
}

void CapturePage::show_picture(const Picture &p)
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
    lv_image_set_src(img_, &dsc_); /* the same descriptor every frame: reads its header again and invalidates */
    if (lv_obj_get_x(img_) != at.x || lv_obj_get_y(img_) != at.y) lv_obj_set_pos(img_, at.x, at.y);
    have_picture_ = true;
}

void CapturePage::update(const Snapshot &s)
{
    const bool nocam = no_camera_page(s.state);
    const bool turned = s.state == CamState::Turned;
    const bool lost = s.state == CamState::Lost;
    const bool live = s.state == CamState::Streaming;
    const bool keep_picture = live || turned || lost;

    /* the chip */
    if (live && s.mode.valid())
        set_text(chip_, std::to_string(s.mode.w) + "x" + std::to_string(s.mode.h) + "  " + std::to_string(s.mode.fps) + " fps");
    show(chip_, live && s.mode.valid() && have_picture_);

    /* the flash: once per photo taken */
    if (s.flash != flash_seen_) {
        const bool first = flash_seen_ == 0 && s.flash > 1; /* a page made after photos: no flash for old ones */
        flash_seen_ = s.flash;
        if (!first && live) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, flash_);
            lv_anim_set_values(&a, 230, 0);
            lv_anim_set_duration(&a, 250);
            lv_anim_set_exec_cb(&a, flash_anim_cb);
            lv_obj_set_style_bg_opa(flash_, 230, 0);
            show(flash_, true);
            lv_obj_move_foreground(flash_);
            lv_anim_start(&a);
        }
    }
    /* the countdown */
    const bool counting = s.countdown > 0 && live;
    if (counting) set_text(cd_num_, std::to_string(s.countdown));

    /* the mode word and the note */
    const bool photo = s.capture_mode == CaptureMode::Photo;
    std::string note;
    if (turned) {
        note = kTurnedNote;
    } else if (counting) {
        note = countdown_note(s.timer_total);
    } else if (!s.camera.empty()) {
        note = s.camera + (s.yuyv ? ", YUYV. " : ", MJPEG. ");
        /* the Video line is provisional (the canvas draws the Photo mode only) */
        note += photo ? "Space takes a photo, Tab switches to Video, R records." : "Space or R records, Tab switches to Photo.";
    }
    set_text(title_, counting ? "Photo in " + std::to_string(s.countdown) + " s" : (photo ? "Photo" : "Video"));
    set_text(note_, note);
    lv_obj_set_pos(note_, geo_.note.x, turned ? geo_.title.y : geo_.note.y);

    /* the toast replaces the mode word and the note */
    const bool toast = toast_->set(s.notice);

    const std::string key = std::string(state_name(s.state)) + "|" + (s.replug_hint ? "hint" : "") + "|" + s.detail +
                            "|" + (toast ? "toast" : "") + "|" + (note.empty() ? "" : "note") + "|" +
                            (counting ? "count" : "");
    if (key == last_key_) return;
    last_key_ = key;
    counting_ = counting;
    show(cd_ring_, counting);
    if (counting) lv_obj_move_foreground(cd_ring_);
    if (counting) lv_obj_add_flag(box_, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(box_, LV_OBJ_FLAG_CLICKABLE);

    for (lv_obj_t *o : {box_, shutter_, settings_}) show(o, !nocam);
    show(toggle_, !nocam && !turned);
    show(scenes_, !nocam && !counting);
    show(cancel_, !nocam && counting);
    dim(toggle_, counting);
    show(title_, !nocam && !turned && !toast);
    show(note_, !nocam && !toast && !note.empty());
    dim(scenes_, turned);
    dim(settings_, turned || counting);
    if (!keep_picture && have_picture_) {
        lv_image_set_src(img_, nullptr);
        have_picture_ = false;
    }
    show(dim_, turned || lost);
    if (turned || lost) {
        set_text(dim_text_, turned ? kTurnedOverlay : kLostOverlay);
        lv_obj_center(dim_text_);
    }
    /* the NoCamera page */
    std::string hint;
    if (s.state == CamState::NotBack && s.replug_hint) hint = kReplugHint;
    else if (s.state == CamState::Failed) hint = s.detail;
    for (lv_obj_t *o : {nc_title_, nc_text_, nc_scenes_}) show(o, nocam);
    show(nc_hint_, nocam && !hint.empty());
    if (nocam) {
        set_text(nc_hint_, hint);
        place_nocamera(!hint.empty());
    }
    focus_first();
}

} // namespace ui
} // namespace vhs
