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
 *   Countdown          (canvas Countdown-480x320) the self-timer's red ring and number over the live picture, Cancel in
 *                      Scenes' place, Photo | Video and Settings dimmed, "Photo in 3 s" and the timer's note; a tap on
 *                      the picture cancels too ("Video in 3 s" when the countdown starts a video)
 *   Recording          (canvas Recording-480x320) the REC tag with the timer in the chip's place, the Sound-on tag at
 *                      the picture's bottom left while the camera microphone hears something (not drawn without a
 *                      microphone; hidden while the lamp is off: provisional), the red shutter, "Photo now" in
 *                      Photo | Video's place, Scenes and Settings dimmed, "size, time left" and the canvas's note;
 *                      until the first frame is written the dot blinks (500 ms on / off), the tag reads
 *                      "REC 00:00:00", Sound on is hidden and Photo now dimmed (M4, the start indicator)
 * 0.1.2: with two or more cameras a chip with the camera's name sits at the picture's top right while the live view runs
 * (not while recording or counting down); a tap takes the next camera.
 * 0.1.2: in Photo mode, with a camera that can do manual exposure, an Exposure row ([-] "Exposure Auto" [+]) sits over the
 * picture's bottom while the live view runs (not while recording or counting down); the buttons move one stop, the label
 * brings Auto back; the format chip says "EV +1" while a stop is on, in either mode.
 * A photo taken flashes the picture once (D30; brief 12.4 item 2: provisional, a white veil fading out in 0.25 s).
 * Settings does nothing yet (M6). LVGL thread only.
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
#include <vector>

namespace vhs {
namespace ui {

/* The texts the canvas does not give (brief 12.4 item 3; the Controller's wording at the gates). */
constexpr const char *kLostOverlay = "Waiting for the camera...";
constexpr const char *kReplugHint = "If you moved the camera's switch, unplug and replug it, or restart the board.";

struct CaptureActions {
    std::function<void()> shutter, toggle, scenes, settings;
    std::function<void()> camera; /* 0.1.2: the camera chip: the next camera */
    std::function<void()> cancel; /* the countdown's Cancel and a tap on the picture during it */
    std::function<void(int)> exposure; /* 0.1.2: the Exposure row: -1 darker, +1 brighter, 0 Auto (the label) */
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
    /* The page shown or hidden (the photo review over it): its buttons leave the keypad group while hidden. */
    void show_page(bool on);
    /* The frame timer: the REC dot's blink while the scene starts (app/rec_indicator.hpp). */
    void tick(std::int64_t now_ms);

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
    lv_obj_t *photo_btn_ = nullptr, *video_btn_ = nullptr, *status_r_ = nullptr; /* Square class */
    bool photo_mode_ = true;
    std::string status_r_text_;
    lv_obj_t *nc_title_ = nullptr, *nc_text_ = nullptr, *nc_hint_ = nullptr, *nc_scenes_ = nullptr;
    lv_obj_t *cd_ring_ = nullptr, *cd_num_ = nullptr, *cancel_ = nullptr, *flash_ = nullptr;
    lv_obj_t *face_ = nullptr, *rec_face_ = nullptr, *toggle_text_ = nullptr;
    lv_obj_t *cam_ = nullptr, *cam_text_ = nullptr; /* 0.1.2: the camera chip */
    lv_obj_t *exp_minus_ = nullptr, *exp_label_ = nullptr, *exp_plus_ = nullptr; /* 0.1.2: the Exposure row */
    lv_obj_t *exp_text_ = nullptr;
    std::string cam_label_;
    int cam_w_ = 0;
    lv_obj_t *rec_tag_ = nullptr, *rec_text_ = nullptr, *lamp_tag_ = nullptr;
    lv_obj_t *make_tag(int x, int y, int h, int pad_l, int pad_r, int gap, int dot_d, int px, int radius,
                       std::uint32_t dot, lv_obj_t **text, lv_obj_t **dot_obj = nullptr);
    void set_status(const std::vector<std::string> &lefts, const std::vector<std::string> &rights);
    std::vector<lv_obj_t *> group_objs() const;
    void set_selected(lv_obj_t *b, bool on);
    lv_obj_t *rec_dot_ = nullptr;
    bool starting_ = false;
    std::int64_t since_ms_ = 0;
    std::uint64_t flash_seen_ = 0;
    bool counting_ = false;
    static void box_cb(lv_event_t *e);
    static void flash_anim_cb(void *obj, int32_t v);
    std::unique_ptr<Toast> toast_;
    lv_image_dsc_t dsc_{};
    bool have_picture_ = false;
    std::string last_key_;
    std::function<void()> fns_[9];
    std::function<void(int)> exp_fn_;
};

} // namespace ui
} // namespace vhs
