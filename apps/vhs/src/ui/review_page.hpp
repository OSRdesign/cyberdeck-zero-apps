/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the photo review, minimal (canvas PhotoReview-480x320, brief 9.5): the photo fitted to the picture box with
 * the "n of N" chip, Previous, Next and Delete (Send to Notes is optional, M6: its slot is kept, the button not drawn),
 * the file name and its line ("1920x1080, 98 KB, Nov 14 2026 18:47. Left and Right show the other photos; Delete asks
 * first."). Left / Right walk the photos (newest first), Del or the Delete button asks first (the toolkit's dialog),
 * Esc goes back. Geometry: core/layout/capture_layout.hpp review_layout(). In M2 it opens from the Capture page's
 * Scenes button (the scene list is M4). LVGL thread only.
 */
#pragma once

#include "core/app/controller.hpp"
#include "core/layout/capture_layout.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace vhs {
namespace ui {

struct ReviewActions {
    std::function<void()> prev, next, del;
};

class ReviewPage {
public:
    ReviewPage(lv_obj_t *root, const ReviewLayout &geo, ReviewActions act);
    ~ReviewPage();
    ReviewPage(const ReviewPage &) = delete;
    ReviewPage &operator=(const ReviewPage &) = delete;

    void show(bool on);
    bool shown() const { return shown_; }
    /* The view (the list's place, the names) and its picture (nullptr: none yet / none). */
    void update(const ReviewView &v, const Picture *pic);
    void focus_first();

private:
    lv_obj_t *button(const Rect &r, const char *text, int i);
    static void button_cb(lv_event_t *e);

    lv_obj_t *root_ = nullptr;
    ReviewLayout geo_;
    ReviewActions act_;
    lv_obj_t *panel_ = nullptr, *box_ = nullptr, *img_ = nullptr, *chip_ = nullptr;
    lv_obj_t *prev_ = nullptr, *next_ = nullptr, *del_ = nullptr, *title_ = nullptr, *note_ = nullptr;
    lv_image_dsc_t dsc_{};
    std::uint64_t pic_gen_ = 0;
    bool shown_ = false;
};

} // namespace ui
} // namespace vhs
