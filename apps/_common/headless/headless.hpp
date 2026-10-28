/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness of the cdk apps (see README.md): shared declarations of its pieces. Nothing here is linked into an
 * app's device binary.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

extern "C" void headless_keyboard_init(void);

namespace headless {

/* ------------------------------------------------------------------ images (png_io.cpp) */

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px; /* 0x00RRGGBB */
};

bool write_png(const std::string &path, const Image &img);
bool read_png(const std::string &path, Image &img);
/* Text drawn with FreeType (font: a .ttf path) in rgb at x, y (top of the line), px high. False without the font. */
bool draw_text(Image &img, const std::string &font, int px, int x, int y, uint32_t rgb, const std::string &text);

/* A contact sheet: one row per shot, one column per size, at 1:1, a grey frame around each screen, the size above
 * each column and the shot name above each row. Missing images (a size that skipped a shot) stay empty. */
bool write_sheet(const std::string &path, const std::string &title, const std::vector<std::string> &sizes,
                 const std::vector<std::string> &shots, const std::string &out_dir, const std::string &font);

/* ------------------------------------------------------------------ the fake display and input (platform) */

struct Platform {
    int w = 0, h = 0;
    lv_display_t *display = nullptr;
    lv_indev_t *pointer = nullptr;
    std::vector<uint32_t> shadow; /* the screen, XRGB8888 */
};

Platform &platform();
/* LVGL, a W x H display (XRGB8888, memory), the layout service's metrics for it at ppmm_x100 (<= 0: default
 * density; the generic profile reports the size in mm like a driver, as the launcher's render harness does), the
 * keyboard queue + bridge, a pointer indev, the filesystem service answering font and resource paths. */
bool platform_init(int w, int h, int ppmm_x100, const std::string &resource_root);
void platform_touch(bool pressed, int x, int y);
/* Runs LVGL (and the app's lifecycle service, when on) for ms of real time. */
void run_for(int ms);
/* The screen as shown: the app's display, with the writing zone's band over its bottom rows while the band shows. */
Image screenshot();

/* ------------------------------------------------------------------ the PDA writing zone (report 044) */

/* The harness plays the display backend of an app the launcher runs upright with APPLAUNCH_PDA=1: the band's own LVGL
 * display (cp0_zone_ui_create, the shipped Strokes tables of strokes_dir, no user overlay) under the app's, the touch
 * router (a touch belongs to the app area, the strip or the writing zone by where it starts), the band's pointer, and
 * cp0_display_available() / cp0_display_zone_active() for cp0::ui::zone_host. After platform_init, before cdk::start.
 * False when this size has no band (landscape, or no room: cp0_zone_layout), as on a board. */
bool band_begin(const std::string &strokes_dir);
bool band_on();
bool band_shown();
int band_strip_y(); /* the panel row of the band's top (-1 without the band) */
/* A strip key (Keyb Tab Ctrl Alt Left Up Down Right Enter) or a box (letters digits), screen coordinates. */
bool band_rect(const std::string &name, int *x, int *y, int *w, int *h);
/* A pen stroke through the touch path: the points (screen px), a frame every 1 mm and 10 ms, the lift, 400 ms. */
void band_stroke(const std::vector<std::pair<int, int>> &pts);
/* The glyph's shipped template (its first variant; Strokes table id: a letter, a digit, or a gesture: space enter bksp
 * shift ext) written 12 mm high in the letters or digits box. False without the band or such a glyph. */
bool band_write(bool digits, const std::string &glyph);

/* ------------------------------------------------------------------ the launcher's lifecycle (multitasking P-e) */

/* The harness plays a multitasking launcher (APP-CONTRACT section 13): a SOCK_SEQPACKET pair, the app's end in
 * APPLAUNCH_LIFECYCLE / APPLAUNCH_CONTROL_FD, the toolkit's helper started with the harness's port
 * (cp0_app_lifecycle_lvgl_start: output = the display's flush, input = the scripted pointer); close calls
 * cdk::request_quit(0) as the device runner does. After platform_init and cdk::start. False when it cannot start. */
bool lifecycle_begin();
/* "hide" / "show" / "close": sends "<word> <n>" and runs the app until its answer "ack <n>" (hide, show) or until it asks
 * to quit (close), at most deadline_ms. The answer's time in ms, or -1 when none came. */
int lifecycle_send(const std::string &word, int deadline_ms = 300);
/* What the app sent the launcher meanwhile besides its answers ("home", "switcher"), oldest first; emptied by the call. */
std::vector<std::string> lifecycle_heard();

struct OutputStats {
    bool on = true;             /* the helper's port says the framebuffer may be written */
    long flushes_while_off = 0; /* flushes LVGL made while it was off (nothing written): rendering while hidden */
    long pixels_since_on = 0;   /* distinct screen pixels flushed since it was last turned on */
};
const OutputStats &output_stats();

/* ------------------------------------------------------------------ layout checks (layout_check.cpp) */

struct CheckResult {
    std::vector<std::string> failures;
    std::vector<std::string> notes;
    int interactive = 0;
};

/* Checks the active screen and the top layer (the on-screen keyboard) of the platform's display:
 *   on-screen   nothing is cut by the screen edge (only a scrolling container may hide part of an object)
 *   size        every interactive element (a clickable object with no clickable child) is tall enough: page rows and
 *               fields >= ROW, buttons >= TARGET, the header's Esc >= min(TARGET, header - 2 x status_top) (the layout
 *               service's header rule), on-screen keyboard keys >= CP0_UI_OSK_KEY_MIN_PX
 *   header      no interactive element of the body reaches under the header
 *   keyboard    while the on-screen keyboard is up: the field it is for is visible above it and no interactive
 *               element of the page lies under it
 *   notice      a screen under the app's minimum shows the notice and nothing else to press */
CheckResult check_layout(bool notice_expected);

/* Visible label with exactly this text (top layer first when osk is true, else the active screen first). */
lv_obj_t *find_label(const std::string &text, bool osk);
/* Its nearest clickable ancestor (or itself). */
lv_obj_t *clickable_of(lv_obj_t *obj);
/* The part of obj that is drawn: its coords clipped by every ancestor (empty: w or h <= 0). */
lv_area_t visible_area(lv_obj_t *obj);

} // namespace headless
