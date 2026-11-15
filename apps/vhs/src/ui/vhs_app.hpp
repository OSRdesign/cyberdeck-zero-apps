/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the app. One cp0::ui::Page (title "VHS", "Scenes" on the scenes list, "Photo" on the review; the on-screen
 * Esc, the status strip; REC never goes in the band, D-03), the Capture page, the Scenes page with its options sheet,
 * the photo review, and the Controller (core/app/controller.hpp) that owns the threads.
 *
 * How the picture gets here: the pipeline runs on its own threads and leaves the newest picture in a mailbox; an LVGL
 * timer at the display refresh (LV_DEF_REFR_PERIOD, Cassette's polling pattern) takes it, points the lv_image at it (no
 * copy) and invalidates the image only; the same timer blinks the REC dot while a scene starts and follows a held row.
 * A second timer (250 ms) takes the controller's snapshot (state, chip, note, toast, countdown, flash, the scenes, the
 * review). The camera is open only while the Capture page shows: the Scenes page, the review and a launcher's hide
 * release it (brief 3.3 rule 5; a hide during a recording finishes the scene first, section 13).
 *
 * Keys (brief 12.3, by key code). Capture: Space the photo (Photo mode), or starts / stops a scene (Video mode, and
 * always while recording); R starts / stops a scene in either mode; Tab Photo / Video; Esc cancels the countdown, else
 * finishes a recording and quits (12.4 item 1), else quits; Stop cancels the countdown. Scenes: Up / Down move, Enter
 * opens (a photo: the review; a movie: playback, M5), Ctrl+O the options sheet, N rename, L lock, U copy (M6), Del
 * delete (asks first), Tab the next filter (provisional), Esc back to Capture. The sheet: Up / Down, Enter, its keys, Esc.
 * Review: Left / Right, Del asks, Esc back to where it came from. S does nothing yet (settings M6). LVGL thread only.
 */
#pragma once

#include "capture_page.hpp"
#include "core/app/controller.hpp"
#include "core/layout/capture_layout.hpp"
#include "core/layout/scenes_layout.hpp"
#include "review_page.hpp"
#include "scene_sheet.hpp"
#include "scenes_page.hpp"

#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
    enum class View { Capture, Scenes, Review };
    void on_frame_timer();
    void on_status_timer();
    void on_key(const key_item &item);
    void on_back();
    void on_display_event(lv_event_t *e);
    void demo_next_notice();
    void open_scenes();
    void close_scenes();
    void open_review(const std::string &start);
    void close_review();
    void scene_open(int index);
    void open_sheet(int index);
    void sheet_act(SheetAction a);
    void ask_delete();
    void ask_rename();
    void close_dialog();
    void dialog_choose(bool yes);
    void close_text();
    const SceneRow *scene_at(int index) const;
    void scenes_key(std::uint32_t code, char ch, bool ctrl);

    static void frame_timer_cb(lv_timer_t *t);
    static void status_timer_cb(lv_timer_t *t);
    static void keyboard_cb(lv_event_t *e);
    static void display_cb(lv_event_t *e);
    static void dialog_cancel_cb(void *p);
    static void dialog_yes_cb(void *p);

    lv_obj_t *screen_ = nullptr;
    lv_display_t *display_ = nullptr;
    cp0_ui_roller_page_t layout_{};
    CaptureLayout geo_;
    ReviewLayout rgeo_;
    ScenesLayout sgeo_;
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<CapturePage> capture_;
    std::unique_ptr<ScenesPage> scenes_;
    std::unique_ptr<SceneSheet> sheet_;
    std::unique_ptr<ReviewPage> review_;
    std::unique_ptr<cp0::ui::Dialog> dlg_;
    std::unique_ptr<cp0::ui::Dialog> text_dlg_;
    std::unique_ptr<cp0::ui::TextField> field_;
    std::function<void(bool)> on_dialog_;
    std::function<void(const std::string &)> on_text_;
    std::unique_ptr<Controller> ctl_;
    lv_timer_t *frame_timer_ = nullptr, *status_timer_ = nullptr;
    bool demo_notices_ = false;
    int demo_index_ = -1;
    View view_ = View::Capture;
    bool review_from_scenes_ = false;
    std::string sheet_name_;   /* the file the sheet (and its dialogs) act on */
    bool sheet_photo_ = false;
    std::string pending_review_; /* Play on a photo: the review opens on the next LVGL cycle */

    /* the redraw timing of the last picture handed over */
    std::int64_t handoff_us_ = 0, handoff_ts_us_ = 0, refr_start_us_ = 0;
    bool handoff_pending_ = false;
};

} // namespace ui
} // namespace vhs
