/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the app. One cp0::ui::Page (title "VHS", "Photo" on the review; the on-screen Esc, the status strip; REC never
 * goes in the band, D-03), the Capture page and the photo review under it, and the Controller (core/app/controller.hpp)
 * that owns the threads.
 *
 * How the picture gets here: the pipeline runs on its own threads and leaves the newest picture in a mailbox; an LVGL
 * timer at the display refresh (LV_DEF_REFR_PERIOD, Cassette's polling pattern) takes it, points the lv_image at it (no
 * copy) and invalidates the image only. LVGL never scales it. A second timer (250 ms) takes the controller's snapshot
 * (state, chip, note, toast, countdown, flash, the review). The camera is open only while the Capture page shows: the
 * review and a launcher's hide release it (brief 3.3 rule 5; the rest of section 13 is M6).
 *
 * Keys (brief 12.3, by key code): Space the photo (Photo mode); Tab Photo / Video; Esc cancels the countdown, else
 * closes the review, else quits; Stop cancels the countdown; Up / Down / Left / Right move between the buttons, Enter
 * presses one; on the review Left / Right show the other photos and Del asks to delete. R and S do nothing yet
 * (recording M3, settings M6). LVGL thread only.
 */
#pragma once

#include "capture_page.hpp"
#include "core/app/controller.hpp"
#include "core/layout/capture_layout.hpp"
#include "review_page.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <memory>

struct key_item;

namespace vhs {
namespace ui {

class VhsApp {
public:
    explicit VhsApp(lv_obj_t *screen);
    ~VhsApp();
    VhsApp(const VhsApp &) = delete;
    VhsApp &operator=(const VhsApp &) = delete;

private:
    void on_frame_timer();
    void on_status_timer();
    void on_key(const key_item &item);
    void on_back();
    void on_display_event(lv_event_t *e);
    void demo_next_notice();
    void open_review();
    void close_review();
    void ask_delete();
    void close_dialog();
    void dialog_choose(bool del);

    static void frame_timer_cb(lv_timer_t *t);
    static void status_timer_cb(lv_timer_t *t);
    static void keyboard_cb(lv_event_t *e);
    static void display_cb(lv_event_t *e);
    static void dialog_cancel_cb(void *p);
    static void dialog_delete_cb(void *p);

    lv_obj_t *screen_ = nullptr;
    lv_display_t *display_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    CaptureLayout geo_;
    ReviewLayout rgeo_;
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<CapturePage> capture_;
    std::unique_ptr<ReviewPage> review_;
    std::unique_ptr<cp0::ui::Dialog> dlg_;
    std::unique_ptr<Controller> ctl_;
    lv_timer_t *frame_timer_ = nullptr, *status_timer_ = nullptr;
    bool demo_notices_ = false;
    int demo_index_ = -1;
    bool in_review_ = false;

    /* the redraw timing of the last picture handed over */
    std::int64_t handoff_us_ = 0, handoff_ts_us_ = 0, refr_start_us_ = 0;
    bool handoff_pending_ = false;
};

} // namespace ui
} // namespace vhs
