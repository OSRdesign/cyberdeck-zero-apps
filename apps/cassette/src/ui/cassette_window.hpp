/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the cassette window of the recorder page.
 *
 * The dark window with its 3 px frame and, inside, the cassette: one object (cass_) that holds the canvas the cassette is
 * drawn into once from primitives (shell, cream label with the red stripe, SIDE A, the format, the hub window, the head
 * opening, two screws; no tape: the shell hides it), the two hub images and the take name. The canvas is drawn again only
 * when SIDE A or the format change. The name is a label in the handwriting face (handwriting.hpp) inside a container that
 * clips it: the container's width is the reveal of the load animation (the label inside is laid out at full width, so the
 * letters do not move). The whole cassette object sits in a second container (clip_) that is the window's inner area and
 * clips it, so the load animation moves ONE object (cass_) per frame: nothing else redraws because of it, nothing is
 * allocated.
 *
 * LOAD animation (load_anim.hpp has the timeline): when RecorderVm::tape.load_seq moves and a cassette is to show, the old
 * cassette (if any) drops out through the bottom of the window, the new one falls in from above with a 4 px bounce and
 * the name is written onto the label; 900 ms in all (660 ms without an old cassette). The hubs stay still, the label
 * shows no name before the cassette has landed; loading() is true meanwhile (the UI ignores keys and touches but Esc),
 * before_landing() is true until the landing (the recorder page keeps the strip text and the counter until then; the
 * landed callback tells it to update). finish_load() ends it at once. A change of the cassette without a move of load_seq
 * (a new recording, a rename, the take loaded at launch, an empty deck) is applied at once, no animation.
 *
 * The hubs: one white hub with three spokes (a 60-degree symmetry, so 120 degrees repeat it too) is pre-rendered at
 * start as 12 frames of 10 degrees each (opaque, on the hub window's colour); two small image objects show the frame of
 * their hub. A timer (fps adjustable, CASSETTE_ANIM_FPS, default 20; CASSETTE_ANIM=0 stops it) advances each hub by its
 * angular speed (RecorderVm::hub_dps) and swaps the frame only when the 10-degree step changes, so a frame costs two image
 * blits of about 26 x 26 px: no per-frame image rotation (the Zero 2 W is the slowest board). A tick turns a hub at most
 * 20 degrees (the spokes repeat every 60: a bigger step would look still or backwards). Both hubs turn together, the way a
 * real cassette's do (ui_format.hpp hub_speed_dps):
 *   recording, playing             both counterclockwise, one turn in 2.4 s
 *   FWD (scan or a 5 s jump)       both counterclockwise at 3x
 *   REW (scan or a 5 s jump)       both clockwise at 3x
 *   anything else                  still
 * CASSETTE_ANIM_TICKS=<n> (the scenes): every tick steps 1 / fps seconds and the hubs stop after n ticks, so a shot shows
 * a fixed angle per state.
 *
 * Overlays inside the window: the record countdown (a dark panel, RECORDING IN, a red ring that empties, the big
 * number, the hint; a tap on it calls on_cancel), the scan badge ("<< 5x" / ">> 5x"), and the empty window of the
 * no-sound-input state with its message.
 */
#pragma once

#include "fit.hpp"
#include "load_anim.hpp"
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
    int ticks = 0; /* > 0: a fixed step per tick and only this many ticks (CASSETTE_ANIM_TICKS, the scenes) */
    int load_step_ms = 10; /* the load animation's fixed step per tick in that mode (CASSETTE_LOAD_STEP_MS; 0 shows its first frame) */
};
/* From the environment: CASSETTE_ANIM=0 disables, CASSETTE_ANIM_FPS=<1..60>, CASSETTE_ANIM_TICKS=<n>,
 * CASSETTE_LOAD_STEP_MS=<0..100>. */
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

    /* The load animation runs (input is ignored but Esc). */
    bool loading() const { return load_.active(); }
    /* It runs and the cassette has not landed yet: the strip text and the counter keep what they showed. */
    bool before_landing() const { return pending_load_ || (load_.active() && !landed_); }
    /* Ends the animation at once (Esc): the cassette in its place, the name written, the landed callback called. */
    void finish_load();
    /* Called once per load when the cassette lands (from the timer, from finish_load, or never when it ends otherwise). */
    void set_landed_callback(std::function<void()> cb) { on_landed_ = std::move(cb); }

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
    void apply_target();      /* the cassette as the model says, at rest */
    void begin_load();
    void swap_to_target();    /* the new cassette's texts into the canvas and the name label (no cassette in view) */
    void apply_pose();        /* places the one moving object and the name reveal for the animation's time */
    void layout_name();       /* font rung, size and place of the name label for label_ */
    void set_reveal(int w);
    void step_load(std::uint32_t now);

    Fit fit_;
    Layout lay_;
    AnimOptions anim_;
    std::function<void()> on_cancel_;
    double k_ = 1;              /* cassette px per drawing unit */
    lv_obj_t *window_ = nullptr;
    lv_obj_t *clip_ = nullptr;  /* the window's inner area: clips the moving cassette */
    lv_obj_t *cass_ = nullptr;  /* THE moving object: canvas, hubs and name */
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *name_clip_ = nullptr, *name_label_ = nullptr; /* the reveal container and the handwritten name inside it */
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
    int ticks_done_ = 0;

    lv_obj_t *count_ = nullptr; /* the countdown panel (clickable) */
    lv_obj_t *count_title_ = nullptr, *count_ring_ = nullptr, *count_number_ = nullptr, *count_hint_ = nullptr;
    float ring_fraction_ = 1.0f;
    lv_obj_t *badge_ = nullptr;
    bool badge_reverse_ = true;
    std::string badge_text_;
    lv_obj_t *empty_ = nullptr, *empty_msg_ = nullptr, *empty_hint_ = nullptr;

    std::string label_, side_, format_; /* what the cassette shows now */
    bool loaded_ = true;                /* a cassette is in the window */
    std::string t_label_, t_side_, t_format_; /* what the model says (reached at once, or at the swap of a load) */
    bool t_loaded_ = true;
    bool have_seq_ = false;
    bool pending_load_ = false; /* a load came while another page was in front: it plays when the recorder shows */
    std::uint32_t seq_ = 0;
    LoadAnim load_;
    bool swapped_ = false, landed_ = false;
    int load_ticks_ = 0;
    std::uint32_t load_last_ms_ = 0;
    int base_y_ = 0;       /* cass_'s y at rest, in clip_ */
    int box_w_ = 0;        /* the name's full width (118 draw units) */
    int name_w_ = 0;       /* the revealed name's width at 100 % */
    int reveal_w_ = -1;
    int cass_y_ = 0;
    bool cass_shown_ = true;
    bool counting_ = false; /* the countdown panel covers the cassette (the layout check leaves the name under it out) */
    std::function<void()> on_landed_;
};

} // namespace ui
} // namespace cassette
