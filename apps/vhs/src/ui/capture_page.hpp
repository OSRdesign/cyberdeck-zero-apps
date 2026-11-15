/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the Capture page (canvas boards Capture-480x320, Capture-320x480, CameraTurned-480x320, NoCamera-480x320 and
 * the toast of Notices-480x320; brief 12.1). Geometry from core/layout/capture_layout.hpp. In the dark panel under the
 * toolkit's header: the picture box (the whole frame centred, a black border; the "WxH  30 fps" chip at its top left),
 * the shutter, Photo | Video, Scenes and Settings, the mode word and the note. States (core/camera/capture.hpp):
 *   Streaming          the live picture
 *   Lost               the last picture dimmed, with an overlay line (provisional text, brief 12.4 item 3)
 *   Turned             the last picture dimmed, "Camera turned. Restarting...", Photo | Video hidden, Scenes and Settings
 *                      dimmed, the canvas's note in the mode word's place
 *   NoCamera, NotBack, Failed   the NoCamera page: "No camera found", its line, [the replug hint or the reason], Scenes
 *   Busy, NoAccess     the page with an empty picture box; the toast says why while it lasts
 * M1 has no photo, recording, scenes or settings yet: the shutter, Scenes and Settings are drawn and focusable but do
 * nothing (M2, M4, M6). LVGL thread only.
 */
#pragma once

#include "core/app/controller.hpp"
#include "core/layout/capture_layout.hpp"
#include "core/live/pipeline.hpp"
#include "notices.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>

namespace vhs {
namespace ui {

/* The texts the canvas does not give (brief 12.4 item 3; the Controller's wording at the gates). */
constexpr const char *kLostOverlay = "Waiting for the camera...";
constexpr const char *kReplugHint = "If you moved the camera's switch, unplug and replug it, or restart the board.";

struct CaptureActions {
    std::function<void()> shutter, toggle, scenes, settings;
};

class CapturePage {
public:
    CapturePage(lv_obj_t *root, const CaptureLayout &geo, CaptureActions act);
    ~CapturePage();
    CapturePage(const CapturePage &) = delete;
    CapturePage &operator=(const CapturePage &) = delete;

    /* The state, the chip, the note, the toast. */
    void update(const Snapshot &s);
    /* A new picture (the frame timer): points the image at it (no copy), centred in the box. */
    void show_picture(const Picture &p);
    void focus_first();

private:
    lv_obj_t *make_button(lv_obj_t *parent, const Rect &r, const char *name, int px, std::function<void()> fn,
                          lv_obj_t **sub = nullptr);
    void place_nocamera(bool hint);
    static void button_cb(lv_event_t *e);

    lv_obj_t *root_ = nullptr;
    CaptureLayout geo_;
    CaptureActions act_;
    lv_obj_t *panel_ = nullptr, *box_ = nullptr, *img_ = nullptr, *chip_ = nullptr;
    lv_obj_t *dim_ = nullptr, *dim_text_ = nullptr;
    lv_obj_t *shutter_ = nullptr, *toggle_ = nullptr, *scenes_ = nullptr, *settings_ = nullptr, *scenes_sub_ = nullptr;
    lv_obj_t *title_ = nullptr, *note_ = nullptr;
    lv_obj_t *nc_title_ = nullptr, *nc_text_ = nullptr, *nc_hint_ = nullptr, *nc_scenes_ = nullptr;
    std::unique_ptr<Toast> toast_;
    lv_image_dsc_t dsc_{};
    bool have_picture_ = false;
    std::string last_key_;
    std::function<void()> fns_[5];
};

} // namespace ui
} // namespace vhs
