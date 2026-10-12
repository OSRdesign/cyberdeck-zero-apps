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
void run_for(int ms);
Image screenshot();

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
