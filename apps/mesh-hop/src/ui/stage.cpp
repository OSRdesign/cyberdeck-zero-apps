/*
 * SPDX-License-Identifier: MIT
 *
 * The stage and the keyboard bridge of Mesh Hop on the shared app platform (see stage.hpp).
 */

#include "stage.hpp"

#include "keyboard_input.h"

#include <chrono>

namespace meshhop {

static_assert(KeyBridge::kReleased == KBD_KEY_RELEASED && KeyBridge::kPressed == KBD_KEY_PRESSED &&
                  KeyBridge::kRepeated == KBD_KEY_REPEATED,
              "KeyBridge key states are cp0_lvgl's");
static_assert(KeyBridge::kModShift == KBD_MOD_SHIFT && KeyBridge::kModCtrl == KBD_MOD_CTRL &&
                  KeyBridge::kModAlt == KBD_MOD_ALT,
              "KeyBridge modifiers are cp0_lvgl's");

Platform *Platform::current_ = nullptr;
bool Platform::keyboard_assumed_ = false;

uint64_t mono_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

Platform::Platform(lv_obj_t *screen) : screen_(screen), keys_(detect_layout())
{
    current_ = this;
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x000000), 0);     /* the launcher's black ground */
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    /* the drag feel of the app's own touch reader, kept: a scroll starts after 12 px, a flick throws less far */
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_scroll_limit(indev, 12);
            lv_indev_set_scroll_throw(indev, 20);
        }

    /* every key reaches the app as LV_EVENT_KEYBOARD on the active screen; the LVGL keypad path (the app has no
     * focusable widget in the input group) is intercepted, as the Serial Console's raw mode does */
    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    saved_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
    cp0_keyboard_set_lvgl_keypad_intercept(1);
}

Platform::~Platform()
{
    close_stage();
    cp0_keyboard_set_lvgl_keypad_intercept(saved_intercept_);
    if (LV_EVENT_KEYBOARD != 0) lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    if (current_ == this) current_ = nullptr;
}

void Platform::close_stage()
{
    on_key = nullptr;
    if (stage_) lv_obj_delete(stage_);        /* (deleted with its page too: the app resets stage_ through adopt_stage) */
    stage_ = nullptr;
}

lv_point_t Platform::to_stage(lv_point_t p) const
{
    if (!stage_) return p;
    lv_area_t a;
    lv_obj_get_coords(stage_, &a);
    p.x -= a.x1;
    p.y -= a.y1;
    return p;
}

lv_point_t stage_point(lv_point_t screen_point)
{
    const Platform *p = Platform::current();
    return p ? p->to_stage(screen_point) : screen_point;
}

bool Platform::touching() const
{
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED)
            return true;
    return false;
}

void Platform::keyboard_cb(lv_event_t *event)
{
    auto *self = static_cast<Platform *>(lv_event_get_user_data(event));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(event));
    if (!self || !item) return;
    KeyEvent e;
    if (self->keys_.feed(item->key_code, item->key_state, item->mods, item->keysym, item->utf8, e) && self->on_key)
        self->on_key(e);
}

} // namespace meshhop
