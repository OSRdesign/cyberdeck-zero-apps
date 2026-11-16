/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the toast (canvas board Notices-480x320: a pill 400 x 30 at 480x320, cream #dcd4bd with #222 text, or red
 * #e0553a with white text, 12 px bold, centred). It takes the place of the Capture page's mode word and note
 * (core/layout/capture_layout.hpp); a second line makes it taller. Timed (core/app/notice_queue.hpp: 4 s each); it
 * takes a tap only when given on_tap (the 30 px pill is under the touch target, so M1 gives none). LVGL thread only.
 */
#pragma once

#include "core/app/notice_queue.hpp"
#include "core/live/convert.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace vhs {
namespace ui {

class Toast {
public:
    Toast(lv_obj_t *parent, const Rect &area, int font_px, std::function<void()> on_tap);
    /* Shows n (text "" hides it); true when it shows. */
    bool set(const Notice &n);
    bool shown() const { return shown_; }
    lv_obj_t *obj() const { return bg_; }

private:
    static void click_cb(lv_event_t *e);
    lv_obj_t *bg_ = nullptr, *text_ = nullptr;
    Rect area_;
    std::function<void()> on_tap_;
    Notice cur_;
    bool shown_ = false;
};

} // namespace ui
} // namespace vhs
