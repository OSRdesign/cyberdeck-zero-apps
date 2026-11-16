/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the windowed Playback page (canvas PlaybackWindowed-480x320 without its Full screen button, which is P2;
 * brief 10, 12.1). The picture box of the Capture page (the movie fitted, never cropped), Pause / Play (the primary
 * button), "-" and "+" with "Volume 72%" under them, the seek bar (the gold fill, the white knob) with the times under
 * it, and the scene's name. A tap or a drag on the seek bar moves the knob and seeks once, on release (Vinyl's
 * SeekScrub rule). The toast line at the panel's bottom (provisional place, as on Scenes). Geometry from
 * core/layout/playback_layout.hpp. LVGL thread only.
 */
#pragma once

#include "core/app/controller.hpp"
#include "core/layout/playback_layout.hpp"
#include "notices.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>

namespace vhs {
namespace ui {

struct PlaybackActions {
    std::function<void()> toggle, minus, plus;
    std::function<void(std::int64_t ms)> seek; /* the release of a tap or a drag on the bar */
};

class PlaybackPage {
public:
    PlaybackPage(lv_obj_t *root, const PlaybackLayout &geo, PlaybackActions act);
    ~PlaybackPage();
    PlaybackPage(const PlaybackPage &) = delete;
    PlaybackPage &operator=(const PlaybackPage &) = delete;

    void show(bool on);
    void update(const PlaybackView &v, const Notice &notice);
    void show_picture(const Picture &p);
    void clear_picture();
    void focus_first();

private:
    lv_obj_t *button(const Rect &r, const char *text, int px, bool primary, int id);
    void place_knob(std::int64_t ms, std::int64_t total);
    static void button_cb(lv_event_t *e);
    static void bar_cb(lv_event_t *e);

    lv_obj_t *root_ = nullptr;
    PlaybackLayout geo_;
    PlaybackActions act_;
    lv_obj_t *panel_ = nullptr, *box_ = nullptr, *img_ = nullptr;
    lv_obj_t *pause_ = nullptr, *pause_text_ = nullptr, *minus_ = nullptr, *plus_ = nullptr, *volume_ = nullptr;
    lv_obj_t *track_ = nullptr, *fill_ = nullptr, *knob_ = nullptr, *hit_ = nullptr;
    lv_obj_t *t_left_ = nullptr, *t_right_ = nullptr, *name_ = nullptr;
    std::unique_ptr<Toast> toast_;
    lv_image_dsc_t dsc_{};
    bool have_picture_ = false;
    bool dragging_ = false;
    std::int64_t drag_ms_ = 0, total_ = 0;
};

} // namespace ui
} // namespace vhs
