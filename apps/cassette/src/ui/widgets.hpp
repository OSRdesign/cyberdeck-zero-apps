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
    double icon = 1; /* the icon's size against the board's (the square layout draws it larger) */
};

class PianoKey {
public:
    PianoKey(lv_obj_t *parent, const Fit &fit, DRect r, KeyType type, std::function<void(KeyEdge)> on_edge);
    void set(Icon icon, const std::string &caption, KeyLook look, bool enabled, bool latched);
    /* Drawn pressed (a keyboard key held), as a finger on it does. */
    void set_pressed(bool pressed);
    lv_obj_t *obj() const { return obj_; }

private:
    static void draw_cb(lv_event_t *e);
    static void event_cb(lv_event_t *e);
    void place_caption();

    lv_obj_t *obj_ = nullptr;
    lv_obj_t *caption_ = nullptr;
    double s_ = 1, icon_k_ = 1;
    int body_h_ = 0, lip_ = 4, off_ = 3, radius_ = 6, gap_ = 3, icon_h_ = 15, px_ = 12, ls_ = 1;
    Icon icon_ = Icon::None;
    KeyLook look_ = KeyLook::Charcoal;
    bool enabled_ = true, latched_ = false, pressed_ = false, first_ = true;
    std::string text_;
    std::function<void(KeyEdge)> on_edge_;
};

class IconButton {
public:
    IconButton(lv_obj_t *parent, const Fit &fit, DRect r, Icon icon, std::function<void()> on_click, double icon_scale = 1);
    void set_enabled(bool enabled);
    void set_icon(Icon icon);
    lv_obj_t *obj() const { return obj_; }

private:
    static void draw_cb(lv_event_t *e);
    static void click_cb(lv_event_t *e);
    lv_obj_t *obj_ = nullptr;
    Icon icon_ = Icon::None;
    double s_ = 1, icon_k_ = 1;
    bool enabled_ = true;
    std::function<void()> on_click_;
};

/* The stepper's inner measures in board px (the defaults are the 44 px boards'): the - / + buttons' width, the text block
 * (centred in the stepper), the caption's size, letter spacing, top and line, the value's top and line, the buttons' icons. */
struct StepperLook {
    double btn_w = 44, block_h = 44, cap_px = 11, cap_ls = 1.2, cap_top = 6, cap_line = 15, val_top = 21, val_line = 20,
           icon = 1;
};

/* The value's font px per layout (17 landscape, 14 / 15 / 16 narrow on the boards). */
class Stepper {
public:
    Stepper(lv_obj_t *parent, const Fit &fit, DRect r, const char *caption, double value_px, Icon minus, Icon plus,
            std::function<void(int dir)> on_step, const StepperLook &look = StepperLook());
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
    StepperLook look_;
};

class Lamp {
public:
    Lamp(lv_obj_t *parent, int x, int y, int d, std::uint32_t rgb, int glow);
    void set(bool lit, std::uint32_t rgb);
    lv_obj_t *obj() const { return obj_; }

private:
    lv_obj_t *obj_ = nullptr;
    bool known_ = false, lit_ = false;
    std::uint32_t rgb_ = 0;
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

/* The notice of a page: the tan strip of the page's own info line with the message in the strip's bold face, replacing
 * the line while it shows. The text takes one line at the largest size from px down to min_px that fits; a narrow
 * strip takes two lines (the largest size at which both fit the height allowed) before the size shrinks. The height
 * grows from h_min to what the text needs, within h_max; anchor_bottom keeps the bottom edge (y is then that edge),
 * else the top edge. With a tap handler the strip is a touch target (the narrow recorder's counter row). */
struct NoticeSpec {
    int x = 0, y = 0, w = 0, h_min = 20, h_max = 20;
    int px = 14, min_px = 11, ls = 0, pad = 10, radius = 5;
    bool anchor_bottom = false;
};
class NoticeBox {
public:
    NoticeBox(lv_obj_t *parent, const NoticeSpec &spec, std::function<void()> on_tap = nullptr);
    /* Shows `text` (empty: hides the strip); true when it shows. */
    bool set(const std::string &text);
    bool shown() const { return shown_; }
    int height() const { return h_; }
    lv_obj_t *obj() const { return bg_; }
    int lines() const { return lines_; }

private:
    static void click_cb(lv_event_t *e);
    lv_obj_t *bg_ = nullptr, *text_ = nullptr;
    NoticeSpec spec_;
    std::function<void()> on_tap_;
    std::string shown_text_;
    bool shown_ = false;
    int h_ = 0, lines_ = 1;
};

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
