/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the small widgets of the boards, each placed in board coordinates through a Fit (fit.hpp):
 *   PianoKey    a deck key: charcoal (or red REC / DELETE, green SAVE) gradient, a 4 px lip under it, an icon over its
 *               caption; latched = pressed 3 px down in dark red, dimmed = 35 %. Reports Down, Up and Click.
 *   IconButton  the small charcoal button (Recordings, Settings, stepper ends), icon only.
 *   Stepper     minus button, caption over a value, plus button, in a framed dark panel (CARD, SOFT, ZOOM, POSITION).
 *   Lamp        a small round lamp with its glow (LIM, LOW CUT, REC / PLAY).
 *   Toggle      the settings switch (drawn; the row takes the tap).
 *   SliderBar   the settings slider (drawn; the row takes the touch).
 *   Sticker     the cassette label sticker of the list rows and the trim strip.
 * LVGL thread only; the objects belong to their parent.
 */
#pragma once

#include "fit.hpp"
#include "paint.hpp"
#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>

namespace cassette {
namespace ui {

/* A rectangle in board px. */
struct DRect {
    double x, y, w, h;
};

enum class KeyLook { Charcoal, Red, Green };

/* The caption of a key: font px, letter spacing and the gap between icon and caption, in board px. */
struct KeyType {
    double px = 12, ls = 1.5, gap = 3;
};

class PianoKey {
public:
    PianoKey(lv_obj_t *parent, const Fit &fit, DRect r, KeyType type, std::function<void(KeyEdge)> on_edge);
    void set(Icon icon, const std::string &caption, KeyLook look, bool enabled, bool latched);
    lv_obj_t *obj() const { return obj_; }

private:
    static void draw_cb(lv_event_t *e);
    static void event_cb(lv_event_t *e);
    void place_caption();

    lv_obj_t *obj_ = nullptr;
    lv_obj_t *caption_ = nullptr;
    double s_ = 1;
    int body_h_ = 0, lip_ = 4, off_ = 3, radius_ = 6, gap_ = 3, icon_h_ = 15, px_ = 12, ls_ = 1;
    Icon icon_ = Icon::None;
    KeyLook look_ = KeyLook::Charcoal;
    bool enabled_ = true, latched_ = false, pressed_ = false;
    std::string text_;
    std::function<void(KeyEdge)> on_edge_;
};

class IconButton {
public:
    IconButton(lv_obj_t *parent, const Fit &fit, DRect r, Icon icon, std::function<void()> on_click);
    void set_enabled(bool enabled);
    void set_icon(Icon icon);
    lv_obj_t *obj() const { return obj_; }

private:
    static void draw_cb(lv_event_t *e);
    static void click_cb(lv_event_t *e);
    lv_obj_t *obj_ = nullptr;
    Icon icon_ = Icon::None;
    double s_ = 1;
    bool enabled_ = true;
    std::function<void()> on_click_;
};

/* The value's font px per layout (17 landscape, 14 / 15 / 16 narrow on the boards). */
class Stepper {
public:
    Stepper(lv_obj_t *parent, const Fit &fit, DRect r, const char *caption, double value_px, Icon minus, Icon plus,
            std::function<void(int dir)> on_step);
    /* Moves the stepper (the CARD row hides, SOFT takes the full width). */
    void place(const Fit &fit, DRect r, double value_px);
    void set_value(const std::string &value);
    void set_enabled(bool down, bool up);
    void set_shown(bool shown);
    lv_obj_t *frame() const { return frame_; }

private:
    lv_obj_t *frame_ = nullptr;
    lv_obj_t *caption_ = nullptr, *value_ = nullptr;
    std::unique_ptr<IconButton> down_, up_;
    double value_px_ = 17;
    std::string value_text_;
    double s_ = 1;
};

class Lamp {
public:
    Lamp(lv_obj_t *parent, int x, int y, int d, std::uint32_t rgb, int glow);
    void set(bool lit, std::uint32_t rgb);
    lv_obj_t *obj() const { return obj_; }

private:
    lv_obj_t *obj_ = nullptr;
};

/* Drawn controls of the settings rows (not clickable: the row is). */
lv_obj_t *make_toggle(lv_obj_t *parent, const Fit &fit, double w, double h);
void set_toggle(lv_obj_t *toggle, bool on);
lv_obj_t *make_slider_bar(lv_obj_t *parent, const Fit &fit, double w, double h);
void set_slider_bar(lv_obj_t *bar, int min, int max, int pos);
lv_obj_t *make_chevron(lv_obj_t *parent, const Fit &fit, std::uint32_t rgb);
/* The label sticker: paper, red stripe, the take number in the bold face. w, h, stripe and px in board px. */
lv_obj_t *make_sticker(lv_obj_t *parent, const Fit &fit, double w, double h, double stripe, double px, double text_x,
                       double text_y, int radius);
void set_sticker(lv_obj_t *sticker, const std::string &text);

/* A label in board coordinates: px, letter spacing ls and the line box line_h (the text centred in it vertically, as
 * the boards' line-height does); w <= 0: as wide as its text. */
lv_obj_t *fit_label(lv_obj_t *parent, const Fit &fit, Face face, double px, std::uint32_t rgb, double x, double y, double w,
                    double line_h, lv_text_align_t align = LV_TEXT_ALIGN_LEFT, double ls = 0);
/* Letter spacing in screen px for a board spacing (truncated: Montserrat is already wider than the boards' face). */
int ls_screen(double ls, double s);

/* The dark framed panel (window, meter, steppers): bg, 3 px frame, radius 8, in board coordinates. */
lv_obj_t *framed_panel(lv_obj_t *parent, const Fit &fit, DRect r, std::uint32_t bg);
/* The cream deck body: the panel filling the body under the header with the boards' 8 px margins. */
lv_obj_t *deck_body(lv_obj_t *parent, const Fit &fit);

} // namespace ui
} // namespace cassette
