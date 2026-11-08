/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the cassette window of the recorder page.
 *
 * The dark window with its 3 px frame and, inside, the cassette drawn once from primitives into a canvas (shell, cream
 * label with the red stripe, the handwritten take name, SIDE A, the format, the hub window, the head opening, two
 * screws; no tape: the shell hides it). The canvas is drawn again only when the label's texts change.
 *
 * The hubs: one white hub with three spokes (a 60-degree symmetry, so 120 degrees repeat it too) is pre-rendered at
 * start as 12 frames of 10 degrees each (opaque, on the hub window's colour); two small image objects show the frame of
 * their hub. A timer (fps adjustable, CASSETTE_ANIM_FPS, default 20; CASSETTE_ANIM=0 stops it) advances each hub by its
 * angular speed and swaps the frame only when the 10-degree step changes, so a frame costs two image blits of about
 * 26 x 26 px: no per-frame image rotation (the Zero 2 W is the slowest board).
 *   recording, playing        counterclockwise, left hub 2.4 s and right hub 1.8 s a turn (the boards' speeds)
 *   scanning (REW / FWD 5x)   clockwise (REW) or counterclockwise (FWD), 0.45 s and 0.3 s a turn
 *   anything else             still
 *
 * Overlays inside the window: the record countdown (a dark panel, RECORDING IN, a red ring that empties, the big
 * number, the hint; a tap on it calls on_cancel), the scan badge ("<< 5x" / ">> 5x"), and the empty window of the
 * no-sound-input state with its message.
 */
#pragma once

#include "fit.hpp"
#include "view_model.hpp"
#include "widgets.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cassette {
namespace ui {

struct AnimOptions {
    bool enabled = true;
    int fps = 20;
};
/* From the environment: CASSETTE_ANIM=0 disables, CASSETTE_ANIM_FPS=<1..60>. */
AnimOptions anim_options_from_env();

class CassetteWindow {
public:
    /* window: the frame in board px; tape: where the 200 x 104 cassette drawing sits and its board scale (1 landscape,
     * 1.4 portrait); ring_d / number_px / label_y / ring_y / number_y / hint_y: the countdown layout in board px. */
    struct Layout {
        DRect window;
        double tape_x, tape_y, tape_scale;
        double ring_d, ring_y, number_px, number_y, label_y, hint_y;
        DRect badge;
    };
    CassetteWindow(lv_obj_t *parent, const Fit &fit, const Layout &lay, AnimOptions anim, std::function<void()> on_cancel);
    ~CassetteWindow();
    CassetteWindow(const CassetteWindow &) = delete;
    CassetteWindow &operator=(const CassetteWindow &) = delete;

    void update(const RecorderVm &rec);
    /* Pauses the hubs (another page in front) without forgetting the transport. */
    void set_visible(bool visible);

    /* For the timing log: frames swapped since start. */
    long frames_swapped() const { return swaps_; }

private:
    static void timer_cb(lv_timer_t *t);
    static void ring_draw_cb(lv_event_t *e);
    static void badge_draw_cb(lv_event_t *e);
    static void cancel_cb(lv_event_t *e);
    void render_tape();
    void render_hub_frames();
    void tick();
    void set_speeds(Transport t);

    Fit fit_;
    Layout lay_;
    AnimOptions anim_;
    std::function<void()> on_cancel_;
    double k_ = 1;              /* cassette px per drawing unit */
    lv_obj_t *window_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_draw_buf_t *tape_buf_ = nullptr;
    lv_obj_t *hub_[2] = {};
    lv_draw_buf_t *frames_[12] = {};
    int hub_box_ = 0;
    double angle_[2] = {0, 0};  /* degrees, counterclockwise positive */
    double speed_[2] = {0, 0};  /* degrees per second */
    int frame_[2] = {0, 0};
    std::uint32_t last_ms_ = 0;
    lv_timer_t *timer_ = nullptr;
    bool visible_ = true;
    long swaps_ = 0;

    lv_obj_t *count_ = nullptr; /* the countdown panel (clickable) */
    lv_obj_t *count_title_ = nullptr, *count_ring_ = nullptr, *count_number_ = nullptr, *count_hint_ = nullptr;
    float ring_fraction_ = 1.0f;
    lv_obj_t *badge_ = nullptr;
    bool badge_reverse_ = true;
    std::string badge_text_;
    lv_obj_t *empty_ = nullptr, *empty_msg_ = nullptr, *empty_hint_ = nullptr;

    std::string label_, side_, format_;
    bool loaded_ = true;
    Transport transport_ = Transport::Idle;
};

} // namespace ui
} // namespace cassette
